/* Controllers through SDL3 (tools/sdl3, zlib license): every pad SDL knows
 * (Xbox, PlayStation, Switch Pro and Joy-Cons, 8BitDo and most others),
 * natively, without Steam Input or DS4Windows; their buttons by position
 * (the bottom face button is XPAD_A whatever is printed on it), rumble, and
 * each pad's type for the button icons (pad_icons.cpp).
 *
 * Pads are kept in the order they connect: the first is player 1's (the
 * only one the game reads yet; multiplayer will give the others theirs).
 * A pad that goes away frees its place; the next one connected takes it.
 * SDL reads the devices on a thread of its own (SDL_HINT_JOYSTICK_THREAD);
 * pad_sdl_state brings its view up to date at each poll. */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "host_settings.h"
#include "pad_icons.h"
#include "pad_sdl.h"

#define MAX_PADS 4

static SDL_Gamepad* sPad[MAX_PADS];
static int sReady;
static unsigned sChanges;

static void log_pad(const char* what, int slot) {
    SDL_Gamepad* g = sPad[slot];
    fprintf(stderr, "pad %d %s: %s (%s, %04X:%04X)\n", slot + 1, what, g ? SDL_GetGamepadName(g) : "?",
            g ? SDL_GetGamepadStringForType(SDL_GetGamepadType(g)) : "?", g ? SDL_GetGamepadVendor(g) : 0,
            g ? SDL_GetGamepadProduct(g) : 0);
}

static void open_pad(SDL_JoystickID id) {
    int i;
    for (i = 0; i < MAX_PADS; i++)
        if (sPad[i] && SDL_GetGamepadID(sPad[i]) == id) return; /* already open */
    for (i = 0; i < MAX_PADS && sPad[i]; i++) {}
    if (i == MAX_PADS) return;
    if ((sPad[i] = SDL_OpenGamepad(id)) != NULL) {
        sChanges++;
        log_pad("connected", i);
        pad_icons_hotplug(); /* the icons follow the pad's type */
    }
}

static void close_pad(SDL_JoystickID id) {
    int i;
    for (i = 0; i < MAX_PADS; i++)
        if (sPad[i] && SDL_GetGamepadID(sPad[i]) == id) {
            log_pad("went away", i);
            SDL_CloseGamepad(sPad[i]);
            sPad[i] = NULL;
            sChanges++;
            pad_icons_hotplug();
        }
}

void pad_sdl_init(void) {
    int n = 0, i;
    SDL_JoystickID* ids;
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    /* Keep reading pads while another window has focus: the game decides
     * itself whether input counts then. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "pad: SDL couldn't start (%s): no controllers\n", SDL_GetError());
        return;
    }
    sReady = 1;
    ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n; i++) open_pad(ids[i]);
    SDL_free(ids);
    if (!n) fprintf(stderr, "pad: no controllers connected\n");
}

/* New and gone pads, from SDL's events. */
static void poll_events(void) {
    SDL_Event e;
    SDL_UpdateGamepads();
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) open_pad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) close_pad(e.gdevice.which);
    }
}

static short axis(SDL_Gamepad* g, SDL_GamepadAxis a) { return (short)SDL_GetGamepadAxis(g, a); }

int pad_sdl_state(int player, PadState* out) {
    static const struct { SDL_GamepadButton b; uint32_t x; } kMap[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, XPAD_A}, {SDL_GAMEPAD_BUTTON_EAST, XPAD_B},
        {SDL_GAMEPAD_BUTTON_WEST, XPAD_X}, {SDL_GAMEPAD_BUTTON_NORTH, XPAD_Y},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XPAD_LB}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XPAD_RB},
        {SDL_GAMEPAD_BUTTON_START, XPAD_START}, {SDL_GAMEPAD_BUTTON_BACK, XPAD_BACK},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, XPAD_LTHUMB}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XPAD_RTHUMB},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, XPAD_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XPAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XPAD_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XPAD_RIGHT},
    };
    SDL_Gamepad* g;
    size_t i;
    memset(out, 0, sizeof(*out));
    if (!sReady || player < 0 || player >= MAX_PADS) return 0;
    if (player == 0) poll_events(); /* once a poll: player 1's comes first */
    if (!(g = sPad[player]) || !SDL_GamepadConnected(g)) return 0;
    for (i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++)
        if (SDL_GetGamepadButton(g, kMap[i].b)) out->buttons |= kMap[i].x;
    /* Triggers 0-32767 as XInput's 0-255; sticks with y up, as XInput's. */
    out->lt = (uint8_t)(axis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    out->rt = (uint8_t)(axis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    out->lx = axis(g, SDL_GAMEPAD_AXIS_LEFTX);
    out->ly = (short)(-1 - axis(g, SDL_GAMEPAD_AXIS_LEFTY)); /* (-32768 stays in range) */
    out->rx = axis(g, SDL_GAMEPAD_AXIS_RIGHTX);
    out->ry = (short)(-1 - axis(g, SDL_GAMEPAD_AXIS_RIGHTY));
    return 1;
}

void pad_sdl_rumble(int player, uint16_t low, uint16_t high) {
    if (!sReady || player < 0 || player >= MAX_PADS || !sPad[player]) return;
    /* Until changed again (an hour), as XInput's motors stay as set. */
    SDL_RumbleGamepad(sPad[player], low, high, 3600 * 1000);
}

int pad_sdl_type(int player) {
    if (!sReady || player < 0 || player >= MAX_PADS || !sPad[player]) return 0;
    switch (SDL_GetGamepadType(sPad[player])) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return PADSET_PS;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return PADSET_NS;
    default: return PADSET_XB;
    }
}

unsigned pad_sdl_changes(void) { return sChanges; }
