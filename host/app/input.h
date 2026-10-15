/*
 * input.h -- what the player holds, once a display frame (the Input System's XR / Gamepad / Keyboard layouts).
 *
 * THE SNAPSHOT. The host fills one Input before each update (the front's thread, where update runs): the two
 * Touch controllers (OpenXR actions, xr/xr_actions.cpp; none in the sim), the head, the pads (SDL gamepads, both
 * hosts) and the keyboard (SDL's key state, both hosts). Levels and poses only, read once a frame; the edges a
 * front needs it keeps itself (as the C# does), except the keyboard's and the mouse's button, which the host keeps
 * (pressed this frame).
 * Unfocused (Horizon's menu up) every hand reads inactive and zero.
 *
 * THE OUTPUTS. haptic / stop_haptics drive the Touch controllers (xrApplyHapticFeedback), pad_rumble a pad's
 * motors (SDL_RumbleGamepad). Called from update.
 *
 * LAST DEVICE. The menu's pointer shows only while the controllers are the player's latest input. A hand's
 * `last_input_s` is the last time it had a button, the stick, the trigger or the grip, moved more than 2 cm
 * (against where it last counted) or became tracked; `last_pad_key_s` the last time a gamepad had a button, stick
 * or trigger, or a key went down. controller_shown(hand) is the rule: tracked, and newer than the pads and keys.
 * The host keeps both (input.cpp); a front reads them.
 *
 * Poses are in the host's LOCAL space (the room, as the eye poses in gpu::Eye): metres, y up, -z forward.
 */
#pragma once

#include <cstdint>

#include "../gpu/vmath.h"

namespace vrcore {

struct Pose {
    float pos[3] = {0, 0, 0};
    Quat rot;
};

// One Touch controller (or whatever the runtime's interaction profile gives for that hand).
struct Hand {
    bool active = false;            // the actions are bound and the session focused
    bool tracked = false;           // `grip`'s position and orientation are valid this frame
    Pose grip;                      // the grip pose, at the display time
    float trigger = 0;              // 0..1
    float squeeze = 0;              // 0..1
    bool squeezed = false;          // the grip "pressed": on at 0.5 of squeeze, off under 0.375 (the Input System's)
    float stick[2] = {0, 0};        // thumbstick x right, y up, -1..1
    bool stick_click = false;
    bool primary = false;           // A (right) / X (left)
    bool secondary = false;         // B (right) / Y (left)
    bool menu = false;              // the left controller's menu button (the right one's is the runtime's)
    Pose aim;                       // the aim pose: where a ray from this hand starts and points (-z), at the display time
    bool aim_tracked = false;       // `aim`'s position and orientation are valid this frame
    double last_input_s = -1;       // when this hand last had input (LAST DEVICE, below); the host's clock, -1 never
};

// One gamepad, SDL's names (the Xbox layout: south A, east B, west X, north Y).
struct Pad {
    uint32_t id = 0;                // SDL_JoystickID: the same pad keeps it until removed
    float left[2] = {0, 0};         // sticks: x right, y UP (SDL's y down, turned), -1..1
    float right[2] = {0, 0};
    float left_trigger = 0, right_trigger = 0;      // 0..1
    bool south = false, east = false, west = false, north = false;
    bool back = false, start = false, guide = false;
    bool left_shoulder = false, right_shoulder = false;
    bool left_stick = false, right_stick = false;   // the sticks' clicks
    bool up = false, down = false, left_dpad = false, right_dpad = false;
};

// The mouse (the sim's window; all zero on the headset).
struct Mouse {
    bool present = false;           // a window with a pointer
    bool inside = false;            // the pointer is over the window
    float x = 0, y = 0;             // window pixels (the swapchain's), from the top left
    float nx = 0, ny = 0;           // 0..1 over the window, from the top left
    bool left = false;              // the left button held
    bool left_pressed = false;      // it went down since the last snapshot
    int eye = -1;                   // the eye whose picture is under the pointer (0 left, 1 right); -1 none
    float eye_u = 0, eye_v = 0;     // the point in that eye's picture, 0..1 from its top left
};

struct Input {
    static constexpr int kMaxPads = 8;
    static constexpr int kKeys = 512;               // SDL_SCANCODE_COUNT

    enum { kLeft = 0, kRight = 1 };
    Hand hands[2];
    bool head_tracked = false;
    Pose head;                                      // between the eyes, at the display time
    Pad pads[kMaxPads];
    int pad_count = 0;                              // in the order added
    bool keys[kKeys] = {};                          // held, by SDL_Scancode
    bool pressed[kKeys] = {};                       // went down since the last snapshot
    bool shift = false, alt = false, ctrl = false;  // either side
    Mouse mouse;
    float dt = 0;                                   // seconds since the last snapshot (0 on the first)
    double last_pad_key_s = -1;                     // when a pad or the keyboard last had input (LAST DEVICE); -1 never

    // The controller is tracked and its last input is newer than the last pad or key input.
    bool controller_shown(int hand) const
    {
        return hands[hand].tracked && hands[hand].last_input_s > last_pad_key_s;
    }

    const Pad *pad(uint32_t id) const               // null: not there (removed)
    {
        for (int i = 0; i < pad_count; ++i)
            if (pads[i].id == id) return &pads[i];
        return nullptr;
    }
};

// This frame's snapshot (valid in update; zero before the first).
const Input &input();

// The interaction profile in force on a hand ("oculus/touch_controller", "valve/index_controller"...), as the runtime
// last reported it; "" before one is, and always in the sim. Changes between frames: the menu picks its model by it.
const char *controller_profile(int hand);

// A Touch controller's vibration: `amplitude` 0..1 for `seconds` (a new one replaces the last). No-op in the sim.
void haptic(int hand, float amplitude, float seconds);
// Both hands' vibration stopped now.
void stop_haptics();
// A pad's motors, 0..1 each, until changed (the host renews them so a stopped app leaves nothing running).
// An id no longer there is ignored.
void pad_rumble(uint32_t id, float low, float high);

}
