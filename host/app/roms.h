/*
 * roms.h -- the game's ROM set, for a plugin that exports vr_roms (abi/vrcore.h): what the "could not start" panel
 * says when the set is missing or wrong (check, message), and the panel's row that takes the files the player picks
 * (pick_row: ui/pick.h's picker, each file adopted by what is in it and copied into the set's folder, then a
 * restart). No cable on the headset.
 *
 * THE SET (Set, the game's table): MAME's sets as the plugin reads them, in its order: the game's 7z, the game's own
 * zip, the parent's zip. A non-merged zip (or the 7z) is the whole set; a split one needs the parent's zip beside
 * it; a merged parent's zip holds both. The checks go by the zips' central directories (name, size, CRC) against
 * vr_roms, each chip taken from the first zip that has its name, as the plugin takes it. A 7z cannot be looked into
 * here: any picked 7z is taken as the game's and the plugin says if it is not the set.
 *
 * THE TEXTS are built from the table: the folder empty (ROM Required), a zip that is not one, a 7z the plugin could
 * not read, the parent's zip alone, the table's notes ("held X but not Y", e.g. an older MAME's set), a split zip
 * without its parent, and the wrong set with what it lacks.
 *
 * THE FOLDER. Android: roms/ beside the app's external files folder (folder(); no permission needed), where the
 * picked files are copied. Desktop: the front's default (a file or a folder), and no picker.
 */
#pragma once

#include <string>
#include <vector>

namespace vrcore::roms {

// A set that holds `held` but not `missing` gets `text` on the panel.
struct Note {
    const char *held;
    const char *missing;
    const char *text;
};

struct Set {
    const char *name;           // MAME's short name ("game93")
    const char *player;         // the set as the player reads it ("MAME's game93")
    const char *title;          // the game ("Game Title ('93 version)")
    const char *title_aside;    // the same in brackets after `player` ("Game Title, '93 version")
    const char *seven_z;        // the 7z the set may come as ("game93.7z"; null: none): a picked .7z is taken as it
    const char *zip;            // the game's own zip ("game93.zip")
    const char *marker;         // a chip only the game's own zip holds: a picked zip with it is that zip
    const char *parent_zip;     // the parent's zip ("game.zip"; null: the set has no parent)
    const char *parent_marker;  // a chip only the parent's zip holds
    const char *parent_title;   // the parent game ("Game Title")
    std::vector<Note> notes;
};

// This run's set: the game's table, where it is looked for (a folder; on the desktop the file itself too), and
// vr_roms' list (null or empty: the plugin has none: no chip is checked, no file adopted, no picker row).
void use(const Set &set, const std::string &where, const char *list);
// Android: the external files folder's parent + "/roms" (empty: no external storage). Not created here.
std::string folder();
// Before the start. Android: the folder created; no file at `where` and none of the set's archives in it: the panel's
// text (message()). Desktop: nothing at `where`: "ROM Required to Start" and the path. Empty: start the game.
std::string check();
// The panel's text for the set as it is in the folder now (the plugin could not read it, or nothing is there).
std::string message();
// ui::Hooks::pick for the message panel (Android; elsewhere empty, and with no list): "A: Choose ROM file...", the
// picker on `select`, each pick taken (take), why not in red, and request_restart() once one is copied.
std::string pick_row(bool select);

// A picked file (`uri` to read it by, `name` the player's) adopted, copied to `dest`, and only then the files that
// would stand before it deleted: empty, or why not, for the player.
std::string take(const std::string &uri, const std::string &name, std::string &dest);

// The parts, for --roms-test.
// Which file a pick is: empty when it is taken, with `dest` where it goes and `stale` the files that would stand
// before it (to delete once it is there); else why not, for the player.
std::string adopt(const std::string &uri, const std::string &name, std::string &dest, std::vector<std::string> &stale);
// `uri`'s bytes to `dest` (through dest + ".part", then renamed); empty, or why not.
std::string copy(const std::string &uri, const std::string &dest);

}
