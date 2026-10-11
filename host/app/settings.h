/*
 * settings.h -- the saved choices: a key=value text file.
 *
 * One pair a line, UTF-8; a line starting with # is a comment. Keys this build does not know, and comments, are
 * kept and written back (spaces round a key or value are not part of it), so a newer or older build's file is not cut down. A line that is neither is logged and
 * dropped at the next write. Every set_* / erase writes the whole file at once: to settings.txt.tmp beside it, then
 * renamed over the old one, so a crash never leaves half a file.
 *
 * MAIN THREAD ONLY: no locking, no observers. The front reads its choices once at start (AppInfo::load_settings);
 * the menu writes single keys as rows change.
 *
 * Where: SDL_GetPrefPath(org, app) + "settings.txt" (Mac: ~/Library/Application Support/<org>/<app>/; Android: the
 * app's internal files folder), or the file given to set_path (--settings FILE, the tests).
 */
#pragma once

#include <string>
#include <vector>

namespace vrcore {
namespace settings {

// The --settings FILE override; call before open. Null or empty: the preferences folder's.
void set_path(const char *file);

// Loads the file (none yet: nothing saved, not an error) and logs its path and how many keys it has. A second call
// loads again, dropping what was in memory. False: no place to keep the file (reads still work, defaults).
bool open(const char *org, const char *app);

// The file's path (empty before open).
const std::string &path();

bool has(const char *key);
int get_int(const char *key, int def);
float get_float(const char *key, float def);
std::string get_string(const char *key, const char *def);
bool get_bool(const char *key, bool def);       // stored as 0 / 1; any other whole number is true

// Each changes the value and writes the file. False: the write failed (the value stays in memory).
bool set_int(const char *key, int v);
bool set_float(const char *key, float v);       // written %.9g: reads back bit-exact
bool set_string(const char *key, const char *v);    // one line: a newline in v becomes a space
bool set_bool(const char *key, bool v);
bool erase(const char *key);

// a new default never reaches a file that has been through the menu, so raising `version` drops
// the keys listed for each `since` above the saved version (1 when none is saved), once.
struct Reset {
    int since;
    std::vector<std::string> keys;
};
void upgrade(const char *version_key, int version, const std::vector<Reset> &resets);

}
}
