/*
 * library.h -- the player's music as the soundtrack page sees it: the Soundtracks folder scanned into playlists of
 * named tracks, and the way in to it (the permission, or a folder the player chose instead). The page drives it;
 * soundtrack.h plays what it picks (a playlist's refs to set_tracks).
 *
 * THE FOLDER. Music/Soundtracks in the Quest's shared storage, ~/Music/Soundtracks on the Mac. Each subfolder (one
 * level deep; deeper ones are not read) is a playlist named after it; files loose in the top folder make the
 * playlist kLoose. Only files decodable_name() takes are tracks (hidden ones, starting with a dot, never are).
 * Playlists come in name order with kLoose last, tracks in file name order (numbers by value: "2" before "10").
 * A subfolder with no tracks is no playlist.
 *
 * THE WAY IN (Android). Reading the shared folder needs READ_MEDIA_AUDIO, asked (ask) when the page first needs it,
 * never at launch. Refused, the page offers the system picker (pick): the folder chosen there is kept for this game
 * (its settings, kFolderKey, with a read grant that lasts across launches) and read when the permission is not
 * granted. Granted, the shared folder is read and a chosen one is not. The Mac reads its folder directly.
 *
 * THE SCAN runs on a thread of its own, when asked (scan: the page asks each time it opens; nothing watches the
 * folder): the listing, and each track's title and artist from its tags. It never touches the audio thread. The
 * newest finished scan is kept whole (last), so the page can show the old one while a new one runs.
 *
 * MAIN THREAD: every call here (they read and write the settings), except last and scanning, from any thread.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "storage.h"

namespace vrcore::audio::library {

constexpr const char *kLoose = "All tracks";
constexpr const char *kFolderKey = "soundtrack.folder";     // the chosen folder (a ref), in the game's settings

struct Track {
    std::string ref;            // the file, for soundtrack::set_tracks
    std::string file;           // its name
    std::string title, artist;  // its tags (empty: none)
    // "title - artist", the title alone, else the file's name without its extension
    std::string label() const;
};

struct Playlist {
    std::string name;           // the subfolder's, or kLoose
    bool loose = false;
    std::vector<Track> tracks;
};

enum class Found {
    Ok,             // at least one track
    NoAccess,       // the shared folder cannot be read (the permission) and no folder has been chosen
    PickLost,       // the chosen folder's grant is gone (and no permission): choose it again
    Missing,        // no such folder
    Empty,          // the folder has no track in it
    Unreadable,     // it could not be listed
};

struct Scan {
    Found found = Found::Missing;
    std::string where;          // the folder read, as the player reads it
    std::string text;           // for the player, when found is not Ok
    std::vector<Playlist> playlists;
    int tracks = 0;             // in all the playlists
    double seconds = 0;         // the scan's time
};

// (tests, the sim) This folder is read instead of the platform's, whatever the permission; empty: the platform's.
void set_folder(const std::string &dir);

// A scan started on its own thread; false when one is running already (update starts another once it ends).
bool scan();
bool scanning();
// The newest finished scan; null before the first.
std::shared_ptr<const Scan> last();

// The permission to read the shared folder, with a refusal made this run counted as Refused.
storage::Permission permission();
// The request shown (Android: a 2D panel, back into VR after it); false when it cannot be (granted, the Mac, or a
// request is waiting). The answer comes to update.
bool ask();
bool asking();

// The system's folder picker opened (Android); false when it cannot be. The answer comes to update.
bool pick();
bool picking();
// The folder chosen in the picker (a ref; empty: none), and as the player reads it.
std::string picked();
std::string picked_name();
// The chosen folder forgotten (its grant stays with the system until the app's data is cleared).
void forget_picked();

// Each frame while the page shows: the permission's answer and the picker's taken; a new scan started when either
// opens a way in. True when something changed (the page redraws).
bool update();

// The scan stopped and its thread joined (at exit).
void shutdown();

}
