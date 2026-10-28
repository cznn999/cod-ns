/* msvcrt.dll: the Microsoft C runtime CoDSP.exe imports.
 *
 * Every function is cdecl (the caller pops the arguments) unless noted. Strings
 * and buffers are guest memory; FILE objects are guest structures (the game only
 * passes them around) mapped to host streams by their index. printf and scanf
 * read their variable arguments from the guest stack. Doubles come and go on
 * the x87 stack where MSVC puts them (tools/recomp/gen.py IMPORT_X87 knows the
 * stack effect of each such import). */
#include "shim.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static inline void cdecl_ret(Ctx *c, uint32_t v)
{
    c->r[0] = v;
    c->r[4] += 4u;
}

static inline double arg_f64(Ctx *c, int n)
{
    return RDF64(c->r[4] + 4u + 4u * (uint32_t)n);
}

/* Returns a double in ST(0). */
static inline void cdecl_ret_f64(Ctx *c, double v)
{
    fpush(c, v);
    c->r[4] += 4u;
}

/* ---- data imports ----------------------------------------------------------- */

#define GFILE_SIZE 32u
#define MAX_FILES 64u

static uint32_t iob_base;            /* guest FILE array; _iob[0..2] = stdin, stdout, stderr */
static FILE *host_files[MAX_FILES];
static uint32_t acmdln_var, fmode_var, commode_var, adjust_fdiv_var;

static void msvcrt_init_data(void)
{
    if (iob_base)
        return;
    iob_base = scratch_alloc(GFILE_SIZE * MAX_FILES, 16);
    for (uint32_t i = 0; i < 3u; i++)
        WR32(iob_base + i * GFILE_SIZE + 16u, i);     /* _file */
    host_files[0] = stdin;
    host_files[1] = stdout;
    host_files[2] = stderr;
    acmdln_var = scratch_alloc(4, 4);
    WR32(acmdln_var, scratch_strdup(COD_EXE_NAME));
    fmode_var = scratch_alloc(4, 4);
    commode_var = scratch_alloc(4, 4);
    adjust_fdiv_var = scratch_alloc(4, 4);
}

/* Imports that are variables: the IAT slot holds the variable's address. */
uint32_t shim_import_data(const char *dll, const char *name)
{
    if (strcmp(dll, "msvcrt.dll") != 0)
        return 0;
    msvcrt_init_data();
    if (!strcmp(name, "_iob"))
        return iob_base;
    if (!strcmp(name, "_acmdln"))
        return acmdln_var;
    if (!strcmp(name, "_adjust_fdiv"))
        return adjust_fdiv_var;
    return 0;
}

static FILE *host_file(uint32_t f)
{
    if (f < iob_base)
        return NULL;
    uint32_t i = (f - iob_base) / GFILE_SIZE;
    return i < MAX_FILES ? host_files[i] : NULL;
}

static uint32_t file_new(FILE *h)
{
    for (uint32_t i = 3; i < MAX_FILES; i++)
        if (!host_files[i]) {
            host_files[i] = h;
            uint32_t g = iob_base + i * GFILE_SIZE;
            memset(g_mem + g, 0, GFILE_SIZE);
            WR32(g + 16u, i);
            return g;
        }
    rt_log("msvcrt: out of FILE slots");
    return 0;
}

/* ---- startup and exit ---------------------------------------------------------- */

SHIM(msvcrt, __set_app_type) { cdecl_ret(c, 0); }
SHIM(msvcrt, __setusermatherr) { cdecl_ret(c, 0); }
SHIM(msvcrt, __p__fmode) { msvcrt_init_data(); cdecl_ret(c, fmode_var); }
SHIM(msvcrt, __p__commode) { msvcrt_init_data(); cdecl_ret(c, commode_var); }

/* __getmainargs(&argc, &argv, &envp, expand_wildcards, &startinfo) */
SHIM(msvcrt, __getmainargs)
{
    uint32_t argv = scratch_alloc(8, 4);
    WR32(argv, scratch_strdup(COD_EXE_NAME));
    uint32_t envp = scratch_alloc(4, 4);
    WR32(ARG(0), 1);
    WR32(ARG(1), argv);
    WR32(ARG(2), envp);
    cdecl_ret(c, 0);
}

/* _initterm(begin, end): call every non-null function pointer in [begin, end). */
SHIM(msvcrt, _initterm)
{
    uint32_t b = ARG(0), e = ARG(1);
    uint32_t ret_esp = c->r[4];
    for (uint32_t p = b; p < e; p += 4u) {
        uint32_t fn = RD32(p);
        if (fn)
            guest_call(c, fn, 0, NULL);
    }
    c->r[4] = ret_esp;
    cdecl_ret(c, 0);
}

#define MAX_ONEXIT 256
static uint32_t onexit_fns[MAX_ONEXIT];
static int onexit_count;

static uint32_t add_onexit(uint32_t fn)
{
    if (onexit_count == MAX_ONEXIT)
        return 0;
    onexit_fns[onexit_count++] = fn;
    return fn;
}

SHIM(msvcrt, _onexit) { cdecl_ret(c, add_onexit(ARG(0))); }

/* __dllonexit(fn, &begin, &end): the module's own table; one list is enough here. */
SHIM(msvcrt, __dllonexit) { cdecl_ret(c, add_onexit(ARG(0))); }

void platform_shutdown(void);

static void run_exit(Ctx *c, int code)
{
    rt_log("exit(%d)", code);
    while (onexit_count > 0) {
        uint32_t fn = onexit_fns[--onexit_count];
        guest_call(c, fn, 0, NULL);
    }
    fflush(NULL);
    platform_shutdown();
    exit(code);
}

SHIM(msvcrt, exit) { run_exit(c, (int)ARG(0)); }

SHIM(msvcrt, _exit)
{
    rt_log("_exit(%d)", (int)ARG(0));
    platform_shutdown();
    exit((int)ARG(0));
}

SHIM(msvcrt, _XcptFilter) { cdecl_ret(c, 0); }   /* EXCEPTION_CONTINUE_SEARCH */

/* ---- exceptions ---------------------------------------------------------------- */

/* The name of the thrown type: ThrowInfo -> CatchableTypeArray -> CatchableType -> TypeDescriptor. */
static const char *throw_type_name(uint32_t info)
{
    if (!info)
        return "?";
    uint32_t cta = RD32(info + 12u);
    if (!cta || !RD32(cta))
        return "?";
    uint32_t ct = RD32(cta + 4u);
    uint32_t td = ct ? RD32(ct + 4u) : 0;
    return td ? gstr(td + 8u) : "?";
}

void rt_eh_throw(Ctx *c, uint32_t obj, uint32_t info) __attribute__((noreturn));

SHIM(msvcrt, _CxxThrowException)
{
    (void)throw_type_name;
    rt_eh_throw(c, ARG(0), ARG(1));
}

SHIM(msvcrt, __CxxFrameHandler) { rt_fatal(c, "__CxxFrameHandler: SEH dispatch is not supported"); }
SHIM(msvcrt, _except_handler3) { rt_fatal(c, "_except_handler3: SEH dispatch is not supported"); }
SHIM(msvcrt, _terminate__YAXXZ) { rt_fatal(c, "terminate()"); }
SHIM(msvcrt, _purecall) { rt_fatal(c, "pure virtual function call"); }

static uint32_t se_translator;

/* _set_se_translator(fn): returns the previous one. */
SHIM(msvcrt, __set_se_translator__YAP6AXIPAU_EXCEPTION_POINTERS___ZP6AXI0_Z_Z)
{
    uint32_t old = se_translator;
    se_translator = ARG(0);
    cdecl_ret(c, old);
}

/* type_info::~type_info (thiscall, `this` in ecx). */
SHIM(msvcrt, __1type_info__UAE_XZ) { stdcall_ret(c, 0, 0); }

/* ---- memory -------------------------------------------------------------------- */

SHIM(msvcrt, malloc) { cdecl_ret(c, heap_alloc(ARG(0))); }

SHIM(msvcrt, calloc)
{
    uint32_t n = ARG(0) * ARG(1);
    uint32_t p = heap_alloc(n);
    if (p)
        memset(g_mem + p, 0, n);
    cdecl_ret(c, p);
}

SHIM(msvcrt, realloc) { cdecl_ret(c, heap_realloc(ARG(0), ARG(1))); }

SHIM(msvcrt, free)
{
    if (ARG(0))
        heap_free(ARG(0));
    cdecl_ret(c, 0);
}

/* operator delete(void *) */
SHIM(msvcrt, __3_YAXPAX_Z)
{
    if (ARG(0))
        heap_free(ARG(0));
    cdecl_ret(c, 0);
}

SHIM(msvcrt, memmove)
{
    memmove(g_mem + ARG(0), g_mem + ARG(1), ARG(2));
    RT_REPORT_WRITE(ARG(0), ARG(2));
    cdecl_ret(c, ARG(0));
}

SHIM(msvcrt, memchr)
{
    const uint8_t *p = memchr(g_mem + ARG(0), (int)(ARG(1) & 0xFFu), ARG(2));
    cdecl_ret(c, p ? (uint32_t)(p - g_mem) : 0);
}

SHIM(msvcrt, _memicmp)
{
    const uint8_t *a = g_mem + ARG(0), *b = g_mem + ARG(1);
    uint32_t n = ARG(2);
    int r = 0;
    for (uint32_t i = 0; i < n && !r; i++)
        r = tolower(a[i]) - tolower(b[i]);
    cdecl_ret(c, (uint32_t)r);
}

/* ---- strings ------------------------------------------------------------------- */

static uint32_t gaddr(const char *p) { return p ? (uint32_t)((const uint8_t *)p - g_mem) : 0; }

SHIM(msvcrt, strchr) { cdecl_ret(c, gaddr(strchr(gstr(ARG(0)), (int)(ARG(1) & 0xFFu)))); }
SHIM(msvcrt, strrchr) { cdecl_ret(c, gaddr(strrchr(gstr(ARG(0)), (int)(ARG(1) & 0xFFu)))); }
SHIM(msvcrt, strstr) { cdecl_ret(c, gaddr(strstr(gstr(ARG(0)), gstr(ARG(1))))); }
SHIM(msvcrt, strpbrk) { cdecl_ret(c, gaddr(strpbrk(gstr(ARG(0)), gstr(ARG(1))))); }
SHIM(msvcrt, strncmp) { cdecl_ret(c, (uint32_t)strncmp(gstr(ARG(0)), gstr(ARG(1)), ARG(2))); }
SHIM(msvcrt, _stricmp) { cdecl_ret(c, (uint32_t)strcasecmp(gstr(ARG(0)), gstr(ARG(1)))); }
SHIM(msvcrt, _strcmpi) { cdecl_ret(c, (uint32_t)strcasecmp(gstr(ARG(0)), gstr(ARG(1)))); }
SHIM(msvcrt, _strnicmp) { cdecl_ret(c, (uint32_t)strncasecmp(gstr(ARG(0)), gstr(ARG(1)), ARG(2))); }

SHIM(msvcrt, strncpy)
{
    strncpy(gbuf(ARG(0)), gstr(ARG(1)), ARG(2));
    cdecl_ret(c, ARG(0));
}

SHIM(msvcrt, strncat)
{
    strncat(gbuf(ARG(0)), gstr(ARG(1)), ARG(2));
    cdecl_ret(c, ARG(0));
}

SHIM(msvcrt, _strdup)
{
    const char *s = gstr(ARG(0));
    if (!s) {
        cdecl_ret(c, 0);
        return;
    }
    uint32_t n = (uint32_t)strlen(s) + 1u;
    uint32_t p = heap_alloc(n);
    memcpy(g_mem + p, s, n);
    cdecl_ret(c, p);
}

static uint32_t strtok_next;

SHIM(msvcrt, strtok)
{
    uint32_t s = ARG(0) ? ARG(0) : strtok_next;
    const char *delim = gstr(ARG(1));
    if (!s) {
        cdecl_ret(c, 0);
        return;
    }
    while (g_mem[s] && strchr(delim, g_mem[s]))
        s++;
    if (!g_mem[s]) {
        strtok_next = 0;
        cdecl_ret(c, 0);
        return;
    }
    uint32_t e = s;
    while (g_mem[e] && !strchr(delim, g_mem[e]))
        e++;
    if (g_mem[e]) {
        g_mem[e] = 0;
        strtok_next = e + 1u;
    } else {
        strtok_next = 0;
    }
    cdecl_ret(c, s);
}

SHIM(msvcrt, toupper) { cdecl_ret(c, (uint32_t)toupper((int)ARG(0))); }
SHIM(msvcrt, tolower) { cdecl_ret(c, (uint32_t)tolower((int)ARG(0))); }
SHIM(msvcrt, isspace) { cdecl_ret(c, ARG(0) < 256u && isspace((int)ARG(0)) ? 8u : 0u); }
SHIM(msvcrt, isprint) { cdecl_ret(c, ARG(0) < 256u && isprint((int)ARG(0)) ? 0x157u : 0u); }

SHIM(msvcrt, wcslen)
{
    uint32_t p = ARG(0), n = 0;
    while (RD16(p + 2u * n))
        n++;
    cdecl_ret(c, n);
}

/* ---- conversions --------------------------------------------------------------- */

SHIM(msvcrt, atoi) { cdecl_ret(c, (uint32_t)atoi(gstr(ARG(0)))); }
SHIM(msvcrt, atol) { cdecl_ret(c, (uint32_t)atol(gstr(ARG(0)))); }
SHIM(msvcrt, atof) { cdecl_ret_f64(c, atof(gstr(ARG(0)))); }

SHIM(msvcrt, strtol)
{
    char *end;
    const char *s = gstr(ARG(0));
    long v = strtol(s, &end, (int)ARG(2));
    if (v > INT32_MAX)
        v = INT32_MAX;
    if (v < INT32_MIN)
        v = INT32_MIN;
    if (ARG(1))
        WR32(ARG(1), gaddr(end));
    cdecl_ret(c, (uint32_t)(int32_t)v);
}

SHIM(msvcrt, _ltoa)
{
    int32_t v = (int32_t)ARG(0);
    uint32_t radix = ARG(2);
    char tmp[40];
    uint32_t u = (radix == 10u && v < 0) ? (uint32_t)-(int64_t)v : (uint32_t)v;
    int n = 0;
    do {
        uint32_t d = u % radix;
        tmp[n++] = (char)(d < 10u ? '0' + d : 'a' + d - 10u);
        u /= radix;
    } while (u);
    char *out = gbuf(ARG(1));
    int k = 0;
    if (radix == 10u && v < 0)
        out[k++] = '-';
    while (n)
        out[k++] = tmp[--n];
    out[k] = 0;
    cdecl_ret(c, ARG(1));
}

/* ---- math ---------------------------------------------------------------------- */

SHIM(msvcrt, floor) { cdecl_ret_f64(c, floor(arg_f64(c, 0))); }
SHIM(msvcrt, ceil) { cdecl_ret_f64(c, ceil(arg_f64(c, 0))); }
SHIM(msvcrt, ldexp) { cdecl_ret_f64(c, ldexp(arg_f64(c, 0), (int)ARG(2))); }
SHIM(msvcrt, _isnan) { cdecl_ret(c, isnan(arg_f64(c, 0)) ? 1u : 0u); }
SHIM(msvcrt, _finite) { cdecl_ret(c, isfinite(arg_f64(c, 0)) ? 1u : 0u); }

/* _CI* intrinsics: operands on the x87 stack, result in ST(0). */
SHIM(msvcrt, _CIpow)
{
    double y = ST(0), x = ST(1);
    fpop(c);
    ST(0) = pow(x, y);
    cdecl_ret(c, c->r[0]);
}

SHIM(msvcrt, _CIfmod)
{
    double y = ST(0), x = ST(1);
    fpop(c);
    ST(0) = fmod(x, y);
    cdecl_ret(c, c->r[0]);
}

SHIM(msvcrt, _CIasin)
{
    ST(0) = asin(ST(0));
    cdecl_ret(c, c->r[0]);
}

SHIM(msvcrt, _CIacos)
{
    ST(0) = acos(ST(0));
    cdecl_ret(c, c->r[0]);
}

/* _ftol: ST(0) truncated to a 64-bit integer in edx:eax, popped. */
SHIM(msvcrt, _ftol)
{
    double v = ST(0);
    fpop(c);
    int64_t r;
    if (!(v > -9223372036854775808.0 && v < 9223372036854775808.0))
        r = INT64_MIN;
    else
        r = (int64_t)v;
    c->r[0] = (uint32_t)r;
    c->r[2] = (uint32_t)((uint64_t)r >> 32);
    c->r[4] += 4u;
}

/* _controlfp(new, mask): MSVC control word <-> x87 control word. */
static uint32_t x87_to_msvc(uint32_t cw)
{
    uint32_t m = 0;
    if (cw & 0x01u) m |= 0x10u;          /* invalid */
    if (cw & 0x02u) m |= 0x80000u;       /* denormal */
    if (cw & 0x04u) m |= 0x08u;          /* zero divide */
    if (cw & 0x08u) m |= 0x04u;          /* overflow */
    if (cw & 0x10u) m |= 0x02u;          /* underflow */
    if (cw & 0x20u) m |= 0x01u;          /* inexact */
    static const uint32_t rc[4] = { 0x000u, 0x100u, 0x200u, 0x300u };
    m |= rc[(cw >> 10) & 3u];
    static const uint32_t pc[4] = { 0x20000u, 0x20000u, 0x10000u, 0x00000u };
    m |= pc[(cw >> 8) & 3u];
    return m;
}

static uint32_t msvc_to_x87(uint32_t m)
{
    uint32_t cw = 0x1040u;
    if (m & 0x10u) cw |= 0x01u;
    if (m & 0x80000u) cw |= 0x02u;
    if (m & 0x08u) cw |= 0x04u;
    if (m & 0x04u) cw |= 0x08u;
    if (m & 0x02u) cw |= 0x10u;
    if (m & 0x01u) cw |= 0x20u;
    cw |= ((m >> 8) & 3u) << 10;
    switch (m & 0x30000u) {
    case 0x20000u: break;                  /* 24 bits */
    case 0x10000u: cw |= 0x200u; break;    /* 53 bits */
    default: cw |= 0x300u; break;          /* 64 bits */
    }
    return cw;
}

SHIM(msvcrt, _controlfp)
{
    uint32_t nw = ARG(0), mask = ARG(1) & 0x000B031Fu;
    uint32_t cur = x87_to_msvc(c->fcw);
    if (mask) {
        cur = (cur & ~mask) | (nw & mask);
        c->fcw = msvc_to_x87(cur);
    }
    cdecl_ret(c, cur);
}

/* MSVC's rand: a linear congruential generator, kept identical. */
static uint32_t holdrand = 1;

SHIM(msvcrt, srand)
{
    holdrand = ARG(0);
    cdecl_ret(c, 0);
}

SHIM(msvcrt, rand)
{
    holdrand = holdrand * 214013u + 2531011u;
    cdecl_ret(c, (holdrand >> 16) & 0x7FFFu);
}

/* ---- sorting and searching (guest comparators) -------------------------------- */

static Ctx *cmp_ctx;
static uint32_t cmp_fn;

static int guest_compare(uint32_t a, uint32_t b)
{
    uint32_t args[2] = { a, b };
    return (int)guest_call(cmp_ctx, cmp_fn, 2, args);
}

static void sort_range(uint32_t base, uint32_t n, uint32_t size, uint8_t *tmp)
{
    /* Insertion sort for small ranges, else median-free quicksort on the guest bytes. */
    while (n > 1u) {
        if (n <= 8u) {
            for (uint32_t i = 1; i < n; i++)
                for (uint32_t j = i; j > 0 && guest_compare(base + (j - 1u) * size, base + j * size) > 0; j--) {
                    memcpy(tmp, g_mem + base + j * size, size);
                    memcpy(g_mem + base + j * size, g_mem + base + (j - 1u) * size, size);
                    memcpy(g_mem + base + (j - 1u) * size, tmp, size);
                }
            return;
        }
        uint32_t mid = n / 2u;
        uint32_t last = base + (n - 1u) * size;
        memcpy(tmp, g_mem + base + mid * size, size);
        memcpy(g_mem + base + mid * size, g_mem + last, size);
        memcpy(g_mem + last, tmp, size);
        uint32_t store = 0;
        for (uint32_t i = 0; i + 1u < n; i++) {
            if (guest_compare(base + i * size, last) < 0) {
                if (i != store) {
                    memcpy(tmp, g_mem + base + i * size, size);
                    memcpy(g_mem + base + i * size, g_mem + base + store * size, size);
                    memcpy(g_mem + base + store * size, tmp, size);
                }
                store++;
            }
        }
        memcpy(tmp, g_mem + base + store * size, size);
        memcpy(g_mem + base + store * size, g_mem + last, size);
        memcpy(g_mem + last, tmp, size);
        /* Recurse into the smaller part. */
        uint32_t left = store, right = n - store - 1u;
        if (left < right) {
            sort_range(base, left, size, tmp);
            base += (store + 1u) * size;
            n = right;
        } else {
            sort_range(base + (store + 1u) * size, right, size, tmp);
            n = left;
        }
    }
}

SHIM(msvcrt, qsort)
{
    uint32_t base = ARG(0), n = ARG(1), size = ARG(2), fn = ARG(3);
    uint32_t ret_esp = c->r[4];
    Ctx *pc = cmp_ctx;
    uint32_t pf = cmp_fn;
    cmp_ctx = c;
    cmp_fn = fn;
    uint8_t *tmp = malloc(size ? size : 1u);
    sort_range(base, n, size, tmp);
    free(tmp);
    cmp_ctx = pc;
    cmp_fn = pf;
    c->r[4] = ret_esp;
    cdecl_ret(c, 0);
}

SHIM(msvcrt, bsearch)
{
    uint32_t key = ARG(0), base = ARG(1), n = ARG(2), size = ARG(3), fn = ARG(4);
    uint32_t ret_esp = c->r[4];
    uint32_t lo = 0, hi = n, found = 0;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t args[2] = { key, base + mid * size };
        int r = (int)guest_call(c, fn, 2, args);
        if (r == 0) {
            found = base + mid * size;
            break;
        }
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1u;
    }
    c->r[4] = ret_esp;
    cdecl_ret(c, found);
}

/* ---- formatted output -------------------------------------------------------- */

typedef struct Out {
    char *buf;
    size_t len, cap;
} Out;

static void out_put(Out *o, const char *s, size_t n)
{
    if (o->len + n + 1u > o->cap) {
        size_t cap = o->cap ? o->cap * 2u : 256u;
        while (cap < o->len + n + 1u)
            cap *= 2u;
        o->buf = realloc(o->buf, cap);
        o->cap = cap;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = 0;
}

/* A guest wide (UTF-16) string as Latin-1. */
static void wide_to_narrow(uint32_t p, int max, Out *o)
{
    for (int i = 0; max < 0 || i < max; i++) {
        uint32_t ch = RD16(p + 2u * (uint32_t)i);
        if (!ch)
            break;
        char b = ch < 256u ? (char)ch : '?';
        out_put(o, &b, 1);
    }
}

/* Formats MSVC printf-style `fmt` with arguments read from guest memory at *ap.
 * `fmt_wide`: the format is a guest UTF-16 string (swprintf), whose %s is wide. */
static void format_guest(Out *o, uint32_t fmt, int fmt_wide, uint32_t ap)
{
    uint32_t p = fmt;
#define FCH(q) (fmt_wide ? RD16(q) : RD8(q))
#define FSTEP (fmt_wide ? 2u : 1u)
    for (;;) {
        uint32_t ch = FCH(p);
        if (!ch)
            break;
        if (ch != '%') {
            char b = ch < 256u ? (char)ch : '?';
            out_put(o, &b, 1);
            p += FSTEP;
            continue;
        }
        p += FSTEP;
        char spec[32];
        int sl = 0;
        spec[sl++] = '%';
        while ((ch = FCH(p)) && strchr("-+ #0", (int)ch)) {
            if (sl < 20)
                spec[sl++] = (char)ch;
            p += FSTEP;
        }
        int width = -1, prec = -1;
        if (FCH(p) == '*') {
            width = (int)RD32(ap);
            ap += 4u;
            p += FSTEP;
        } else {
            while ((ch = FCH(p)) >= '0' && ch <= '9') {
                width = (width < 0 ? 0 : width * 10) + (int)(ch - '0');
                p += FSTEP;
            }
        }
        if (FCH(p) == '.') {
            p += FSTEP;
            prec = 0;
            if (FCH(p) == '*') {
                prec = (int)RD32(ap);
                ap += 4u;
                p += FSTEP;
            } else {
                while ((ch = FCH(p)) >= '0' && ch <= '9') {
                    prec = prec * 10 + (int)(ch - '0');
                    p += FSTEP;
                }
            }
        }
        if (width >= 0)
            sl += snprintf(spec + sl, sizeof(spec) - (size_t)sl, "%d", width);
        if (prec >= 0)
            sl += snprintf(spec + sl, sizeof(spec) - (size_t)sl, ".%d", prec);
        int is64 = 0, longmod = 0, shortmod = 0;
        for (;;) {
            ch = FCH(p);
            if (ch == 'l') {
                longmod = 1;
                p += FSTEP;
            } else if (ch == 'h') {
                shortmod = 1;
                p += FSTEP;
            } else if (ch == 'L') {
                p += FSTEP;
            } else if (ch == 'I' && FCH(p + FSTEP) == '6' && FCH(p + 2u * FSTEP) == '4') {
                is64 = 1;
                p += 3u * FSTEP;
            } else {
                break;
            }
        }
        uint32_t conv = FCH(p);
        if (!conv)
            break;
        p += FSTEP;
        char tmp[512];
        int n = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            spec[sl++] = 'l';
            spec[sl++] = 'l';
            spec[sl++] = (char)conv;
            spec[sl] = 0;
            long long v;
            if (is64) {
                v = (long long)RD64(ap);
                ap += 8u;
            } else {
                uint32_t raw = RD32(ap);
                ap += 4u;
                if (shortmod)
                    raw = conv == 'd' || conv == 'i' ? (uint32_t)(int32_t)(int16_t)raw : raw & 0xFFFFu;
                v = conv == 'd' || conv == 'i' ? (long long)(int32_t)raw : (long long)raw;
            }
            n = snprintf(tmp, sizeof(tmp), spec, v);
            break;
        }
        case 'c': case 'C': {
            uint32_t v = RD32(ap);
            ap += 4u;
            char b = (char)(v < 256u ? v : '?');
            spec[sl++] = 'c';
            spec[sl] = 0;
            n = snprintf(tmp, sizeof(tmp), spec, b);
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            double v = RDF64(ap);
            ap += 8u;
            spec[sl++] = (char)conv;
            spec[sl] = 0;
            n = snprintf(tmp, sizeof(tmp), spec, v);
            break;
        }
        case 'p': {
            uint32_t v = RD32(ap);
            ap += 4u;
            n = snprintf(tmp, sizeof(tmp), "%08X", v);
            break;
        }
        case 's': case 'S': {
            uint32_t s = RD32(ap);
            ap += 4u;
            int wide = (conv == 'S') ^ fmt_wide;
            if (longmod)
                wide = 1;
            if (shortmod)
                wide = 0;
            Out str = { 0, 0, 0 };
            if (!s)
                out_put(&str, "(null)", 6);
            else if (wide)
                wide_to_narrow(s, prec, &str);
            else {
                const char *hs = gstr(s);
                size_t len = strlen(hs);
                if (prec >= 0 && (size_t)prec < len)
                    len = (size_t)prec;
                out_put(&str, hs, len);
            }
            size_t len = str.len;
            int pad = width > (int)len ? width - (int)len : 0;
            int left = strchr(spec, '-') != NULL;
            if (!left)
                for (int i = 0; i < pad; i++)
                    out_put(o, " ", 1);
            if (len)
                out_put(o, str.buf, len);
            if (left)
                for (int i = 0; i < pad; i++)
                    out_put(o, " ", 1);
            free(str.buf);
            continue;
        }
        case 'n':
            WR32(RD32(ap), (uint32_t)o->len);
            ap += 4u;
            continue;
        case '%':
            out_put(o, "%", 1);
            continue;
        default:
            rt_log("printf: unsupported conversion %%%c", (char)conv);
            continue;
        }
        if (n > 0)
            out_put(o, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1u);
    }
#undef FCH
#undef FSTEP
}

static uint32_t emit_narrow(uint32_t dst, Out *o)
{
    if (o->len)
        memcpy(g_mem + dst, o->buf, o->len);
    g_mem[dst + o->len] = 0;
    uint32_t n = (uint32_t)o->len;
    free(o->buf);
    return n;
}

SHIM(msvcrt, sprintf)
{
    Out o = { 0, 0, 0 };
    format_guest(&o, ARG(1), 0, c->r[4] + 12u);
    cdecl_ret(c, emit_narrow(ARG(0), &o));
}

SHIM(msvcrt, vsprintf)
{
    Out o = { 0, 0, 0 };
    format_guest(&o, ARG(1), 0, ARG(2));
    cdecl_ret(c, emit_narrow(ARG(0), &o));
}

/* swprintf(wchar_t *buf, const wchar_t *fmt, ...) */
SHIM(msvcrt, swprintf)
{
    Out o = { 0, 0, 0 };
    format_guest(&o, ARG(1), 1, c->r[4] + 12u);
    uint32_t dst = ARG(0);
    for (size_t i = 0; i < o.len; i++)
        WR16(dst + 2u * (uint32_t)i, (uint8_t)o.buf[i]);
    WR16(dst + 2u * (uint32_t)o.len, 0);
    free(o.buf);
    cdecl_ret(c, (uint32_t)o.len);
}

static void log_output(FILE *f, Out *o)
{
    if (!o->len)
        return;
    if (f == stdout || f == stderr) {
        /* The game's console output goes to the log (trimmed of the newline). */
        size_t n = o->len;
        while (n && (o->buf[n - 1] == '\n' || o->buf[n - 1] == '\r'))
            n--;
        rt_log("game: %.*s", (int)n, o->buf);
    } else if (f) {
        fwrite(o->buf, 1, o->len, f);
    }
}

SHIM(msvcrt, printf)
{
    Out o = { 0, 0, 0 };
    format_guest(&o, ARG(0), 0, c->r[4] + 8u);
    log_output(stdout, &o);
    uint32_t n = (uint32_t)o.len;
    free(o.buf);
    cdecl_ret(c, n);
}

SHIM(msvcrt, fprintf)
{
    Out o = { 0, 0, 0 };
    format_guest(&o, ARG(1), 0, c->r[4] + 12u);
    log_output(host_file(ARG(0)), &o);
    uint32_t n = (uint32_t)o.len;
    free(o.buf);
    cdecl_ret(c, n);
}

/* Also used by user32's wsprintfA / wvsprintfA. */
uint32_t msvcrt_format(uint32_t dst, uint32_t fmt, uint32_t ap)
{
    Out o = { 0, 0, 0 };
    format_guest(&o, fmt, 0, ap);
    return emit_narrow(dst, &o);
}

SHIM(msvcrt, perror)
{
    rt_log("game perror: %s", ARG(0) ? gstr(ARG(0)) : "");
    cdecl_ret(c, 0);
}

/* ---- formatted input ------------------------------------------------------------ */

typedef struct In {
    const char *s;          /* string source */
    FILE *f;                /* or stream source */
    long consumed;
} In;

static int in_get(In *in)
{
    int ch;
    if (in->f)
        ch = fgetc(in->f);
    else
        ch = *in->s ? (unsigned char)*in->s++ : EOF;
    if (ch != EOF)
        in->consumed++;
    return ch;
}

static void in_unget(In *in, int ch)
{
    if (ch == EOF)
        return;
    in->consumed--;
    if (in->f)
        ungetc(ch, in->f);
    else
        in->s--;
}

/* scanf over guest arguments: %d %i %u %x %o %f %e %g %s %c %[...] %n, widths,
 * l/h modifiers and assignment suppression. Returns the number of assignments
 * (EOF when the input ends before the first conversion). */
static int scan_guest(In *in, const char *fmt, uint32_t ap)
{
    int assigned = 0, any = 0;
    for (const char *p = fmt; *p; p++) {
        if (isspace((unsigned char)*p)) {
            int ch;
            while ((ch = in_get(in)) != EOF && isspace(ch))
                ;
            in_unget(in, ch);
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            if (*p == '%')
                p++;
            int ch = in_get(in);
            if (ch != (unsigned char)*p) {
                in_unget(in, ch);
                return ch == EOF && !any ? -1 : assigned;
            }
            continue;
        }
        p++;
        int suppress = 0, width = 0, lmod = 0, hmod = 0;
        if (*p == '*') {
            suppress = 1;
            p++;
        }
        while (isdigit((unsigned char)*p))
            width = width * 10 + (*p++ - '0');
        while (*p == 'l' || *p == 'h' || *p == 'L') {
            if (*p == 'l' || *p == 'L')
                lmod = 1;
            else
                hmod = 1;
            p++;
        }
        char conv = *p;
        if (!conv)
            break;
        if (conv == 'n') {
            if (!suppress) {
                WR32(RD32(ap), (uint32_t)in->consumed);
                ap += 4u;
            }
            continue;
        }
        if (conv != 'c' && conv != '[') {
            int ch;
            while ((ch = in_get(in)) != EOF && isspace(ch))
                ;
            in_unget(in, ch);
        }
        char tok[1024];
        int n = 0, max = width ? width : (int)sizeof(tok) - 1;
        if (max > (int)sizeof(tok) - 1)
            max = (int)sizeof(tok) - 1;
        if (conv == 'c') {
            if (!width)
                max = 1;
            for (; n < max; n++) {
                int ch = in_get(in);
                if (ch == EOF)
                    break;
                tok[n] = (char)ch;
            }
            if (!n)
                return any ? assigned : -1;
            any = 1;
            if (!suppress) {
                memcpy(g_mem + RD32(ap), tok, (size_t)n);
                ap += 4u;
                assigned++;
            }
            continue;
        }
        if (conv == '[') {
            p++;
            int neg = 0;
            if (*p == '^') {
                neg = 1;
                p++;
            }
            char set[256] = { 0 };
            if (*p == ']') {
                set[']'] = 1;
                p++;
            }
            for (; *p && *p != ']'; p++) {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    for (int k = (unsigned char)p[0]; k <= (unsigned char)p[2]; k++)
                        set[k] = 1;
                    p += 2;
                } else {
                    set[(unsigned char)*p] = 1;
                }
            }
            for (; n < max; n++) {
                int ch = in_get(in);
                if (ch == EOF || set[ch] == neg) {
                    in_unget(in, ch);
                    break;
                }
                tok[n] = (char)ch;
            }
            if (!n)
                return any ? assigned : -1;
            any = 1;
            if (!suppress) {
                memcpy(g_mem + RD32(ap), tok, (size_t)n);
                g_mem[RD32(ap) + (uint32_t)n] = 0;
                ap += 4u;
                assigned++;
            }
            continue;
        }
        /* Tokens: strings up to whitespace, numbers as far as they parse. */
        int is_num = strchr("diuxXofeEgG", conv) != NULL;
        for (; n < max; n++) {
            int ch = in_get(in);
            if (ch == EOF || isspace(ch)) {
                in_unget(in, ch);
                break;
            }
            if (is_num) {
                int ok;
                if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G')
                    ok = isdigit(ch) || ch == '.' || ch == 'e' || ch == 'E' ||
                         ((ch == '-' || ch == '+') && (n == 0 || tok[n - 1] == 'e' || tok[n - 1] == 'E'));
                else if (conv == 'x' || conv == 'X' || conv == 'i')
                    ok = isxdigit(ch) || ((ch == 'x' || ch == 'X') && n > 0) || ((ch == '-' || ch == '+') && n == 0);
                else
                    ok = isdigit(ch) || ((ch == '-' || ch == '+') && n == 0);
                if (!ok) {
                    in_unget(in, ch);
                    break;
                }
            }
            tok[n] = (char)ch;
        }
        tok[n] = 0;
        if (!n) {
            int ch = in_get(in);
            in_unget(in, ch);
            return ch == EOF && !any ? -1 : assigned;
        }
        any = 1;
        if (suppress)
            continue;
        uint32_t dst = RD32(ap);
        ap += 4u;
        switch (conv) {
        case 's':
            memcpy(g_mem + dst, tok, (size_t)n + 1u);
            break;
        case 'd': case 'u':
            if (hmod)
                WR16(dst, (uint32_t)strtol(tok, NULL, 10));
            else
                WR32(dst, (uint32_t)strtoll(tok, NULL, 10));
            break;
        case 'i':
            WR32(dst, (uint32_t)strtol(tok, NULL, 0));
            break;
        case 'x': case 'X':
            WR32(dst, (uint32_t)strtoul(tok, NULL, 16));
            break;
        case 'o':
            WR32(dst, (uint32_t)strtoul(tok, NULL, 8));
            break;
        default:                                    /* f e g */
            if (lmod)
                WRF64(dst, strtod(tok, NULL));
            else
                WRF32(dst, strtod(tok, NULL));
            break;
        }
        assigned++;
    }
    return assigned;
}

SHIM(msvcrt, sscanf)
{
    In in = { gstr(ARG(0)), NULL, 0 };
    cdecl_ret(c, (uint32_t)scan_guest(&in, gstr(ARG(1)), c->r[4] + 12u));
}

SHIM(msvcrt, fscanf)
{
    FILE *f = host_file(ARG(0));
    if (!f) {
        cdecl_ret(c, (uint32_t)-1);
        return;
    }
    In in = { NULL, f, 0 };
    cdecl_ret(c, (uint32_t)scan_guest(&in, gstr(ARG(1)), c->r[4] + 12u));
}

/* ---- files ------------------------------------------------------------------- */

SHIM(msvcrt, fopen)
{
    const char *win = gstr(ARG(0));
    const char *mode = gstr(ARG(1));
    char path[1100];
    win_path_to_host(win, path, sizeof(path));
    char m[8];
    size_t k = 0;
    for (const char *q = mode; *q && k + 1 < sizeof(m); q++)
        if (*q != 't')
            m[k++] = *q;
    m[k] = 0;
    FILE *f = fopen(path, m);
    if (!f) {
        rt_log("fopen(%s, %s) -> not found", win, mode);
        cdecl_ret(c, 0);
        return;
    }
    uint32_t g = file_new(f);
    if (!g)
        fclose(f);
    if (strchr(mode, 'w') || strchr(mode, 'a'))
        rt_log("fopen(%s, %s) -> %s", win, mode, path);
    cdecl_ret(c, g);
}

SHIM(msvcrt, tmpfile)
{
    FILE *f = tmpfile();
    cdecl_ret(c, f ? file_new(f) : 0);
}

SHIM(msvcrt, fclose)
{
    uint32_t g = ARG(0);
    FILE *f = host_file(g);
    int r = -1;
    if (f && f != stdin && f != stdout && f != stderr) {
        r = fclose(f);
        host_files[(g - iob_base) / GFILE_SIZE] = NULL;
    }
    cdecl_ret(c, (uint32_t)r);
}

SHIM(msvcrt, fread)
{
    FILE *f = host_file(ARG(3));
    size_t n = f ? fread(g_mem + ARG(0), ARG(1), ARG(2), f) : 0;
    cdecl_ret(c, (uint32_t)n);
}

SHIM(msvcrt, fwrite)
{
    FILE *f = host_file(ARG(3));
    size_t n = 0;
    if (f == stdout || f == stderr) {
        Out o = { 0, 0, 0 };
        out_put(&o, (const char *)g_mem + ARG(0), (size_t)ARG(1) * ARG(2));
        log_output(f, &o);
        free(o.buf);
        n = ARG(2);
    } else if (f) {
        n = fwrite(g_mem + ARG(0), ARG(1), ARG(2), f);
    }
    cdecl_ret(c, (uint32_t)n);
}

SHIM(msvcrt, fseek)
{
    FILE *f = host_file(ARG(0));
    cdecl_ret(c, f ? (uint32_t)fseek(f, (long)(int32_t)ARG(1), (int)ARG(2)) : (uint32_t)-1);
}

SHIM(msvcrt, ftell)
{
    FILE *f = host_file(ARG(0));
    cdecl_ret(c, f ? (uint32_t)ftell(f) : (uint32_t)-1);
}

SHIM(msvcrt, rewind)
{
    FILE *f = host_file(ARG(0));
    if (f)
        rewind(f);
    cdecl_ret(c, 0);
}

SHIM(msvcrt, fflush)
{
    FILE *f = ARG(0) ? host_file(ARG(0)) : NULL;
    cdecl_ret(c, (uint32_t)fflush(f));
}

SHIM(msvcrt, _flushall)
{
    fflush(NULL);
    cdecl_ret(c, 0);
}

/* ---- environment and time --------------------------------------------------------- */

SHIM(msvcrt, getenv)
{
    const char *name = gstr(ARG(0));
    rt_log("getenv(%s) -> not set", name);
    cdecl_ret(c, 0);
}

SHIM(msvcrt, clock)
{
    cdecl_ret(c, win32_tick_count());   /* CLOCKS_PER_SEC is 1000 */
}

SHIM(msvcrt, time)
{
    uint32_t t = (uint32_t)time(NULL);
    if (ARG(0))
        WR32(ARG(0), t);
    cdecl_ret(c, t);
}

static uint32_t tm_buf, ctime_buf;

SHIM(msvcrt, localtime)
{
    time_t t = (time_t)(int32_t)RD32(ARG(0));
    struct tm *tm = localtime(&t);
    if (!tm) {
        cdecl_ret(c, 0);
        return;
    }
    if (!tm_buf)
        tm_buf = scratch_alloc(36, 4);
    const int v[9] = { tm->tm_sec, tm->tm_min, tm->tm_hour, tm->tm_mday, tm->tm_mon, tm->tm_year,
                       tm->tm_wday, tm->tm_yday, tm->tm_isdst };
    for (int i = 0; i < 9; i++)
        WR32(tm_buf + 4u * (uint32_t)i, (uint32_t)v[i]);
    cdecl_ret(c, tm_buf);
}

SHIM(msvcrt, ctime)
{
    time_t t = (time_t)(int32_t)RD32(ARG(0));
    if (!ctime_buf)
        ctime_buf = scratch_alloc(32, 4);
    char *s = ctime(&t);
    snprintf(gbuf(ctime_buf), 26, "%s", s ? s : "");
    cdecl_ret(c, ctime_buf);
}
