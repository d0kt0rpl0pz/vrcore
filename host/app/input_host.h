/*
 * input_host.h -- the host's side of input.h: the snapshot it fills before each update, and where the Touch
 * controllers' haptics go (the XR host's actions; none in the sim). Host-internal.
 */
#pragma once

#include "input.h"

namespace vrcore {

// The snapshot being filled (the one input() returns).
Input &input_fill();
// Each display frame before update: the time since the last one, and the keyboard and pads from SDL
// (pads.cpp; after the host pumped SDL's events). Clears `pressed` from the frame before.
void input_begin_frame(double now_s);

// The Touch controllers' haptics (xr/xr_actions.cpp sets them; null: none).
struct HapticSink {
    void (*apply)(int hand, float amplitude, float seconds) = nullptr;
    void (*stop)(int hand) = nullptr;
};
void input_set_haptics(const HapticSink &s);

// pads.cpp: SDL gamepads in and out (both hosts), and the keyboard.
void pads_init();                                   // after SDL_Init (SDL_INIT_GAMEPAD)
void pads_event(const void *sdl_event);             // every SDL event the host pumps (added / removed, keys)
void pads_read(Input &in, double now_s);            // the pads' state and the keyboard into the snapshot
void pads_shutdown();                               // motors off, pads closed

}
