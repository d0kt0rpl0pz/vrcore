/*
 * input.cpp -- the snapshot input() returns and the Touch haptics' way out (input.h, input_host.h).
 */
#include "input_host.h"

#include <cmath>

namespace vrcore {

namespace {

Input g_input;
HapticSink g_haptics;
double g_last = -1;

// LAST DEVICE (input.h): what each hand was doing last frame, and the place its movement is counted from.
constexpr float kMoveInput = 0.02f;         // metres a hand moves before that is input
constexpr float kLevel = 0.1f;              // a trigger or stick this far in is input
struct HandWas {
    bool tracked = false;
    bool buttons = false;
    float anchor[3] = {0, 0, 0};
    double last = -1;                       // the last input; the snapshot's hand is rebuilt every frame
} g_was[2];

bool hand_buttons(const Hand &h)
{
    return h.trigger > kLevel || h.squeeze > kLevel || std::fabs(h.stick[0]) > kLevel || std::fabs(h.stick[1]) > kLevel ||
           h.stick_click || h.primary || h.secondary || h.menu;
}

bool pad_active(const Pad &p)
{
    return p.south || p.east || p.west || p.north || p.back || p.start || p.guide || p.left_shoulder ||
           p.right_shoulder || p.left_stick || p.right_stick || p.up || p.down || p.left_dpad || p.right_dpad ||
           p.left_trigger > kLevel || p.right_trigger > kLevel || std::fabs(p.left[0]) > 0 || std::fabs(p.left[1]) > 0 ||
           std::fabs(p.right[0]) > 0 || std::fabs(p.right[1]) > 0;
}

// After the hands and pads are in the snapshot: the times of the last input of each device.
void track_devices(Input &in, double now_s)
{
    for (int i = 0; i < 2; ++i) {
        Hand &h = in.hands[i];
        HandWas &w = g_was[i];
        bool input = false;
        if (h.tracked) {
            if (!w.tracked) input = true;                   // picked up, or found again
            else {
                float dx = h.grip.pos[0] - w.anchor[0], dy = h.grip.pos[1] - w.anchor[1],
                      dz = h.grip.pos[2] - w.anchor[2];
                if (dx * dx + dy * dy + dz * dz > kMoveInput * kMoveInput) input = true;
            }
            if (input) {
                for (int k = 0; k < 3; ++k) w.anchor[k] = h.grip.pos[k];
            }
        }
        // a button or the stick counts while held and when it lets go (the level drops to 0)
        bool buttons = hand_buttons(h);
        if (buttons || w.buttons) input = true;
        w.buttons = buttons;
        w.tracked = h.tracked;
        if (input) w.last = now_s;
        h.last_input_s = w.last;
    }
    bool pad = false;
    for (int i = 0; i < in.pad_count; ++i) pad |= pad_active(in.pads[i]);
    bool key = false;
    for (int i = 0; i < Input::kKeys; ++i) key |= in.pressed[i];
    if (pad || key) in.last_pad_key_s = now_s;
}

}

const Input &input() { return g_input; }
Input &input_fill() { return g_input; }

void input_begin_frame(double now_s)
{
    g_input.dt = g_last < 0 ? 0.f : (float)(now_s - g_last);
    g_last = now_s;
    pads_read(g_input, now_s);
    track_devices(g_input, now_s);
}

void input_set_haptics(const HapticSink &s) { g_haptics = s; }

void haptic(int hand, float amplitude, float seconds)
{
    if (g_haptics.apply && (hand == Input::kLeft || hand == Input::kRight)) g_haptics.apply(hand, amplitude, seconds);
}

void stop_haptics()
{
    if (!g_haptics.stop) return;
    g_haptics.stop(Input::kLeft);
    g_haptics.stop(Input::kRight);
}

}
