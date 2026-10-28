/* Pad controls (see pad_input.h): every control presses what the game's default
 * PC binding of its action is (main/default.cfg), a key, a mouse button or a
 * mouse wheel step:
 *
 *   left stick  move (W A S D)           right stick  look / aim (mouse)
 *   R  fire (left button)                L  aim down the sights (right button)
 *   X  stand up / jump (Space)           O  crouch (C)
 *   [] reload (R)                        /\ use (F)
 *   up  grenade (4)                      down  prone (Ctrl)
 *   left / right  lean (Q / E)
 *   SELECT  objectives (Tab)             START  menu (Esc)
 *   rear touch  upper half next weapon (mouse wheel), lower half melee (Shift)
 *
 * In the menus: D-pad and left stick the arrow keys, X Enter, O and START Esc,
 * R the left mouse button (the right stick and the front panel move the cursor). */
#include "pad_input.h"
#include "platform.h"
#include "runtime.h"

enum { OUT_NONE, OUT_KEY, OUT_MOUSE, OUT_WHEEL };

typedef struct Output {
    uint8_t kind;
    uint8_t code;            /* DIK code, mouse button, or wheel direction (1 up, 2 down) */
    uint8_t kind2, code2;    /* a second output pressed together with the first */
} Output;

#define KEY(k) { OUT_KEY, (k), OUT_NONE, 0 }
#define MOUSE(b) { OUT_MOUSE, (b), OUT_NONE, 0 }

#define DIK_ESCAPE 0x01
#define DIK_4 0x05
#define DIK_TAB 0x0F
#define DIK_Q 0x10
#define DIK_W 0x11
#define DIK_E 0x12
#define DIK_R 0x13
#define DIK_RETURN 0x1C
#define DIK_LCONTROL 0x1D
#define DIK_A 0x1E
#define DIK_S 0x1F
#define DIK_D 0x20
#define DIK_F 0x21
#define DIK_LSHIFT 0x2A
#define DIK_C 0x2E
#define DIK_SPACE 0x39
#define DIK_UP 0xC8
#define DIK_LEFT 0xCB
#define DIK_RIGHT 0xCD
#define DIK_DOWN 0xD0

static const Output control_out[PAD_CONTROL_COUNT] = {
    [PAD_CROSS] = KEY(DIK_SPACE),
    [PAD_CIRCLE] = KEY(DIK_C),
    [PAD_SQUARE] = KEY(DIK_R),
    [PAD_TRIANGLE] = KEY(DIK_F),
    [PAD_L] = MOUSE(1),
    [PAD_R] = MOUSE(0),
    [PAD_SELECT] = KEY(DIK_TAB),
    [PAD_START] = KEY(DIK_ESCAPE),
    [PAD_DPAD_UP] = KEY(DIK_4),
    [PAD_DPAD_DOWN] = KEY(DIK_LCONTROL),
    [PAD_DPAD_LEFT] = KEY(DIK_Q),
    [PAD_DPAD_RIGHT] = KEY(DIK_E),
    [PAD_LSTICK_UP] = KEY(DIK_W),
    [PAD_LSTICK_DOWN] = KEY(DIK_S),
    [PAD_LSTICK_LEFT] = KEY(DIK_A),
    [PAD_LSTICK_RIGHT] = KEY(DIK_D),
    [PAD_REAR_UP] = { OUT_WHEEL, 2, OUT_NONE, 0 },
    [PAD_REAR_DOWN] = KEY(DIK_LSHIFT),
};

static const Output menu_out[PAD_CONTROL_COUNT] = {
    [PAD_CROSS] = KEY(DIK_RETURN),
    [PAD_CIRCLE] = KEY(DIK_ESCAPE),
    [PAD_R] = MOUSE(0),
    [PAD_START] = KEY(DIK_ESCAPE),
    [PAD_DPAD_UP] = KEY(DIK_UP),
    [PAD_DPAD_DOWN] = KEY(DIK_DOWN),
    [PAD_DPAD_LEFT] = KEY(DIK_LEFT),
    [PAD_DPAD_RIGHT] = KEY(DIK_RIGHT),
    [PAD_LSTICK_UP] = KEY(DIK_UP),
    [PAD_LSTICK_DOWN] = KEY(DIK_DOWN),
    [PAD_LSTICK_LEFT] = KEY(DIK_LEFT),
    [PAD_LSTICK_RIGHT] = KEY(DIK_RIGHT),
    [PAD_REAR_UP] = { OUT_WHEEL, 1, OUT_NONE, 0 },
    [PAD_REAR_DOWN] = { OUT_WHEEL, 2, OUT_NONE, 0 },
};

/* ---- output: keys and mouse buttons, reference counted -------------------- */

static uint8_t key_refs[256];
static uint8_t mouse_refs[4];

void pad_key(uint8_t dik, int down)
{
    if (!dik)
        return;
    if (down) {
        if (key_refs[dik]++ == 0u && !g_key_state[dik])
            platform_key(dik, 1);
    } else if (key_refs[dik] && --key_refs[dik] == 0u && g_key_state[dik]) {
        platform_key(dik, 0);
    }
}

void pad_mouse_button(int b, int down)
{
    if (down ? mouse_refs[b]++ != 0u : (!mouse_refs[b] || --mouse_refs[b] != 0u))
        return;
    g_mouse.buttons[b] = down ? 0x80 : 0;
    if (b < 3) {
        static const uint32_t msgs[3][2] = { { 0x0202, 0x0201 }, { 0x0205, 0x0204 }, { 0x0208, 0x0207 } };
        user32_post_message(msgs[b][down ? 1 : 0], 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
    }
}

/* One wheel notch (DirectInput reads it as z motion), on press only. */
static void wheel_step(int up, int down)
{
    if (!down)
        return;
    g_mouse.dz += up ? 120 : -120;
    user32_post_message(0x020A, (uint32_t)(up ? 120 : -120) << 16,
                        ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
}

static void output(uint8_t kind, uint8_t code, int down)
{
    switch (kind) {
    case OUT_KEY: pad_key(code, down); break;
    case OUT_MOUSE: pad_mouse_button(code, down); break;
    case OUT_WHEEL: wheel_step(code == 1, down); break;
    default: break;
    }
}

/* cls.keyCatchers (COD_KEY_CATCHERS, game.h): who takes the keys (console, UI,
 * message mode); 0 while the game is played. */

int pad_in_gameplay(void)
{
    return RD32(COD_KEY_CATCHERS) == 0u;
}

int pad_capturing_key(void)
{
    return 0;
}

void pad_control(PadControl control, int down)
{
    /* What each held control pressed, so its release lets go of that even if the
     * game went between play and the menus meanwhile. */
    static Output held[PAD_CONTROL_COUNT];
    if ((unsigned)control >= PAD_CONTROL_COUNT)
        return;
    Output *o = &held[control];
    if (down) {
        output(o->kind, o->code, 0);
        output(o->kind2, o->code2, 0);
        *o = pad_in_gameplay() ? control_out[control] : menu_out[control];
    }
    output(o->kind, o->code, down);
    output(o->kind2, o->code2, down);
    if (!down)
        *o = (Output){ OUT_NONE, 0, OUT_NONE, 0 };
}

void pad_tick(void)
{
}
