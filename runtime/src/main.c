/* Loads CoDSP.exe into guest memory (module.c), sets up the main thread and runs it. */
#include "module.h"
#include "runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

char g_game_dir[1024];

static Ctx g_ctx;

/* ---- logging ------------------------------------------------------------ */

static FILE *log_file;

void platform_shutdown(void);

uint32_t win32_perf_counter_ms(void);

static double log_time(void)
{
    return (double)win32_perf_counter_ms() / 1000.0;
}

/* One line per call, whole lines even from several threads. The log file is
 * buffered and written out at most every 250 ms (on the Vita every write is a
 * slow call into the file system) and at once for fatal errors (rt_log_flush).
 * The Vita has no console: stderr only on the desktop. */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static double log_flushed_at;

void rt_log_flush(void)
{
    pthread_mutex_lock(&log_lock);
    if (log_file)
        fflush(log_file);
    pthread_mutex_unlock(&log_lock);
}

void rt_log(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    double t = log_time();
    pthread_mutex_lock(&log_lock);
#if !defined(__vita__)
    fprintf(stderr, "[%7.3f] %s\n", t, line);
#endif
    if (log_file) {
        fprintf(log_file, "[%7.3f] %s\n", t, line);
        static int sync = -1;
        if (sync < 0)
            sync = getenv("COD_LOG_SYNC") != NULL;   /* every line (crash hunting) */
        if (sync || t - log_flushed_at >= 0.25) {
            fflush(log_file);
            log_flushed_at = t;
        }
    }
    pthread_mutex_unlock(&log_lock);
}

void rt_fatal(Ctx *c, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s", msg);
    if (c) {
        rt_log("  eax=%08X ecx=%08X edx=%08X ebx=%08X", c->r[0], c->r[1], c->r[2], c->r[3]);
        rt_log("  esp=%08X ebp=%08X esi=%08X edi=%08X", c->r[4], c->r[5], c->r[6], c->r[7]);
        rt_log("  stack:");
        for (uint32_t i = 0; i < 16u; i++) {
            uint32_t a = c->r[4] + i * 4u;
            if (a < GUEST_STACK_BASE || a >= GUEST_STACK_BASE + GUEST_STACK_SIZE)
                break;
            rt_log("    [esp+%02X] %08X", i * 4u, RD32(a));
        }
    }
    rt_log_flush();
    platform_shutdown();
    exit(1);
}

void rt_missing_import(Ctx *c, const char *name)
{
    rt_fatal(c, "import not implemented: %s (called from %08X)", name, RD32(c->r[4]));
}

/* ---- thunks ------------------------------------------------------------- */

static GuestFn thunk_fns[THUNK_MAX];
static const char *thunk_names[THUNK_MAX];
static uint32_t thunk_count;

/* Call thunk `i` with eax, ecx and esp already stored in the context (what
 * rt_dispatch does for a thunk address, without the range check and lookup). */
/* COD_LOG_CALLS=T: log every host call (imports, COM methods) from T seconds
 * after start for 100 ms (diagnostics). */
static double log_calls_at = -1.0;

static void log_call(uint32_t i);

static uint8_t thunk_gated[THUNK_MAX];
void (*rt_thunk_gate)(void);

void thunk_mark_gated(uint32_t addr)
{
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    if (i < THUNK_MAX)
        thunk_gated[i] = 1;
}

int thunk_is_gated(uint32_t addr)
{
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return i < THUNK_MAX && thunk_gated[i];
}

static uint64_t thunk_run(Ctx *c, uint32_t i)
{
    const uint32_t target = THUNK_BASE + i * THUNK_STRIDE;
    if (thunk_gated[i] && rt_thunk_gate)
        rt_thunk_gate();
    if (log_calls_at >= 0.0)
        log_call(i);
#ifdef COD_PROFILE
    uint32_t prof_prev = c->prof->cur;
    c->prof->cur = target;
#endif
    g_current_thunk = target;
    thunk_fns[i](c);
#ifdef COD_PROFILE
    c->prof->cur = prof_prev;
#endif
    return ((uint64_t)c->r[4] << 32) | c->r[0];
}

/* Dispatch entries for the first THUNK_FAST_MAX thunks, cached by indirect call
 * sites like the entries of generated functions: `fast` is a per-thunk entry with
 * the GuestFastFn convention (the thunk index is a constant in it). */
#define THUNK_FAST_MAX 1024u
#define TF1(n)                                                                  \
    static uint64_t thunk_fast_##n(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax) \
    {                                                                           \
        c->r[0] = eax;                                                          \
        c->r[1] = ecx;                                                          \
        c->r[4] = esp;                                                          \
        return thunk_run(c, 0##n);                                              \
    }
#define TF8(n) TF1(n##0) TF1(n##1) TF1(n##2) TF1(n##3) TF1(n##4) TF1(n##5) TF1(n##6) TF1(n##7)
#define TF64(n) TF8(n##0) TF8(n##1) TF8(n##2) TF8(n##3) TF8(n##4) TF8(n##5) TF8(n##6) TF8(n##7)
#define TF512(n) TF64(n##0) TF64(n##1) TF64(n##2) TF64(n##3) TF64(n##4) TF64(n##5) TF64(n##6) TF64(n##7)
TF512(0)
TF512(1)
#define TP1(n) thunk_fast_##n,
#define TP8(n) TP1(n##0) TP1(n##1) TP1(n##2) TP1(n##3) TP1(n##4) TP1(n##5) TP1(n##6) TP1(n##7)
#define TP64(n) TP8(n##0) TP8(n##1) TP8(n##2) TP8(n##3) TP8(n##4) TP8(n##5) TP8(n##6) TP8(n##7)
#define TP512(n) TP64(n##0) TP64(n##1) TP64(n##2) TP64(n##3) TP64(n##4) TP64(n##5) TP64(n##6) TP64(n##7)
static const GuestFastFn thunk_fast[THUNK_FAST_MAX] = {TP512(0) TP512(1)};
static GuestFunc thunk_entries[THUNK_FAST_MAX];

static uint32_t thunk_add(GuestFn fn, const char *name)
{
    if (thunk_count == THUNK_MAX)
        rt_fatal(NULL, "too many host thunks");
    uint32_t i = thunk_count;
    uint32_t addr = THUNK_BASE + i * THUNK_STRIDE;
    thunk_fns[i] = fn;
    thunk_names[i] = name;
    if (i < THUNK_FAST_MAX) {
        thunk_entries[i].fn = fn;
        thunk_entries[i].fast = thunk_fast[i];
        thunk_entries[i].addr = addr;
    }
    thunk_count = i + 1u;
    return addr;
}

uint32_t thunk_register(GuestFn fn, const char *name)
{
    for (uint32_t i = 0; i < thunk_count; i++)
        if (thunk_fns[i] == fn)
            return THUNK_BASE + i * THUNK_STRIDE;
    return thunk_add(fn, name);
}

uint32_t thunk_register_unique(GuestFn fn, const char *name)
{
    return thunk_add(fn, name);
}

void thunk_set_fast(GuestFn fn, GuestFastFn fast)
{
#ifndef COD_PROFILE
    for (uint32_t i = 0; i < thunk_count && i < THUNK_FAST_MAX; i++)
        if (thunk_fns[i] == fn)
            thunk_entries[i].fast = fast;
#else
    (void)fn;
    (void)fast;
#endif
}

GuestFn thunk_lookup(uint32_t addr)
{
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return i < thunk_count ? thunk_fns[i] : NULL;
}

const GuestFunc *thunk_entry(uint32_t addr)
{
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return i < thunk_count && i < THUNK_FAST_MAX ? &thunk_entries[i] : NULL;
}

const char *thunk_name(uint32_t addr)
{
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return i < thunk_count ? thunk_names[i] : "?";
}

/* ---- guest calls -------------------------------------------------------- */

uint32_t guest_call(Ctx *c, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof(saved));
    uint32_t esp = c->r[4];
    for (int i = nargs - 1; i >= 0; i--) {
        esp -= 4u;
        WR32(esp, args[i]);
    }
    esp -= 4u;
    WR32(esp, GUEST_RETURN_MAGIC);
    c->r[4] = esp;
    rt_dispatch(c, fn);
    uint32_t ret = c->r[0];
    memcpy(c->r, saved, sizeof(saved));
    return ret;
}

/* Thread information block: SEH chain head, stack bounds, self pointer. */
uint32_t thread_setup_tib(Ctx *c, uint32_t stack_lo, uint32_t stack_hi)
{
    uint32_t tib = heap_alloc(0x1000);
    memset(g_mem + tib, 0, 0x1000);
    WR32(tib + 0x00u, 0xFFFFFFFFu);
    WR32(tib + 0x04u, stack_hi);
    WR32(tib + 0x08u, stack_lo);
    WR32(tib + 0x18u, tib);
    c->fs_base = tib;
    return tib;
}

extern void win32_init(int argc, char **argv);
uint32_t shim_import_data(const char *dll, const char *name);

void rt_log_thunk_call(uint32_t addr)
{
    if (log_calls_at >= 0.0)
        log_call((addr - THUNK_BASE) / THUNK_STRIDE);
}

static void log_call(uint32_t i)
{
    double t = log_time();
    if (t >= log_calls_at && t < log_calls_at + 0.1)
        rt_log("call %s", thunk_names[i]);
}

/* Diagnostic entry probes (COD_PROBES at configure time): log the call. */
/* COD_SELFTEST: at the first call of the file name hash (CoDSP.exe 0x421980,
 * eax = name, [esp+4] = table size), hash a few names and log the results
 * (comparing hosts). */
static void selftest_hash(Ctx *c)
{
    static int done;
    if (done || !getenv("COD_SELFTEST"))
        return;
    done = 1;
    static const char *const names[] = { "ui\\menudef.h", "ui/menudef.h", "textures/belgium/walls/decal@clifface_trans2.dds",
                                         "sound/voiceovers/stalingrad/commissar1_line_46a.wav" };
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof(saved));
    uint32_t str = scratch_alloc(64, 4);
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        memcpy(gptr(str), names[i], strlen(names[i]) + 1);
        uint32_t esp = saved[4] - 256u;
        esp -= 4u;
        WR32(esp, 2048u);
        esp -= 4u;
        WR32(esp, GUEST_RETURN_MAGIC);
        c->r[4] = esp;
        c->r[0] = str;
        rt_dispatch(c, 0x00421990u - 0x10u);
        rt_log("selftest hash(\"%s\", 2048) = %u", names[i], c->r[0]);
    }
    memcpy(c->r, saved, sizeof(saved));
}

/* Printable guest string at a (or "?"), for COD_PROBE_STRS. */
static const char *probe_str(uint32_t a)
{
    if (a < GUEST_NULL_GUARD || a >= g_guest_mem_size - 64u)
        return "?";
    return (const char *)g_mem + a;
}

/* 1 while the render thread draws and the caller is another thread (render_async.c). */
int render_async_foreign(void) __attribute__((weak));
int render_async_busy(void) __attribute__((weak));

void rt_probe(Ctx *c, uint32_t func)
{
    /* COD_PROBE_RACE: only probes reached by the game thread while the render thread
     * draws (back-end code that must not run on both). */
    static int race = -1;
    if (race < 0)
        race = getenv("COD_PROBE_RACE") != NULL;
    /* VM_Call on the render thread (COD_PROBES=44F270): who, from its shadow stack. A
     * module call there switches the current VM under the game thread's own calls. */
    if (func == 0x0044F270u) {
        static unsigned n_logged;
        if (render_async_foreign && !render_async_foreign() && render_async_busy && render_async_busy() &&
            n_logged < 20u) {
            n_logged++;
            char line[600];
            size_t n = 0;
#ifdef COD_PROFILE
            uint32_t d = c->prof->depth;
            for (uint32_t i = d > 24u ? d - 24u : 0u; i < d && i < RT_PROF_MAX; i++)
                n += (size_t)snprintf(line + n, sizeof(line) - n, " %08X", c->prof->stack[i]);
#endif
            line[n] = 0;
            rt_log("VM_Call(%08X, %u) on the render thread from %08X, stack%s", RD32(c->r[4] + 4u),
                   RD32(c->r[4] + 8u), RD32(c->r[4]), line);
        }
        return;
    }
    if (race) {
        static unsigned logged;
        if (render_async_foreign && render_async_foreign() && logged < 200u) {
            logged++;
            rt_log("race: %08X on the game thread while the render thread draws, from %08X", func, RD32(c->r[4]));
        }
        return;
    }
    /* COD_PROBE_STRS: the strings eax and edx point to (register arguments). */
    if (getenv("COD_PROBE_STRS"))
        rt_log("probe %08X: eax \"%.80s\" edx \"%.80s\"", func, probe_str(c->r[0]), probe_str(c->r[2]));
    if (func == 0x004218D0u && getenv("COD_PROBE_STRS"))
        rt_log("pak %s: hash size %u, files %u", probe_str(c->r[2]), RD32(c->r[2] + 0x314u), RD32(c->r[2] + 0x30Cu));
    if (func == 0x00421980u) {
        static int in;
        if (!in) {
            in = 1;
            selftest_hash(c);
            in = 0;
        }
    }
    /* COD_PEEK="addr[:words],...": guest memory logged at every probe (diagnostics). */
    if (getenv("COD_PEEK")) {
        char line[1024];
        size_t n = 0;
        for (const char *p = getenv("COD_PEEK"); *p && n < sizeof(line) - 64;) {
            char *end;
            /* "*A+O": the dword at guest address A, plus O (one level of pointer). */
            int deref = *p == '*';
            uint32_t a = (uint32_t)strtoul(p + deref, &end, 16), w = 2;
            if (deref) {
                a = RD32(a);
                if (*end == '+')
                    a += (uint32_t)strtoul(end + 1, &end, 16);
            }
            if (*end == ':')
                w = (uint32_t)strtoul(end + 1, &end, 10);
            n += (size_t)snprintf(line + n, sizeof(line) - n, " %08X:", a);
            for (uint32_t k = 0; k < w && n < sizeof(line) - 16; k++)
                n += (size_t)snprintf(line + n, sizeof(line) - n, " %08X", RD32(a + 4u * k));
            if (*end != ',')
                break;
            p = end + 1;
        }
        rt_log("peek%s", line);
    }
    rt_log("probe %08X: ret=%08X ecx=%08X args %08X %08X %08X", func, RD32(c->r[4]), c->r[1],
           RD32(c->r[4] + 4u), RD32(c->r[4] + 8u), RD32(c->r[4] + 12u));
}

#if defined(__vita__) || defined(__SWITCH__)
#if defined(__vita__)
#define COD_ENV_FILE "ux0:data/cod1/env.txt"
#else
#define COD_ENV_FILE "sdmc:/switch/cod1/env.txt"
#endif
/* No environment on the consoles: env.txt holds NAME=value lines (the
 * diagnostics read environment variables). */
static void load_env_file(void)
{
    FILE *f = fopen(COD_ENV_FILE, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (eq && eq != line) {
            *eq = 0;
            setenv(line, eq + 1, 1);
        }
    }
    fclose(f);
}
#endif

#if defined(__SWITCH__)
static int cod_main(int argc, char **argv);

/* The main thread of a homebrew app has a small stack and the generated code
 * nests one C frame per guest call: the game runs on a thread of its own with a
 * desktop-sized stack. */
typedef struct MainArgs {
    int argc;
    char **argv;
    int ret;
} MainArgs;

static void *cod_main_thread(void *p)
{
    MainArgs *a = p;
    a->ret = cod_main(a->argc, a->argv);
    return NULL;
}

int main(int argc, char **argv)
{
    MainArgs a = { argc, argv, 0 };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u * 1024u * 1024u);
    pthread_t t;
    if (pthread_create(&t, &attr, cod_main_thread, &a) != 0)
        return cod_main(argc, argv);
    pthread_join(t, NULL);
    return a.ret;
}

static int cod_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
#if defined(__vita__) || defined(__SWITCH__)
    load_env_file();
#endif
    const char *dir = getenv("COD_DIR");
    if (argc > 1)
        dir = argv[1];
#if defined(__vita__)
    if (!dir)
        dir = "ux0:data/cod1/pc";
    log_file = fopen("ux0:data/cod1/cod1.log", "w");
#elif defined(__SWITCH__)
    if (!dir)
        dir = "sdmc:/switch/cod1/pc";
    log_file = fopen("sdmc:/switch/cod1/cod1.log", "w");
#else
    if (!dir)
        dir = ".";
    log_file = fopen(getenv("COD_LOG") ? getenv("COD_LOG") : "cod1.log", "w");
#endif
    snprintf(g_game_dir, sizeof(g_game_dir), "%s", dir);
    if (log_file)
        setvbuf(log_file, NULL, _IOFBF, 64 * 1024);

    mem_init();
    mem_report("start");
    rt_log("guest memory at host %p..%p", (void *)g_mem, (void *)(g_mem + g_guest_mem_size));
    if (getenv("COD_LOG_CALLS"))
        log_calls_at = atof(getenv("COD_LOG_CALLS"));
    uint32_t entry = module_load_exe();
    rt_log("loaded %s, entry %08X, %u functions, %u imports", g_guest_modules[0].name, entry, g_guest_func_count,
           g_guest_import_count);

#ifdef COD_PROFILE
    rt_prof_start();
#endif
    Ctx *c = &g_ctx;
#ifdef COD_PROFILE
    c->prof = &rt_prof_main;
#endif
    rt_fninit(c);
    thread_setup_tib(c, GUEST_STACK_BASE, GUEST_STACK_BASE + GUEST_STACK_SIZE);
    c->r[4] = GUEST_STACK_BASE + GUEST_STACK_SIZE - 64u;
    win32_init(argc, argv);

    c->r[4] -= 4u;
    WR32(c->r[4], GUEST_RETURN_MAGIC);
    rt_dispatch(c, entry);
    rt_log("guest entry point returned, eax=%08X", c->r[0]);
    return (int)c->r[0];
}
