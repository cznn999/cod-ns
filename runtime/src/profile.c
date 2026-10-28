/* Sampling profiler for the recompiled game (CMake option COD_PROFILE).
 *
 * Generated code publishes the id of the code that is running in rt_prof_cur (a
 * guest function address, the IAT slot of a host import, or a host thunk address
 * for COM methods and callbacks) and keeps a shadow stack of guest functions (see
 * PROF_ENTER in recomp.h). A thread on another core samples both about 1000
 * times a second and accumulates:
 *   - exclusive samples per id (where the time is spent),
 *   - inclusive samples per guest function (on the shadow stack),
 *   - caller/callee pairs (which guest function the current code was called from),
 *   - call edges on the shadow stack (inclusive samples per caller -> callee: a call tree).
 * Results go to ux0:data/cod1/profile.txt (Vita), sdmc:/switch/cod1/profile.txt
 * (Switch) or cod1_profile.txt (desktop) every
 * 10 seconds and at exit.
 *
 * Per-frame mode (rt_prof_reset(1)): samples wait in a buffer until the game thread
 * ends the frame (rt_prof_frame_end) and are counted only if it keeps the frame. */
#include "runtime.h"

typedef int profile_translation_unit_is_not_empty;

#ifdef COD_PROFILE

#include <stdio.h>
#include <stdlib.h>

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#define PROFILE_PATH "ux0:data/cod1/profile.txt"
#elif defined(__SWITCH__)
#include <pthread.h>
#include <unistd.h>
#define PROFILE_PATH "sdmc:/switch/cod1/profile.txt"
#else
#include <pthread.h>
#include <unistd.h>
#define PROFILE_PATH "cod1_profile.txt"
#endif

RtProf rt_prof_main;
volatile uint32_t rt_prof_render;

#define SAMPLE_US 1000u
#define DUMP_EVERY 10000u        /* samples */
#define MAP_BITS 15
#define MAP_SIZE (1u << MAP_BITS)
#define TOP 80

typedef struct Entry {
    uint64_t key;
    uint32_t count;
    uint32_t used;
} Entry;

typedef struct Map {
    Entry e[MAP_SIZE];
    uint32_t n;
} Map;

static Map excl, incl, pairs, edges, render_excl;
static uint32_t total, taken;
static volatile int reset_requested, stopped;

/* Per-frame mode: the game thread writes the frame's verdict to keep_frame[], then
 * bumps frame_seq; the sampler commits or drops its buffer when the seq changes. */
#define PEND_MAX 1024u
#define PEND_DEPTH 64u
typedef struct Pending {
    uint32_t cur, depth;
    uint32_t stack[PEND_DEPTH];
} Pending;
static Pending pend[PEND_MAX];
static uint32_t n_pend, pend_seq, frames_kept, frames_seen;
static volatile int per_frame, per_frame_requested;
static volatile uint32_t frame_seq;
static volatile uint8_t keep_frame[256];

static void map_add(Map *m, uint64_t key)
{
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> (64 - MAP_BITS));
    for (uint32_t probes = 0; probes < MAP_SIZE; probes++) {
        Entry *e = &m->e[i];
        if (e->used && e->key == key) {
            e->count++;
            return;
        }
        if (!e->used) {
            if (m->n >= MAP_SIZE * 3u / 4u)
                return;              /* full: drop rare keys */
            e->used = 1;
            e->key = key;
            e->count = 1;
            m->n++;
            return;
        }
        i = (i + 1u) & (MAP_SIZE - 1u);
    }
}

static void count(uint32_t cur, const uint32_t *stack, uint32_t depth)
{
    total++;
    map_add(&excl, cur);
    /* Each guest function on the stack counts once per sample (recursion). */
    for (uint32_t i = 0; i < depth; i++) {
        uint32_t j = 0;
        while (j < i && stack[j] != stack[i])
            j++;
        if (j == i)
            map_add(&incl, stack[i]);
    }
    for (uint32_t i = 1; i < depth; i++) {
        uint64_t key = ((uint64_t)stack[i - 1] << 32) | stack[i];
        uint32_t j = 1;
        while (j < i && (stack[j - 1] != stack[i - 1] || stack[j] != stack[i]))
            j++;
        if (j == i)
            map_add(&edges, key);
    }
    /* Caller: the innermost guest function below the current code. */
    uint32_t caller = 0;
    if (depth && stack[depth - 1] != cur)
        caller = stack[depth - 1];
    else if (depth >= 2)
        caller = stack[depth - 2];
    map_add(&pairs, ((uint64_t)caller << 32) | cur);
}

extern RtProf rt_prof_render_thread __attribute__((weak));

static void sample(void)
{
    /* The render thread (shim/src/render_async.c) when there is one: 0 is idle. */
    if (&rt_prof_render_thread) {
        uint32_t r = rt_prof_render_thread.cur;
        map_add(&render_excl, r ? r : PROF_H_RIDLE);
    } else {
        map_add(&render_excl, rt_prof_render ? rt_prof_render : PROF_H_RUN);
    }
    uint32_t cur = rt_prof_cur;
    uint32_t depth = rt_prof_main.depth;
    if (depth > RT_PROF_MAX)
        depth = RT_PROF_MAX;
    if (!per_frame) {
        uint32_t stack[RT_PROF_MAX];
        for (uint32_t i = 0; i < depth; i++)
            stack[i] = rt_prof_main.stack[i];
        count(cur, stack, depth);
        return;
    }
    uint32_t seq = frame_seq;
    __sync_synchronize();
    while (pend_seq != seq) {
        /* Frames ended since the last sample: the buffer belongs to the first one. */
        frames_seen++;
        if (keep_frame[pend_seq & 255u]) {
            frames_kept++;
            for (uint32_t i = 0; i < n_pend; i++)
                count(pend[i].cur, pend[i].stack, pend[i].depth);
        }
        n_pend = 0;
        pend_seq++;
    }
    if (n_pend >= PEND_MAX)
        return;
    /* The outermost frames are kept: they name the scheduler step (update or render). */
    Pending *p = &pend[n_pend++];
    p->cur = cur;
    p->depth = depth < PEND_DEPTH ? depth : PEND_DEPTH;
    for (uint32_t i = 0; i < p->depth; i++)
        p->stack[i] = rt_prof_main.stack[i];
}

static const char *name_of(uint32_t id, char *buf, size_t n)
{
    if (id == 0) {
        snprintf(buf, n, "(host, outside guest code)");
        return buf;
    }
    for (uint32_t i = 0; i < g_guest_import_count; i++)
        if (g_guest_imports[i].slot == id) {
            snprintf(buf, n, "%s!%s", g_guest_imports[i].dll, g_guest_imports[i].name);
            return buf;
        }
    static const char *const host[] = { "?", "decode", "near clip", "pack (compacted draw gather)", "batch", "gl state", "gl draw (vitaGL draw call)",
                                        "texture", "blit", "audio mix", "index rebase", "fill (render target)",
                                        "fill (memory surface)", "blit (memory surface)",
                                        "render thread idle (waiting for the game thread)",
                                        "render thread swap (vsync, GPU)", "render thread other commands",
                                        "draw: buffer objects (cache lookup/refresh)", "draw: render states", "draw: texture stages",
                                        "draw: transforms", "draw: vertex pointers (gl*Pointer)", "draw: gl draw call" };
    if ((id & 0xFFFFFF00u) == 0xFD000000u && (id & 0xFFu) < sizeof(host) / sizeof(host[0])) {
        snprintf(buf, n, "host %s", host[id & 0xFFu]);
        return buf;
    }
    static const char *const vgl[] = { "draw entry", "ffp mask", "ffp attribs", "vertex patch",
                                       "fragment/programs", "uniforms", "textures", "indices",
                                       "sceGxmDraw", "vertex streams" };
    if (id >= PROF_H_VGL && id < PROF_H_VGL + sizeof(vgl) / sizeof(vgl[0])) {
        snprintf(buf, n, "vgl %s", vgl[id - PROF_H_VGL]);
        return buf;
    }
    if (id >= THUNK_BASE && id < THUNK_BASE + THUNK_MAX * THUNK_STRIDE) {
        snprintf(buf, n, "thunk %s", thunk_name(id));
        return buf;
    }
    snprintf(buf, n, "f_%08X", id);
    return buf;
}

static int by_count(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1;
    return x->key < y->key ? -1 : x->key > y->key;
}

static void dump_map(FILE *f, const Map *m, const char *title, int is_pair, uint32_t top)
{
    static Entry sorted[MAP_SIZE];
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAP_SIZE; i++)
        if (m->e[i].used)
            sorted[n++] = m->e[i];
    qsort(sorted, n, sizeof(Entry), by_count);
    fprintf(f, "\n%s\n", title);
    for (uint32_t i = 0; i < n && i < top; i++) {
        char a[160], b[160];
        double pct = total ? 100.0 * sorted[i].count / total : 0.0;
        if (is_pair)
            fprintf(f, "%6.2f%% %8u  %-32s <- %s\n", pct, sorted[i].count,
                    name_of((uint32_t)sorted[i].key, a, sizeof(a)),
                    name_of((uint32_t)(sorted[i].key >> 32), b, sizeof(b)));
        else
            fprintf(f, "%6.2f%% %8u  %08X  %s\n", pct, sorted[i].count, (uint32_t)sorted[i].key,
                    name_of((uint32_t)sorted[i].key, a, sizeof(a)));
    }
}

#if defined(COD_CALLCOUNT) && defined(__ELF__)
/* The generated functions' counters (PROF_COUNT in recomp.h), bounds from the linker. */
extern RtCallCount __start_cod_calls[], __stop_cod_calls[];
#if defined(__vita__)
#define CALLS_PATH "ux0:data/cod1/calls.txt"
#elif defined(__SWITCH__)
#define CALLS_PATH "sdmc:/switch/cod1/calls.txt"
#else
#define CALLS_PATH "cod_calls.txt"
#endif
static void calls_reset(void)
{
    for (RtCallCount *p = __start_cod_calls; p < __stop_cod_calls; p++)
        p->n = 0;
}
/* "address count" of every function called since the last reset (tools/recomp/hot.py). */
static void calls_dump(void)
{
    FILE *f = fopen(CALLS_PATH, "w");
    if (!f)
        return;
    for (const RtCallCount *p = __start_cod_calls; p < __stop_cod_calls; p++)
        if (p->n)
            fprintf(f, "%08X %u\n", p->addr, p->n);
    fclose(f);
}
#else
static void calls_reset(void) {}
static void calls_dump(void) {}
#endif

static void dump(void)
{
    calls_dump();
    FILE *f = fopen(PROFILE_PATH, "w");
    if (!f)
        return;
    fprintf(f, "FF profile: %u samples (about %u ms of run time, one sample per %u us)\n", total,
            total * SAMPLE_US / 1000u, SAMPLE_US);
    if (per_frame)
        fprintf(f, "Per-frame mode: samples of %u of %u frames kept\n", frames_kept, frames_seen);
    dump_map(f, &excl, "Exclusive (code running when sampled; imports as dll!name, COM/callbacks as thunk):", 0, TOP);
    dump_map(f, &render_excl, "Render thread (same samples; vgl = inside vitaGL):", 0, TOP);
    dump_map(f, &incl, "Inclusive (guest functions on the shadow call stack):", 0, TOP);
    dump_map(f, &pairs, "Current code <- calling guest function:", 1, TOP);
    dump_map(f, &edges, "Call edges, inclusive (callee <- caller):", 1, TOP * 4u);
    fclose(f);
}

static void sampler_loop(void)
{
    for (;;) {
#if defined(__vita__)
        sceKernelDelayThread(SAMPLE_US);
#else
        usleep(SAMPLE_US);
#endif
        if (stopped)
            continue;
        if (reset_requested) {
            memset(&excl, 0, sizeof(excl));
            memset(&incl, 0, sizeof(incl));
            memset(&pairs, 0, sizeof(pairs));
            memset(&edges, 0, sizeof(edges));
            memset(&render_excl, 0, sizeof(render_excl));
            calls_reset();
            total = 0;
            n_pend = 0;
            frames_kept = frames_seen = 0;
            pend_seq = frame_seq;
            per_frame = per_frame_requested;
            reset_requested = 0;
        }
        sample();
        /* Every DUMP_EVERY samples taken (counted or not: per-frame mode adds a
         * frame's samples at once, and may keep none for a while). */
        if (++taken % DUMP_EVERY == 0)
            dump();
    }
}

#if defined(__vita__)
static int sampler_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    sampler_loop();
    return 0;
}
#else
static void *sampler_thread(void *arg)
{
    (void)arg;
    sampler_loop();
    return NULL;
}
#endif

void rt_prof_reset(int per_frame_mode)
{
    per_frame_requested = per_frame_mode;
    reset_requested = 1;   /* done by the sampler thread, between samples */
}

void rt_prof_frame_end(int keep)
{
    uint32_t seq = frame_seq;
    keep_frame[seq & 255u] = (uint8_t)(keep != 0);
    __sync_synchronize();
    frame_seq = seq + 1u;
}

void rt_prof_flush(void)
{
    stopped = 1;
    /* Let a sample in progress finish before reading the maps. */
#if defined(__vita__)
    sceKernelDelayThread(2 * SAMPLE_US);
#else
    usleep(2 * SAMPLE_US);
#endif
    dump();
}

void rt_prof_start(void)
{
    atexit(dump);
#if defined(__vita__)
    /* High priority on a core the game thread does not use. */
    SceUID t = sceKernelCreateThread("ff_profiler", sampler_thread, 0x10000100 - 20, 0x4000, 0,
                                     SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (t >= 0)
        sceKernelStartThread(t, 0, NULL);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, sampler_thread, NULL) == 0)
        pthread_detach(t);
#endif
    rt_log("profiler: sampling every %u us into %s", SAMPLE_US, PROFILE_PATH);
}

#endif
