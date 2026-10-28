/* kernel32.dll: the APIs the game uses beyond kernel32.c's set (file mappings, atoms,
 * directories, file time conversions, messages). */
#include "shim.h"

#include <string.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define INVALID_HANDLE_VALUE 0xFFFFFFFFu
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_ALREADY_EXISTS 183u

/* ---- file mappings: the whole file read into guest memory ------------------------- */

typedef struct Mapping {
    int fd;
    uint32_t size;
    uint32_t view;           /* guest copy, made by the first MapViewOfFile */
    int views;
} Mapping;

#define MAX_ANON 32
static uint32_t anon_views[MAX_ANON];   /* guest bases of the memory-only mappings */

/* CreateFileMappingA(file, attributes, protect, size high, size low, name) */
SHIM(kernel32, CreateFileMappingA)
{
    if (ARG(0) == INVALID_HANDLE_VALUE) {
        /* Memory backed by the paging file: one shared block, zeroed. */
        uint32_t size = ARG(4);
        Mapping *m = calloc(1, sizeof(*m));
        m->fd = -1;
        m->size = size;
        m->view = heap_alloc(size ? size : 1u);
        memset(g_mem + m->view, 0, size);
        for (int i = 0; i < MAX_ANON; i++)
            if (!anon_views[i]) {
                anon_views[i] = m->view;
                break;
            }
        rt_log("CreateFileMappingA(memory, %u bytes, %s)", size, ARG(5) ? gstr(ARG(5)) : "unnamed");
        uint32_t h = handle_new(H_MAPPING);
        handle_get(h, H_MAPPING)->ptr = m;
        win32_set_last_error(0);
        stdcall_ret(c, h, 6);
        return;
    }
    HandleObj *f = handle_get(ARG(0), H_FILE);
    if (!f) {
        rt_log("CreateFileMappingA: not a file handle %08X", ARG(0));
        stdcall_ret(c, 0, 6);
        return;
    }
    struct stat st;
    if (fstat(f->fd, &st) != 0) {
        stdcall_ret(c, 0, 6);
        return;
    }
    Mapping *m = calloc(1, sizeof(*m));
    m->fd = dup(f->fd);
    m->size = ARG(4) ? ARG(4) : (uint32_t)st.st_size;
    uint32_t h = handle_new(H_MAPPING);
    handle_get(h, H_MAPPING)->ptr = m;
    stdcall_ret(c, h, 6);
}

/* MapViewOfFile(mapping, access, offset high, offset low, bytes) */
SHIM(kernel32, MapViewOfFile)
{
    HandleObj *o = handle_get(ARG(0), H_MAPPING);
    if (!o) {
        stdcall_ret(c, 0, 5);
        return;
    }
    Mapping *m = o->ptr;
    uint32_t off = ARG(3), len = ARG(4) ? ARG(4) : m->size - off;
    if (m->fd < 0) {
        stdcall_ret(c, m->view + off, 5);
        return;
    }
    uint32_t p = heap_alloc(len ? len : 1u);
    if (!p) {
        rt_log("MapViewOfFile: out of guest memory for %u bytes", len);
        stdcall_ret(c, 0, 5);
        return;
    }
    uint32_t got = 0;
    while (got < len) {
        ssize_t r = pread(m->fd, g_mem + p + got, len - got, (off_t)off + got);
        if (r <= 0)
            break;
        got += (uint32_t)r;
    }
    if (got < len)
        memset(g_mem + p + got, 0, len - got);
    m->views++;
    stdcall_ret(c, p, 5);
}

SHIM(kernel32, UnmapViewOfFile)
{
    int anon = 0;
    for (int i = 0; i < MAX_ANON; i++)
        if (anon_views[i] && anon_views[i] == ARG(0))
            anon = 1;
    if (ARG(0) && !anon)
        heap_free(ARG(0));
    stdcall_ret(c, 1, 1);
}

/* CloseHandle of a mapping goes through handle_close, which frees the slot only;
 * the descriptor stays open (a few per run). */

/* ---- atoms -------------------------------------------------------------------- */

#define MAX_ATOMS 64
static char atoms[MAX_ATOMS][256];

SHIM(kernel32, GlobalAddAtomA)
{
    const char *s = gstr(ARG(0));
    int free_slot = -1;
    for (int i = 0; i < MAX_ATOMS; i++) {
        if (atoms[i][0] && strcasecmp(atoms[i], s) == 0) {
            stdcall_ret(c, 0xC000u + (uint32_t)i, 1);
            return;
        }
        if (!atoms[i][0] && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0) {
        stdcall_ret(c, 0, 1);
        return;
    }
    snprintf(atoms[free_slot], sizeof(atoms[free_slot]), "%s", s);
    stdcall_ret(c, 0xC000u + (uint32_t)free_slot, 1);
}

SHIM(kernel32, GlobalDeleteAtom)
{
    uint32_t i = ARG(0) - 0xC000u;
    if (i < MAX_ATOMS)
        atoms[i][0] = 0;
    stdcall_ret(c, 0, 1);
}

SHIM(kernel32, GlobalGetAtomNameA)
{
    uint32_t i = ARG(0) - 0xC000u, size = ARG(2);
    uint32_t n = 0;
    if (i < MAX_ATOMS && atoms[i][0] && size) {
        n = (uint32_t)strlen(atoms[i]);
        if (n >= size)
            n = size - 1u;
        memcpy(gbuf(ARG(1)), atoms[i], n);
        WR8(ARG(1) + n, 0);
    }
    stdcall_ret(c, n, 3);
}

/* ---- directories and files ---------------------------------------------------- */

SHIM(kernel32, CreateDirectoryA)
{
    char path[1100];
    win_path_to_host(gstr(ARG(0)), path, sizeof(path));
    int r = mkdir(path, 0755);
    if (r != 0)
        win32_set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_FILE_NOT_FOUND);
    stdcall_ret(c, r == 0, 2);
}

SHIM(kernel32, RemoveDirectoryA)
{
    char path[1100];
    win_path_to_host(gstr(ARG(0)), path, sizeof(path));
    stdcall_ret(c, rmdir(path) == 0, 1);
}

/* The parent directories of a file's path (the game moves saves into
 * save/autosave/ without creating it first). */
static void make_parents(const char *path)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
}

SHIM(kernel32, MoveFileA)
{
    char a[1100], b[1100];
    win_path_to_host(gstr(ARG(0)), a, sizeof(a));
    win_path_to_host(gstr(ARG(1)), b, sizeof(b));
    make_parents(b);
    int ok = rename(a, b) == 0;
    /* The game moves its new save (temp.svg, temp.psw) over the previous one. A
     * POSIX rename replaces the target; the Switch's SD card file system does not
     * (EEXIST), and the failed move made the game report a full disk and drop out
     * at the end of a level: the target goes first, then the move again. */
    if (!ok && errno == EEXIST && remove(b) == 0)
        ok = rename(a, b) == 0;
    if (!ok)
        rt_log("MoveFileA(%s, %s): %s", a, b, strerror(errno));
    stdcall_ret(c, ok, 2);
}

SHIM(kernel32, CopyFileA)
{
    char a[1100], b[1100];
    win_path_to_host(gstr(ARG(0)), a, sizeof(a));
    win_path_to_host(gstr(ARG(1)), b, sizeof(b));
    make_parents(b);
    int ok = 0;
    if (!(ARG(2) && access(b, F_OK) == 0)) {
        FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL;
        if (in && out) {
            char buf[65536];
            size_t n;
            ok = 1;
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
                if (fwrite(buf, 1, n, out) != n)
                    ok = 0;
        }
        if (in)
            fclose(in);
        if (out)
            fclose(out);
    }
    stdcall_ret(c, (uint32_t)ok, 3);
}

SHIM(kernel32, SetFileAttributesA) { stdcall_ret(c, 1, 2); }

SHIM(kernel32, GetFileSize)
{
    HandleObj *o = handle_get(ARG(0), H_FILE);
    struct stat st;
    if (!o || fstat(o->fd, &st) != 0) {
        stdcall_ret(c, 0xFFFFFFFFu, 2);
        return;
    }
    if (ARG(1))
        WR32(ARG(1), 0);
    stdcall_ret(c, (uint32_t)st.st_size, 2);
}

/* The game's current directory is its install directory. */
SHIM(kernel32, GetCurrentDirectoryA)
{
    const char *dir = WIN_INSTALL_DIR;
    uint32_t n = (uint32_t)strlen(dir);
    if (ARG(0) > n) {
        memcpy(gbuf(ARG(1)), dir, n + 1u);
        stdcall_ret(c, n, 2);
    } else {
        stdcall_ret(c, n + 1u, 2);
    }
}

SHIM(kernel32, SetCurrentDirectoryA)
{
    rt_log("SetCurrentDirectoryA(%s)", gstr(ARG(0)));
    stdcall_ret(c, 1, 1);
}

/* GetDiskFreeSpaceExA(dir, &avail, &total, &free): plenty. */
SHIM(kernel32, GetDiskFreeSpaceExA)
{
    for (int i = 1; i <= 3; i++)
        if (ARG(i))
            WR64(ARG(i), 4ull << 30);
    stdcall_ret(c, 1, 4);
}

/* ---- file times ----------------------------------------------------------------- */

#define FT_UNIX_EPOCH 116444736000000000ull   /* 1970-01-01 in 100 ns units since 1601 */

SHIM(kernel32, GetSystemTimeAsFileTime)
{
    WR64(ARG(0), (uint64_t)time(NULL) * 10000000ull + FT_UNIX_EPOCH);
    stdcall_ret(c, 0, 1);
}

SHIM(kernel32, FileTimeToLocalFileTime)
{
    WR64(ARG(1), RD64(ARG(0)));
    stdcall_ret(c, 1, 2);
}

static void ft_to_tm(uint64_t ft, struct tm *tm, uint32_t *ms)
{
    time_t t = (time_t)((ft - FT_UNIX_EPOCH) / 10000000ull);
    gmtime_r(&t, tm);
    if (ms)
        *ms = (uint32_t)((ft / 10000ull) % 1000ull);
}

/* Days from 1970-01-01 to a civil date (no timegm in the Vita's newlib). */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static uint64_t tm_to_ft(struct tm *tm)
{
    int64_t days = days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    int64_t secs = days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
    return (uint64_t)secs * 10000000ull + FT_UNIX_EPOCH;
}

SHIM(kernel32, FileTimeToSystemTime)
{
    struct tm tm;
    uint32_t ms;
    ft_to_tm(RD64(ARG(0)), &tm, &ms);
    uint32_t st = ARG(1);
    WR16(st + 0u, (uint32_t)tm.tm_year + 1900u);
    WR16(st + 2u, (uint32_t)tm.tm_mon + 1u);
    WR16(st + 4u, (uint32_t)tm.tm_wday);
    WR16(st + 6u, (uint32_t)tm.tm_mday);
    WR16(st + 8u, (uint32_t)tm.tm_hour);
    WR16(st + 10u, (uint32_t)tm.tm_min);
    WR16(st + 12u, (uint32_t)tm.tm_sec);
    WR16(st + 14u, ms);
    stdcall_ret(c, 1, 2);
}

/* FileTimeToDosDateTime(&ft, &date, &time) */
SHIM(kernel32, FileTimeToDosDateTime)
{
    struct tm tm;
    ft_to_tm(RD64(ARG(0)), &tm, NULL);
    WR16(ARG(1), ((uint32_t)(tm.tm_year - 80) << 9) | ((uint32_t)(tm.tm_mon + 1) << 5) | (uint32_t)tm.tm_mday);
    WR16(ARG(2), ((uint32_t)tm.tm_hour << 11) | ((uint32_t)tm.tm_min << 5) | ((uint32_t)tm.tm_sec / 2u));
    stdcall_ret(c, 1, 3);
}

/* DosDateTimeToFileTime(date, time, &ft) */
SHIM(kernel32, DosDateTimeToFileTime)
{
    uint32_t d = ARG(0) & 0xFFFFu, t = ARG(1) & 0xFFFFu;
    struct tm tm = { 0 };
    tm.tm_year = (int)(d >> 9) + 80;
    tm.tm_mon = (int)((d >> 5) & 15u) - 1;
    tm.tm_mday = (int)(d & 31u);
    tm.tm_hour = (int)(t >> 11);
    tm.tm_min = (int)((t >> 5) & 63u);
    tm.tm_sec = (int)(t & 31u) * 2;
    WR64(ARG(2), tm_to_ft(&tm));
    stdcall_ret(c, 1, 3);
}

/* ---- process and memory ---------------------------------------------------------- */

SHIM(kernel32, IsDebuggerPresent) { stdcall_ret(c, 0, 0); }
SHIM(kernel32, GetProcessHeap) { stdcall_ret(c, 0x00500000u, 0); }

SHIM(kernel32, IsBadReadPtr)
{
    uint32_t p = ARG(0), n = ARG(1);
    stdcall_ret(c, !(p >= 0x10000u && p + n >= p && p + n <= g_guest_mem_size), 2);
}

SHIM(kernel32, LocalFree)
{
    if (ARG(0))
        heap_free(ARG(0));
    stdcall_ret(c, 0, 1);
}

/* VirtualQuery(address, MEMORY_BASIC_INFORMATION *, size): everything is committed. */
SHIM(kernel32, VirtualQuery)
{
    uint32_t a = ARG(0), mbi = ARG(1);
    memset(g_mem + mbi, 0, 28);
    WR32(mbi + 0u, a & ~0xFFFu);                 /* BaseAddress */
    WR32(mbi + 4u, GUEST_IMAGE_BASE);            /* AllocationBase */
    WR32(mbi + 8u, 0x04u);                       /* AllocationProtect: PAGE_READWRITE */
    WR32(mbi + 12u, 0x1000u);                    /* RegionSize */
    WR32(mbi + 16u, 0x1000u);                    /* State: MEM_COMMIT */
    WR32(mbi + 20u, 0x04u);                      /* Protect */
    WR32(mbi + 24u, 0x20000u);                   /* Type: MEM_PRIVATE */
    stdcall_ret(c, 28, 3);
}

SHIM(kernel32, OutputDebugStringW)
{
    char buf[512];
    uint32_t s = ARG(0);
    size_t i;
    for (i = 0; s && i + 1 < sizeof(buf) && RD16(s + 2u * i); i++)
        buf[i] = (char)RD16(s + 2u * i);
    buf[i] = 0;
    rt_log("debug: %s", buf);
    stdcall_ret(c, 0, 1);
}

/* ExpandEnvironmentStringsA(src, dst, size): %USERPROFILE% and friends become
 * the install directory. */
SHIM(kernel32, ExpandEnvironmentStringsA)
{
    const char *src = gstr(ARG(0));
    char out[1024];
    size_t n = 0;
    for (const char *p = src; *p && n + 1 < sizeof(out);) {
        if (*p == '%') {
            const char *e = strchr(p + 1, '%');
            if (e) {
                const char *val = WIN_INSTALL_DIR;
                size_t vl = strlen(val);
                if (n + vl < sizeof(out)) {
                    memcpy(out + n, val, vl);
                    n += vl;
                }
                p = e + 1;
                continue;
            }
        }
        out[n++] = *p++;
    }
    out[n] = 0;
    uint32_t need = (uint32_t)n + 1u;
    if (ARG(2) >= need)
        memcpy(gbuf(ARG(1)), out, need);
    stdcall_ret(c, need, 3);
}

/* FormatMessageA(flags, source, id, language, buffer, size, args) */
SHIM(kernel32, FormatMessageA)
{
    uint32_t flags = ARG(0), id = ARG(2), buf = ARG(4), size = ARG(5);
    char msg[64];
    snprintf(msg, sizeof(msg), "Error %u.\r\n", id);
    uint32_t n = (uint32_t)strlen(msg);
    if (flags & 0x100u) {                        /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        uint32_t p = heap_alloc(n + 1u);
        memcpy(g_mem + p, msg, n + 1u);
        WR32(buf, p);
    } else {
        if (size <= n)
            n = size ? size - 1u : 0u;
        memcpy(gbuf(buf), msg, n);
        if (size)
            WR8(buf + n, 0);
    }
    stdcall_ret(c, n, 7);
}

/* ---- advapi32: no registry, no security tokens ------------------------------------- */

SHIM(advapi32, RegOpenKeyExA)
{
    rt_log("RegOpenKeyExA(%08X, %s) -> not found", ARG(0), ARG(1) ? gstr(ARG(1)) : "");
    stdcall_ret(c, ERROR_FILE_NOT_FOUND, 5);
}

SHIM(advapi32, RegQueryValueExA) { stdcall_ret(c, ERROR_FILE_NOT_FOUND, 6); }
SHIM(advapi32, RegCloseKey) { stdcall_ret(c, 0, 1); }
SHIM(advapi32, OpenProcessToken) { stdcall_ret(c, 0, 3); }
SHIM(advapi32, OpenThreadToken) { stdcall_ret(c, 0, 4); }
SHIM(advapi32, GetTokenInformation) { stdcall_ret(c, 0, 5); }
SHIM(advapi32, LookupAccountSidA) { stdcall_ret(c, 0, 7); }
