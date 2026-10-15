/*
 * soundtrack_test.cpp -- the soundtrack's device-free checks, run by `--soundtrack-test <dir>` (Mac) before any host
 * starts. The songs are made when it runs (a tone a file, written as a WAV and encoded with afconvert; nothing is
 * committed) in <dir>/Soundtracks, which the library is pointed at. The audio callback's part is played by a thread
 * of the test, calling soundtrack::mix as a device would, so the engine, the ring and the page run as they do on
 * the headset, without a sound out.
 *
 * Each tone has its own pitch, so a decoded or mixed signal says which file it came from.
 */
#include "soundtrack_test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../audio/decode.h"
#include "../audio/library.h"
#include "../audio/soundtrack.h"
#include "log.h"
#include "settings.h"
#include "soundtrack_page.h"

namespace vrcore::ui {

namespace {

namespace fs = std::filesystem;
namespace st = settings;
namespace lib = audio::library;
namespace snd = audio::soundtrack;

constexpr int kHz = 48000;
constexpr int kSrcHz = 44100;               // the files' own rate: the resampler has work to do
constexpr double kSeconds = 1.0;

struct Report {
    std::FILE *f = nullptr;
    int pass = 0, fail = 0;

    void check(bool ok, const char *fmt, ...)
    {
        char buf[320];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        ++(ok ? pass : fail);
        log("soundtrack-test: %s %s", ok ? "PASS" : "FAIL", buf);
        if (f) std::fprintf(f, "%s %s\n", ok ? "PASS" : "FAIL", buf);
    }
};

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---- the songs ----

bool write_wav(const fs::path &p, double hz)
{
    const int n = (int)(kSrcHz * kSeconds);
    std::vector<int16_t> pcm(n);
    for (int i = 0; i < n; ++i) pcm[i] = (int16_t)std::lround(0.5 * 32767.0 * std::sin(6.283185307 * hz * i / kSrcHz));
    std::FILE *f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + n * 2);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(1), u32(kSrcHz), u32(kSrcHz * 2), u16(2), u16(16);
    std::fwrite("data", 1, 4, f);
    u32(n * 2);
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
    return true;
}

// `out`'s tone encoded from a WAV with afconvert, `fmt` and `data` its -f and -d; false when it did not.
bool encode(const fs::path &wav, const fs::path &out, const char *fmt, const char *data)
{
    const std::string cmd = "afconvert -f " + std::string(fmt) + " -d " + data + " '" + wav.string() + "' '" +
                            out.string() + "' >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0 && fs::exists(out);
}

bool touch(const fs::path &p, const char *text = "x")
{
    std::FILE *f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    std::fputs(text, f);
    std::fclose(f);
    return true;
}

// The pitch of the first channel of `x` (interleaved, `ch` wide) at `hz`, by its rising zero crossings.
double pitch(const std::vector<float> &x, int ch, int hz)
{
    const size_t n = x.size() / ch;
    int up = 0;
    for (size_t i = 1; i < n; ++i)
        if (x[(i - 1) * ch] < 0.f && x[i * ch] >= 0.f) ++up;
    return n ? up * (double)hz / (double)n : 0.0;
}

float peak(const std::vector<float> &x)
{
    float p = 0.f;
    for (float v : x) p = std::max(p, std::fabs(v));
    return p;
}

// The audio callback's part: mixes `block` frames of the soundtrack onto silence every `ms`, keeping what it heard.
class Reader {
public:
    Reader(int block, int ms) : block_(block), ms_(ms) { th_ = std::thread([this] { run(); }); }
    ~Reader() { stop(); }
    void stop()
    {
        quit_ = true;
        if (th_.joinable()) th_.join();
    }
    void record(bool on)
    {
        std::lock_guard<std::mutex> lk(m_);
        rec_ = on;
        heard_.clear();
    }
    std::vector<float> heard()
    {
        std::lock_guard<std::mutex> lk(m_);
        return heard_;
    }

private:
    void run()
    {
        std::vector<float> buf((size_t)block_ * 2);
        while (!quit_) {
            std::fill(buf.begin(), buf.end(), 0.f);
            const bool any = snd::mix(buf.data(), block_);
            {
                std::lock_guard<std::mutex> lk(m_);
                if (rec_ && any) heard_.insert(heard_.end(), buf.begin(), buf.end());
            }
            if (ms_ > 0) sleep_ms(ms_);
        }
    }
    const int block_, ms_;
    std::atomic<bool> quit_{false};
    std::thread th_;
    std::mutex m_;
    bool rec_ = false;
    std::vector<float> heard_;
};

// ---- the checks ----

void check_scan(Report &r, const fs::path &root, std::vector<lib::Playlist> *got)
{
    lib::set_folder(root.string());
    lib::scan();
    for (int i = 0; i < 500 && (lib::scanning() || !lib::last()); ++i) sleep_ms(10);
    auto s = lib::last();
    r.check(s && s->found == lib::Found::Ok, "scan finds the folder (%d tracks, %.3f s)", s ? s->tracks : -1,
            s ? s->seconds : 0.0);
    if (!s) return;
    std::string names;
    for (const lib::Playlist &p : s->playlists) names += (names.empty() ? "" : ", ") + p.name;
    r.check(s->playlists.size() == 3 && s->playlists[0].name == "Alpha" && s->playlists[1].name == "Beta" &&
                s->playlists[2].name == lib::kLoose && s->playlists[2].loose,
            "playlists in name order, loose last, the empty folder none: %s", names.c_str());
    if (s->playlists.size() != 3) return;
    std::string order;
    for (const lib::Track &t : s->playlists[2].tracks) order += (order.empty() ? "" : ", ") + t.file;
    r.check(s->playlists[2].tracks.size() == 2 && s->playlists[2].tracks[0].file.rfind("2 ", 0) == 0,
            "loose tracks by number value, hidden and non-audio files left out: %s", order.c_str());
    r.check(s->playlists[0].tracks.size() == 3, "a subfolder's tracks, nothing from a folder inside it (%zu)",
            s->playlists[0].tracks.size());
    r.check(s->tracks == 3 + 1 + 2, "the scan's total counts every playlist (%d)", s->tracks);
    *got = s->playlists;
}

// The tone of each file is its pitch: a file's name carries it ("440.m4a"), so the heard signal names the track.
double name_hz(const std::string &file) { return std::atof(file.c_str()); }

void check_decode(Report &r, const std::vector<lib::Playlist> &pls)
{
    for (const lib::Playlist &p : pls)
        for (const lib::Track &t : p.tracks) {
            std::string why;
            auto d = audio::open_decoder(t.ref.c_str(), &why);
            if (!d) {
                r.check(false, "decode %s: %s", t.file.c_str(), why.c_str());
                continue;
            }
            std::vector<float> all, buf(4096 * audio::kMaxDecodeChannels);
            int got;
            int ch = 0, hz = 0;
            while ((got = d->read(buf.data(), 4096)) > 0) {
                ch = d->channels();
                hz = d->hz();
                all.insert(all.end(), buf.begin(), buf.begin() + (size_t)got * ch);
            }
            const double secs = ch && hz ? (double)all.size() / ch / hz : 0.0;
            const double f = ch && hz ? pitch(all, ch, hz) : 0.0;
            const double want = name_hz(t.file);    // the loose files' names start with a number, not a pitch
            const bool tuned = want < 100 || std::fabs(f - want) < 0.02 * want;
            r.check(got == 0 && secs > 0.9 && secs < 1.2 && peak(all) > 0.3f && peak(all) <= 1.0f && tuned,
                    "decode %s: %.2f s at %d Hz x%d, peak %.2f, pitch %.0f Hz", t.file.c_str(), secs, hz, ch,
                    peak(all), f);
        }
}

void check_ring(Report &r, const lib::Playlist &p)
{
    snd::set_rate(kHz);
    std::vector<std::string> refs;
    for (const lib::Track &t : p.tracks) refs.push_back(t.ref);
    snd::set_tracks(refs);
    snd::set_volume(100, 100);
    // a slow reader: small blocks and long waits, so the ring fills and the streaming thread waits for it
    Reader rd(256, 12);
    rd.record(true);
    r.check(snd::play(0), "play(0) starts");
    sleep_ms(1500);
    snd::Status s = snd::status();
    const uint64_t ahead = s.decoded - s.frames;
    r.check(s.playing && s.decoded > s.frames && ahead <= (1u << 16),
            "slow reader: the ring holds what the reader has not taken, no more than its size (%llu frames ahead)",
            (unsigned long long)ahead);
    sleep_ms(1500);
    snd::Status s2 = snd::status();
    r.check(s2.dry_frames <= s.dry_frames + 256 && s2.frames > s.frames,
            "slow reader: no frame silent once it is fed (dry %llu -> %llu)", (unsigned long long)s.dry_frames,
            (unsigned long long)s2.dry_frames);
    std::vector<float> heard = rd.heard();
    const double f = pitch(heard, 2, kHz);
    r.check(!heard.empty() && std::fabs(f - name_hz(p.tracks[0].file)) < 0.04 * name_hz(p.tracks[0].file),
            "the mix carries track 0 at the device rate: %.0f Hz (the file's %.0f)", f, name_hz(p.tracks[0].file));
    // the volume: the soundtrack's times the master
    rd.record(true);
    snd::set_volume(50, 50);
    sleep_ms(300);
    rd.record(true);
    sleep_ms(200);
    const float quiet = peak(rd.heard());
    r.check(quiet > 0.05f && quiet < 0.5f * 0.25f * 1.3f, "volume 50 under master 50 plays at a quarter (peak %.3f)",
            quiet);
    snd::stop();
    sleep_ms(200);
    rd.record(true);
    sleep_ms(100);
    r.check(!snd::status().playing && peak(rd.heard()) == 0.f, "stop fades to silence and leaves the game's sound alone");
    rd.stop();
    snd::set_tracks({});
}

// ---- the page ----

struct Driver {
    Menu menu;
    Tick t;
    Input in;
    double s = 0;

    void tick(int n = 1)
    {
        for (int i = 0; i < n; ++i) {
            s += 0.011;
            t.display_ns = (int64_t)(s * 1e9);
            t.period_ns = 11000000;
            soundtrack_update(menu, t, in);
        }
    }
    bool until(const std::function<bool()> &done, int ms = 4000)
    {
        for (int i = 0; i < ms / 10; ++i) {
            tick();
            if (done()) return true;
            sleep_ms(10);
        }
        return done();
    }
};

std::string saved_track(const char *prefix) { return st::get_string((std::string(prefix) + "soundtrack.track").c_str(), ""); }

// The files heard in turn, from the track playing now, as `next` moves on `n` times.
std::vector<std::string> walk(Driver &d, Item &next, const char *prefix, int n)
{
    std::vector<std::string> seq;
    d.until([&] { return !saved_track(prefix).empty(); });
    seq.push_back(saved_track(prefix));
    for (int i = 1; i < n; ++i) {
        const std::string before = seq.back();
        next.change(+1);
        d.until([&] { return saved_track(prefix) != before; });
        seq.push_back(saved_track(prefix));
    }
    return seq;
}

Item *row(Page &page, const char *label)
{
    for (Item &it : page)
        if (it.label == label) return &it;
    return nullptr;
}

void check_page(Report &r, const fs::path &dir)
{
    const char *prefix = "t.";
    st::set_path((dir / "settings.txt").string().c_str());
    st::open("vrcore", "soundtrack-test");
    st::set_bool("t.soundtrack.on", true);
    st::set_int("t.soundtrack.mode", 1);                // shuffle
    st::set_string("t.soundtrack.playlist", "Alpha");
    st::set_bool("t.soundtrack.nowplaying", false);     // no panel: the menu was never started

    std::vector<int> music;                             // what a front would send VR_OPT_MUSIC_VOL, in turn
    const int saved_music = 70;
    SoundtrackHooks h;
    h.prefix = prefix;
    h.options_changed = [&] { music.push_back(soundtrack_replacing() ? 0 : saved_music); };
    soundtrack_load(h);

    Driver d;
    Item top = soundtrack_item(d.menu);
    Page &pg = *top.sub;
    Item *on = row(pg, "Custom soundtrack"), *next = row(pg, "Next"), *mode = row(pg, "Mode");
    r.check(on && next && mode && row(pg, "Tracks") && row(pg, "Playlist") && row(pg, "Volume"), "the page has its rows");
    if (!on || !next || !mode) return;

    // the device opens after the launch scan (as on the headset): the start that failed is tried again
    snd::set_rate(0);
    std::thread device([] {
        sleep_ms(1500);
        snd::set_rate(kHz);
    });
    Reader rd(512, 3);                                  // about 3.5x real time: a track a second, not 3.5 s
    // saved on at launch: scanned and started with nothing asked
    const bool started = d.until([&] { return !saved_track(prefix).empty() && soundtrack_replacing(); });
    device.join();
    r.check(started && !music.empty() && music.back() == 0,
            "saved on at launch, the device opening 1.5 s late: the game's music goes to 0 once a track plays (sent %zu, last %d)", music.size(),
            music.empty() ? -1 : music.back());

    // shuffle: Alpha's three tracks, each once, then the same cycle again
    std::vector<std::string> seq = walk(d, *next, prefix, 6);
    std::set<std::string> first(seq.begin(), seq.begin() + 3);
    std::set<std::string> all(seq.begin(), seq.end());
    std::string line;
    for (const std::string &s : seq) line += (line.empty() ? "" : ", ") + s;
    r.check(first.size() == 3 && all.size() == 3, "shuffle plays each of the 3 tracks before a repeat: %s", line.c_str());
    r.check(seq[3] == seq[0] && seq[4] == seq[1] && seq[5] == seq[2], "the shuffled order is made once and wraps");

    // in order
    mode->pick(0);
    d.tick(5);
    seq = walk(d, *next, prefix, 4);
    line.clear();
    for (const std::string &s : seq) line += (line.empty() ? "" : ", ") + s;
    const lib::Playlist &alpha = lib::last()->playlists[0];
    int at = -1;
    for (size_t i = 0; i < alpha.tracks.size(); ++i)
        if (alpha.tracks[i].file == seq[0]) at = (int)i;
    bool ordered = at >= 0;
    for (int i = 1; i < 4 && ordered; ++i) ordered = seq[i] == alpha.tracks[(at + i) % 3].file;
    r.check(ordered, "in order follows the playlist and wraps: %s", line.c_str());

    // repeat one: Next stays on the track
    mode->pick(2);
    d.tick(5);
    seq = walk(d, *next, prefix, 2);
    r.check(seq[0] == seq[1], "repeat one stays on its track: %s, %s", seq[0].c_str(), seq[1].c_str());

    // the game's music volume: 0 while it plays, back when it is off, 0 again when on
    music.clear();
    on->change(+1);                                     // off
    d.tick(5);
    r.check(!soundtrack_replacing() && !music.empty() && music.back() == saved_music,
            "switched off: the game's music is back at its saved %d (sent %zu)", saved_music, music.size());
    music.clear();
    on->change(+1);                                     // on again
    const bool again = d.until([&] { return soundtrack_replacing(); });
    r.check(again && !music.empty() && music.back() == 0, "switched on again: the game's music goes to 0 (sent %zu)",
            music.size());

    // headset focus: stopped without it, the track again with it
    d.until([&] { return snd::status().playing; });
    d.t.focused = false;
    d.tick(3);
    sleep_ms(100);
    const bool silent = !snd::status().playing;
    d.t.focused = true;
    const bool back = d.until([&] { return snd::status().playing; });
    r.check(silent && back, "headset off stops the music, on plays it again (%d, %d)", (int)silent, (int)back);

    rd.stop();
    snd::stop();
}

}

int soundtrack_test(const char *dir)
{
    std::error_code ec;
    const fs::path base = fs::absolute(dir);
    fs::remove_all(base, ec);
    const fs::path root = base / "Soundtracks", src = base / "src";
    fs::create_directories(src, ec);
    fs::create_directories(root / "Alpha" / "deep", ec);
    fs::create_directories(root / "Beta", ec);
    fs::create_directories(root / "Empty", ec);
    Report r;
    r.f = std::fopen((base / "soundtrack_test.txt").string().c_str(), "w");
    if (!r.f) {
        log("soundtrack-test: cannot write %s/soundtrack_test.txt", dir);
        return 1;
    }

    // the songs: each file named by its pitch (its tone), encoded with afconvert
    struct Song {
        double hz;
        const char *where, *name, *fmt, *data;
    };
    const Song songs[] = {
        {330, "", "2 two.wav", nullptr, nullptr},      {392, "", "10 ten.wav", nullptr, nullptr},
        {440, "Alpha", "440.m4a", "m4af", "aac"},      {523, "Alpha", "523.m4a", "m4af", "aac"},
        {659, "Alpha", "659.flac", "flac", "flac"},    {587, "Beta", "587.mp3", "MPG3", ".mp3"},
    };
    int made = 0;
    bool mp3 = true;
    for (const Song &s : songs) {
        const fs::path wav = src / (std::to_string((int)s.hz) + ".wav");
        const fs::path out = root / s.where / s.name;
        bool ok = write_wav(wav, s.hz) && (s.fmt ? encode(wav, out, s.fmt, s.data) : (fs::copy_file(wav, out, ec), true));
        if (!ok && s.fmt && !std::strcmp(s.fmt, "MPG3")) {
            // this Mac has no MP3 encoder: the file is a WAV under that name's place instead
            mp3 = false;
            ok = fs::copy_file(wav, root / s.where / "587.wav", ec);
        }
        made += ok;
    }
    r.check(made == 6, "the test songs were made with afconvert (%d of 6%s)", made, mp3 ? "" : "; no MP3 encoder: a WAV stands in");
    touch(root / ".hidden.wav");
    touch(root / "notes.txt");
    write_wav(root / "Alpha" / "deep" / "9.wav", 700);
    touch(root / "Empty" / "readme.txt");

    std::vector<lib::Playlist> pls;
    check_scan(r, root, &pls);
    if (pls.size() == 3) {
        check_decode(r, pls);
        check_ring(r, pls[0]);
        check_page(r, base);
    }

    lib::shutdown();
    snd::shutdown();
    std::fprintf(r.f, "%d passed, %d failed\n", r.pass, r.fail);
    std::fclose(r.f);
    log("soundtrack-test: %d passed, %d failed (%s/soundtrack_test.txt)", r.pass, r.fail, dir);
    return r.fail ? 1 : 0;
}

}
