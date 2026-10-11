/*
 * soak.h -- the soak test: --soak [seed], --soak-crash MINUTES. A game's front names its hooks, and plays;
 * soak.cpp says what the test does.
 */
#pragma once

#include <cstdint>
#include <random>
#include <string>

#include "plugin.h"

namespace vrcore {
namespace soak {

// The controls for one display frame (Plugin::set_controls' arguments).
struct Controls {
    uint32_t held = 0;
    bool steer_on = false;
    int steer = 0;
    bool gas_on = false;
    int gas = 0;
    bool brake_on = false;
    int brake = 0;
};

// What the game's front gives the test (every function below that is not an option takes it).
struct Hooks {
    const char *tag = "[vrcore]";               // the log lines' first word
    const char *counters[3] = {"c0", "c1", "c2"};   // the names of vr_stats' three counters (the 60-s line: [2], [1])
    // The game's state now: a mode, a screen; a change of it ends the stuck clock and calls `enter`. Required.
    int (*state)(Plugin &p) = nullptr;
    // The seconds one state may last before "stuck" is logged (once per state). Required.
    double (*stuck_s)(int state) = nullptr;
    // The state changed (from: -1 the first time): the front's own lines and its picks. `rng` is the seeded one.
    void (*enter)(Plugin &p, int from, int to, uint32_t frame, double now, std::mt19937 &rng) = nullptr;
    // The controls for this display frame, over nothing: now and dt in seconds, since the state began.
    void (*drive)(Plugin &p, std::mt19937 &rng, int state, double now, double since, float dt, Controls &c) = nullptr;
    // The 60-s line's totals, "races n (...), wrong course n" (null: none), and the game's counter after the counters.
    std::string (*totals)() = nullptr;
    std::string (*extra)(Plugin &p) = nullptr;
};

// A launch option of the soak's (as AppInfo::option): the words taken, 0 not ours, -1 bad.
int option(const Hooks &h, int argc, char **argv, int i);
bool on();
// The game has started: the clocks and the seed's line (a restart runs this again: the options stay, the run starts over).
void start(const Hooks &h);
// The controls for this display frame, over nothing (the player's have been cleared). dt: the frame's seconds.
void drive(const Hooks &h, Plugin &p, float dt, Controls &c);
// The name of the game's saves folder: a soak keeps its own, apart from the player's (`normal`).
const char *nvram_name(const char *normal);

}
}
