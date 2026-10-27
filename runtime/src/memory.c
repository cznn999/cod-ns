/* Guest address space, scratch allocator and guest heap. */
#include "runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__vita__) || defined(__SWITCH__)
#include <malloc.h>
#include <string.h>
#endif
#if defined(__vita__)
/* newlib heap: the guest address space (160 MiB) plus host allocations (thread
 * stacks, the shader compiler, the renderer's buffers); textures live in vitaGL's
 * pools. With the extended memory mode (ATTRIBUTE2=12 in param.sfo); the rest of
 * it is vitaGL's. */
int _newlib_heap_size_user = 256 * 1024 * 1024;
#elif !defined(__SWITCH__)
#include <sys/mman.h>
#endif

uint32_t g_guest_mem_size;

void mem_init(void)
{
#if defined(__vita__)
    g_guest_mem_size = 0x0C000000u;   /* 192 MiB: modules, stack, 144 MiB guest heap (hunk inside) */
    g_mem = memalign(4096, g_guest_mem_size);
    if (!g_mem) {
        fprintf(stderr, "cannot allocate guest memory\n");
        exit(1);
    }
    /* All of it: fresh Windows memory (VirtualAlloc, a new process's heap) reads as
     * zeros and the game relies on it, as on the desktop (mmap). */
    memset(g_mem, 0, g_guest_mem_size);
#elif defined(__SWITCH__)
    /* No mmap in libnx: one block from the newlib heap, as on the Vita. Started
     * from the HOME menu over a game (title takeover) the heap has about 3 GiB;
     * from the album (applet mode) a few hundred MiB, so smaller sizes are tried
     * too (kernel32.c then lowers com_hunkMegs). */
    static const uint32_t sizes[] = { 0x20000000u, 0x10000000u, 0x0C000000u };   /* 512, 256, 192 MiB */
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && !g_mem; i++) {
        g_guest_mem_size = sizes[i];
        g_mem = memalign(4096, g_guest_mem_size);
    }
    if (!g_mem) {
        fprintf(stderr, "cannot allocate guest memory\n");
        exit(1);
    }
    /* Fresh Windows memory reads as zeros and the game relies on it. */
    memset(g_mem, 0, g_guest_mem_size);
#else
    g_guest_mem_size = 0x40000000u;   /* 1 GiB, committed lazily by the OS */
    void *p = mmap(NULL, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap guest space");
        exit(1);
    }
    g_mem = p;
    /* Everything but the null guard is read/write. Addresses past the heap stay
     * PROT_NONE so wild pointers fault instead of silently corrupting. */
    if (mprotect(g_mem + GUEST_NULL_GUARD, g_guest_mem_size - GUEST_NULL_GUARD, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect guest space");
        exit(1);
    }
#endif
}

uint32_t mem_heap_limit(void) { return g_guest_mem_size; }

#if defined(__vita__)
#include <psp2/kernel/sysmem.h>
#elif defined(__SWITCH__)
#include <switch/kernel/svc.h>
#endif

/* Host memory budget in the log (Vita: newlib heap and the system's free memory). */
void mem_report(const char *where)
{
#if defined(__vita__)
    struct mallinfo mi = mallinfo();
    SceKernelFreeMemorySizeInfo fm;
    memset(&fm, 0, sizeof(fm));
    fm.size = sizeof(fm);
    sceKernelGetFreeMemorySize(&fm);
    rt_log("memory (%s): newlib %u KiB in use of %u KiB taken, %u KiB free in it; system free: main %u KiB, cdram %u KiB, phycont %u KiB",
           where, (unsigned)(mi.uordblks >> 10), (unsigned)(mi.arena >> 10), (unsigned)(mi.fordblks >> 10),
           (unsigned)(fm.size_user >> 10), (unsigned)(fm.size_cdram >> 10), (unsigned)(fm.size_phycont >> 10));
#elif defined(__SWITCH__)
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    rt_log("memory (%s): %u MiB used of %u MiB, guest space %u MiB", where, (unsigned)(used >> 20),
           (unsigned)(total >> 20), g_guest_mem_size >> 20);
#else
    (void)where;
#endif
}

/* ---- scratch ------------------------------------------------------------ */

static uint32_t scratch_top = GUEST_SCRATCH_BASE;

static pthread_mutex_t scratch_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t scratch_alloc(uint32_t size, uint32_t align)
{
    pthread_mutex_lock(&scratch_lock);
    uint32_t a = (scratch_top + align - 1u) & ~(align - 1u);
    if (a + size > GUEST_SCRATCH_END) {
        fprintf(stderr, "guest scratch area exhausted\n");
        exit(1);
    }
    scratch_top = a + size;
    pthread_mutex_unlock(&scratch_lock);
    memset(g_mem + a, 0, size);
    return a;
}

uint32_t scratch_strdup(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1u;
    uint32_t a = scratch_alloc(n, 1);
    memcpy(g_mem + a, s, n);
    return a;
}

/* ---- heap ---------------------------------------------------------------
 * Boundary-tag allocator with segregated free lists. Every block starts with a
 * 16-byte header: size|used, previous block size, and free-list links. Payloads
 * are 16-byte aligned, which also satisfies what MSVC code expects from HeapAlloc.
 */

#define HDR 16u
#define MIN_BLOCK 32u
#define NBINS 64

static uint32_t heap_start, heap_end;
static uint32_t bins[NBINS];

static inline uint32_t b_size(uint32_t b) { return RD32(b) & ~15u; }
static inline int b_used(uint32_t b) { return (int)(RD32(b) & 1u); }
static inline void b_set(uint32_t b, uint32_t size, int used) { WR32(b, size | (uint32_t)used); }
static inline uint32_t b_prev_size(uint32_t b) { return RD32(b + 4u); }

static int bin_of(uint32_t size)
{
    if (size < 1024u)
        return (int)(size / 32u);                       /* 0..31, exact classes */
    int lg = 31 - __builtin_clz(size);                   /* 10..31 */
    int bin = 32 + (lg - 10) * 2 + (int)((size >> (lg - 1)) & 1u);
    return bin < NBINS ? bin : NBINS - 1;
}

static void list_remove(uint32_t b)
{
    uint32_t prev = RD32(b + 8u), next = RD32(b + 12u);
    if (prev)
        WR32(prev + 12u, next);
    else
        bins[bin_of(b_size(b))] = next;
    if (next)
        WR32(next + 8u, prev);
}

static void list_insert(uint32_t b)
{
    int i = bin_of(b_size(b));
    WR32(b + 8u, 0);
    WR32(b + 12u, bins[i]);
    if (bins[i])
        WR32(bins[i] + 8u, b);
    bins[i] = b;
}

static void set_block(uint32_t b, uint32_t size, int used)
{
    b_set(b, size, used);
    uint32_t n = b + size;
    if (n < heap_end)
        WR32(n + 4u, size);
}

static void heap_init(void)
{
    heap_start = GUEST_HEAP_BASE;
    heap_end = mem_heap_limit() - HDR;
    uint32_t size = heap_end - heap_start;
    set_block(heap_start, size, 0);
    WR32(heap_start + 4u, 0);
    b_set(heap_end, 0, 1);       /* sentinel */
    list_insert(heap_start);
}

static uint32_t heap_alloc_locked(uint32_t size)
{
    if (!heap_start)
        heap_init();
    uint32_t need = (size + HDR + 15u) & ~15u;
    if (need < MIN_BLOCK)
        need = MIN_BLOCK;
    for (int i = bin_of(need); i < NBINS; i++) {
        for (uint32_t b = bins[i]; b; b = RD32(b + 12u)) {
            uint32_t bs = b_size(b);
            if (bs < need)
                continue;
            list_remove(b);
            if (bs - need >= MIN_BLOCK) {
                set_block(b, need, 1);
                uint32_t rest = b + need;
                set_block(rest, bs - need, 0);
                list_insert(rest);
            } else {
                set_block(b, bs, 1);
            }
            return b + HDR;
        }
    }
    return 0;
}

static void heap_free_locked(uint32_t ptr)
{
    if (!ptr)
        return;
    uint32_t b = ptr - HDR;
    uint32_t size = b_size(b);
    uint32_t n = b + size;
    if (n < heap_end && !b_used(n)) {
        list_remove(n);
        size += b_size(n);
    }
    if (b > heap_start) {
        uint32_t p = b - b_prev_size(b);
        if (!b_used(p)) {
            list_remove(p);
            size += b_size(p);
            b = p;
        }
    }
    set_block(b, size, 0);
    list_insert(b);
}

uint32_t heap_size(uint32_t ptr)
{
    return ptr ? b_size(ptr - HDR) - HDR : 0;
}

/* The guest threads share the heap. */
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

/* Highest address handed out so far, logged in 16 MiB steps (memory budget). */
static uint32_t heap_top, heap_top_logged;

uint32_t heap_alloc(uint32_t size)
{
    pthread_mutex_lock(&heap_lock);
    uint32_t p = heap_alloc_locked(size);
    if (p && p + size > heap_top) {
        heap_top = p + size;
        if (heap_top - GUEST_HEAP_BASE >= heap_top_logged + (16u << 20)) {
            heap_top_logged = (heap_top - GUEST_HEAP_BASE) & ~((16u << 20) - 1u);
            rt_log("guest heap: %u MiB in use at the top", (heap_top - GUEST_HEAP_BASE) >> 20);
        }
    }
    pthread_mutex_unlock(&heap_lock);
    if (!p)
        rt_log("guest heap: out of memory for %u bytes", size);
    return p;
}

void heap_free(uint32_t ptr)
{
    pthread_mutex_lock(&heap_lock);
    heap_free_locked(ptr);
    pthread_mutex_unlock(&heap_lock);
}

uint32_t heap_realloc(uint32_t ptr, uint32_t size)
{
    if (!ptr)
        return heap_alloc(size);
    pthread_mutex_lock(&heap_lock);
    uint32_t old = heap_size(ptr);
    uint32_t n = ptr;
    if (size > old) {
        n = heap_alloc_locked(size);
        if (n) {
            memcpy(g_mem + n, g_mem + ptr, old);
            heap_free_locked(ptr);
        }
    }
    pthread_mutex_unlock(&heap_lock);
    return n;
}
