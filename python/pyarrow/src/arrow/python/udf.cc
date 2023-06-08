// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/python/udf.h"
#include "arrow/compute/api_aggregate.h"
#include "arrow/compute/function.h"
#include "arrow/compute/kernel.h"
#include "arrow/python/common.h"
#include "arrow/table.h"
#include "arrow/util/checked_cast.h"

namespace arrow {
using internal::checked_cast;
namespace py {
namespace {

struct PythonUdfKernelState : public compute::KernelState {
  explicit PythonUdfKernelState(std::shared_ptr<OwnedRefNoGIL> function)
      : function(function) {
    Py_INCREF(function->obj());
  }

  // function needs to be destroyed at process exit
  // and Python may no longer be initialized.
  ~PythonUdfKernelState() {
    if (_Py_IsFinalizing()) {
      function->detach();
    }
  }

  std::shared_ptr<OwnedRefNoGIL> function;
};

struct PythonUdfKernelInit {
  explicit PythonUdfKernelInit(std::shared_ptr<OwnedRefNoGIL> function)
      : function(function) {
    Py_INCREF(function->obj());
  }

  // function needs to be destroyed at process exit
  // and Python may no longer be initialized.
  ~PythonUdfKernelInit() {
    if (_Py_IsFinalizing()) {
      function->detach();
    }
  }

  Result<std::unique_ptr<compute::KernelState>> operator()(
      compute::KernelContext*, const compute::KernelInitArgs&) {
    return std::make_unique<PythonUdfKernelState>(function);
  }

  std::shared_ptr<OwnedRefNoGIL> function;
};

struct ScalarUdfAggregator : public compute::KernelState {
  virtual Status Consume(compute::KernelContext* ctx, const compute::ExecSpan& batch) = 0;
  virtual Status MergeFrom(compute::KernelContext* ctx, compute::KernelState&& src) = 0;
  virtual Status Finalize(compute::KernelContext* ctx, Datum* out) = 0;
};

arrow::Status AggregateUdfConsume(compute::KernelContext* ctx,
                                  const compute::ExecSpan& batch) {
  return checked_cast<ScalarUdfAggregator*>(ctx->state())->Consume(ctx, batch);
}

arrow::Status AggregateUdfMerge(compute::KernelContext* ctx, compute::KernelState&& src,
                                compute::KernelState* dst) {
  return checked_cast<ScalarUdfAggregator*>(dst)->MergeFrom(ctx, std::move(src));
}

arrow::Status AggregateUdfFinalize(compute::KernelContext* ctx, arrow::Datum* out) {
  return checked_cast<ScalarUdfAggregator*>(ctx->state())->Finalize(ctx, out);
}

struct PythonTableUdfKernelInit {
  PythonTableUdfKernelInit(std::shared_ptr<OwnedRefNoGIL> function_maker,
                           UdfWrapperCallback cb)
      : function_maker(function_maker), cb(cb) {
    Py_INCREF(function_maker->obj());
  }

  // function needs to be destroyed at process exit
  // and Python may no longer be initialized.
  ~PythonTableUdfKernelInit() {
    if (_Py_IsFinalizing()) {
      function_maker->detach();
    }
  }

  Result<std::unique_ptr<compute::KernelState>> operator()(
      compute::KernelContext* ctx, const compute::KernelInitArgs&) {
    UdfContext udf_context{ctx->memory_pool(), /*batch_length=*/0};
    std::unique_ptr<OwnedRefNoGIL> function;
    RETURN_NOT_OK(SafeCallIntoPython([this, &udf_context, &function] {
      OwnedRef empty_tuple(PyTuple_New(0));
      function = std::make_unique<OwnedRefNoGIL>(
          cb(function_maker->obj(), udf_context, empty_tuple.obj()));
      RETURN_NOT_OK(CheckPyError());
      return Status::OK();
    }));
    if (!PyCallable_Check(function->obj())) {
      return Status::TypeError("Expected a callable Python object.");
    }
    return std::make_unique<PythonUdfKernelState>(std::move(function));
  }

  std::shared_ptr<OwnedRefNoGIL> function_maker;
  UdfWrapperCallback cb;
};

struct PythonUdfScalarAggregatorImpl : public ScalarUdfAggregator {
  PythonUdfScalarAggregatorImpl(UdfWrapperCallback agg_cb,
                                std::shared_ptr<OwnedRefNoGIL> agg_function,
                                std::vector<std::shared_ptr<DataType>> input_types,
                                std::shared_ptr<DataType> output_type)
      : agg_cb(std::move(agg_cb)),
        agg_function(agg_function),
        output_type(std::move(output_type)) {
    Py_INCREF(agg_function->obj());
    std::vector<std::shared_ptr<Field>> fields;
    for (size_t i = 0; i < input_types.size(); i++) {
      fields.push_back(field("", input_types[i]));
    }
    input_schema = schema(std::move(fields));
  };

  ~PythonUdfScalarAggregatorImpl() override {
    if (_Py_IsFinalizing()) {
      agg_function->detach();
    }
  }

  Status Consume(compute::KernelContext* ctx, const compute::ExecSpan& batch) override {
    ARROW_ASSIGN_OR_RAISE(
        auto rb, batch.ToExecBatch().ToRecordBatch(input_schema, ctx->memory_pool()));
    values.push_back(std::move(rb));
    return Status::OK();
  }

  Status MergeFrom(compute::KernelContext* ctx, compute::KernelState&& src) override {
    auto& other_values = checked_cast<PythonUdfScalarAggregatorImpl&>(src).values;
    values.insert(values.end(), std::make_move_iterator(other_values.begin()),
                  std::make_move_iterator(other_values.end()));

    other_values.erase(other_values.begin(), other_values.end());
    return Status::OK();
  }

  Status Finalize(compute::KernelContext* ctx, Datum* out) override {
    auto state =
        arrow::internal::checked_cast<PythonUdfScalarAggregatorImpl*>(ctx->state());
    std::shared_ptr<OwnedRefNoGIL>& function = state->agg_function;
    const int num_args = input_schema->num_fields();

    // Note: The way that batches are concatenated together
    // would result in using double amount of the memory.
    // This is OK for now because non decomposable aggregate
    // UDF is supposed to be used with segmented aggregation
    // where the size of the segment is more or less constant
    // so doubling that is not a big deal. This can be also
    // improved in the future to use more efficient way to
    // concatenate.
    ARROW_ASSIGN_OR_RAISE(auto table,
                          arrow::Table::FromRecordBatches(input_schema, values));
    ARROW_ASSIGN_OR_RAISE(table, table->CombineChunks(ctx->memory_pool()));
    UdfContext udf_context{ctx->memory_pool(), table->num_rows()};

    if (table->num_rows() == 0) {
      return Status::Invalid("Finalized is called with empty inputs");
    }

    RETURN_NOT_OK(SafeCallIntoPython([&] {
      std::unique_ptr<OwnedRef> result;
      OwnedRef arg_tuple(PyTuple_New(num_args));
      RETURN_NOT_OK(CheckPyError());

      for (int arg_id = 0; arg_id < num_args; arg_id++) {
        // Since we combined chunks there is only one chunk
        std::shared_ptr<Array> c_data = table->column(arg_id)->chunk(0);
        PyObject* data = wrap_array(c_data);
        PyTuple_SetItem(arg_tuple.obj(), arg_id, data);
      }
      result = std::make_unique<OwnedRef>(
          agg_cb(function->obj(), udf_context, arg_tuple.obj()));
      RETURN_NOT_OK(CheckPyError());
      // unwrapping the output for expected output type
      if (is_scalar(result->obj())) {
        ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Scalar> val, unwrap_scalar(result->obj()));
        if (*output_type != *val->type) {
          return Status::TypeError("Expected output datatype ", output_type->ToString(),
                                   ", but function returned datatype ",
                                   val->type->ToString());
        }
        out->value = std::move(val);
        return Status::OK();
      }
      return Status::TypeError("Unexpected output type: ",
                               Py_TYPE(result->obj())->tp_name, " (expected Scalar)");
    }));
    return Status::OK();
  }

  UdfWrapperCallback agg_cb;
  std::vector<std::shared_ptr<RecordBatch>> values;
  std::shared_ptr<OwnedRefNoGIL> agg_function;
  std::shared_ptr<Schema> input_schema;
  std::shared_ptr<DataType> output_type;
};

struct PythonUdf : public PythonUdfKernelState {
  PythonUdf(std::shared_ptr<OwnedRefNoGIL> function, UdfWrapperCallback cb,
            std::vector<TypeHolder> input_types, compute::OutputType output_type)
      : PythonUdfKernelState(function),
        cb(cb),
        input_types(input_types),
        output_type(output_type) {}

  UdfWrapperCallback cb;
  std::vector<TypeHolder> input_types;
  compute::OutputType output_type;
  TypeHolder resolved_type;

  Result<TypeHolder> ResolveType(compute::KernelContext* ctx,
                                 const std::vector<TypeHolder>& types) {
    if (input_types == types) {
      if (!resolved_type) {
        ARROW_ASSIGN_OR_RAISE(resolved_type, output_type.Resolve(ctx, input_types));
      }
      return resolved_type;
    }
    return output_type.Resolve(ctx, types);
  }

  Status Exec(compute::KernelContext* ctx, const compute::ExecSpan& batch,
              compute::ExecResult* out) {
    auto state = arrow::internal::checked_cast<PythonUdfKernelState*>(ctx->state());
    std::shared_ptr<OwnedRefNoGIL>& function = state->function;
    const int num_args = batch.num_values();
    UdfContext udf_context{ctx->memory_pool(), batch.length};

    OwnedRef arg_tuple(PyTuple_New(num_args));
    RETURN_NOT_OK(CheckPyError());
    for (int arg_id = 0; arg_id < num_args; arg_id++) {
      if (batch[arg_id].is_scalar()) {
        std::shared_ptr<Scalar> c_data = batch[arg_id].scalar->GetSharedPtr();
        PyObject* data = wrap_scalar(c_data);
        PyTuple_SetItem(arg_tuple.obj(), arg_id, data);
      } else {
        std::shared_ptr<Array> c_data = batch[arg_id].array.ToArray();
        PyObject* data = wrap_array(c_data);
        PyTuple_SetItem(arg_tuple.obj(), arg_id, data);
      }
    }

    OwnedRef result(cb(function->obj(), udf_context, arg_tuple.obj()));
    RETURN_NOT_OK(CheckPyError());
    // unwrapping the output for expected output type
    if (is_array(result.obj())) {
      ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Array> val, unwrap_array(result.obj()));
      ARROW_ASSIGN_OR_RAISE(TypeHolder type, ResolveType(ctx, batch.GetTypes()));
      if (type.type == NULLPTR) {
        return Status::TypeError("expected output datatype is null");
      }
      if (*type.type != *val->type()) {
        return Status::TypeError("Expected output datatype ", type.type->ToString(),
                                 ", but function returned datatype ",
                                 val->type()->ToString());
      }
      out->value = std::move(val->data());
      return Status::OK();
    } else {
      return Status::TypeError("Unexpected output type: ", Py_TYPE(result.obj())->tp_name,
                               " (expected Array)");
    }
    return Status::OK();
  }
};

Status PythonUdfExec(compute::KernelContext* ctx, const compute::ExecSpan& batch,
                     compute::ExecResult* out) {
  auto udf = static_cast<PythonUdf*>(ctx->kernel()->data.get());
  return SafeCallIntoPython([&]() -> Status { return udf->Exec(ctx, batch, out); });
}

Status RegisterUdf(PyObject* user_function, compute::KernelInit kernel_init,
                   UdfWrapperCallback wrapper, const UdfOptions& options,
                   compute::FunctionRegistry* registry) {
  if (!PyCallable_Check(user_function)) {
    return Status::TypeError("Expected a callable Python object.");
  }
  auto scalar_func = std::make_shared<compute::ScalarFunction>(
      options.func_name, options.arity, options.func_doc);
  Py_INCREF(user_function);
  std::vector<compute::InputType> input_types;
  for (const auto& in_dtype : options.input_types) {
    input_types.emplace_back(in_dtype);
  }
  compute::OutputType output_type(options.output_type);
  auto udf_data = std::make_shared<PythonUdf>(
      std::make_shared<OwnedRefNoGIL>(user_function), wrapper,
      TypeHolder::FromTypes(options.input_types), options.output_type);
  compute::ScalarKernel kernel(
      compute::KernelSignature::Make(std::move(input_types), std::move(output_type),
                                     options.arity.is_varargs),
      PythonUdfExec, kernel_init);
  kernel.data = std::move(udf_data);

  kernel.mem_allocation = compute::MemAllocation::NO_PREALLOCATE;
  kernel.null_handling = compute::NullHandling::COMPUTED_NO_PREALLOCATE;
  RETURN_NOT_OK(scalar_func->AddKernel(std::move(kernel)));
  if (registry == NULLPTR) {
    registry = compute::GetFunctionRegistry();
  }
  RETURN_NOT_OK(registry->AddFunction(std::move(scalar_func)));
  return Status::OK();
}

}  // namespace

Status RegisterScalarFunction(PyObject* user_function, UdfWrapperCallback wrapper,
                              const UdfOptions& options,
                              compute::FunctionRegistry* registry) {
  return RegisterUdf(user_function,
                     PythonUdfKernelInit{std::make_shared<OwnedRefNoGIL>(user_function)},
                     wrapper, options, registry);
}

Status RegisterTabularFunction(PyObject* user_function, UdfWrapperCallback wrapper,
                               const UdfOptions& options,
                               compute::FunctionRegistry* registry) {
  if (options.arity.num_args != 0 || options.arity.is_varargs) {
    return Status::NotImplemented("tabular function of non-null arity");
  }
  if (options.output_type->id() != Type::type::STRUCT) {
    return Status::Invalid("tabular function with non-struct output");
  }
  return RegisterUdf(
      user_function,
      PythonTableUdfKernelInit{std::make_shared<OwnedRefNoGIL>(user_function), wrapper},
      wrapper, options, registry);
}

Status AddAggKernel(std::shared_ptr<compute::KernelSignature> sig,
                    compute::KernelInit init, compute::ScalarAggregateFunction* func) {
  compute::ScalarAggregateKernel kernel(std::move(sig), std::move(init),
                                        AggregateUdfConsume, AggregateUdfMerge,
                                        AggregateUdfFinalize, /*ordered=*/false);
  RETURN_NOT_OK(func->AddKernel(std::move(kernel)));
  return Status::OK();
}

Status RegisterAggregateFunction(PyObject* agg_function, UdfWrapperCallback agg_wrapper,
                                 const UdfOptions& options,
                                 compute::FunctionRegistry* registry) {
  if (!PyCallable_Check(agg_function)) {
    return Status::TypeError("Expected a callable Python object.");
  }

  if (registry == NULLPTR) {
    registry = compute::GetFunctionRegistry();
  }

  // Py_INCREF here so that once a function is registered
  // its refcount gets increased by 1 and doesn't get gced
  // if all existing refs are gone
  Py_INCREF(agg_function);

  static auto default_scalar_aggregate_options =
      compute::ScalarAggregateOptions::Defaults();
  auto aggregate_func = std::make_shared<compute::ScalarAggregateFunction>(
      options.func_name, options.arity, options.func_doc,
      &default_scalar_aggregate_options);

  std::vector<compute::InputType> input_types;
  for (const auto& in_dtype : options.input_types) {
    input_types.emplace_back(in_dtype);
  }
  compute::OutputType output_type(options.output_type);

  compute::KernelInit init = [agg_wrapper, agg_function, options](
                                 compute::KernelContext* ctx,
                                 const compute::KernelInitArgs& args)
      -> Result<std::unique_ptr<compute::KernelState>> {
    return std::make_unique<PythonUdfScalarAggregatorImpl>(
        agg_wrapper, std::make_shared<OwnedRefNoGIL>(agg_function), options.input_types,
        options.output_type);
  };

  RETURN_NOT_OK(AddAggKernel(
      compute::KernelSignature::Make(std::move(input_types), std::move(output_type),
                                     options.arity.is_varargs),
      init, aggregate_func.get()));

  RETURN_NOT_OK(registry->AddFunction(std::move(aggregate_func)));
  return Status::OK();
}

Result<std::shared_ptr<RecordBatchReader>> CallTabularFunction(
    const std::string& func_name, const std::vector<Datum>& args,
    compute::FunctionRegistry* registry) {
  if (args.size() != 0) {
    return Status::NotImplemented("non-empty arguments to tabular function");
  }
  if (registry == NULLPTR) {
    registry = compute::GetFunctionRegistry();
  }
  ARROW_ASSIGN_OR_RAISE(auto func, registry->GetFunction(func_name));
  if (func->kind() != compute::Function::SCALAR) {
    return Status::Invalid("tabular function of non-scalar kind");
  }
  auto arity = func->arity();
  if (arity.num_args != 0 || arity.is_varargs) {
    return Status::NotImplemented("tabular function of non-null arity");
  }
  auto kernels =
      arrow::internal::checked_pointer_cast<compute::ScalarFunction>(func)->kernels();
  if (kernels.size() != 1) {
    return Status::NotImplemented("tabular function with non-single kernel");
  }
  const compute::ScalarKernel* kernel = kernels[0];
  auto out_type = kernel->signature->out_type();
  if (out_type.kind() != compute::OutputType::FIXED) {
    return Status::Invalid("tabular kernel of non-fixed kind");
  }
  auto datatype = out_type.type();
  if (datatype->id() != Type::type::STRUCT) {
    return Status::Invalid("tabular kernel with non-struct output");
  }
  auto struct_type = arrow::internal::checked_cast<StructType*>(datatype.get());
  auto schema = ::arrow::schema(struct_type->fields());
  std::vector<TypeHolder> in_types;
  ARROW_ASSIGN_OR_RAISE(auto func_exec,
                        GetFunctionExecutor(func_name, in_types, NULLPTR, registry));
  auto next_func = [schema, func_exec = std::move(
                                func_exec)]() -> Result<std::shared_ptr<RecordBatch>> {
    std::vector<Datum> args;
    // passed_length of -1 or 0 with args.size() of 0 leads to an empty ExecSpanIterator
    // in exec.cc and to never invoking the source function, so 1 is passed instead
    // TODO: GH-33612: Support batch size in user-defined tabular functions
    ARROW_ASSIGN_OR_RAISE(auto datum, func_exec->Execute(args, /*passed_length=*/1));
    if (!datum.is_array()) {
      return Status::Invalid("UDF result of non-array kind");
    }
    std::shared_ptr<Array> array = datum.make_array();
    if (array->length() == 0) {
      return IterationTraits<std::shared_ptr<RecordBatch>>::End();
    }
    ARROW_ASSIGN_OR_RAISE(auto batch, RecordBatch::FromStructArray(std::move(array)));
    if (!schema->Equals(batch->schema())) {
      return Status::Invalid("UDF result with shape not conforming to schema");
    }
    return std::move(batch);
  };
  return RecordBatchReader::MakeFromIterator(MakeFunctionIterator(std::move(next_func)),
                                             schema);
}

}  // namespace py
}  // namespace arrow
