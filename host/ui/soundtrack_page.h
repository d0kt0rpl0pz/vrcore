/*
 * soundtrack_page.h -- the Soundtrack page: the player's own music (audio/soundtrack.h, played from the folder
 * audio/library.h scans) chosen from a menu page every game gets the same way. A front adds one row to its top
 * menu and makes three calls; the host does the rest, and no plugin export or ABI call is involved (the soundtrack
 * is mixed after the game's sound).
 *
 *   soundtrack_load(hooks)            AppInfo::load_settings (the settings are open): the saved choices read.
 *   soundtrack_item(menu)             the "Soundtrack" row for the top page (it builds the pages it opens).
 *   soundtrack_menu_opened()          Hooks::on_open: the folder is scanned again (never asking for anything).
 *   soundtrack_update(menu, t, in)    every update: the library's answers, the music's start and its fades when the
 *                                     headset loses focus, the "now playing" line.
 *   soundtrack_replacing()            true while the player's music stands in for the game's: the front sends the
 *                                     game VR_OPT_MUSIC_VOL 0 then (its saved volume otherwise) and does so again
 *                                     when hooks.options_changed is called.
 *
 * THE ROWS: Custom soundtrack On / Off, Playlist, Tracks (a window of ten rows that scrolls with up / down off its
 * ends, and Earlier / Later rows for the pointer), Mode (In order / Shuffle / Repeat one), Next, Previous, Volume,
 * Now playing On / Off. With no songs to show the page says why and offers what can be done (allow access to the
 * music). The permission is asked for only when its row is pressed, never at launch.
 *
 * WHEN IT PLAYS: from when it is switched on (and at launch when it was saved on and the folder can be read without
 * asking) until it is switched off, through the pause menu; it stops when the headset loses focus and starts the
 * track again from its beginning when the focus comes back. Shuffle plays the playlist once in a random order
 * before any track repeats (through a loss of focus, and a new scan, too). In Repeat one, Next and Previous move to
 * the playlist's next or previous track, which then repeats.
 *
 * THE KEYS, after the hooks' prefix: soundtrack.on, soundtrack.volume (0..100), soundtrack.playlist (its folder's
 * name; empty: the loose files, library::kLoose), soundtrack.mode (0 In order, 1 Shuffle, 2 Repeat one),
 * soundtrack.track (the file's name), soundtrack.nowplaying. Main thread only.
 */
#pragma once

#include <functional>
#include <string>

#include "app.h"
#include "input.h"
#include "ui/menu.h"

namespace vrcore::ui {

struct SoundtrackHooks {
    const char *prefix = "";                // the keys' prefix
    // The game's music volume must be sent again: soundtrack_replacing() changed.
    std::function<void()> options_changed;
    // The master volume (VR_OPT_VOLUME's value, 0..100) the soundtrack sits under; empty: 100.
    std::function<int()> master_volume;
};

void soundtrack_load(const SoundtrackHooks &hooks);
Item soundtrack_item(Menu &menu, const char *label = txt::soundtrack_default_label);
void soundtrack_menu_opened();
void soundtrack_update(Menu &menu, const Tick &t, const Input &in);
bool soundtrack_replacing();
// (tests) The "Now playing" line, as last set (shown or not).
std::string soundtrack_now_playing();

}
