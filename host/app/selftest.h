/*
 * selftest.h -- the timed self test: --self-test SECONDS DIR, --expect HASH. A game's front names its hooks;
 * selftest.cpp says what the test does.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "plugin.h"

namespace vrcore {
namespace selftest {

// What the game's front gives the test (every function below takes it). Null hooks are the defaults named.
struct Hooks {
    Plugin *plugin = nullptr;                   // the running game (required)
    const char *tag = "[vrcore]";               // the log lines' first word
    const char *counters[3] = {"c0", "c1", "c2"};   // the names of vr_stats' three counters; the first must stay 0
    const char *timed = "timed";                // what a timed run plays, for the start line ("... , <timed>")
    // The newest picture of the board's screen into `rgba` (bottom row first), its size in w and h; the game frame
    // it is, 0 when there is none yet. Null: vr_picture's flat picture as it comes.
    uint32_t (*screen)(std::vector<uint8_t> &rgba, int &w, int &h) = nullptr;
    // The game's part of the result line, after the counters: "name n, name n, ..." (null: none).
    std::string (*extra)() = nullptr;
    // Whether the game holds at a frame this run (its arguments say so): a held run saves and hashes that frame's
    // screen; a timed run saves one every 300 game frames. Null: timed.
    bool (*held)() = nullptr;
    // Whether the game holds now (null: never).
    bool (*holding)() = nullptr;
    // A timed run is starting: the game's own presses go on here (null: nothing to do).
    void (*prepare_timed)() = nullptr;
};

// A launch option of the self test's (as AppInfo::option): the words taken, 0 not ours, -1 bad.
int option(const Hooks &h, int argc, char **argv, int i);
bool on();
// Once the options are parsed, before the game starts: the folder, and held or timed.
void prepare(const Hooks &h);
// A test keeps no saves unless it is given --nvram: true when the game must not keep any (`nvram`: --nvram's folder).
bool nvram_off(const std::string &nvram);
// The game has started: the clock.
void start(const Hooks &h);
// Every update while the game runs.
void update(const Hooks &h);
// The run cannot go on (the game stopped, did not start, has no ROM set): result.txt says FAIL, the exit code is 2.
void fail(const Hooks &h, const std::string &why);

}
}
