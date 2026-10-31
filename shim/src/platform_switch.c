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
 * invert_y=1, touch=0, gyro=0|1, gyro_ads=1 (gyro only while aiming with ZL),
 * gyro_yaw / gyro_roll / gyro_pitch=<signed counts per degree> (defaults -10 / -12 / -10),
 * gyro_toggle=0 (R3, a click of the right stick, switches the gyro on and off),
 * gyro_debug=1 (logs the raw axes). */

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

/* Gyro: the angular velocity is integrated over every sensor sample of the frame
 * (the lifo holds 17) and turned into mouse counts, added to the right stick's.
 * Sensor Y (tilting the device) turns, as does Z (sweeping the whole controller:
 * the two gestures players make), X (nodding) pitches. Each has a signed
 * counts-per-degree gain in the cfg, so a wrong direction is a minus sign. */
static int gyro_on = 1, gyro_ads, gyro_debug, gyro_toggle = 1;
static float gyro_yaw = -10.0f, gyro_roll = -12.0f, gyro_pitch = -10.0f;
static HidSixAxisSensorHandle sx_handheld, sx_full, sx_dual_l, sx_dual_r;

static void controls_init(void)
{
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&sx_handheld, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld)))
        hidStartSixAxisSensor(sx_handheld);
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&sx_full, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey)))
        hidStartSixAxisSensor(sx_full);
    HidSixAxisSensorHandle dual[2];
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(dual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual))) {
        sx_dual_l = dual[0];
        sx_dual_r = dual[1];
        hidStartSixAxisSensor(sx_dual_l);
        hidStartSixAxisSensor(sx_dual_r);
    }
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
            else if (!strncmp(line, "gyro=", 5))
                gyro_on = line[5] != '0';
            else if (!strncmp(line, "gyro_toggle=", 12))
                gyro_toggle = line[12] != '0';
            else if (!strncmp(line, "gyro_ads=", 9))
                gyro_ads = line[9] == '1';
            else if (!strncmp(line, "gyro_debug=", 11))
                gyro_debug = line[11] == '1';
            else if (!strncmp(line, "gyro_yaw=", 9))
                gyro_yaw = strtof(line + 9, NULL);
            else if (!strncmp(line, "gyro_roll=", 10))
                gyro_roll = strtof(line + 10, NULL);
            else if (!strncmp(line, "gyro_pitch=", 11))
                gyro_pitch = strtof(line + 11, NULL);
        }
        fclose(f);
    }
    if (!(stick_sensitivity >= 0.1f && stick_sensitivity <= 10.0f))
        stick_sensitivity = 1.0f;
    rt_log("controls: right stick x%.2f%s, touch %s", (double)stick_sensitivity, invert_y ? ", inverted" : "",
           touch_on ? "on" : "off");
    rt_log("controls: gyro %s%s, yaw %.1f roll %.1f pitch %.1f", gyro_on ? "on" : "off", gyro_ads ? " (aiming only)" : "",
           (double)gyro_yaw, (double)gyro_roll, (double)gyro_pitch);
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

/* The sensor to read: a paired controller wins over the console's own (a Pro
 * Controller on an undocked console also reports the handheld style). */
static HidSixAxisSensorHandle gyro_handle(void)
{
    u32 styles = padGetStyleSet(&pad);
    if (styles & HidNpadStyleTag_NpadFullKey)
        return sx_full;
    if (styles & HidNpadStyleTag_NpadJoyDual)
        return sx_dual_r;
    return sx_handheld;
}

/* Gyro look: mouse counts from every sensor sample not yet seen. */
static void gyro_poll(int aiming)
{
    static HidSixAxisSensorHandle cur;
    static uint64_t last_sample;
    static float acc_x, acc_y;
    static unsigned dbg_n;
    HidSixAxisSensorHandle h = gyro_handle();
    HidSixAxisSensorState st[17];
    size_t n = hidGetSixAxisSensorStates(h, st, 17);   /* newest first */
    if (h.type_value != cur.type_value) {
        cur = h;
        last_sample = n ? st[0].sampling_number : 0;   /* a new sensor: start clean */
        return;
    }
    float yaw = 0.0f, roll = 0.0f, pitch = 0.0f;
    for (size_t i = n; i-- > 0;) {
        if (st[i].sampling_number <= last_sample)
            continue;
        last_sample = st[i].sampling_number;
        /* angular_velocity is in turns per second; delta_time is in ns. */
        float dt = (float)st[i].delta_time * 1e-9f;
        if (dt <= 0.0f || dt > 0.1f)
            continue;
        float ax = st[i].angular_velocity.x, ay = st[i].angular_velocity.y, az = st[i].angular_velocity.z;
        if (gyro_debug && ++dbg_n % 200 == 0)
            rt_log("gyro raw x %.3f y %.3f z %.3f", (double)ax, (double)ay, (double)az);
        /* about a degree per second of noise is ignored */
        if (ax > -0.003f && ax < 0.003f) ax = 0.0f;
        if (ay > -0.003f && ay < 0.003f) ay = 0.0f;
        if (az > -0.003f && az < 0.003f) az = 0.0f;
        pitch += ax * dt * 360.0f;
        yaw += ay * dt * 360.0f;
        roll += az * dt * 360.0f;
    }
    if (!gyro_on || (gyro_ads && !aiming) || !pad_in_gameplay()) {
        acc_x = acc_y = 0.0f;
        return;
    }
    acc_x += yaw * gyro_yaw + roll * gyro_roll;
    acc_y += pitch * gyro_pitch * (invert_y ? -1.0f : 1.0f);
    int32_t mx = (int32_t)acc_x, my = (int32_t)acc_y;
    acc_x -= (float)mx;
    acc_y -= (float)my;
    mouse_motion(mx, my);
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
    if (gyro_toggle && (buttons & HidNpadButton_StickR) && !(prev_buttons & HidNpadButton_StickR)) {
        gyro_on = !gyro_on;
        rt_log("gyro %s", gyro_on ? "on" : "off");
    }
    prev_buttons = buttons;

    /* Left stick: four controls, one per direction. */
    HidAnalogStickState ls = padGetStickPos(&pad, 0), rs = padGetStickPos(&pad, 1);
    int lx = axis(ls.x, 0), ly = axis(ls.y, 1);
    uint8_t want[4] = { ly < -40, ly > 40, lx < -40, lx > 40 };
    for (int i = 0; i < 4; i++) {
        if (want[i] != stick_held[i]) {
            stick_held[i] = want[i];
            pad_control(stick_controls[i], want[i]);
        }
    }

    if (touch_on)
        touch_poll();
    gyro_poll((buttons & HidNpadButton_ZL) != 0);

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
