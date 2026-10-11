/*
 * input.cpp -- the snapshot input() returns and the Touch haptics' way out (input.h, input_host.h).
 */
#include "input_host.h"

namespace vrcore {

namespace {

Input g_input;
HapticSink g_haptics;
double g_last = -1;

}

const Input &input() { return g_input; }
Input &input_fill() { return g_input; }

void input_begin_frame(double now_s)
{
    g_input.dt = g_last < 0 ? 0.f : (float)(now_s - g_last);
    g_last = now_s;
    pads_read(g_input, now_s);
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
