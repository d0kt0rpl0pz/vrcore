/*
 * pick.h -- the system file picker for the menu's "could not start" panel:
 * the player picks the game's ROM set (several files at once if need be) and the front copies each into its
 * folder, then restarts. No cable.
 *
 * SDL's dialog (SDL_ShowOpenFileDialog, no filters: the Quest's picker greys out files a filter does not name by
 * their MIME type, and a .7z has none it knows). Its callback runs on Android's UI thread; the picks are handed to
 * the main thread (take). Each is a URI (content://... on Android) that SDL_IOFromFile opens while the grant
 * lasts (the activity's lifetime). BACK INTO VR: when the picker closes, the activity's backToVr brings the app
 * to the front again (LetsGoAwayActivity.java), or the Quest leaves the player in its own UI.
 */
#pragma once

#include <string>
#include <vector>

namespace vrcore::ui {

struct Pick {
    std::string uri;            // what SDL_IOFromFile opens
    std::string name;           // the file's name, for the player (the URI's last part)
};

// Opens the picker; false: it could not be opened, or one is open already.
bool pick_open(bool multiple);
// One is open.
bool pick_busy();
// The last pick's end, once: true with `picks` (empty: cancelled; `error` set: the picker failed).
bool pick_take(std::vector<Pick> &picks, std::string &error);

}
