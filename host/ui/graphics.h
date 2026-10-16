/*
 * graphics.h -- the headset's half of a Graphics page: the rows Graphics profile, Refresh rate, Eye resolution and
 * Foveated rendering, and Sharpening (shown where the headset offers it), each asking the host for the whole
 * Graphics in force when it changes (app.h's request_graphics). The front builds the page round them, with its own
 * rows in between (graphics_rows).
 *
 * THE PROFILES: Performance and Fidelity are this headset's rows of the front's device profiles (host_profile, keyed
 * by headset.h's HeadsetKind), and the game's share of them (draw distance, say: the game_* hooks). Choosing one puts
 * its values in force and saves each row's key; changing one of those rows by hand (the game's call
 * graphics_by_hand) makes the profile Custom and keeps the values aside (<prefix>custom.*); Custom again brings them
 * back. Sharpening is no part of a profile.
 *
 * THE START (graphics_load, then graphics_start once the headset is known): a saved Performance or Fidelity takes
 * this headset's row as it is now (a retuned profile reaches a headset that chose it before), and so does nothing
 * saved at all (the start profile). Custom, or values saved without a profile key, keep the saved values; the saved
 * rate is an index into this headset's rate_list. A headset without the Sharpening row starts with it off (a saved
 * value could not be undone).
 *
 * THE KEYS, after the front's prefix: gfxprofile (0 Performance, 1 Fidelity, 2 Custom), rate (an index into
 * rate_list), eyescale, foveation, sharpen, and custom.eyescale / custom.rate / custom.foveation. Each row writes its
 * own key(s) when it changes, never the others. Main thread only.
 */
#pragma once

#include <functional>

#include "app.h"
#include "headset.h"
#include "ui/menu.h"

namespace vrcore::ui {

enum { kPerformance = 0, kFidelity = 1, kCustom = 2 };     // the Graphics profile's choices

// A profile's host graphics on one headset.
struct HostProfile {
    float eye;          // x the recommended eye size
    float hz;           // the refresh rate asked for (the listed rate nearest it)
    int foveation;      // 0-3: Off, Low, Medium, High
};

struct GraphicsHooks {
    const char *prefix = "";                // the keys' prefix (THE KEYS)
    int start_profile = kFidelity;          // a headset with nothing saved
    // profile: kPerformance or kFidelity. Required: without it a profile changes nothing and the start keeps the
    // saved values.
    std::function<HostProfile(HeadsetKind k, int profile)> host_profile;
    // The game's share of a profile (each may be empty: none).
    std::function<bool(int profile)> game_matches;  // the game's choices are profile's
    std::function<void(int profile)> game_apply;    // profile's game values in force (not saved yet)
    std::function<void()> game_save;                // the game's values saved and sent to it (a profile was chosen)
    std::function<void()> game_save_custom;         // the game's values kept aside as Custom's
    std::function<void()> game_load_custom;         // Custom's game values back in force (not saved yet)
    // The rows' help texts (null: none).
    const char *help_profile = nullptr, *help_rate = nullptr, *help_eye = nullptr, *help_foveation = nullptr,
               *help_sharpen = nullptr;
};

// AppInfo::load_settings (the settings open): the hooks kept for the run, the saved graphics read into `o` (rate,
// eyescale, foveation, sharpen; a key never saved leaves `o`'s own) and the saved profile kept.
void graphics_load(const GraphicsHooks &hooks, Options &o);

// AppInfo::headset's host half (THE START): `o` gets this headset's profile, or keeps the saved values.
struct GraphicsStart {
    int profile;        // kPerformance / kFidelity (`o` holds its graphics), kCustom, or -1: values saved, no profile
    bool fresh;         // nothing was saved: the start profile
};
GraphicsStart graphics_start(const Headset &h, Options &o);

// The rows, made when the menu's pages are (after graphics_load): `o` the graphics in force (the launch's, flags
// applied; after a restart the host's, with the profile the player saved since). `profile`:
// Graphics profile, Refresh rate, Eye resolution, Foveated rendering; `sharpen`: Sharpening, with a gap above it
// and shown only where the headset offers it (`sharpen_shown`). The first profile row starts a PROFILE card whose
// title shows the current profile.
struct GraphicsRows {
    Page profile;
    Item sharpen;
    bool sharpen_shown = false;
};
GraphicsRows graphics_rows(const Options &o);

// One of the game's profile rows changed by hand: the profile is Custom, its values kept aside.
void graphics_by_hand();

}
