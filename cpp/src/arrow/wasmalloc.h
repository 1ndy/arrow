#include <stdio.h>
#include <string.h>

#include "wasm.h"
#include "wasmtime.h"

#include "tinyalloc.h"

/*

WasmAlloc allocates within a memory region that is visible to functions inside a
webassembly runtime. Uses [tinyalloc](https://github.com/thi-ng/tinyalloc).

*/

#define MODULE_FILE_ENV_VAR "ARROW_WASMALLOC_MODULE_FILE"

bool initialized = false;
char* filename = NULL;
const char* default_module = "(module (memory (export \"memory\") 65536 65536))";

// default module is just 4 GiB of memory
wasm_engine_t *engine;
wasmtime_store_t *store;
wasmtime_context_t *context;
wasmtime_module_t *module;
wasmtime_instance_t instance;
uint8_t* memory_addr;
uint8_t* base;
uint8_t* end;

void create_default_module(wasm_byte_vec_t* wat) {
    int default_module_size = strlen(default_module);
    printf("module size is %d\n", default_module_size);
    wasm_byte_vec_new_uninitialized(wat, default_module_size);
    printf("calling strcpy\n");
    strcpy(wat->data, default_module);
    printf("done\n");
}

void read_wat(wasm_byte_vec_t* wat, char* filename) {
    FILE *watfile;
    size_t filesize;
    watfile = fopen(filename, "r");
    if (watfile == NULL) {
        printf("Failed to open wat file '%s', using default module\n", filename);
        return create_default_module(wat);
    }
    fseek(watfile, 0L, SEEK_END);
    filesize = ftell(watfile);
    fseek(watfile, 0L, SEEK_SET);
    wasm_byte_vec_new_uninitialized(wat, filesize);
    if (fread(wat->data, filesize, 1, watfile) != 1) {
        printf("Failed to load module from '%s', using default module\n", filename);
        fclose(watfile);
        return create_default_module(wat);
    }
    fclose(watfile);
}

void wasmalloc_init() {
    // create a wasm module from a WAT file or use the default memory-only
    // module
    wasm_byte_vec_t wat;
    filename = getenv(MODULE_FILE_ENV_VAR);
    if (filename == NULL) {
        printf("%s was not set; using default module\n", MODULE_FILE_ENV_VAR);
        create_default_module(&wat);
    } else {
        read_wat(&wat, filename);
    }
    wasm_byte_vec_t wasm;
    // printf("Converting wat to wasm\n");
    wasmtime_error_t *error = wasmtime_wat2wasm(wat.data, wat.size, &wasm);
    if (error != NULL) {
        printf("Failed to parse wat in wasmalloc\n");
        exit(1);
    }

    // wasmtime boilerplate
    // printf("Creating wasmtime engine\n");
    engine = wasm_engine_new();
    store = wasmtime_store_new(engine, NULL, NULL);
    context = wasmtime_store_context(store);
    wasmtime_module_new(engine, (uint8_t*)wasm.data, wasm.size, &(module));

    // compile wasm bytes
    // printf("Compiling wasm\n");
    wasm_byte_vec_delete(&wasm);
    if (error != NULL) {
        printf("Failed to compile wasm module in wasmalloc\n");
        exit(1);
    }

    wasm_trap_t *trap = NULL;
    error = wasmtime_instance_new(context, module, NULL, 0, &instance, &trap);
    if (error != NULL || trap != NULL) {
        printf("Failed to create instance\n");
        exit(1);
    }

    // grab exported memory
    // printf("Fetching exported memory\n");
    wasmtime_extern_t item;
    bool ok = wasmtime_instance_export_get(context, &instance, "memory", strlen("memory"), &item);
    if(!ok || item.kind != WASMTIME_EXTERN_MEMORY) {
        printf("Failed to get linear memory");
        exit(1);
    }
    memory_addr = (uint8_t*)wasmtime_memory_data(context, &(item.of.memory));
    printf("[wasmalloc] have memory ptr at %p\n", memory_addr);

    base = memory_addr;
    printf("wasmalloc has %ld bytes of memory available\n", wasmtime_memory_size(context, &(item.of.memory)) * 64 * 1024);
    end = base + wasmtime_memory_size(context, &(item.of.memory)) * 64 * 1024; // wasm page size = 64 KiB
    
    ta_init(memory_addr, end, 256, 16, 128);
    
    initialized = true;
}

bool wasmalloc_allocate_aligned(int64_t size, int64_t alignment, uint8_t** out) {
    if (!initialized) {
        // printf("This is the first call, initializing wasmalloc\n");
        wasmalloc_init();
    }
    if (alignment > 128) {
        // tinayalloc alignment is fixed at 128
        return false;
    } else {
        uint8_t* addr = (uint8_t*)ta_alloc(size);
        if (addr != NULL) {
            *out = addr;
            return true;
        } else {
            return false;
        }
    }
}

void wasmalloc_free(uint8_t* ptr) {
    ta_free((void*)ptr);
}

void wasmalloc_print_stats() {
    if (ta_check()) {
        printf("arrow::MemoryPool stats: Wasmalloc is vibing\n");
    } else {
        printf("arrow::MemoryPool stats: Wasmalloc is not feeling it\n");
    }
    // printf("Wasmalloc stats:\n\tBase:\t%ld\n\tEnd:\t%ld(Offset %ld)\n\tCurrent:\t%ld\n\tRemaining:\t%ld\n\n",
    //     (uint64_t)memory_addr, 
    //     (uint64_t)end, 
    //     ((uint64_t)end - (uint64_t)memory_addr), 
    //     (uint64_t)base, 
    //     ((uint64_t)end - (uint64_t)base));
}