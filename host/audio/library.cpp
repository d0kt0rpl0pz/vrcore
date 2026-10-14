/*
 * library.cpp -- library.h: the scan's thread, the playlists, and the permission's and picker's answers.
 *
 * THREADS. The main thread starts a scan with what it is to read (the folder, decided there: it needs the settings
 * and the permission); the scan thread lists, reads tags and publishes the finished Scan under a mutex. shutdown
 * asks it to stop between files. The permission's answer comes on Android's UI thread, as an atomic.
 */
#include "library.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>

#include "../app/log.h"
#include "../app/settings.h"
#include "decode.h"

namespace vrcore::audio::library {

namespace {

enum : int { kNoAnswer, kGranted, kRefused };

struct State {
    std::string folder;                     // set_folder's
    std::mutex m;                           // last
    std::shared_ptr<const Scan> last;
    std::thread thread;
    std::atomic<bool> scanning{false}, stop{false};
    bool asking = false, picking = false, refused = false;
    bool again = false;                     // a scan asked for while one ran: started by update
    std::atomic<int> answer{kNoAnswer};
} g;

// a before b, case aside and runs of digits by their value ("Track 2" before "Track 10")
bool natural_less(const std::string &a, const std::string &b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[j];
        if (std::isdigit(x) && std::isdigit(y)) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && a[i2] == '0') ++i2;
            while (j2 < b.size() && b[j2] == '0') ++j2;
            size_t ie = i2, je = j2;
            while (ie < a.size() && std::isdigit((unsigned char)a[ie])) ++ie;
            while (je < b.size() && std::isdigit((unsigned char)b[je])) ++je;
            if (ie - i2 != je - j2) return ie - i2 < je - j2;
            int c = a.compare(i2, ie - i2, b, j2, je - j2);
            if (c) return c < 0;
            i = ie;
            j = je;
            continue;
        }
        x = (unsigned char)std::tolower(x);
        y = (unsigned char)std::tolower(y);
        if (x != y) return x < y;
        ++i;
        ++j;
    }
    if (a.size() - i != b.size() - j) return a.size() - i < b.size() - j;
    return a < b;                           // the same but for case: still one order
}

bool hidden(const std::string &name)
{
    return name.empty() || name[0] == '.';
}

// The folder's tracks, with their tags, into `tracks`; its subfolders into `folders` (null: not wanted).
bool read_folder(const std::string &ref, std::vector<Track> &tracks, std::vector<storage::Entry> *folders,
                 bool *missing, std::string *why)
{
    std::vector<storage::Entry> entries;
    if (!storage::list(ref, entries, missing, why)) return false;
    for (storage::Entry &e : entries) {
        if (g.stop) return false;
        if (hidden(e.name)) continue;
        if (e.folder) {
            if (folders) folders->push_back(e);
            continue;
        }
        if (!decodable_name(e.name.c_str())) continue;
        Track t;
        t.ref = e.ref;
        t.file = e.name;
        read_tags(t.ref.c_str(), &t.title, &t.artist);
        tracks.push_back(std::move(t));
    }
    std::sort(tracks.begin(), tracks.end(), [](const Track &a, const Track &b) { return natural_less(a.file, b.file); });
    return true;
}

std::string copy_hint(const std::string &where)
{
#ifdef __ANDROID__
    (void)where;
    return "Copy songs to " + storage::music_folder_name() + " on the Quest over USB";
#else
    return "Copy songs to " + where;
#endif
}

void scan_main(std::string ref, std::string where)
{
    Uint64 t0 = SDL_GetTicksNS();
    auto s = std::make_shared<Scan>();
    s->where = where;
    std::vector<Track> loose;
    std::vector<storage::Entry> folders;
    bool missing = false;
    std::string why;
    if (!read_folder(ref, loose, &folders, &missing, &why)) {
        s->found = missing ? Found::Missing : Found::Unreadable;
        s->text = missing ? copy_hint(where) : "Cannot read " + where + " (" + why + ")";
    } else {
        std::sort(folders.begin(), folders.end(),
                  [](const storage::Entry &a, const storage::Entry &b) { return natural_less(a.name, b.name); });
        for (const storage::Entry &f : folders) {
            Playlist p;
            p.name = f.name;
            bool gone = false;
            std::string fwhy;
            if (!read_folder(f.ref, p.tracks, nullptr, &gone, &fwhy) && !g.stop)
                log("soundtrack: library: cannot read %s (%s)", f.name.c_str(), fwhy.c_str());
            if (!p.tracks.empty()) s->playlists.push_back(std::move(p));
        }
        if (!loose.empty()) {
            Playlist p;
            p.name = kLoose;
            p.loose = true;
            p.tracks = std::move(loose);
            s->playlists.push_back(std::move(p));
        }
        for (const Playlist &p : s->playlists) s->tracks += (int)p.tracks.size();
        s->found = s->tracks ? Found::Ok : Found::Empty;
        if (!s->tracks) s->text = copy_hint(where);
    }
    s->seconds = (SDL_GetTicksNS() - t0) * 1e-9;
    if (g.stop) {
        log("soundtrack: library: scan of %s stopped", where.c_str());
    } else {
        log("soundtrack: library: %s: %d tracks in %zu playlists (%.2f s)%s%s", where.c_str(), s->tracks,
            s->playlists.size(), s->seconds, s->text.empty() ? "" : ": ", s->text.c_str());
        std::lock_guard<std::mutex> lk(g.m);
        g.last = s;
    }
    g.scanning = false;
}

// A scan with nothing to list: its result now.
void publish(Found found, const std::string &where, const std::string &text)
{
    auto s = std::make_shared<Scan>();
    s->found = found;
    s->where = where;
    s->text = text;
    log("soundtrack: library: %s: %s", where.c_str(), text.c_str());
    std::lock_guard<std::mutex> lk(g.m);
    g.last = s;
}

void on_answer(bool granted)
{
    g.answer = granted ? kGranted : kRefused;
}

}

std::string Track::label() const
{
    if (!title.empty()) return artist.empty() ? title : title + " - " + artist;
    size_t dot = file.rfind('.');
    return dot == std::string::npos || dot == 0 ? file : file.substr(0, dot);
}

void set_folder(const std::string &dir)
{
    g.folder = dir;
}

bool scan()
{
    if (g.scanning) {
        g.again = true;
        return false;
    }
    g.again = false;
    if (g.thread.joinable()) g.thread.join();          // the last one, finished
    std::string ref, where;
    if (!g.folder.empty()) {
        ref = where = g.folder;
    } else if (permission() == storage::Permission::Granted) {
        ref = storage::music_folder();
        where = storage::music_folder_name();
        if (ref.empty()) {
            publish(Found::Unreadable, where, "There is no shared storage");
            return true;
        }
    } else if (!picked().empty()) {
        ref = picked();
        where = picked_name();
        if (!storage::folder_granted(ref)) {
            publish(Found::PickLost, where, "The chosen folder cannot be read now: choose it again");
            return true;
        }
    } else {
        publish(Found::NoAccess, storage::music_folder_name(),
                "Allow access to music to play songs from " + storage::music_folder_name() + ", or choose the folder");
        return true;
    }
    g.stop = false;
    g.scanning = true;
    g.thread = std::thread(scan_main, ref, where);
    return true;
}

bool scanning()
{
    return g.scanning;
}

std::shared_ptr<const Scan> last()
{
    std::lock_guard<std::mutex> lk(g.m);
    return g.last;
}

storage::Permission permission()
{
    storage::Permission p = storage::permission();
    return p == storage::Permission::Ask && g.refused ? storage::Permission::Refused : p;
}

bool ask()
{
    if (g.asking || storage::permission() == storage::Permission::Granted) return false;
    g.answer = kNoAnswer;
    g.asking = storage::request_permission(on_answer);
    return g.asking;
}

bool asking()
{
    return g.asking;
}

bool pick()
{
    if (g.picking) return false;
    g.picking = storage::pick_folder();
    return g.picking;
}

bool picking()
{
    return g.picking;
}

std::string picked()
{
    return settings::get_string(kFolderKey, "");
}

std::string picked_name()
{
    std::string ref = picked();
    return ref.empty() ? ref : storage::ref_name(ref);
}

void forget_picked()
{
    settings::erase(kFolderKey);
}

bool update()
{
    bool changed = false;
    if (g.asking) {
        int a = g.answer.exchange(kNoAnswer);
        if (a != kNoAnswer) {
            g.asking = false;
            g.refused = a == kRefused;
            changed = true;
            if (a == kGranted) scan();
        }
    }
    std::string ref;
    if (g.picking && storage::take_folder(ref)) {
        g.picking = false;
        changed = true;
        if (!ref.empty()) {
            settings::set_string(kFolderKey, ref.c_str());
            scan();
        }
    }
    if (g.again && !g.scanning) changed |= scan();
    return changed;
}

void shutdown()
{
    g.stop = true;
    if (g.thread.joinable()) g.thread.join();
    g.stop = false;
}

}
