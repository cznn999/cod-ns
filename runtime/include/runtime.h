/* Host-side runtime: guest memory layout, heap, thunks, guest calls, logging. */
#ifndef COD_RUNTIME_H
#define COD_RUNTIME_H

#include "recomp.h"
#include "game.h"

#include <stdarg.h>
#include <stddef.h>

/* Guest address space layout. */
#define GUEST_NULL_GUARD   0x00010000u   /* [0, guard) is never mapped */
#define GUEST_SCRATCH_BASE 0x00010000u   /* host-owned static structures (TIB, strings) */
#define GUEST_SCRATCH_END  0x003FF000u
#define GUEST_IMAGE_BASE   0x00400000u   /* the exe: up to COD_IMAGE_END (game.h) */
/* The game DLLs follow the exe (their bases are chosen in CMakeLists.txt, COD_MODULES)
 * and end below THUNK_BASE. */
#define GUEST_STACK_BASE   COD_STACK_BASE
#define GUEST_STACK_SIZE   0x00800000u
#define GUEST_HEAP_BASE    COD_HEAP_BASE

/* Host thunks: fake guest code addresses that dispatch to host functions. They
 * are inside the mapped guest space (between the modules and the stack): code
 * sometimes reads a few bytes at an imported function's address (a dead load),
 * which must not fault. */
#define THUNK_BASE   COD_THUNK_BASE
#define THUNK_STRIDE 4u
#define THUNK_MAX    8192u

/* Return address pushed when the host calls into guest code. */
#define GUEST_RETURN_MAGIC 0xFDFDFD00u

extern uint32_t g_guest_mem_size;

void mem_init(void);
/* Logs the host memory budget (Vita). */
void mem_report(const char *where);
uint32_t mem_heap_limit(void);

/* Scratch allocator for host-owned guest structures that live forever. */
uint32_t scratch_alloc(uint32_t size, uint32_t align);
uint32_t scratch_strdup(const char *s);

/* General guest heap (backs HeapAlloc / VirtualAlloc / COM objects). */
uint32_t heap_alloc(uint32_t size);
uint32_t heap_realloc(uint32_t ptr, uint32_t size);
void heap_free(uint32_t ptr);
uint32_t heap_size(uint32_t ptr);

uint32_t thunk_register(GuestFn fn, const char *name);
uint32_t thunk_register_unique(GuestFn fn, const char *name);
/* Gives every thunk of fn its own entry for cached call sites (GuestFastFn
 * convention: arguments read from the guest stack at esp, the result in the
 * return value; it leaves eax, ecx and esp in the context as the thunk would).
 * Not in profiling builds, which bill thunks in the generic entry. */
void thunk_set_fast(GuestFn fn, GuestFastFn fast);
extern __thread uint32_t g_current_thunk;   /* address of the thunk being executed */
GuestFn thunk_lookup(uint32_t addr);
/* Dispatch entry of a thunk (`fast` with the GuestFastFn convention) for the
 * indirect call caches, or NULL when it has none. */
const GuestFunc *thunk_entry(uint32_t addr);
const char *thunk_name(uint32_t addr);
/* COD_LOG_CALLS diagnostics: log a host call made through a thunk address. */
void rt_log_thunk_call(uint32_t addr);

/* Call guest code from the host (stdcall or cdecl; stack is restored either way). */
uint32_t guest_call(Ctx *c, uint32_t fn, int nargs, const uint32_t *args);
/* A cheap identity of the calling host thread: on the Vita the thread's TLS
 * pointer (TPIDRURO, one instruction; pthread_self is a system call and a list
 * search, and __thread variables are not per thread there). */
#if defined(__vita__)
static inline uintptr_t rt_thread_tag(void)
{
    uintptr_t v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
}
#else
#include <pthread.h>
static inline uintptr_t rt_thread_tag(void) { return (uintptr_t)pthread_self(); }
#endif

/* Gated thunks call rt_thunk_gate (when set) before running: the GL entry points
 * wait there for the render thread (shim/src/render_async.c). */
void thunk_mark_gated(uint32_t addr);
int thunk_is_gated(uint32_t addr);
extern void (*rt_thunk_gate)(void);

/* Host pointer for a guest address. */
static inline void *gptr(uint32_t a) { return a ? (void *)(g_mem + a) : NULL; }
static inline char *gstr(uint32_t a) { return a ? (char *)(g_mem + a) : NULL; }
/* Guest buffer the host writes into (never NULL, unlike gstr). */
static inline char *gbuf(uint32_t a) { return (char *)(g_mem + a); }

/* Stack arguments of a stdcall import: ARG(0) is the first parameter. */
#define ARG(n) RD32(c->r[4] + 4u + 4u * (uint32_t)(n))

static inline void stdcall_ret(Ctx *c, uint32_t value, int nargs)
{
    c->r[0] = value;
    c->r[4] += 4u + 4u * (uint32_t)nargs;
}

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_fatal(Ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
void rt_missing_import(Ctx *c, const char *name);
/* Writes out the buffered log (before exiting). */
void rt_log_flush(void);

/* Game data directory (contains CoDSP.exe, the game DLLs and main/). */
extern char g_game_dir[1024];

#endif
