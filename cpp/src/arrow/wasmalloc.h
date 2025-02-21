#include <stdio.h>
#include <string.h>

#include "wasm.h"
#include "wasmtime.h"


bool initialized = false;
const char* moduletxt = "(module (memory (export \"memory\") 8 8))";
wasm_engine_t *engine;
wasmtime_store_t *store;
wasmtime_context_t *context;
wasmtime_module_t *module;
wasmtime_instance_t instance;
uint8_t* memory_addr;
uint8_t* base;
uint8_t* end;

void wasmalloc_init() {
    // turn wat into wasm bytes
    wasm_byte_vec_t wat;
    printf("Initializing wat struct\n");
    wasm_byte_vec_new_uninitialized(&wat, strlen(moduletxt));
    strcpy(wat.data, moduletxt);
    wasm_byte_vec_t wasm;
    printf("Converting wat to wasm\n");
    wasmtime_error_t *error = wasmtime_wat2wasm(wat.data, wat.size, &wasm);
    if (error != NULL) {
        printf("Failed to parse wat in wasmalloc\n");
        exit(1);
    }

    // wasmtime boilerplate
    printf("Creating wasmtime engine\n");
    engine = wasm_engine_new();
    store = wasmtime_store_new(engine, NULL, NULL);
    context = wasmtime_store_context(store);
    wasmtime_module_new(engine, (uint8_t*)wasm.data, wasm.size, &(module));

    // compile wasm bytes
    printf("Compiling wasm\n");
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
    printf("Fetching exported memory\n");
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
    initialized = true;
}

bool wasmalloc_allocate_aligned(int64_t size, int64_t alignment, uint8_t** out) {
    printf("Wasmalloc called for %ld bytes\n", size);
    if (!initialized) {
        printf("This is the first call, initializing wasmalloc\n");
        wasmalloc_init();
    }
    if (base + size > end) {
        return false;
    } else {
        printf("Aligning pointer\n");
        uint64_t oldbase = (uint64_t) base;
        while ((uint64_t)base % alignment != 0) {
            base++;
        }
        printf("Advanced base %d bytes to %p\n", (uint64_t)(base - oldbase), base);
        *out = base;
        base += size;
        return true;
    }
}

void wasmalloc_print_stats() {
    printf("Wasmalloc stats:\n\tBase:\t%ld\n\tEnd:\t%ld(Offset %ld)\n\tCurrent:\t%ld\n\tRemaining:\t%ld\n\n",
        (uint64_t)memory_addr, 
        (uint64_t)end, 
        ((uint64_t)end - (uint64_t)memory_addr), 
        (uint64_t)base, 
        ((uint64_t)end - (uint64_t)base + (uint64_t)memory_addr));
}