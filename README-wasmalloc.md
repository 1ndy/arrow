# WasmAlloc extension to Arrow

The WasmAlloc arrow extension adds a new allocator to Arrow's
MemroyPool. This new allocator spins up an instance of
[wasmtime](https://wasmtime.dev) and uses the exported linear memory
as the space in which to allocate bytes. This enables zero-copy data
sharing between functions executing on the host and those executing
within Wasmtime.

## Building

1. Add Wasmtime to `cpp/thirdparty/`. You will need the C API version
   of a Wasmtime release. This can be obtained from the [Wasmtime
   Release
   Page](https://github.com/bytecodealliance/wasmtime/releases) and
   will be named something like `
   wasmtime-dev-<arch>-<os>-c-api.tar.xz `. Once this is done,
   `cpp/thirdparty/wasmtime/lib` and `cpp/thirdparty/wasmtime/include`
   should both be valid directories.

2. Arrow is built with CMake and I prefer the Ninja backend. The Arrow
   maintainers recommend and out-of-source build. To configure and
   build the project using the WasmAlloc preset, run

   ```
   cmake <path-to-arrow-src-dir>/cpp -GNinja --preset ninja-features-python-wasmtime
   ```

   and then `ninja` to build. You may need to `ninja install` and
   `ldconfig` to use the newly built Arrow library. It should be
   linkable with `-larrow`.

3. To build pyarrow with WasmAlloc support, I use
   [`uv`](https://github.com/astral-sh/uv). Navigate to the `python/`
   within Arrow and execute

   ```
   uv build
   ```

   The build will produce .whl's and .tar.gz's in `arrow/dist`.