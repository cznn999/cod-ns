/* Nintendo Switch implementation of the platform layer: SDL2 (switch-sdl2) for
 * the window, the OpenGL context (EGL on switch-mesa) and audio; libnx's pad and
 * touch screen sent as the game's key and mouse input, as on the Vita.
 *
 * Files live in sdmc:/switch/cod1/ (runtime/src/main.c): the game in pc/, the
 * log, args.txt (command line), env.txt (diagnostics) and cod1.cfg (controls). */
#if defined(__SWITCH__)
#include "platform.h"
#include "bench.h"
#include "gl_switch.h"
#include "pad_input.h"
#include "runtime.h"

#include <SDL.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <switch.h>
#include <unistd.h>

#define COD_SWITCH_CFG "sdmc:/switch/cod1/cod1" COD_SW_TAG ".cfg"

/* The touch screen's resolution (handheld). */
#define TOUCH_W 1280
#define TOUCH_H 720

static SDL_Window *window;
static SDL_GLContext gl;
static int mode_w = 640, mode_h = 480;
static int gl_loaded;

/* ---- libc ----------------------------------------------------------------
 * newlib declares pread but libnx does not provide it. Every caller reads from a
 * descriptor of its own (kernel32.c: the read-ahead thread opens the file again;
 * MapViewOfFile: the mapping's descriptor), so seeking there is safe; the old
 * position is put back all the same, under one lock. */
ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    ssize_t r = -1;
    off_t old = lseek(fd, 0, SEEK_CUR);
    if (old >= 0 && lseek(fd, offset, SEEK_SET) == offset) {
        r = read(fd, buf, count);
        int err = errno;
        lseek(fd, old, SEEK_SET);
        errno = err;
    }
    pthread_mutex_unlock(&lock);
    return r;
}

/* ---- window ------------------------------------------------------------- */

void platform_init(void)
{
    /* No SDL joystick subsystem: the pad is read through libnx below. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0)
        rt_fatal(NULL, "SDL_Init: %s", SDL_GetError());
    rt_log("switch: %s mode", appletGetOperationMode() == AppletOperationMode_Handheld ? "handheld" : "docked");
}

void platform_create_window(int w, int h, const char *title)
{
    if (window)
        return;
    mode_w = w;
    mode_h = h;
    /* The renderer is the PC's fixed-function OpenGL 1.x one: a compatibility
     * context from Mesa's desktop GL (not GLES). */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /* 1280x720 handheld, 1920x1080 docked. RESIZABLE makes SDL follow the dock
     * (it resizes the window when the mode changes); the game's viewports are
     * scaled to the drawable (opengl32.c), so its resolution can stay as it is. */
    int docked = appletGetOperationMode() != AppletOperationMode_Handheld;
    window = SDL_CreateWindow(title, 0, 0, docked ? 1920 : 1280, docked ? 1080 : 720,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window)
        rt_fatal(NULL, "SDL_CreateWindow: %s", SDL_GetError());
}

void platform_show_window(int show) { (void)show; }
void platform_set_title(const char *title) { (void)title; }

void platform_set_video_mode(int w, int h)
{
    if (!window)
        platform_create_window(w, h, "Call of Duty");
    mode_w = w;
    mode_h = h;
    if (!gl) {
        gl = SDL_GL_CreateContext(window);
        if (!gl)
            rt_fatal(NULL, "SDL_GL_CreateContext: %s", SDL_GetError());
        const char *vs = getenv("COD_VSYNC");
        SDL_GL_SetSwapInterval(vs && vs[0] == '0' ? 0 : 1);
    }
}

void platform_get_video_mode(int *w, int *h)
{
    *w = mode_w;
    *h = mode_h;
}

void platform_get_drawable_size(int *w, int *h)
{
    SDL_GL_GetDrawableSize(window, w, h);
}

void platform_swap(void)
{
    /* Frame rate in the log every 10 seconds (performance reports). */
    static Uint64 last, freq;
    static unsigned frames;
    if (!freq) {
        freq = SDL_GetPerformanceFrequency();
        last = SDL_GetPerformanceCounter();
    }
    frames++;
    Uint64 now = SDL_GetPerformanceCounter();
    if (now - last >= 10u * freq) {
        double secs = (double)(now - last) / (double)freq;
        int dw, dh;
        SDL_GL_GetDrawableSize(window, &dw, &dh);
        rt_log("fps %.1f (%.1f ms), game %dx%d, screen %dx%d", frames / secs, secs * 1000.0 / frames, mode_w, mode_h,
               dw, dh);
        frames = 0;
        last = now;
    }
    SDL_GL_SwapWindow(window);
}

void *platform_gl_proc(const char *name)
{
    return SDL_GL_GetProcAddress(name);
}

void platform_gl_init(void)
{
    if (SDL_GL_MakeCurrent(window, gl) != 0)
        rt_fatal(NULL, "SDL_GL_MakeCurrent: %s", SDL_GetError());
    if (!gl_loaded) {
        gl_loaded = 1;
        int missing = gl_switch_load(platform_gl_proc);
        const GLubyte *(APIENTRYP get)(GLenum) = sw_glGetString;
        if (get)
            rt_log("GL: %s / %s / %s", (const char *)get(GL_VENDOR), (const char *)get(GL_RENDERER),
                   (const char *)get(GL_VERSION));
        if (missing)
            rt_fatal(NULL, "OpenGL: %d functions missing (see the log)", missing);
    }
}

void platform_gl_release(void)
{
    SDL_GL_MakeCurrent(window, NULL);
}

void platform_set_swap_interval(int on)
{
    SDL_GL_SetSwapInterval(on ? 1 : 0);
}

void platform_service_main(void) {}

void platform_show_cursor(int show) { (void)show; }
void platform_capture_mouse(int on) { (void)on; }

/* Stop the audio thread (it reads guest memory) before the process goes away. */
void platform_shutdown(void)
{
    rt_log_flush();
    render_shutdown();
    SDL_Quit();
}

/* Closing the app from the HOME menu asks the game to quit (WM_CLOSE); it may
 * not act on it (loading), so the process ends shortly after anyway. */
static int quit_watchdog(void *unused)
{
    (void)unused;
    SDL_Delay(1500);
    rt_log("app closed: ending the process");
    rt_log_flush();
    exit(0);
}

static void quit_requested(void)
{
    static int started;
    if (started)
        return;
    started = 1;
    user32_post_message(0x0010, 0, 0);   /* WM_CLOSE */
    SDL_DetachThread(SDL_CreateThread(quit_watchdog, "cod_quit", NULL));
}

/* ---- controls -----------------------------------------------------------
 * The Vita's layout (shim/src/pad_input.c) on the Switch's buttons:
 *
 *   left stick  move                  right stick  look / aim
 *   ZR  fire                          ZL  aim down the sights
 *   R   next weapon                   L   melee
 *   A   stand up / jump (menus: OK)   B   crouch (menus: back)
 *   Y   reload                        X   use
 *   D-pad up / down  grenade / prone  D-pad left / right  lean
 *   -   objectives                    +   menu
 *   touch screen (handheld): menus cursor and click, in play a drag looks around
 *
 * sdmc:/switch/cod1/cod1.cfg: stick_sensitivity=<multiplier> (default 1.0),
 * invert_y=1, touch=0, analog_move=0|1 (proportional left stick), move_curve /
 * strafe_curve=<exponent> (defaults 1.6 / 2.0). */

static const struct {
    uint64_t button;
    PadControl control;
} button_map[] = {
    { HidNpadButton_A, PAD_CROSS },
    { HidNpadButton_B, PAD_CIRCLE },
    { HidNpadButton_Y, PAD_SQUARE },
    { HidNpadButton_X, PAD_TRIANGLE },
    { HidNpadButton_ZL, PAD_L },
    { HidNpadButton_ZR, PAD_R },
    { HidNpadButton_R, PAD_REAR_UP },
    { HidNpadButton_L, PAD_REAR_DOWN },
    { HidNpadButton_Minus, PAD_SELECT },
    { HidNpadButton_Plus, PAD_START },
    { HidNpadButton_Up, PAD_DPAD_UP },
    { HidNpadButton_Down, PAD_DPAD_DOWN },
    { HidNpadButton_Left, PAD_DPAD_LEFT },
    { HidNpadButton_Right, PAD_DPAD_RIGHT },
};
#define N_BUTTONS (sizeof(button_map) / sizeof(button_map[0]))

static const PadControl stick_controls[4] = { PAD_LSTICK_UP, PAD_LSTICK_DOWN, PAD_LSTICK_LEFT, PAD_LSTICK_RIGHT };

static PadState pad;
static uint64_t prev_buttons;
static uint8_t stick_held[4];
static float stick_sensitivity = 1.0f;
static int invert_y, touch_on = 1;

/* Analog movement. The engine only knows keys, but like Quake 3's it turns a
 * key into a speed by how much of the frame it was held: CL_KeyState returns
 * kbutton.msec / frame_msec, clamped to 0..1, and CL_KeyMove scales that to
 * +-127. So the left stick, in play, does not press W A S D: the port writes
 * the stick's value as the milliseconds the four kbuttons were "held" this
 * frame, which the engine then reads as a partial key. The stick's length goes
 * through move_curve (heading unchanged); the sideways part is eased by
 * strafe_curve on its own so forward stays dominant and diagonals arrive
 * gradually. cod1.cfg: analog_move=0, move_curve=<n>, strafe_curve=<n>. */
static int analog_move = 1;
static float move_curve = 1.6f, strafe_curve = 2.0f;

static void controls_init(void)
{
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    FILE *f = fopen(COD_SWITCH_CFG, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "stick_sensitivity=", 18))
                stick_sensitivity = strtof(line + 18, NULL);
            else if (!strncmp(line, "invert_y=", 9))
                invert_y = line[9] == '1';
            else if (!strncmp(line, "touch=", 6))
                touch_on = line[6] != '0';
            else if (!strncmp(line, "analog_move=", 12))
                analog_move = line[12] != '0';
            else if (!strncmp(line, "move_curve=", 11))
                move_curve = strtof(line + 11, NULL);
            else if (!strncmp(line, "strafe_curve=", 13))
                strafe_curve = strtof(line + 13, NULL);
        }
        fclose(f);
    }
    if (!(stick_sensitivity >= 0.1f && stick_sensitivity <= 10.0f))
        stick_sensitivity = 1.0f;
    if (!(move_curve >= 0.5f && move_curve <= 4.0f))
        move_curve = 1.6f;
    if (!(strafe_curve >= 0.5f && strafe_curve <= 4.0f))
        strafe_curve = 2.0f;
    rt_log("controls: analog move %s, curves %.2f / %.2f", analog_move ? "on" : "off", (double)move_curve,
           (double)strafe_curve);
    rt_log("controls: right stick x%.2f%s, touch %s", (double)stick_sensitivity, invert_y ? ", inverted" : "",
           touch_on ? "on" : "off");
}

/* Stick axis (libnx: -32767..32767, up positive) -> about -100..100 with the
 * Vita's sign (down positive), 0 inside the deadzone. */
#define DEADZONE 15
static int axis(int32_t v, int flip)
{
    int n = (int)((int64_t)v * 100 / 32767);
    if (flip)
        n = -n;
    if (n > -DEADZONE && n < DEADZONE)
        return 0;
    return n;
}

static void post_mouse_move(void)
{
    user32_post_message(0x0200, 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
}

/* Relative mouse motion (looking), the cursor kept on the screen. */
static void mouse_motion(int32_t mx, int32_t my)
{
    if (!mx && !my)
        return;
    g_mouse.dx += mx;
    g_mouse.dy += my;
    g_mouse.x += mx;
    g_mouse.y += my;
    if (g_mouse.x < 0) g_mouse.x = 0;
    if (g_mouse.y < 0) g_mouse.y = 0;
    if (g_mouse.x >= mode_w) g_mouse.x = mode_w - 1;
    if (g_mouse.y >= mode_h) g_mouse.y = mode_h - 1;
    post_mouse_move();
}

/* Right stick deflection -> mouse counts per second, as on the Vita: linear in
 * play (the game's own sensitivity applies on top), a fixed slow cursor in the menus. */
#define LOOK_PER_UNIT 9.0f
#define MENU_CURSOR_MAX 650.0f
static float look_speed(int v)
{
    if (!v)
        return 0.0f;
    if (pad_in_gameplay())
        return (float)v * LOOK_PER_UNIT * stick_sensitivity;
    float n = (float)((v < 0 ? -v : v) - DEADZONE) / (float)(100 - DEADZONE);
    if (n > 1.0f)
        n = 1.0f;
    float s = MENU_CURSOR_MAX * (0.25f * n + 0.75f * n * n);
    return v < 0 ? -s : s;
}

/* Touch screen (handheld only): in the menus the cursor goes where the finger is
 * and a tap clicks; in play a drag turns the view like the mouse. */
#define TOUCH_LOOK_SCALE 0.75f
static void touch_poll(void)
{
    static int active, menu, clicking, last_x, last_y;
    static float acc_x, acc_y;
    HidTouchScreenState st;
    int down = hidGetTouchScreenStates(&st, 1) > 0 && st.count > 0;
    if (!down) {
        if (clicking)
            pad_mouse_button(0, 0);
        active = clicking = 0;
        return;
    }
    int x = (int)st.touches[0].x, y = (int)st.touches[0].y;
    if (!active) {
        active = 1;
        menu = !pad_in_gameplay();
        last_x = x;
        last_y = y;
        acc_x = acc_y = 0.0f;
        if (!menu)
            return;
    }
    if (menu) {
        int32_t gx = (int32_t)((int64_t)x * mode_w / TOUCH_W), gy = (int32_t)((int64_t)y * mode_h / TOUCH_H);
        if (gx >= mode_w) gx = mode_w - 1;
        if (gy >= mode_h) gy = mode_h - 1;
        if (gx != g_mouse.x || gy != g_mouse.y) {
            g_mouse.dx += gx - g_mouse.x;
            g_mouse.dy += gy - g_mouse.y;
            g_mouse.x = gx;
            g_mouse.y = gy;
            post_mouse_move();
        } else if (!clicking && !pad_capturing_key()) {
            clicking = 1;
            pad_mouse_button(0, 1);
        }
        return;
    }
    acc_x += (float)(x - last_x) * TOUCH_LOOK_SCALE;
    acc_y += (float)(y - last_y) * TOUCH_LOOK_SCALE;
    last_x = x;
    last_y = y;
    int32_t mx = (int32_t)acc_x, my = (int32_t)acc_y;
    acc_x -= (float)mx;
    acc_y -= (float)my;
    mouse_motion(mx, my);
}

/* Left stick -> the four movement kbuttons' held time (see analog_move). The
 * stick is read radially: its length is the pace, its direction the heading. */
static void analog_move_poll(HidAnalogStickState ls)
{
    float x = (float)ls.x / 32767.0f, y = (float)ls.y / 32767.0f;   /* up is positive */
    float m = sqrtf(x * x + y * y), fwd = 0.0f, side = 0.0f;
    const float dead = 0.15f, full = 0.95f;
    if (m > dead) {
        float pace = (m - dead) / (full - dead);
        if (pace > 1.0f)
            pace = 1.0f;
        pace = powf(pace, move_curve);
        fwd = pace * y / m;
        side = pace * x / m;
        float s = fabsf(side);
        side = copysignf(powf(s, strafe_curve), side);
    }
    uint32_t frame = RD32(COD_FRAME_MSEC);   /* the last frame's: the next is about the same */
    if (frame < 1u || frame > 200u)
        frame = 33u;
    float f = (float)frame;
    WR32(COD_KB_FORWARD + 0xCu, fwd > 0.0f ? (uint32_t)(fwd * f + 0.5f) : 0u);
    WR32(COD_KB_BACK + 0xCu, fwd < 0.0f ? (uint32_t)(-fwd * f + 0.5f) : 0u);
    WR32(COD_KB_RIGHT + 0xCu, side > 0.0f ? (uint32_t)(side * f + 0.5f) : 0u);
    WR32(COD_KB_LEFT + 0xCu, side < 0.0f ? (uint32_t)(-side * f + 0.5f) : 0u);
}

static void pad_poll(void)
{
    static uint64_t last_look;
    static int ready;
    if (!ready) {
        ready = 1;
        controls_init();
    }
    padUpdate(&pad);
    uint64_t buttons = padGetButtons(&pad);
    for (unsigned i = 0; i < N_BUTTONS; i++) {
        uint64_t b = button_map[i].button;
        int down = (buttons & b) != 0;
        if (down != ((prev_buttons & b) != 0))
            pad_control(button_map[i].control, down);
    }
    prev_buttons = buttons;

    /* Left stick: four controls, one per direction. */
    HidAnalogStickState ls = padGetStickPos(&pad, 0), rs = padGetStickPos(&pad, 1);
    int lx = axis(ls.x, 0), ly = axis(ls.y, 1);
    uint8_t want[4] = { ly < -40, ly > 40, lx < -40, lx > 40 };
    if (analog_move && pad_in_gameplay()) {
        /* in play the stick is analog, not W A S D (the menus keep the arrows) */
        want[0] = want[1] = want[2] = want[3] = 0;
        analog_move_poll(ls);
    }
    for (int i = 0; i < 4; i++) {
        if (want[i] != stick_held[i]) {
            stick_held[i] = want[i];
            pad_control(stick_controls[i], want[i]);
        }
    }

    if (touch_on)
        touch_poll();

    /* Right stick: relative mouse motion, scaled by elapsed time. */
    uint64_t now = armTicksToNs(armGetSystemTick()) / 1000u;
    if (last_look) {
        float dt = (float)(now - last_look) / 1000000.0f;
        if (dt > 0.1f)
            dt = 0.1f;
        int rx = axis(rs.x, 0), ry = axis(rs.y, !invert_y);
        static float acc_x, acc_y;
        acc_x += look_speed(rx) * dt;
        acc_y += look_speed(ry) * dt;
        int32_t mx = (int32_t)acc_x, my = (int32_t)acc_y;
        acc_x -= (float)mx;
        acc_y -= (float)my;
        mouse_motion(mx, my);
    }
    last_look = now;
}

/* ---- events ------------------------------------------------------------- */

void platform_pump(void)
{
    static uint64_t last_pump;
    platform_scripted_input();
    /* The game polls input and messages many times per frame; once a millisecond
     * or two is plenty. */
    uint64_t t = armTicksToNs(armGetSystemTick()) / 1000u;
    if (last_pump && t - last_pump < 2000u)
        return;
    last_pump = t;
    /* SDL's pump runs the applet loop (HOME menu, sleep, docking). */
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            quit_requested();
    }
    if (bench_enabled())
        return;   /* the benchmark drives the game; the pad would disturb it */
    pad_poll();
}

void platform_wait(void)
{
    SDL_Delay(5);
    platform_pump();
}

#endif
