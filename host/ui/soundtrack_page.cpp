/*
 * soundtrack_page.cpp -- soundtrack_page.h: the saved choices, the playlist's order, and the rows over
 * audio::soundtrack (the engine) and audio::library (the folder).
 *
 * THE ORDER. The engine plays a list and moves on at a track's end, wrapping. In order hands it the playlist; Shuffle
 * a permutation made once (the chosen track first); Repeat one a list of that one track. `g.order` maps the engine's
 * index back to the playlist's. Changing playlist, mode or track makes the list again. The focus coming back plays
 * the same list from the same place. A scan that finds the playlist's files changed renumbers the list with nothing
 * stopped (soundtrack::retarget) while the track heard is still there, and starts again when it is not.
 *
 * WHAT IS SAVED is the file the engine says it is playing (poll_current), compared with the saved name, not with
 * `g.cur`, which a choice sets before the engine has moved. The engine's indices count only once it has taken the
 * list they index (`g.list`).
 */
#include "soundtrack_page.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

#include "../audio/library.h"
#include "../audio/soundtrack.h"
#include "log.h"
#include "settings.h"
#include "text.h"
#include "ui/textstrings.h"

namespace vrcore::ui {

namespace {

namespace st = settings;
namespace lib = audio::library;
namespace snd = audio::soundtrack;

constexpr int kWindow = 10;                 // tracks a window of the Tracks page shows
constexpr size_t kLabelChars = 44;          // a row's text (the font is fixed width: the panel does not grow)
constexpr double kNowPlayingS = 3.0;
enum { kInOrder = 0, kShuffle = 1, kRepeatOne = 2 };
const char *const kModes[] = {txt::soundtrack_mode_in_order, txt::soundtrack_mode_shuffle,
                              txt::soundtrack_mode_repeat_one};

struct State {
    SoundtrackHooks hooks;
    bool on = false;
    int volume = 80;
    int mode = kShuffle;
    bool now_playing = true;
    std::string playlist;                   // the chosen one's name
    std::string track;                      // the last track's file name

    std::shared_ptr<const lib::Scan> scan;  // what the rows read
    int pl = -1;                            // the chosen playlist's index in scan
    std::vector<int> order;                 // the engine's list, as indices into the playlist
    uint32_t list = 0;                      // the engine's number for that list (soundtrack::Status::list)
    int cur = -1;                           // the playlist's track heard (-1 none)
    bool started = false;                   // the engine was given a list for this on-state
    bool suspended = false;                 // stopped for lack of focus
    bool gave_up = false;                   // every track failed
    bool replacing = false;                 // as last told to the front
    bool wanted_scan = false;               // a scan is wanted once the library is not busy
    bool tried_launch = false;
    bool warned = false;                    // a failed start was logged
    double retry_at = 0;                    // when a saved-on start that did not take is tried again
    int window = 0;                         // the Tracks page's first row (a track index)
    double now = 0, note_until = 0;
    std::string note;
    std::mt19937 rng{(unsigned)std::chrono::steady_clock::now().time_since_epoch().count()};
};
State g;

// The pages the row opens: kept for the run (Item::sub points at them).
Page g_page, g_tracks;
Item *g_playlist_row = nullptr;

std::string key(const char *name) { return std::string(g.hooks.prefix) + "soundtrack." + name; }

std::string clip(const std::string &s, size_t width = kLabelChars)
{
    if (s.size() <= width) return s;
    return s.substr(0, width - 3) + "...";
}

const lib::Playlist *playlist()
{
    if (!g.scan || g.pl < 0 || g.pl >= (int)g.scan->playlists.size()) return nullptr;
    return &g.scan->playlists[g.pl];
}

int tracks() { return playlist() ? (int)playlist()->tracks.size() : 0; }

bool can_play() { return g.scan && g.scan->found == lib::Found::Ok && tracks() > 0; }

void update_replacing()
{
    // not yet started (launch, before the scan): the game's music stays until the first track is known
    const bool r = g.on && g.started && !g.gave_up && can_play();
    if (r == g.replacing) return;
    g.replacing = r;
    if (g.hooks.options_changed) g.hooks.options_changed();
}

void apply_volume()
{
    snd::set_volume(g.volume, g.hooks.master_volume ? g.hooks.master_volume() : 100);
}

std::vector<std::string> order_refs(const lib::Playlist &p)
{
    std::vector<std::string> refs;
    for (int i : g.order) refs.push_back(p.tracks[i].ref);
    return refs;
}

// The engine (on g.order) played from its entry `at`.
void start(int at)
{
    const lib::Playlist *p = playlist();
    if (!p || at < 0 || at >= (int)g.order.size()) return;
    const int first = g.order[at];
    g.gave_up = false;
    g.started = snd::play(at);
    // not started (the audio device opens after the page's first scan at launch): begin() tries again each second,
    // and the log says so once
    if (g.started || !g.warned)
        log("soundtrack: \"%s\" track %d of %d, %s: %s", p->name.c_str(), first + 1, (int)p->tracks.size(),
            kModes[g.mode], g.started ? "playing" : "could not start yet (no sound device?)");
    g.warned = !g.started;
    g.cur = g.started ? first : -1;
    g.suspended = false;
    apply_volume();
}

// The engine's list for `first` (a playlist index), and `first` played.
void make_order(int first)
{
    const lib::Playlist *p = playlist();
    if (!p || p->tracks.empty()) return;
    const int n = (int)p->tracks.size();
    first = std::clamp(first, 0, n - 1);
    g.order.clear();
    int at = first;
    if (g.mode == kRepeatOne) {
        g.order = {first};
        at = 0;
    } else {
        g.order.resize(n);
        std::iota(g.order.begin(), g.order.end(), 0);
        if (g.mode == kShuffle) {
            std::shuffle(g.order.begin(), g.order.end(), g.rng);
            std::iter_swap(g.order.begin(), std::find(g.order.begin(), g.order.end(), first));
            at = 0;
        }
    }
    g.list = snd::set_tracks(order_refs(*p));
    start(at);
}

int find_track(const std::string &file)
{
    const lib::Playlist *p = playlist();
    if (!p) return -1;
    for (size_t i = 0; i < p->tracks.size(); ++i)
        if (p->tracks[i].file == file) return (int)i;
    return -1;
}

// Switched on, or a scan arrived while on: begin with the saved track (a random one when shuffling and none saved).
void begin()
{
    if (!g.on || !can_play() || g.started || g.suspended) return;
    int first = find_track(g.track);
    if (first < 0) first = g.mode == kShuffle ? (int)(g.rng() % (unsigned)tracks()) : 0;
    make_order(first);
    update_replacing();
}

void select_playlist_by_name()
{
    g.pl = -1;
    if (!g.scan) return;
    // a folder's name (the loose files are saved as "": no folder can have that name, kLoose included)
    for (size_t i = 0; !g.playlist.empty() && i < g.scan->playlists.size(); ++i)
        if (!g.scan->playlists[i].loose && g.scan->playlists[i].name == g.playlist) g.pl = (int)i;
    // the loose files (or the saved folder is gone), else the first
    for (size_t i = 0; g.pl < 0 && i < g.scan->playlists.size(); ++i)
        if (g.scan->playlists[i].loose) g.pl = (int)i;
    if (g.pl < 0 && !g.scan->playlists.empty()) g.pl = 0;
}

void stop_all()
{
    snd::stop();
    g.started = false;
    g.cur = -1;
    g.order.clear();
}

void set_on(bool on)
{
    if (on == g.on) return;
    g.on = on;
    st::set_bool(key("on").c_str(), on);
    if (on) {
        g.gave_up = false;
        lib::scan();
        g.wanted_scan = false;
        begin();
    } else {
        stop_all();
    }
    update_replacing();
}

void play_track(int i)
{
    if (!can_play()) return;
    if (!g.on) {
        g.on = true;
        st::set_bool(key("on").c_str(), true);
    }
    make_order(i);
    update_replacing();
}

void step(int by)
{
    if (!g.started || tracks() == 0) return;
    const int n = tracks();
    if (g.mode == kRepeatOne) {
        play_track(((g.cur < 0 ? 0 : g.cur) + by + n) % n);
        return;
    }
    snd::Status s = snd::status();
    if (by > 0) {
        snd::next();
    } else if (s.tracks > 0) {
        snd::play(((s.track < 0 ? 0 : s.track) + s.tracks - 1) % s.tracks);
    }
}

// Each frame: which playlist track the engine is on, saved and announced when it changes.
void poll_current()
{
    if (!g.started || g.suspended) return;
    snd::Status s = snd::status();
    if (s.list != g.list) return;           // the engine has not taken g.order yet
    if (s.tracks > 0 && s.failed >= s.tracks && !s.playing) {
        if (!g.gave_up) log("soundtrack: none of the %d tracks could be played", s.tracks);
        g.gave_up = true;
        return;
    }
    if (s.track < 0 || s.track >= (int)g.order.size()) return;
    const int c = g.order[s.track];
    const lib::Playlist *p = playlist();
    if (!p || c >= (int)p->tracks.size()) return;
    g.cur = c;
    const std::string &file = p->tracks[c].file;
    if (file == g.track && !g.note.empty()) return;
    g.track = file;
    st::set_string(key("track").c_str(), g.track.c_str());
    g.note = fmt(txt::soundtrack_now_playing_note, {{"title", clip(p->tracks[c].label())}});
    if (g.now_playing) g.note_until = g.now + kNowPlayingS;
}

// The focus is back: the same list from the same place (Shuffle's tracks to come still to come).
void resume()
{
    auto it = std::find(g.order.begin(), g.order.end(), g.cur);
    if (g.started && it != g.order.end()) {
        start((int)(it - g.order.begin()));
        return;
    }
    g.started = false;
    begin();
}

// A new scan while started: `was` the playlist's refs before it, `order_was` the engine's list, `heard` the track
// heard. Unchanged files: nothing to do. The track heard still there: the list made again round it and the engine
// renumbered with nothing stopped (Shuffle keeps its order, the new tracks after it). Gone: stopped (take_scan's
// begin() starts the playlist again).
void follow(const std::vector<std::string> &was, const std::vector<std::string> &order_was, const std::string &heard)
{
    const lib::Playlist *p = playlist();
    std::vector<std::string> now;
    if (p)
        for (const lib::Track &t : p->tracks) now.push_back(t.ref);
    if (now == was) return;
    auto index = [&](const std::string &ref) {
        auto it = std::find(now.begin(), now.end(), ref);
        return it == now.end() ? -1 : (int)(it - now.begin());
    };
    const int c = heard.empty() ? -1 : index(heard);
    if (c < 0) {
        log("soundtrack: the folder changed and the track playing is gone: starting again");
        stop_all();
        return;
    }
    const int n = (int)now.size();
    std::vector<int> order;
    if (g.mode == kRepeatOne) {
        order = {c};
    } else if (g.mode == kInOrder) {
        order.resize(n);
        std::iota(order.begin(), order.end(), 0);
    } else {
        std::vector<bool> in(n, false);
        for (const std::string &ref : order_was) {
            const int i = index(ref);
            if (i >= 0 && !in[i]) order.push_back(i), in[i] = true;
        }
        std::vector<int> added;
        for (int i = 0; i < n; ++i)
            if (!in[i]) added.push_back(i);
        std::shuffle(added.begin(), added.end(), g.rng);
        order.insert(order.end(), added.begin(), added.end());
    }
    g.order = order;
    g.cur = c;
    g.list = snd::retarget(order_refs(*p));
    log("soundtrack: the folder changed: \"%s\" has %d tracks, still playing track %d", p->name.c_str(), n, c + 1);
}

std::string status_text()
{
    if (lib::asking()) return txt::soundtrack_status_asking;
    if (lib::picking()) return txt::soundtrack_status_picking;
    if (!g.scan) return lib::scanning() ? txt::soundtrack_status_scanning : txt::soundtrack_status_not_scanned;
    if (lib::scanning() && g.scan->found != lib::Found::Ok) return txt::soundtrack_status_scanning;
    switch (g.scan->found) {
    case lib::Found::Ok: return g.gave_up ? txt::soundtrack_status_gave_up : "";
    case lib::Found::NoAccess: return txt::soundtrack_status_no_access;
    case lib::Found::PickLost: return txt::soundtrack_status_pick_lost;
    case lib::Found::Missing: return txt::soundtrack_status_missing;
    case lib::Found::Empty: return txt::soundtrack_status_empty;
    case lib::Found::Unreadable: return txt::soundtrack_status_unreadable;
    }
    return "";
}

bool access_problem()
{
    return g.scan && (g.scan->found == lib::Found::NoAccess || g.scan->found == lib::Found::PickLost);
}

void build_pages(Menu &menu)
{
    g_page.clear();
    g_tracks.clear();

    Item on = toggle(txt::soundtrack_on_label, [] { return g.on; }, [](bool v) { set_on(v); });
    on.help = txt::soundtrack_on_help;
    g_page.push_back(on);

    // why there is nothing to play, and what can be done
    Item why = note(std::string(kLabelChars, 'x'), [] { return clip(status_text()); });
    why.shown = [] { return !status_text().empty(); };
    g_page.push_back(why);
    Item hint = note(std::string(kLabelChars, 'x'), [] { return std::string(txt::soundtrack_copy_hint); });
    hint.shown = [] { return g.scan && (g.scan->found == lib::Found::Missing || g.scan->found == lib::Found::Empty); };
    g_page.push_back(hint);
    Item allow = action(txt::soundtrack_allow_label, [] {
        lib::ask();
    });
    allow.shown = [] { return access_problem() && !lib::asking(); };
    allow.help = txt::soundtrack_allow_help;
    g_page.push_back(allow);

    const size_t pl_at = g_page.size();
    Item pl = choice(txt::soundtrack_playlist_label, {"-"}, [] { return std::max(g.pl, 0); },
                     [](int i) {
                         if (!g.scan || i < 0 || i >= (int)g.scan->playlists.size() || i == g.pl) return;
                         g.pl = i;
                         g.playlist = g.scan->playlists[i].loose ? "" : g.scan->playlists[i].name;
                         g.track.clear();
                         st::set_string(key("playlist").c_str(), g.playlist.c_str());
                         g.window = 0;
                         if (g.on) {
                             stop_all();
                             begin();
                         }
                     });
    // the dropdown lists the scan's names (choices, kept up to date by take_scan), so the value reads the live one
    pl.value = [] { return playlist() ? clip(playlist()->name, 24) : std::string("-"); };
    pl.shown = [] { return can_play(); };
    pl.gap = true;
    g_page.push_back(pl);

    Item tr = page(txt::soundtrack_tracks_label, g_tracks);
    tr.shown = [] { return can_play(); };
    g_page.push_back(tr);

    Item mode = choice(txt::soundtrack_mode_label, {kModes[0], kModes[1], kModes[2]}, [] { return g.mode; },
                       [](int m) {
                           g.mode = m;
                           st::set_int(key("mode").c_str(), m);
                           if (g.on && g.started && g.cur >= 0) make_order(g.cur);
                       });
    mode.shown = [] { return can_play(); };
    mode.help = txt::soundtrack_mode_help;
    g_page.push_back(mode);
    Item next = action(txt::soundtrack_next, [] { step(1); });
    next.shown = [] { return can_play() && g.started; };
    next.gap = true;
    g_page.push_back(next);
    Item prev = action(txt::soundtrack_previous, [] { step(-1); });
    prev.shown = [] { return can_play() && g.started; };
    g_page.push_back(prev);

    Item vol = stepper(txt::soundtrack_volume_label, [] { return (float)g.volume; },
                       [](float v) {
                           g.volume = (int)std::lround(v);
                           st::set_int(key("volume").c_str(), g.volume);
                           apply_volume();
                       },
                       5.f, 0.f, 100.f, [](float v) { return fmt(txt::soundtrack_volume_value, {{"v", std::to_string((int)std::lround(v))}}); });
    vol.gap = true;
    g_page.push_back(vol);
    g_page.push_back(toggle(txt::soundtrack_now_playing_label, [] { return g.now_playing; }, [](bool v) {
        g.now_playing = v;
        st::set_bool(key("nowplaying").c_str(), v);
    }));
    g_page.back().help = txt::soundtrack_now_playing_help;
    g_page.push_back(menu.return_item());
    g_page.back().gap = true;
    g_playlist_row = &g_page[pl_at];

    // the Tracks window. Its rows never come and go as it scrolls (the cursor would land a row off): Earlier is a
    // blank note at the top, Later one at the bottom.
    Item top = note(txt::soundtrack_earlier, [] { return std::string(); });
    top.shown = [] { return g.window == 0; };
    g_tracks.push_back(top);
    Item up = action(txt::soundtrack_earlier, [] { g.window = std::max(0, g.window - kWindow); });
    up.shown = [] { return g.window > 0; };
    g_tracks.push_back(up);
    for (int r = 0; r < kWindow; ++r) {
        Item row;
        row.label = std::string(kLabelChars + 4, 'x');
        row.label_fn = [r] {
            const lib::Playlist *p = playlist();
            const int i = g.window + r;
            if (!p || i >= (int)p->tracks.size()) return std::string();
            char num[16];
            std::snprintf(num, sizeof num, "%3d. ", i + 1);
            const std::string head = std::string(i == g.cur ? "* " : "  ") + num;
            return head + clip(p->tracks[i].label(), kLabelChars + 4 - std::min(head.size(), kLabelChars));
        };
        row.press = [r] { play_track(g.window + r); };
        row.shown = [r] { return g.window + r < tracks(); };
        if (r == 0)
            row.edge = [](int d) {
                if (d < 0 && g.window > 0) return --g.window, true;
                return false;
            };
        if (r == kWindow - 1)
            row.edge = [](int d) {
                if (d > 0 && g.window + kWindow < tracks()) return ++g.window, true;
                return false;
            };
        g_tracks.push_back(row);
    }
    Item down = action(txt::soundtrack_later, [] { g.window = std::min(g.window + kWindow, std::max(0, tracks() - kWindow)); });
    down.shown = [] { return g.window + kWindow < tracks(); };
    g_tracks.push_back(down);
    Item bottom = note(txt::soundtrack_later, [] { return std::string(); });
    bottom.shown = [] { return g.window + kWindow >= tracks(); };
    g_tracks.push_back(bottom);
    Item count = note(std::string(25, 'x'), [] {
        const int n = tracks();
        return fmt(txt::soundtrack_showing, {{"first", std::to_string(std::min(n, g.window + 1))},
                                             {"last", std::to_string(std::min(n, g.window + kWindow))},
                                             {"count", std::to_string(n)}});
    });
    count.gap = true;
    g_tracks.push_back(count);
    g_tracks.push_back(menu.return_item());
}

void take_scan()
{
    std::shared_ptr<const lib::Scan> s = lib::last();
    if (s == g.scan) return;
    // what the engine plays, in the old scan's refs: follow() keeps it going on the new one when it can
    std::vector<std::string> was, order_was;
    std::string heard;
    if (const lib::Playlist *p = playlist()) {
        for (const lib::Track &t : p->tracks) was.push_back(t.ref);
        order_was = order_refs(*p);
        if (g.cur >= 0 && g.cur < (int)p->tracks.size()) heard = p->tracks[g.cur].ref;
    }
    g.scan = s;
    select_playlist_by_name();
    if (g_playlist_row) {
        g_playlist_row->choices.clear();
        if (g.scan)
            for (const lib::Playlist &p : g.scan->playlists) g_playlist_row->choices.push_back(clip(p.name, 24));
        if (g_playlist_row->choices.empty()) g_playlist_row->choices.push_back("-");
    }
    if (g.started) {
        if (can_play()) follow(was, order_was, heard);
        else stop_all();
    }
    const int n = tracks();
    g.window = std::clamp(g.window, 0, std::max(0, n - kWindow));
    begin();
    update_replacing();
}

}

void soundtrack_load(const SoundtrackHooks &hooks)
{
    g.hooks = hooks;
    g.on = st::get_bool(key("on").c_str(), false);
    g.volume = std::clamp(st::get_int(key("volume").c_str(), 80), 0, 100);
    g.mode = std::clamp(st::get_int(key("mode").c_str(), kShuffle), 0, 2);
    g.now_playing = st::get_bool(key("nowplaying").c_str(), true);
    g.playlist = st::get_string(key("playlist").c_str(), "");
    g.track = st::get_string(key("track").c_str(), "");
}

Item soundtrack_item(Menu &menu, const char *label)
{
    build_pages(menu);
    return page(label, g_page);
}

void soundtrack_menu_opened()
{
    // scanned each time the menu opens, but only a way in that is already there: the permission is asked for by
    // its row, never here
    if (lib::scan()) g.wanted_scan = false;
    else g.wanted_scan = true;
    const int c = g.cur >= 0 ? g.cur : 0;
    g.window = std::clamp(c - kWindow / 2, 0, std::max(0, tracks() - kWindow));
}

std::string soundtrack_now_playing() { return g.note; }

bool soundtrack_replacing() { return g.replacing; }

void soundtrack_update(Menu &menu, const Tick &t, const Input &in)
{
    g.now = (double)t.display_ns * 1e-9;

    // at launch: a saved-on soundtrack is scanned for, once, when the folder can be read without asking
    if (!g.tried_launch) {
        g.tried_launch = true;
        if (g.on) lib::scan();
    }
    if (menu.is_open() || lib::asking() || lib::picking() || lib::scanning()) lib::update();
    if (g.wanted_scan && lib::scan()) g.wanted_scan = false;
    take_scan();
    // saved on: the first scan can end before the audio device is open (play fails); try again each second
    if (g.on && !g.started && !g.suspended && !g.gave_up && can_play() && g.now >= g.retry_at) {
        g.retry_at = g.now + 1.0;
        begin();
    }

    // the headset's focus: stopped without it, the track from its start with it
    if (!t.focused && g.started && !g.suspended) {
        snd::stop();
        g.suspended = true;
    } else if (t.focused && g.suspended) {
        g.suspended = false;
        if (g.on && can_play()) resume();
    }

    apply_volume();
    poll_current();
    update_replacing();

    if (g.now < g.note_until && !g.note.empty() && !menu.is_open()) {
        const Quat &q = in.head.rot;
        // forward on the floor plane: -z turned by the head's rotation
        float fx = 2.f * (q.x * q.z + q.w * q.y) * -1.f, fz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
        float len = std::sqrt(fx * fx + fz * fz);
        if (len < 1e-4f) { fx = 0; fz = -1; len = 1; }
        fx /= len;
        fz /= len;
        const float pos[3] = {in.head.pos[0] + fx * 1.0f, in.head.pos[1] - 0.32f, in.head.pos[2] + fz * 1.0f};
        const float yaw = std::atan2(-fx, -fz);
        menu.note(g.note, pos, Quat{0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)}, 0.045f);
    }
}

}
