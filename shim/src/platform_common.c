/* Platform-independent input plumbing shared by the SDL and Vita backends. */
#include "pad_input.h"
#include "platform.h"
#include "runtime.h"
#include "shim.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t g_key_state[256];
int g_trace_next_frame;
MouseState g_mouse;

#define KEYQ 256
static KeyEvent keyq[KEYQ];
static unsigned keyq_head, keyq_tail;

void platform_push_key(uint8_t dik, int down)
{
    unsigned next = (keyq_tail + 1u) % KEYQ;
    if (next == keyq_head)
        return;
    keyq[keyq_tail].dik = dik;
    keyq[keyq_tail].down = (uint8_t)down;
    keyq[keyq_tail].time = win32_tick_count();
    keyq_tail = next;
}

int platform_pop_key_event(KeyEvent *out)
{
    if (keyq_head == keyq_tail)
        return 0;
    *out = keyq[keyq_head];
    keyq_head = (keyq_head + 1u) % KEYQ;
    return 1;
}

/* Press or release a key the way the real keyboard does: the Win32 key messages
 * (WM_KEYDOWN / WM_KEYUP with the virtual key and the scan code; DIK codes are scan
 * codes, 0x80 the extended-key bit) and WM_CHAR for Enter and Esc, which the
 * menus' text fields read. */
uint32_t user32_dik_to_vk(uint8_t dik);

void platform_key(uint8_t dik, int down)
{
    g_key_state[dik] = down ? 0x80 : 0;
    platform_push_key(dik, down);
    uint32_t lparam = 1u | ((uint32_t)(dik & 0x7F) << 16) | ((dik & 0x80) ? 0x01000000u : 0u) |
                      (down ? 0u : 0xC0000000u);
    uint32_t vk = user32_dik_to_vk(dik);
    user32_post_message(down ? 0x0100u : 0x0101u, vk, lparam);
    if (down && (vk == 0x0Du || vk == 0x1Bu))
        user32_post_message(0x0102, vk, lparam);   /* WM_CHAR */
}

/* COD_INPUT="ms:action,ms:action,...": scripted input for automated testing.
 * Actions: a DIK code in hex (tap), "mx+N"/"my+N" relative mouse motion, "lmb" click,
 * "pad+N"/"pad-N" press/release pad control N (hex, PadControl in pad_input.h) the
 * way the Vita's pad does (shim/src/pad_input.c), "key+K"/"key-K" hold/release DIK
 * code K (hex), "diag=N" renderer diagnostic mode N, "shot=NAME" a screenshot of
 * the next frame named NAME (Vita: captured from the Vita3K window by
 * tools/qa_vita3k.sh), "dump=NAME" the executable's data section into NAME.bin
 * (finding game state by comparing dumps), "profreset" restart the profile (COD_PROFILE builds),
 * "quit" end the process (the profile written first). */
void platform_scripted_input(void)
{
    static const char *script;
    static int init;
    static Uint32 t0;
    if (!init) {
        init = 1;
        script = getenv("COD_INPUT");
#if defined(__vita__) || defined(__SWITCH__)
        /* No environment on the consoles: read the script from a file. */
        static char buf[4096];
#if defined(__vita__)
        FILE *f = fopen("ux0:data/cod1/input.txt", "r");
#else
        FILE *f = fopen("sdmc:/switch/cod1/input" COD_SW_TAG ".txt", "r");
#endif
        if (f) {
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            fclose(f);
            script = buf;
        }
#endif
        t0 = win32_tick_count();
    }
    pad_tick();
    static uint8_t release_dik;
    static int release_lmb;
    static Uint32 release_at;
    /* Nothing scripted (left): no clock query. Called on every message poll, at a
     * rate that follows real time, so a query here would make COD_FAKE_CLOCK runs
     * depend on real time (and costs a system call on the Vita). */
    if ((!script || !*script) && !release_dik && !release_lmb)
        return;
    Uint32 now = win32_tick_count() - t0;
    static Uint32 dbg;
    static int input_debug = -1;
    if (input_debug < 0)
        input_debug = getenv("COD_INPUT_DEBUG") != NULL;
    if (input_debug && now / 2000u != dbg) {
        dbg = now / 2000u;
        rt_log("[input-debug] now %u rel %u/%d at %u next '%.12s'", now, release_dik, release_lmb, release_at,
               script ? script : "(end)");
    }
    if ((release_dik || release_lmb) && now >= release_at) {
        if (release_dik) {
            platform_key(release_dik, 0);
            release_dik = 0;
        }
        if (release_lmb) {
            g_mouse.buttons[0] = 0;
            user32_post_message(0x0202, 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
            release_lmb = 0;
        }
    }
    while (script && *script && !release_dik && !release_lmb) {
        unsigned long at = strtoul(script, NULL, 10);
        if (now < at)
            return;
        const char *act = strchr(script, ':');
        if (!act)
            break;
        act++;
        const char *end = strchr(act, ',');
        size_t n = end ? (size_t)(end - act) : strlen(act);
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*s", (int)n, act);
        rt_log("[input] t=%lu %s", at, buf);
        if (!strncmp(buf, "diag=", 5) || !strncmp(buf, "shot=", 5) || !strcmp(buf, "quit") ||
            !strcmp(buf, "profreset")) {
            if (buf[0] == 'p') {
#ifdef COD_PROFILE
                rt_prof_reset(0);   /* measure from here (after loading) */
#endif
            } else if (buf[0] == 'd')
                render_diag_set(atoi(buf + 5));
            else if (buf[0] == 's')
                render_shot_request(buf + 5);
            else {
                rt_log("[input] quit");
#ifdef COD_PROFILE
                rt_prof_flush();
#endif
                platform_shutdown();
                exit(0);
            }
            script = end ? end + 1 : NULL;
            continue;
        }
        if (!strncmp(buf, "dump=", 5)) {
            /* The executable's data section (0x74B000..0x8A4600), for finding game
             * state by comparing dumps. */
            char path[64];
            snprintf(path, sizeof(path), "%s.bin", buf + 5);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(g_mem + 0x74B000u, 1, 0x159600u, f);
                fclose(f);
            }
            script = end ? end + 1 : NULL;
            continue;
        }
        if (strcmp(buf, "trace") == 0) {
            g_trace_next_frame = 1;   /* log the next frame's draws and save it */
            script = end ? end + 1 : NULL;
            continue;
        }
        if ((!strncmp(buf, "pad", 3) || !strncmp(buf, "key", 3)) && (buf[3] == '+' || buf[3] == '-')) {
            uint32_t v = (uint32_t)strtoul(buf + 4, NULL, 16);
            if (buf[0] == 'p') {
                pad_control((PadControl)v, buf[3] == '+');
            } else {
                pad_key((uint8_t)v, buf[3] == '+');
            }
            script = end ? end + 1 : NULL;
            continue;
        }
        if (strcmp(buf, "lmb") == 0) {
            g_mouse.buttons[0] = 0x80;
            user32_post_message(0x0201, 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
            release_lmb = 1;
        } else if (buf[0] == 'm' && (buf[1] == 'x' || buf[1] == 'y')) {
            int v = atoi(buf + 2);
            if (buf[1] == 'x') {
                g_mouse.dx += v;
                g_mouse.x += v;
            } else {
                g_mouse.dy += v;
                g_mouse.y += v;
            }
            user32_post_message(0x0200, 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
        } else {
            uint8_t dik = (uint8_t)strtoul(buf, NULL, 16);
            platform_key(dik, 1);
            release_dik = dik;
        }
        release_at = now + 100;
        script = end ? end + 1 : NULL;
    }
}

