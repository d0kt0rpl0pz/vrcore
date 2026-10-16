/*
 * shifter.h -- shifting on the right thumbstick, a menu choice (Shifter: Grips | Right stick).
 * Off, the game shifts on its own buttons only (the grips, a pad's shoulders); on, the right stick
 * shifts too, and those buttons still do. Look behind is the right stick's click (the rig), so the stick itself
 * is free for this.
 *
 * THE GATES, by the game's gearbox (Mode):
 *   Sequential  a lever that goes up and down (a racing lever): gate +1 while the stick is pushed up (shift up), -1
 *               while pushed down (shift down), 0 at rest. The game holds its shift bit while the gate is held;
 *               the game edges the lever itself.
 *   HGate4      a four-speed H (a four-gear lever): the corners are the gears, 1 up-left, 2 down-left, 3 up-right,
 *               4 down-right. Gate 1..4 while the stick is in that corner, 0 elsewhere. The game holds its
 *               direct-gear action while the gate is held; its gearbox latches, so the gear stays once the stick
 *               is let go, as the real lever's does. The edges between the corners (straight up, down, left,
 *               right) are no gear: sliding from 1 to 2 down the left side chooses nothing on the way.
 *
 * THE THRESHOLDS: engage further out than let go, so a thumb resting at the edge does not flicker the gate.
 * Sequential: in past 0.6 with up or down the stronger axis, out under 0.4. HGate4: in at a length of 0.8 within
 * 25 degrees of the corner's diagonal, out under 0.5 or past 35 degrees.
 *
 * THE TICK. Entering a gate gives a short pulse in the right hand (or the pad it came from): the lever going
 * into gear. The game sends it through its Rumble (pulse), so it layers on the force feedback rather than
 * fighting it.
 *
 * THE GAME'S SIDE. Once a frame call read() with the snapshot and whether the game has the controls (its menu
 * closed, no free camera); it gives the gate, and `engaged` (this frame a new gate) and `from` (the pad it came
 * from, 0 the Touch controller) say where to send the tick.
 */
#pragma once

#include <cstdint>

#include "../app/input.h"
#include "ui/textstrings.h"

namespace vrcore {
namespace rig {

class Shifter {
public:
    enum class Mode { Sequential, HGate4 };

    static constexpr float TickStrength = 0.6f, TickSeconds = 0.04f;    // THE TICK: a click, not a buzz
    static constexpr float SeqIn = 0.6f, SeqOut = 0.4f;
    static constexpr float GateIn = 0.8f, GateOut = 0.5f, GateInDeg = 25.f, GateOutDeg = 35.f;

    explicit Shifter(Mode m) : mode(m) {}

    const Mode mode;
    bool on = false;                    // the menu's choice: Right stick

    int gate = 0;                       // Sequential -1 / 0 / +1, HGate4 0..4
    bool engaged = false;               // a new gate this frame (send the tick)
    uint32_t from = 0;                  // the gate's stick: a pad's id, or 0 the Touch controller

    const char *name() const { return on ? txt::shifter_name_on : txt::shifter_name_off; }
    const char *legend_key() const { return mode == Mode::HGate4 ? txt::shifter_legend_key_h : txt::shifter_legend_key_seq; }
    const char *legend_action() const { return mode == Mode::HGate4 ? txt::shifter_legend_action_h : txt::shifter_legend_action_seq; }

    // Once a frame. `driving`: the game has the controls. The right hand's stick (when active) and every pad's
    // right stick, the strongest wins. Returns the gate (also `gate`).
    int read(const Input &in, bool driving);

    // ---- the maths: device-free ----
    // Sequential: +1 up, -1 down, 0; `was` the gate last frame (hysteresis)
    static int lever(float x, float y, int was);
    // HGate4: the corner 1..4 (1 up-left, 2 down-left, 3 up-right, 4 down-right), 0 none
    static int corner(float x, float y, int was);
};

}
}
