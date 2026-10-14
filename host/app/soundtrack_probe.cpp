/*
 * soundtrack_probe.cpp -- soundtrack_probe.h: one state at a time, moved on by the library's answers.
 */
#include "soundtrack_probe.h"

#include <string>
#include <vector>

#include "../audio/library.h"
#include "../audio/soundtrack.h"
#include "log.h"

namespace vrcore {

namespace {

namespace lib = audio::library;
namespace st = audio::soundtrack;

enum class Step { Start, Scanning, Asking, Picking, Playing, Done };
Step g_step = Step::Start;
double g_at = 0;                // when the step began
double g_logged = 0;            // the last status line (Playing)
int g_heard = -2;
int g_failed = 0;

const char *permission_name(audio::storage::Permission p)
{
    switch (p) {
    case audio::storage::Permission::Granted: return "granted";
    case audio::storage::Permission::Ask: return "not asked";
    case audio::storage::Permission::Refused: return "refused";
    }
    return "?";
}

void step(Step s, double now)
{
    g_step = s;
    g_at = now;
}

// The scan's result logged, and the next step.
void scanned(double now)
{
    std::shared_ptr<const lib::Scan> s = lib::last();
    if (!s) return step(Step::Done, now);
    log("soundtrack probe: scan of %s: %d tracks, %zu playlists, %.2f s%s%s", s->where.c_str(), s->tracks,
        s->playlists.size(), s->seconds, s->text.empty() ? "" : "; the page would say: ", s->text.c_str());
    for (const lib::Playlist &p : s->playlists) {
        log("soundtrack probe:   playlist \"%s\"%s: %zu tracks", p.name.c_str(), p.loose ? " (loose)" : "",
            p.tracks.size());
        for (const lib::Track &t : p.tracks)
            log("soundtrack probe:     %s  [%s | title \"%s\", artist \"%s\"]", t.label().c_str(), t.file.c_str(),
                t.title.c_str(), t.artist.c_str());
    }
    switch (s->found) {
    case lib::Found::Ok: {
        std::vector<std::string> refs;
        for (const lib::Track &t : s->playlists[0].tracks) refs.push_back(t.ref);
        st::set_tracks(refs);
        bool ok = st::play(0);
        log("soundtrack probe: playing \"%s\" from its first track: %s", s->playlists[0].name.c_str(),
            ok ? "started" : "could not start (no sound device?)");
        return step(ok ? Step::Playing : Step::Done, now);
    }
    case lib::Found::NoAccess:
        if (lib::permission() != audio::storage::Permission::Refused && lib::ask()) {
            log("soundtrack probe: asking for the permission");
            return step(Step::Asking, now);
        }
        [[fallthrough]];
    case lib::Found::PickLost:
        if (lib::pick()) {
            log("soundtrack probe: opening the folder picker");
            return step(Step::Picking, now);
        }
        log("soundtrack probe: no folder picker here");
        return step(Step::Done, now);
    default:
        return step(Step::Done, now);
    }
}

}

void soundtrack_probe_poll(double now)
{
    lib::update();
    switch (g_step) {
    case Step::Start:
        log("soundtrack probe: permission %s, chosen folder \"%s\"", permission_name(lib::permission()),
            lib::picked().c_str());
        lib::scan();
        step(Step::Scanning, now);
        break;
    case Step::Scanning:
        if (!lib::scanning()) scanned(now);
        break;
    case Step::Asking:
        if (lib::asking()) break;
        log("soundtrack probe: answered: permission %s", permission_name(lib::permission()));
        if (lib::scanning()) step(Step::Scanning, now);     // granted: update started the scan
        else if (lib::pick()) {
            log("soundtrack probe: refused: opening the folder picker");
            step(Step::Picking, now);
        } else {
            step(Step::Done, now);
        }
        break;
    case Step::Picking:
        if (lib::picking()) break;
        log("soundtrack probe: picker answered: \"%s\" (%s)", lib::picked_name().c_str(), lib::picked().c_str());
        step(lib::scanning() ? Step::Scanning : Step::Done, now);
        break;
    case Step::Playing: {
        st::Status s = st::status();
        if (s.track != g_heard || now - g_logged >= 5.0) {
            g_heard = s.track;
            g_logged = now;
            log("soundtrack probe: %s track %d of %d, %llu frames mixed (%llu dry), %llu decoded, %d failed",
                s.playing ? "playing" : "stopped", s.track, s.tracks, (unsigned long long)s.frames,
                (unsigned long long)s.dry_frames, (unsigned long long)s.decoded, s.failed);
        }
        if (s.failed != g_failed) {
            g_failed = s.failed;
            for (int i = 0; i < s.tracks; ++i)
                if (!st::failure(i).empty()) log("soundtrack probe:   track %d skipped: %s", i, st::failure(i).c_str());
        }
        if (!s.playing && now - g_at > 1.0) step(Step::Done, now);
        break;
    }
    case Step::Done:
        break;
    }
}

}
