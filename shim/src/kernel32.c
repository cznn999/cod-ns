/* kernel32.dll: process, memory, file, time and locale APIs used by the game and its CRT. */
#include "shim.h"
#include "bench.h"
#include "module.h"

#include <SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_FILE_EXISTS 80u
#define ERROR_ALREADY_EXISTS 183u
#define INVALID_HANDLE_VALUE 0xFFFFFFFFu

#define FAKE_MODULE_BASE 0x7F000000u

static uint32_t last_error;

void win32_set_last_error(uint32_t e) { last_error = e; }

/* ---- handles ------------------------------------------------------------ */

#define MAX_HANDLES 1024u
#define HANDLE_BASE 0x1000u
static HandleObj handles[MAX_HANDLES];
static pthread_mutex_t handles_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t handle_new(int type)
{
    pthread_mutex_lock(&handles_lock);
    for (uint32_t i = 1; i < MAX_HANDLES; i++) {
        if (handles[i].type == H_FREE) {
            memset(&handles[i], 0, sizeof(handles[i]));
            handles[i].type = type;
            handles[i].fd = -1;
            pthread_mutex_unlock(&handles_lock);
            return HANDLE_BASE + i * 4u;
        }
    }
    rt_fatal(NULL, "out of Win32 handles");
}

HandleObj *handle_get(uint32_t h, int type)
{
    if (h < HANDLE_BASE)
        return NULL;
    uint32_t i = (h - HANDLE_BASE) / 4u;
    if (i >= MAX_HANDLES || handles[i].type == H_FREE)
        return NULL;
    if (type != H_FREE && handles[i].type != type)
        return NULL;
    return &handles[i];
}

static void rb_release(HandleObj *o);   /* below: buffered reads */

void handle_close(uint32_t h)
{
    HandleObj *o = handle_get(h, H_FREE);
    if (!o)
        return;
    if (o->type == H_FILE)
        rb_release(o);   /* waits for a prefetch still reading this descriptor */
    if (o->type == H_FILE && o->fd >= 0)
        close(o->fd);
    if (o->type == H_FILE) {
        free(o->ptr);
        o->ptr = NULL;
        free(o->path);
        o->path = NULL;
    }
    if (o->type == H_FIND && o->ptr) {
        free(o->ptr);
    }
    o->type = H_FREE;
}

/* ---- paths -------------------------------------------------------------- */

char g_cd_dir[1024];

void win_path_to_host(const char *win, char *out, size_t out_size)
{
    char tmp[1024];
    size_t n = 0;
    for (const char *p = win; *p && n + 1 < sizeof(tmp); p++)
        tmp[n++] = *p == '\\' ? '/' : *p;
    tmp[n] = 0;
    const char *root = g_game_dir;
    const char *rest = tmp;
    if (isalpha((unsigned char)tmp[0]) && tmp[1] == ':') {
        if (toupper((unsigned char)tmp[0]) == WIN_CD_DRIVE) {
            root = g_cd_dir;
            rest = tmp + 2;
        } else {
            rest = tmp + 2;
            /* C:/CoD/... is the install directory. */
            const char *inst = "/CoD";
            size_t il = strlen(inst);
            if (strncasecmp(rest, inst, il) == 0 && (rest[il] == '/' || rest[il] == 0))
                rest += il;
        }
    }
    while (*rest == '/')
        rest++;
    if (rest[0] == '.' && rest[1] == '/')
        rest += 2;
    snprintf(out, out_size, "%s/%s", root, rest);
}

/* ---- process / module --------------------------------------------------- */

SHIM(kernel32, GetVersion) { stdcall_ret(c, 0x0A280105u, 0); }   /* Windows XP 5.1.2600 */

SHIM(kernel32, GetVersionExA)
{
    uint32_t p = ARG(0);
    WR32(p + 4u, 5);
    WR32(p + 8u, 1);
    WR32(p + 12u, 2600);
    WR32(p + 16u, 2);         /* VER_PLATFORM_WIN32_NT */
    WR8(p + 20u, 0);
    stdcall_ret(c, 1, 1);
}

static uint32_t cmdline;

SHIM(kernel32, GetCommandLineA) { stdcall_ret(c, cmdline, 0); }

SHIM(kernel32, GetStartupInfoA)
{
    uint32_t p = ARG(0);
    memset(gptr(p), 0, 68);
    WR32(p, 68);
    stdcall_ret(c, 0, 1);
}

/* System DLLs the game may ask for: fake handles whose GetProcAddress resolves to
 * host implementations. EAXMan.dll is left out on purpose (no EAX). */
static const char *const known_modules[] = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "winmm.dll", "advapi32.dll", "shell32.dll",
    "mss32.dll", "opengl32.dll", "ddraw.dll",
};

static uint32_t module_handle(const char *name)
{
    char lower[64];
    size_t n = 0;
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    for (; base[n] && n + 1 < sizeof(lower); n++)
        lower[n] = (char)tolower((unsigned char)base[n]);
    lower[n] = 0;
    if (!strchr(lower, '.') && n + 5 < sizeof(lower))
        strcat(lower, ".dll");
    for (uint32_t i = 0; i < sizeof(known_modules) / sizeof(known_modules[0]); i++)
        if (strcmp(lower, known_modules[i]) == 0)
            return FAKE_MODULE_BASE + (i + 1u) * 0x10000u;
    return 0;
}

SHIM(kernel32, GetModuleHandleA)
{
    uint32_t name = ARG(0);
    uint32_t h = GUEST_IMAGE_BASE;
    if (name) {
        int m = module_find(gstr(name));
        h = m >= 0 ? g_guest_modules[m].base : module_handle(gstr(name));
    }
    stdcall_ret(c, h, 1);
}

SHIM(kernel32, GetModuleFileNameA)
{
    uint32_t mod = ARG(0), buf = ARG(1), size = ARG(2);
    int m = mod ? module_by_base(mod) : 0;
    char path[300];
    snprintf(path, sizeof(path), WIN_INSTALL_DIR "\\%s", m > 0 ? g_guest_modules[m].name : "CoDSP.exe");
    uint32_t n = (uint32_t)strlen(path);
    if (n >= size)
        n = size ? size - 1u : 0u;
    memcpy(gptr(buf), path, n);
    if (size)
        WR8(buf + n, 0);
    stdcall_ret(c, n, 3);
}

SHIM(kernel32, LoadLibraryA)
{
    const char *name = gstr(ARG(0));
    int m = module_find(name);
    uint32_t h = m >= 0 ? module_load(c, m) : module_handle(name);
    rt_log("LoadLibraryA(%s) -> %08X", name, h);
    if (!h)
        win32_set_last_error(126);
    stdcall_ret(c, h, 1);
}

SHIM(kernel32, FreeLibrary)
{
    uint32_t h = ARG(0);
    if (module_by_base(h) > 0)
        module_free(c, h);
    stdcall_ret(c, 1, 1);
}

SHIM(kernel32, GetProcAddress)
{
    uint32_t h = ARG(0), name = ARG(1);
    uint32_t r = 0;
    if (name >= 0x10000u && module_by_base(h) >= 0) {
        r = module_export(h, gstr(name));
    } else if (h > FAKE_MODULE_BASE && name >= 0x10000u) {
        uint32_t i = (h - FAKE_MODULE_BASE) / 0x10000u - 1u;
        if (i < sizeof(known_modules) / sizeof(known_modules[0]))
            r = win32_get_proc(known_modules[i], gstr(name));
    }
    rt_log("GetProcAddress(%08X, %s) -> %08X", h, name >= 0x10000u ? gstr(name) : "#ordinal", r);
    stdcall_ret(c, r, 2);
}

/* opengl32 (gl* and wgl*) entry points: shim/src/opengl32.c. */
uint32_t opengl32_get_proc(const char *name);

uint32_t win32_get_proc(const char *dll, const char *name)
{
    if (strcasecmp(dll, "opengl32.dll") == 0)
        return opengl32_get_proc(name);
    for (uint32_t i = 0; i < g_guest_import_count; i++) {
        const GuestImport *imp = &g_guest_imports[i];
        if (strcasecmp(imp->dll, dll) == 0 && strcmp(imp->name, name) == 0)
            return thunk_register(imp->fn, imp->name);
    }
    return 0;
}

SHIM(kernel32, GetCurrentProcess) { stdcall_ret(c, 0xFFFFFFFFu, 0); }

void platform_shutdown(void);
void render_async_sync(void);

SHIM(kernel32, ExitProcess)
{
    rt_log("ExitProcess(%u)", ARG(0));
    render_async_sync();
    platform_shutdown();
    exit((int)ARG(0));
}

SHIM(kernel32, TerminateProcess)
{
    rt_log("TerminateProcess(%u)", ARG(1));
    platform_shutdown();
    exit((int)ARG(1));
}

SHIM(kernel32, UnhandledExceptionFilter) { stdcall_ret(c, 1, 1); }

SHIM(kernel32, RaiseException)
{
    rt_fatal(c, "RaiseException(%08X, flags %08X)", ARG(0), ARG(1));
}

SHIM(kernel32, RtlUnwind)
{
    rt_fatal(c, "RtlUnwind(frame %08X, target %08X)", ARG(0), ARG(1));
}

SHIM(kernel32, DebugBreak) { rt_fatal(c, "DebugBreak"); }

SHIM(kernel32, OutputDebugStringA)
{
    rt_log("[debug] %s", gstr(ARG(0)));
    stdcall_ret(c, 0, 1);
}

SHIM(kernel32, GetLastError) { stdcall_ret(c, last_error, 0); }

SHIM(kernel32, CreateMutexA)
{
    win32_set_last_error(0);
    stdcall_ret(c, handle_new(H_MUTEX), 3);
}

SHIM(kernel32, ReleaseMutex) { stdcall_ret(c, 1, 1); }

SHIM(kernel32, CloseHandle)
{
    handle_close(ARG(0));
    stdcall_ret(c, 1, 1);
}

/* ---- environment -------------------------------------------------------- */

static uint32_t env_block, env_block_w;

SHIM(kernel32, GetEnvironmentStrings) { stdcall_ret(c, env_block, 0); }
SHIM(kernel32, GetEnvironmentStringsW) { stdcall_ret(c, env_block_w, 0); }
SHIM(kernel32, FreeEnvironmentStringsA) { stdcall_ret(c, 1, 1); }
SHIM(kernel32, FreeEnvironmentStringsW) { stdcall_ret(c, 1, 1); }
SHIM(kernel32, GetEnvironmentVariableA) { win32_set_last_error(203); stdcall_ret(c, 0, 3); }
SHIM(kernel32, SetHandleCount) { stdcall_ret(c, ARG(0), 1); }

static uint32_t std_handles[3];

SHIM(kernel32, GetStdHandle)
{
    uint32_t which = ARG(0);
    uint32_t r = INVALID_HANDLE_VALUE;
    if (which >= 0xFFFFFFF4u && which <= 0xFFFFFFF6u)
        r = std_handles[0xFFFFFFF6u - which];
    stdcall_ret(c, r, 1);
}

SHIM(kernel32, SetStdHandle) { stdcall_ret(c, 1, 2); }

SHIM(kernel32, GetFileType)
{
    HandleObj *o = handle_get(ARG(0), H_FREE);
    uint32_t t = 0;
    if (o && o->type == H_STD)
        t = 2;   /* FILE_TYPE_CHAR */
    else if (o && o->type == H_FILE)
        t = 1;   /* FILE_TYPE_DISK */
    stdcall_ret(c, t, 1);
}

/* ---- memory ------------------------------------------------------------- */

SHIM(kernel32, HeapCreate) { stdcall_ret(c, handle_new(H_HEAP), 3); }
SHIM(kernel32, HeapDestroy) { handle_close(ARG(0)); stdcall_ret(c, 1, 1); }

SHIM(kernel32, HeapAlloc)
{
    uint32_t flags = ARG(1), size = ARG(2);
    uint32_t p = heap_alloc(size ? size : 1u);
    g_bench_ctr.heap_allocs++;
    if (p && (flags & 0x8u))
        memset(gptr(p), 0, size);
    if (!p)
        rt_log("HeapAlloc(%u) failed", size);
    stdcall_ret(c, p, 3);
}

SHIM(kernel32, HeapFree)
{
    heap_free(ARG(2));
    stdcall_ret(c, 1, 3);
}

SHIM(kernel32, HeapReAlloc)
{
    uint32_t flags = ARG(1), ptr = ARG(2), size = ARG(3);
    uint32_t old = heap_size(ptr);
    uint32_t p;
    if (flags & 0x10u)            /* HEAP_REALLOC_IN_PLACE_ONLY */
        p = size <= old ? ptr : 0u;
    else
        p = heap_realloc(ptr, size);
    if (p && (flags & 0x8u) && size > old)
        memset(gptr(p + old), 0, size - old);
    stdcall_ret(c, p, 4);
}

/* VirtualAlloc reservations: 64 KiB aligned blocks carved from the guest heap. */
typedef struct Region {
    uint32_t base, size, raw;
} Region;

static Region regions[256];

SHIM(kernel32, VirtualAlloc)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    uint32_t r = 0;
    if (addr) {
        /* Commit inside an existing reservation: the memory is already there. */
        for (uint32_t i = 0; i < 256u; i++)
            if (regions[i].base && addr >= regions[i].base && addr + size <= regions[i].base + regions[i].size)
                r = addr & ~0xFFFu;
        if (!r)
            rt_log("VirtualAlloc(%08X, %u, %X): address outside any reservation", addr, size, type);
    } else {
        uint32_t sz = (size + 0xFFFFu) & ~0xFFFFu;
        uint32_t raw = heap_alloc(sz + 0x10000u);
        if (raw) {
            r = (raw + 0xFFFFu) & ~0xFFFFu;
            memset(gptr(r), 0, sz);
            for (uint32_t i = 0; i < 256u; i++) {
                if (!regions[i].base) {
                    regions[i].base = r;
                    regions[i].size = sz;
                    regions[i].raw = raw;
                    break;
                }
            }
        }
    }
    stdcall_ret(c, r, 4);
}

SHIM(kernel32, VirtualFree)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    for (uint32_t i = 0; i < 256u; i++) {
        Region *g = &regions[i];
        if (!g->base || addr < g->base || addr >= g->base + g->size)
            continue;
        if (type & 0x8000u) {        /* MEM_RELEASE */
            heap_free(g->raw);
            g->base = 0;
        } else if (size) {           /* MEM_DECOMMIT: recommitted pages must read as zero */
            memset(gptr(addr), 0, size);
        }
        break;
    }
    stdcall_ret(c, 1, 3);
}

/* ---- files -------------------------------------------------------------- */

SHIM(kernel32, CreateFileA)
{
    const char *win = gstr(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    char path[1100];
    win_path_to_host(win, path, sizeof(path));
    int flags = 0;
    int rw = (access & 0x40000000u) != 0;       /* GENERIC_WRITE */
    int rd = (access & 0x80000000u) != 0 || !rw;
    flags = rw ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case 1: flags |= O_CREAT | O_EXCL; break;   /* CREATE_NEW */
    case 2: flags |= O_CREAT | O_TRUNC; break;  /* CREATE_ALWAYS */
    case 3: break;                              /* OPEN_EXISTING */
    case 4: flags |= O_CREAT; break;            /* OPEN_ALWAYS */
    case 5: flags |= O_TRUNC; break;            /* TRUNCATE_EXISTING */
    default: break;
    }
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        win32_set_last_error(errno == EEXIST ? ERROR_FILE_EXISTS :
                             errno == EACCES ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND);
        rt_log("CreateFileA(%s) -> not found", win);
        stdcall_ret(c, INVALID_HANDLE_VALUE, 7);
        return;
    }
    if (rw)
        rt_log("CreateFileA(%s, write, disp %u) -> %s", win, disp, path);
    g_bench_ctr.file_opens++;
    uint32_t h = handle_new(H_FILE);
    handle_get(h, H_FILE)->fd = fd;
    handle_get(h, H_FILE)->path = rw ? NULL : strdup(path);
    win32_set_last_error(0);
    stdcall_ret(c, h, 7);
}

/* ---- buffered reads ----------------------------------------------------
 * The game reads through its C runtime's small stream buffer: sound is streamed
 * in small ReadFile calls, and the AI graphs (graphs/graph*.dat) are parsed with
 * a seek, a size query (seek to the end and back) and a 512-byte read per item,
 * thousands of times per mission load. Each was a system call or more, slow on
 * the Vita. Reads go through a per-handle read-ahead buffer that stays valid
 * across seeks: while it is active the file position the game sees is b->cur
 * (the descriptor's own position is only set again for a refill or a write),
 * and the file size for seeks from the end is asked once.
 */
#define READ_AHEAD (64u * 1024u)

/* ---- asynchronous read-ahead -----------------------------------------------
 * Sequential readers (the streamed music and ambience, level data) get the next
 * block read on an I/O thread while they consume the current one, so the game
 * thread doesn't wait for the memory card (it stalled frames on the Vita). */
typedef struct Prefetch {
    volatile int state;   /* 0 idle, 1 queued/reading, 2 done */
    int fd;
    off_t base;
    ssize_t len;
    uint8_t data[READ_AHEAD];
} Prefetch;

#define PF_QUEUE 32
static Prefetch *pf_queue[PF_QUEUE];
static unsigned pf_head, pf_tail;
static SDL_mutex *pf_lock;
static SDL_cond *pf_work, *pf_done;

static int pf_thread(void *unused)
{
    (void)unused;
    SDL_LockMutex(pf_lock);
    for (;;) {
        while (pf_head == pf_tail)
            SDL_CondWait(pf_work, pf_lock);
        Prefetch *p = pf_queue[pf_head % PF_QUEUE];
        pf_head++;
        SDL_UnlockMutex(pf_lock);
        ssize_t got = pread(p->fd, p->data, READ_AHEAD, p->base);
        SDL_LockMutex(pf_lock);
        p->len = got;
        p->state = 2;
        SDL_CondBroadcast(pf_done);
    }
    return 0;
}

static int pf_start(void)
{
    static int started = -1;
    if (started < 0) {
        pf_lock = SDL_CreateMutex();
        pf_work = SDL_CreateCond();
        pf_done = SDL_CreateCond();
        started = pf_lock && pf_work && pf_done && SDL_CreateThread(pf_thread, "cod-io", NULL) != NULL;
        if (!started)
            rt_log("I/O read-ahead thread unavailable: reads stay synchronous");
    }
    return started;
}

static void pf_request(Prefetch *p, int fd, off_t base)
{
    SDL_LockMutex(pf_lock);
    if (pf_tail - pf_head < PF_QUEUE) {
        p->fd = fd;
        p->base = base;
        p->state = 1;
        pf_queue[pf_tail % PF_QUEUE] = p;
        pf_tail++;
        SDL_CondSignal(pf_work);
    }
    SDL_UnlockMutex(pf_lock);
}

/* Waits until a queued read is done (state 0 or 2 afterwards). */
static void pf_wait(Prefetch *p)
{
    if (!p || p->state != 1)
        return;
    SDL_LockMutex(pf_lock);
    while (p->state == 1)
        SDL_CondWait(pf_done, pf_lock);
    SDL_UnlockMutex(pf_lock);
}

typedef struct ReadBuf {
    Prefetch *pf;    /* next block read in the background (sequential readers) */
    int pf_fd;       /* the background reads' own descriptor (0: not open yet) */
    off_t last_end;  /* end of the previous refill, to spot sequential reading */
    int active;      /* cur is the file position (else the descriptor's) */
    int size_ok;     /* size is known */
    off_t cur, size;
    off_t fd_at;     /* the descriptor's position while active */
    off_t base;      /* file offset of data[0] */
    uint32_t len;    /* valid bytes in data */
    uint8_t data[READ_AHEAD];
} ReadBuf;

/* Puts the descriptor back at the game's position and drops the buffer (before
 * writes and anything else that uses the descriptor's position). */
static void rb_drop(HandleObj *o)
{
    ReadBuf *b = o->ptr;
    if (b && b->active) {
        lseek(o->fd, b->cur, SEEK_SET);
        b->active = 0;
    }
    if (b) {
        b->len = 0;
        b->size_ok = 0;
    }
}

static void rb_release(HandleObj *o)
{
    ReadBuf *b = o->ptr;
    if (b && b->pf) {
        pf_wait(b->pf);
        free(b->pf);
        b->pf = NULL;
    }
    if (b && b->pf_fd > 0) {
        close(b->pf_fd);
        b->pf_fd = 0;
    }
}

static ReadBuf *rb_activate(HandleObj *o)
{
    ReadBuf *b = o->ptr;
    if (!b) {
        b = calloc(1, sizeof(*b));
        o->ptr = b;
    }
    if (!b->active) {
        b->cur = b->fd_at = lseek(o->fd, 0, SEEK_CUR);
        b->active = 1;
    }
    return b;
}

static ssize_t rb_read(HandleObj *o, uint8_t *dst, uint32_t n)
{
    ReadBuf *b = rb_activate(o);
    ssize_t total = 0;
    while (n) {
        if (b->cur >= b->base && b->cur < b->base + (off_t)b->len) {
            uint32_t at = (uint32_t)(b->cur - b->base);
            uint32_t k = b->len - at < n ? b->len - at : n;
            memcpy(dst, b->data + at, k);
            b->cur += k;
            dst += k;
            n -= k;
            total += k;
            continue;
        }
        if (b->fd_at != b->cur)
            b->fd_at = lseek(o->fd, b->cur, SEEK_SET);
        if (n >= READ_AHEAD) {   /* big reads go straight through */
            ssize_t got = read(o->fd, dst, n);
            if (got < 0) {
                b->fd_at = -1;
                return total ? total : got;
            }
            b->cur += got;
            b->fd_at = b->cur;
            return total + got;
        }
        /* Refill: from the block read in the background if it is the right one. */
        ssize_t got = -1;
        int sequential = b->cur == b->last_end;
        if (b->pf && b->pf->state) {
            pf_wait(b->pf);
            if (b->pf->base == b->cur && b->pf->len > 0) {
                got = b->pf->len;
                memcpy(b->data, b->pf->data, (size_t)got);
            }
            b->pf->state = 0;
        }
        if (got < 0) {
            if (b->fd_at != b->cur)
                b->fd_at = lseek(o->fd, b->cur, SEEK_SET);
            got = read(o->fd, b->data, READ_AHEAD);
            b->fd_at = got > 0 ? b->cur + got : -1;
        } else {
            b->fd_at = -1;   /* the descriptor's position is wherever it was */
        }
        if (got <= 0) {
            b->len = 0;
            break;
        }
        b->base = b->cur;
        b->len = (uint32_t)got;
        b->last_end = b->base + got;
        /* A second sequential full block in a row: read the next one ahead. */
        static int no_pf = -1;
        if (no_pf < 0)
            no_pf = getenv("COD_NO_PREFETCH") != NULL;
        /* The background read has a descriptor of its own: on the Vita a pread on
         * the game thread's descriptor moved its position under the game thread's
         * lseek + read (the pk3 directories came out with pieces of other blocks). */
        if (sequential && got == READ_AHEAD && !no_pf && o->path && pf_start()) {
            if (!b->pf_fd)
                b->pf_fd = open(o->path, O_RDONLY);
            if (b->pf_fd < 0)
                b->pf_fd = -1;
            if (!b->pf && b->pf_fd > 0)
                b->pf = calloc(1, sizeof(*b->pf));
            if (b->pf)
                pf_request(b->pf, b->pf_fd, b->last_end);
        }
    }
    return total;
}

SHIM(kernel32, ReadFile)
{
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), out = ARG(3);
    HandleObj *o = handle_get(h, H_FILE);
    if (!o) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        stdcall_ret(c, 0, 5);
        return;
    }
    uint64_t t0 = win32_perf_counter();
    ssize_t got = rb_read(o, gptr(buf), n);
    if (got < 0)
        got = 0;
    /* I/O statistics (ReadFile shows up in gameplay profiles). */
    static uint64_t io_us, io_bytes, io_last;
    static uint32_t io_calls;
    uint64_t t1 = win32_perf_counter();
    g_bench_ctr.file_reads++;
    g_bench_ctr.read_bytes += (uint32_t)got;
    g_bench_ctr.read_us += t1 - t0;
    io_us += t1 - t0;
    io_bytes += (uint64_t)got;
    io_calls++;
    static int io_log = -1;
    if (io_log < 0)
        io_log = getenv("COD_IO_LOG") != NULL;
    if (io_log && t1 - io_last >= 5000000u) {
        rt_log("io: %u ReadFile calls, %llu KB, %.1f ms in the last %.1f s", io_calls,
               (unsigned long long)(io_bytes / 1024u), (double)io_us / 1000.0, (double)(t1 - io_last) / 1e6);
        io_last = t1;
        io_us = io_bytes = 0;
        io_calls = 0;
    }
    if (out)
        WR32(out, (uint32_t)got);
    stdcall_ret(c, 1, 5);
}

SHIM(kernel32, WriteFile)
{
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), out = ARG(3);
    HandleObj *o = handle_get(h, H_FREE);
    ssize_t put = 0;
    if (o && o->type == H_STD) {
        put = (ssize_t)fwrite(gptr(buf), 1, n, stderr);
    } else if (o && o->type == H_FILE) {
        rb_drop(o);
        put = write(o->fd, gptr(buf), n);
    } else {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        stdcall_ret(c, 0, 5);
        return;
    }
    if (out)
        WR32(out, put < 0 ? 0u : (uint32_t)put);
    stdcall_ret(c, 1, 5);
}

SHIM(kernel32, SetFilePointer)
{
    uint32_t h = ARG(0), dist = ARG(1), high = ARG(2), method = ARG(3);
    HandleObj *o = handle_get(h, H_FILE);
    if (!o) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        stdcall_ret(c, 0xFFFFFFFFu, 4);
        return;
    }
    int64_t off = (int32_t)dist;
    if (high)
        off = (int64_t)(((uint64_t)RD32(high) << 32) | dist);
    int whence = method == 1 ? SEEK_CUR : method == 2 ? SEEK_END : SEEK_SET;
    ReadBuf *b = o->ptr;
    off_t r;
    if (b && b->active) {
        /* No system call, except a size query once per file. */
        off_t from = 0;
        if (whence == SEEK_CUR) {
            from = b->cur;
        } else if (whence == SEEK_END) {
            if (!b->size_ok) {
                struct stat st;
                b->size = fstat(o->fd, &st) == 0 ? st.st_size : 0;
                b->size_ok = 1;
            }
            from = b->size;
        }
        r = from + (off_t)off;
        if (r < 0) {
            win32_set_last_error(131u);   /* ERROR_NEGATIVE_SEEK */
            r = -1;
        } else {
            b->cur = r;
        }
    } else {
        r = lseek(o->fd, (off_t)off, whence);
    }
    if (high)
        WR32(high, (uint32_t)((uint64_t)r >> 32));
    stdcall_ret(c, (uint32_t)r, 4);
}

SHIM(kernel32, SetEndOfFile)
{
    HandleObj *o = handle_get(ARG(0), H_FILE);
    int ok = 0;
    if (o) {
        rb_drop(o);
        off_t pos = lseek(o->fd, 0, SEEK_CUR);
        ok = ftruncate(o->fd, pos) == 0;
    }
    stdcall_ret(c, (uint32_t)ok, 1);
}

SHIM(kernel32, FlushFileBuffers) { stdcall_ret(c, 1, 1); }

SHIM(kernel32, DeleteFileA)
{
    char path[1100];
    win_path_to_host(gstr(ARG(0)), path, sizeof(path));
    stdcall_ret(c, unlink(path) == 0, 1);
}

static void unix_to_filetime(time_t t, uint32_t dst)
{
    uint64_t ft = ((uint64_t)t + 11644473600ull) * 10000000ull;
    WR64(dst, ft);
}

SHIM(kernel32, GetFileTime)
{
    HandleObj *o = handle_get(ARG(0), H_FILE);
    struct stat st;
    if (!o || fstat(o->fd, &st) != 0) {
        stdcall_ret(c, 0, 4);
        return;
    }
    for (int i = 1; i <= 3; i++)
        if (ARG(i))
            unix_to_filetime(st.st_mtime, ARG(i));
    stdcall_ret(c, 1, 4);
}

SHIM(kernel32, CompareFileTime)
{
    uint64_t a = RD64(ARG(0)), b = RD64(ARG(1));
    stdcall_ret(c, a < b ? 0xFFFFFFFFu : a > b ? 1u : 0u, 2);
}

SHIM(kernel32, GetDriveTypeA)
{
    uint32_t root = ARG(0);
    uint32_t t = 1;   /* DRIVE_NO_ROOT_DIR */
    if (root) {
        int d = toupper((unsigned char)*gstr(root));
        if (d == 'C')
            t = 3;    /* DRIVE_FIXED */
        else if (d == WIN_CD_DRIVE)
            t = 5;    /* DRIVE_CDROM */
    } else {
        t = 3;
    }
    stdcall_ret(c, t, 1);
}

/* FindFirstFile / FindNextFile over a directory with a case-insensitive wildcard. */
typedef struct FindState {
    DIR *dir;
    char dir_path[1100];
    char pattern[260];
} FindState;

static int wild_match(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*')
                p++;
            if (!p[1])
                return 1;
            for (; *s; s++)
                if (wild_match(p + 1, s))
                    return 1;
            return 0;
        }
        if (!*s)
            return 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s))
            return 0;
    }
    if (*s == 0)
        return 1;
    /* "*.*" style patterns also match names without a dot. */
    return 0;
}

static int find_next(FindState *f, uint32_t data)
{
    struct dirent *e;
    while ((e = readdir(f->dir)) != NULL) {
        const char *pat = f->pattern;
        int match = wild_match(pat, e->d_name);
        if (!match && strcmp(pat, "*.*") == 0)
            match = 1;
        if (!match)
            continue;
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s", f->dir_path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        memset(gptr(data), 0, 320);
        WR32(data, S_ISDIR(st.st_mode) ? 0x10u : 0x20u);
        unix_to_filetime(st.st_mtime, data + 4u);
        unix_to_filetime(st.st_mtime, data + 12u);
        unix_to_filetime(st.st_mtime, data + 20u);
        WR32(data + 28u, (uint32_t)((uint64_t)st.st_size >> 32));
        WR32(data + 32u, (uint32_t)st.st_size);
        snprintf(gbuf(data + 44u), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}

SHIM(kernel32, FindFirstFileA)
{
    const char *win = gstr(ARG(0));
    uint32_t data = ARG(1);
    char path[1100];
    win_path_to_host(win, path, sizeof(path));
    char *slash = strrchr(path, '/');
    FindState *f = calloc(1, sizeof(*f));
    if (slash) {
        *slash = 0;
        snprintf(f->dir_path, sizeof(f->dir_path), "%s", path);
        snprintf(f->pattern, sizeof(f->pattern), "%s", slash + 1);
    }
    f->dir = opendir(f->dir_path);
    if (!f->dir || !find_next(f, data)) {
        if (f->dir)
            closedir(f->dir);
        free(f);
        win32_set_last_error(ERROR_FILE_NOT_FOUND);
        stdcall_ret(c, INVALID_HANDLE_VALUE, 2);
        return;
    }
    uint32_t h = handle_new(H_FIND);
    handle_get(h, H_FIND)->ptr = f;
    stdcall_ret(c, h, 2);
}

SHIM(kernel32, FindNextFileA)
{
    HandleObj *o = handle_get(ARG(0), H_FIND);
    int ok = o && find_next(o->ptr, ARG(1));
    if (!ok)
        win32_set_last_error(ERROR_NO_MORE_FILES);
    stdcall_ret(c, (uint32_t)ok, 2);
}

SHIM(kernel32, FindClose)
{
    HandleObj *o = handle_get(ARG(0), H_FIND);
    if (o) {
        FindState *f = o->ptr;
        closedir(f->dir);
    }
    handle_close(ARG(0));
    stdcall_ret(c, 1, 1);
}

/* ---- time --------------------------------------------------------------- */

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
static uint64_t t0_us;

static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }
#else
static uint64_t t0_us;

/* COD_FAKE_CLOCK=N (debug): the clock advances N microseconds per query instead
 * of following real time, so a run (e.g. the benchmark with COD_BENCH_SHOT) plays
 * the same frames every time and screenshots of two builds can be compared. */
static int fake_clock = -1;
static uint64_t fake_us, fake_step;

static uint64_t now_us(void)
{
    if (fake_clock < 0) {
        const char *e = getenv("COD_FAKE_CLOCK");
        fake_step = e ? strtoull(e, NULL, 10) : 0u;
        fake_clock = fake_step != 0;
        fake_us = 1000000u;
    }
    if (fake_clock)
        return fake_us += fake_step;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull;
}
#endif

/* Microseconds -> milliseconds without a 64-bit division (a library call on the
 * Vita's CPU; the game polls the tick count thousands of times per frame): only
 * the 32-bit delta since the previous call is divided (by a constant). */
static uint32_t us_to_ms(uint64_t us)
{
    static uint64_t base_us;
    static uint32_t base_ms;
    uint64_t d = us - base_us;
    if (us < base_us || d >= 0x7FFFFFFFull) {   /* rare: first call or a long gap */
        base_ms = (uint32_t)(us / 1000ull);
        base_us = (uint64_t)base_ms * 1000u;
        return base_ms;
    }
    uint32_t dms = (uint32_t)d / 1000u;
    base_us += (uint64_t)dms * 1000u;
    base_ms += dms;
    return base_ms;
}

uint32_t win32_tick_count(void)
{
    return us_to_ms(now_us() - t0_us);
}

uint64_t win32_perf_counter(void)
{
    return now_us() - t0_us;
}

/* Milliseconds since start, for log timestamps (valid before kernel32_init too). */
uint32_t win32_perf_counter_ms(void)
{
    if (!t0_us)
        t0_us = now_us();
    return (uint32_t)((now_us() - t0_us) / 1000ull);
}

/* Time the game spent waiting for its next fixed step (its 30 fps cap), for
 * benchmarks and the overlay; accumulated by the scheduler hook in
 * runtime/src/igi_fixes.c. */
uint64_t g_idle_us;
BenchCounters g_bench_ctr;

SHIM(kernel32, GetTickCount)
{
    stdcall_ret(c, us_to_ms(win32_perf_counter()) + 60000u, 0);
}

SHIM(kernel32, QueryPerformanceCounter)
{
    WR64(ARG(0), win32_perf_counter());
    stdcall_ret(c, 1, 1);
}

SHIM(kernel32, Sleep)
{
    usleep(ARG(0) * 1000u);
    stdcall_ret(c, 0, 1);
}

SHIM(kernel32, GetLocalTime)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    uint32_t p = ARG(0);
    WR16(p + 0u, (uint32_t)tm.tm_year + 1900u);
    WR16(p + 2u, (uint32_t)tm.tm_mon + 1u);
    WR16(p + 4u, (uint32_t)tm.tm_wday);
    WR16(p + 6u, (uint32_t)tm.tm_mday);
    WR16(p + 8u, (uint32_t)tm.tm_hour);
    WR16(p + 10u, (uint32_t)tm.tm_min);
    WR16(p + 12u, (uint32_t)tm.tm_sec);
    WR16(p + 14u, 0);
    stdcall_ret(c, 0, 1);
}

/* ---- code pages / locale (Latin-1 only) --------------------------------- */

SHIM(kernel32, GetACP) { stdcall_ret(c, 1252, 0); }
SHIM(kernel32, GetOEMCP) { stdcall_ret(c, 437, 0); }

SHIM(kernel32, GetCPInfo)
{
    uint32_t p = ARG(1);
    memset(gptr(p), 0, 20);
    WR32(p, 1);
    WR8(p + 4u, '?');
    stdcall_ret(c, 1, 2);
}

SHIM(kernel32, MultiByteToWideChar)
{
    uint32_t src = ARG(2), dst = ARG(4), dst_n = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0)
        n = (int32_t)strlen(gstr(src)) + 1;
    if (dst_n) {
        if ((uint32_t)n > dst_n)
            n = (int32_t)dst_n;
        for (int32_t i = 0; i < n; i++)
            WR16(dst + (uint32_t)i * 2u, RD8(src + (uint32_t)i));
    }
    stdcall_ret(c, (uint32_t)n, 6);
}

SHIM(kernel32, WideCharToMultiByte)
{
    uint32_t src = ARG(2), dst = ARG(4), dst_n = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) {
        n = 0;
        while (RD16(src + (uint32_t)n * 2u))
            n++;
        n++;
    }
    if (dst_n) {
        if ((uint32_t)n > dst_n)
            n = (int32_t)dst_n;
        for (int32_t i = 0; i < n; i++) {
            uint32_t ch = RD16(src + (uint32_t)i * 2u);
            WR8(dst + (uint32_t)i, ch < 256u ? ch : '?');
        }
    }
    stdcall_ret(c, (uint32_t)n, 8);
}

static uint32_t ctype1(uint32_t ch)
{
    uint32_t t = 0;
    if (ch >= 256u)
        return 0x100u;   /* C1_ALPHA */
    if (isupper((int)ch)) t |= 0x001u;
    if (islower((int)ch)) t |= 0x002u;
    if (isdigit((int)ch)) t |= 0x004u;
    if (isspace((int)ch)) t |= 0x008u;
    if (ispunct((int)ch)) t |= 0x010u;
    if (iscntrl((int)ch)) t |= 0x020u;
    if (ch == ' ' || ch == '\t') t |= 0x040u;
    if (isxdigit((int)ch)) t |= 0x080u;
    if (isalpha((int)ch)) t |= 0x100u;
    return t;
}

SHIM(kernel32, GetStringTypeA)
{
    uint32_t src = ARG(2), out = ARG(4);
    int32_t n = (int32_t)ARG(3);
    if (n < 0)
        n = (int32_t)strlen(gstr(src)) + 1;
    for (int32_t i = 0; i < n; i++)
        WR16(out + (uint32_t)i * 2u, ctype1(RD8(src + (uint32_t)i)));
    stdcall_ret(c, 1, 5);
}

SHIM(kernel32, GetStringTypeW)
{
    uint32_t src = ARG(1), out = ARG(3);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) {
        n = 0;
        while (RD16(src + (uint32_t)n * 2u))
            n++;
        n++;
    }
    for (int32_t i = 0; i < n; i++)
        WR16(out + (uint32_t)i * 2u, ctype1(RD16(src + (uint32_t)i * 2u)));
    stdcall_ret(c, 1, 4);
}

static uint32_t map_char(uint32_t flags, uint32_t ch)
{
    if (ch < 256u && (flags & 0x100u))
        return (uint32_t)tolower((int)ch);
    if (ch < 256u && (flags & 0x200u))
        return (uint32_t)toupper((int)ch);
    return ch;
}

SHIM(kernel32, LCMapStringA)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), dst_n = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0)
        n = (int32_t)strlen(gstr(src)) + 1;
    if (dst_n) {
        if ((uint32_t)n > dst_n)
            n = (int32_t)dst_n;
        for (int32_t i = 0; i < n; i++)
            WR8(dst + (uint32_t)i, map_char(flags, RD8(src + (uint32_t)i)));
    }
    stdcall_ret(c, (uint32_t)n, 6);
}

SHIM(kernel32, LCMapStringW)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), dst_n = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) {
        n = 0;
        while (RD16(src + (uint32_t)n * 2u))
            n++;
        n++;
    }
    if (dst_n) {
        if ((uint32_t)n > dst_n)
            n = (int32_t)dst_n;
        for (int32_t i = 0; i < n; i++)
            WR16(dst + (uint32_t)i * 2u, map_char(flags, RD16(src + (uint32_t)i * 2u)));
    }
    stdcall_ret(c, (uint32_t)n, 6);
}

SHIM(kernel32, GetLocaleInfoA) { stdcall_ret(c, 0, 4); }

/* ---- init --------------------------------------------------------------- */

void kernel32_init(void)
{
    if (!t0_us)
        t0_us = now_us();
    /* Extra command line (e.g. "+set r_mode 4 +map training"): COD_ARGS, or on the
     * Vita the first line of ux0:data/cod1/args.txt. The Vita's default lowers the
     * hunk (128 MiB on the PC; the guest space is 192 MiB) and turns the light
     * flares off (their occlusion test reads the depth buffer back, which waits for
     * the GPU every frame). */
    const char *args = getenv("COD_ARGS");
#if defined(__vita__)
    static char file_args[512];
    FILE *af = fopen("ux0:data/cod1/args.txt", "r");
    if (af) {
        if (fgets(file_args, sizeof(file_args), af))
            file_args[strcspn(file_args, "\r\n")] = 0;
        fclose(af);
        args = file_args;
    } else {
        args = "+set com_hunkMegs 96 +set r_flares 0";
    }
#elif defined(__SWITCH__)
    /* sdmc:/switch/cod1/args.txt, else: the flares off (their occlusion test reads
     * the depth buffer back every frame) and, with a small guest space (started
     * from the album), a smaller hunk. */
    static char file_args[512];
    FILE *af = fopen("sdmc:/switch/cod1/args.txt", "r");
    if (af) {
        if (fgets(file_args, sizeof(file_args), af))
            file_args[strcspn(file_args, "\r\n")] = 0;
        fclose(af);
        args = file_args;
    } else if (g_guest_mem_size < 0x20000000u) {
        args = "+set com_hunkMegs 96 +set r_flares 0";
    } else {
        args = "+set r_flares 0";
    }
#endif
    char line[1024];
    snprintf(line, sizeof(line), "\"" WIN_INSTALL_DIR "\\CoDSP.exe\" %s", args ? args : "");
    cmdline = scratch_strdup(line);
    env_block = scratch_alloc(4, 4);
    env_block_w = scratch_alloc(4, 4);
    for (int i = 0; i < 3; i++)
        std_handles[i] = handle_new(H_STD);
#if defined(__SWITCH__)
    /* The parent directory without "..", which the SD card's paths do not take. */
    if (!g_cd_dir[0]) {
        snprintf(g_cd_dir, sizeof(g_cd_dir), "%s", g_game_dir);
        char *slash = strrchr(g_cd_dir, '/'), *colon = strchr(g_cd_dir, ':');
        if (slash && (!colon || slash > colon + 1))
            *slash = 0;
    }
#endif
    if (!g_cd_dir[0])
        if (snprintf(g_cd_dir, sizeof(g_cd_dir), "%.1000s/..", g_game_dir) >= (int)sizeof(g_cd_dir))
            g_cd_dir[0] = 0;
}
