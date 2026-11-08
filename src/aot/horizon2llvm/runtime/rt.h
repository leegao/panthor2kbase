// Runtime-internal declarations: guest address layout, loader, dispatch.
// Structure follows LASRecomp runtime/rt.h (MIT); the layout and module set are TOTK's.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "cpu.h"
#include "platform.h"

#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define FATAL(...) (fprintf(stderr, "FATAL: " __VA_ARGS__), rt_fatal_trace())

// ---- guest address layout (guest addresses == host addresses)
// The SDK encodes IPC buffer addresses in 39 bits, so all guest memory lies below 2^39. The window is reserved at
// startup; pages are committed when first used. The code bases are baked into the generated C (tools/gen_game.py).
#define GUEST_LO      0x0800000000ull
#define GUEST_HI      0x8000000000ull   // 2^39
#define MAIN_BASE     0x0800000000ull
#define SUBSDK0_BASE  0x0804000000ull
#define SDK_BASE      0x0805000000ull
#define HLE_BASE      0x0808000000ull   // fake guest addresses of host-implemented functions (16 bytes each)
#define HLE_SIZE      0x0000100000ull
#define TLS_BASE      0x0809000000ull   // per-thread TLS blocks (tpidrro_el0)
#define TLS_SIZE      0x0000100000ull
#define HLE_HEAP      0x080A000000ull   // guest-visible memory owned by host code
#define HLE_HEAP_SIZE 0x000E000000ull

// ---- loader.c
typedef struct Module {
    const char *name;
    uint64_t base, size;
    uint64_t text_off, text_size, ro_off, ro_size, data_off, data_size;
    uint64_t dynamic, init, fini, init_array, init_array_sz, fini_array, fini_array_sz;
    uint64_t symtab, strtab, nsyms;
} Module;

#define MAX_MODULES 3
extern Module g_modules[MAX_MODULES];
extern int g_nmodules;
void loader_load(const char *exefs_dir);
uint64_t loader_find_symbol(const char *name);
const char *loader_symbol_at(uint64_t addr, uint64_t *offset);
const char *loader_module_at(uint64_t addr, uint64_t *offset);

// ---- dispatch.c
typedef void (*HostFn)(Cpu *c);

// Global O(1) direct-mapped shadow dispatch cache used by ShadowDispatchTablePass
// in lifted LLVM IR (`@g_icache_pc` and `@g_icache_fn`, indexed by `(pc >> 2) & 65535`).
#define G_ICACHE_SIZE 65536
#define G_ICACHE_MASK (G_ICACHE_SIZE - 1)
extern uint64_t g_icache_pc[G_ICACHE_SIZE];
extern GuestFn  g_icache_fn[G_ICACHE_SIZE];

void dispatch_init(void);
void dispatch_set_debug(int enable);
int dispatch_get_debug(void);
void dispatch_register_fast(uint64_t addr, GuestFn fn);
GuestFn dispatch_lookup(uint64_t addr);
uint64_t rt_guest_fn_at(uint64_t addr, uint64_t *offset);
const char *rt_format_guest_addr(uint64_t addr, char *buf, size_t cap);
uint64_t hle_register(const char *name, HostFn fn);  // returns the fake guest address
HostFn hle_lookup_impl(const char *name);
uint64_t hle_register_entry(const char *name, const void *entry);  // a registry entry (hle.h HleImplEntry)
const void *hle_lookup_entry_public(const char *name);             // the registry entry for a symbol, or NULL
void hle_override(uint64_t guest_addr, HostFn fn, const char *name);
void *hle_alloc(uint64_t size);
void guest_call(Cpu *c, uint64_t addr);
void rt_fatal_trace(void) __attribute__((noreturn));