/*
 * storage.h -- where the player's music is, and the platform's way in: the soundtrack library's (library.h) layer
 * under it. A folder is named by a REF: a path (the Mac's ~/Music/Soundtracks, the Quest's shared Music/Soundtracks
 * read with READ_MEDIA_AUDIO), or on Android a content:// URI under a folder the player chose in the system picker
 * (a document tree, whose read grant is kept across launches). A file is named the same way; open_fd reads either.
 *
 * ANDROID. The calls go to the activity (VrcoreActivity.java) through JNI, from any thread (SDL attaches a thread of
 * ours the first time and detaches it when it ends). The permission request and the picker are 2D panels: when one
 * closes the activity brings the app back into VR (backToVr), as after the ROM picker. The permission is asked
 * through SDL_RequestAndroidPermission; the picker is the activity's own (SDL's dialog cannot choose a folder).
 *
 * THE MAC (and any desktop): paths only. The permission is always granted and there is no picker.
 */
#pragma once

#include <string>
#include <vector>

namespace vrcore::audio::storage {

// The shared music folder's Soundtracks folder: a path (empty when there is no shared storage).
std::string music_folder();
// The same as the player would read it on this device ("Music/Soundtracks", "~/Music/Soundtracks").
std::string music_folder_name();

enum class Permission {
    Granted,        // the shared folder can be read (the Mac: always)
    Ask,            // not granted, and asking would show the request (or Android says nothing more: asking will tell)
    Refused,        // refused before: asking again shows it once more at most
};
Permission permission();
// The request shown (main thread); `done` gets the answer later on another thread. False: it could not be made (the
// Mac: there is nothing to ask).
bool request_permission(void (*done)(bool granted));

// The system's folder picker opened (main thread), starting at the shared folder; false: there is none here, or it
// could not be opened. The answer comes through take_folder.
bool pick_folder();
// The picker's answer, once: true with `ref` the chosen folder (its read grant kept), empty when cancelled.
bool take_folder(std::string &ref);
// The chosen folder's grant is still held (the player can revoke it, or the app's data be cleared).
bool folder_granted(const std::string &ref);
// A ref as the player would read it ("Music/Soundtracks" for a picked folder there).
std::string ref_name(const std::string &ref);

struct Entry {
    std::string name;       // the file's or folder's own name
    std::string ref;        // to list it (a folder) or open it (a file)
    bool folder = false;
};
// The folder's entries, unsorted; false (with `why`, for the log) when it cannot be read. Missing: *missing true.
bool list(const std::string &folder, std::vector<Entry> &out, bool *missing, std::string *why);

// A descriptor to read the file by (close it after), or -1 with *why. Not on the audio thread.
int open_fd(const std::string &ref, std::string *why);

}
