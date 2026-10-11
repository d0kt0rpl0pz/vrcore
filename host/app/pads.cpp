/*
 * pads.cpp -- SDL gamepads and the keyboard into the snapshot (app/input.h), on both hosts: the Input System's
 * Gamepad.all and Keyboard.current (a Bluetooth keyboard on the Quest).
 *
 * THE PADS. Opened when SDL adds them, closed when it removes them: a pad gone is gone from the snapshot, and
 * Pad::id finds it no more (a pad that returns is a new input). The sticks go through the
 * Input System's default stick deadzone (radial, 0.125 to 0.925 of the travel rescaled to 0..1: Gamepad's
 * "stickDeadzone"), y turned up. Triggers 0..1 as SDL gives them.
 *
 * THE MOTORS (pad_rumble). Set when asked, then renewed every 0.5 s with a 1 s duration: a pad's motors run
 * until told otherwise, so an app that stops (a crash, a pause) leaves them buzzing for a second at most.
 *
 * THE KEYBOARD. Held from SDL's key state; pressed-this-frame from the key-down events (not repeats) since the
 * last snapshot.
 */
#include <SDL3/SDL.h>

#include <cmath>
#include <cstring>

#include "input_host.h"
#include "log.h"

namespace vrcore {

namespace {

constexpr float kDeadMin = 0.125f, kDeadMax = 0.925f;  // InputSettings.defaultDeadzoneMin / Max
constexpr double kRenew = 0.5;                          // seconds between motor renewals
constexpr Uint32 kRumbleMs = 1000;

struct Open {
    SDL_Gamepad *pad = nullptr;
    SDL_JoystickID id = 0;
    float low = 0, high = 0;            // the motors as asked
    double renewed = -1;                // when last sent (-1: send now)
};

Open g_open[Input::kMaxPads];
int g_count = 0;
bool g_pressed[Input::kKeys] = {};      // key-downs since the last snapshot
bool g_init = false;
double g_now = 0;

float axis(SDL_Gamepad *p, SDL_GamepadAxis a)
{
    float v = SDL_GetGamepadAxis(p, a) / 32767.f;
    return v < -1.f ? -1.f : v > 1.f ? 1.f : v;
}

// the Input System's StickDeadzoneProcessor
void stick(SDL_Gamepad *p, SDL_GamepadAxis ax, SDL_GamepadAxis ay, float out[2])
{
    float x = axis(p, ax), y = -axis(p, ay);
    float m = std::sqrt(x * x + y * y);
    if (m < kDeadMin) {
        out[0] = out[1] = 0;
        return;
    }
    float k = (m >= kDeadMax ? 1.f : (m - kDeadMin) / (kDeadMax - kDeadMin)) / m;
    out[0] = x * k;
    out[1] = y * k;
}

void send(Open &o)
{
    SDL_RumbleGamepad(o.pad, (Uint16)(o.low * 65535.f + 0.5f), (Uint16)(o.high * 65535.f + 0.5f),
                      o.low > 0 || o.high > 0 ? kRumbleMs : 0);
    o.renewed = g_now;
}

void add(SDL_JoystickID id)
{
    for (int i = 0; i < g_count; ++i)
        if (g_open[i].id == id) return;
    if (SDL_GetGamepadVendorForID(id) == 0x2833) {      // Oculus: the Touch controllers mirrored by Android (OpenXR reads them)
        const char *name = SDL_GetGamepadNameForID(id);
        log("pads: skipped %u, %s (the headset's own controllers)", (unsigned)id, name ? name : "?");
        return;
    }
    if (g_count == Input::kMaxPads) {
        log("pads: %u not opened (%d open already)", (unsigned)id, Input::kMaxPads);
        return;
    }
    SDL_Gamepad *p = SDL_OpenGamepad(id);
    if (!p) {
        log("pads: SDL_OpenGamepad(%u) failed: %s", (unsigned)id, SDL_GetError());
        return;
    }
    g_open[g_count++] = {p, id, 0, 0, -1};
    const char *name = SDL_GetGamepadName(p);
    log("pads: added %u, %s (%d open)", (unsigned)id, name ? name : "?", g_count);
}

void remove(SDL_JoystickID id)
{
    for (int i = 0; i < g_count; ++i) {
        if (g_open[i].id != id) continue;
        SDL_CloseGamepad(g_open[i].pad);
        for (int j = i; j + 1 < g_count; ++j) g_open[j] = g_open[j + 1];
        g_open[--g_count] = Open();
        log("pads: removed %u (%d open)", (unsigned)id, g_count);
        return;
    }
}

}

void pads_init()
{
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        log("pads: SDL_INIT_GAMEPAD failed: %s (no pads)", SDL_GetError());
        return;
    }
    g_init = true;
    // the ones already there come as GAMEPAD_ADDED events too
}

void pads_event(const void *event)
{
    const SDL_Event &e = *static_cast<const SDL_Event *>(event);
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED: add(e.gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: remove(e.gdevice.which); break;
    case SDL_EVENT_KEY_DOWN:
        if (!e.key.repeat && e.key.scancode < Input::kKeys) g_pressed[e.key.scancode] = true;
        break;
    default: break;
    }
}

void pads_read(Input &in, double now_s)
{
    g_now = now_s;
    int nkeys = 0;
    const bool *k = SDL_GetKeyboardState(&nkeys);
    for (int i = 0; i < Input::kKeys; ++i) {
        in.keys[i] = k && i < nkeys && k[i];
        in.pressed[i] = g_pressed[i];
    }
    std::memset(g_pressed, 0, sizeof g_pressed);
    SDL_Keymod mod = SDL_GetModState();
    in.shift = mod & SDL_KMOD_SHIFT;
    in.alt = mod & SDL_KMOD_ALT;
    in.ctrl = mod & SDL_KMOD_CTRL;

    in.pad_count = g_count;
    for (int i = 0; i < g_count; ++i) {
        Open &o = g_open[i];
        SDL_Gamepad *p = o.pad;
        Pad &d = in.pads[i];
        d = Pad();
        d.id = o.id;
        stick(p, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, d.left);
        stick(p, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, d.right);
        d.left_trigger = axis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        d.right_trigger = axis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        if (d.left_trigger < 0) d.left_trigger = 0;
        if (d.right_trigger < 0) d.right_trigger = 0;
        d.south = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH);
        d.east = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST);
        d.west = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_WEST);
        d.north = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_NORTH);
        d.back = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_BACK);
        d.start = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_START);
        d.guide = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_GUIDE);
        d.left_shoulder = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        d.right_shoulder = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        d.left_stick = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_STICK);
        d.right_stick = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        d.up = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP);
        d.down = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        d.left_dpad = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        d.right_dpad = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        // the motors renewed while running
        if ((o.low > 0 || o.high > 0) && now_s - o.renewed >= kRenew) send(o);
    }
}

void pad_rumble(uint32_t id, float low, float high)
{
    low = low < 0 ? 0.f : low > 1 ? 1.f : low;
    high = high < 0 ? 0.f : high > 1 ? 1.f : high;
    for (int i = 0; i < g_count; ++i) {
        Open &o = g_open[i];
        if (o.id != id) continue;
        o.low = low;
        o.high = high;
        send(o);
        return;
    }
}

void pads_shutdown()
{
    for (int i = 0; i < g_count; ++i) {
        Open &o = g_open[i];
        if (o.low > 0 || o.high > 0) SDL_RumbleGamepad(o.pad, 0, 0, 0);
        SDL_CloseGamepad(o.pad);
        o = Open();
    }
    g_count = 0;
    if (g_init) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    g_init = false;
}

}
