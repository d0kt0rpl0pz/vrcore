/*
 * selftest.cpp -- THE SELF TEST (--self-test SECONDS DIR runs on the Mac sim and on the
 * headset): a timed run of the game, then DIR/result.txt gets one line, "OK" or "FAIL" first, and the exit code is 0
 * or 3 (2: the game did not run). The line: the game frame and its rate, the three vr_stats counters, the game's
 * own part (Hooks::extra), the host's display rate, the sound's device and counters. The first counter must be 0.
 *
 * - A timed run saves the board's screen as DIR/picture_fNNNNN.png every 300 game frames (the first 20).
 * - Held (the game holds at a frame): the board's screen of that frame is saved as DIR/picture_fNNNNN.png and hashed
 *   (FNV-1a 64, top row first, each pixel 0xAARRGGBB low byte first), and with --expect HASH the run fails if they
 *   differ.
 * - No --nvram: the game's saves are not kept (a test must not touch the player's saves): nvram_off.
 */
#include "selftest.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "app.h"
#include "audio/audio.h"
#include "log.h"

namespace vrcore {
namespace selftest {

namespace {

// The options (they stay across a restart) and the run.
struct Args {
    float seconds = 0;                          // --self-test SECONDS DIR
    std::string dir;
    std::string expect;                         // --expect HASH: the held frame's screen hash it must have
} a;

struct Test {
    bool done = false;
    bool hold = false;
    double start = 0;
    uint32_t next_shot = 0;
    int shots = 0;
    long updates = 0;
} t;

constexpr int kMaxShots = 20;               // a long timed run keeps its first pictures only (each is 0.75 MB)

double now_s()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- the picture ----

// The screen hashed: top row first, each pixel 0xAARRGGBB low byte first (FNV-1a 64).
uint64_t screen_hash(const std::vector<uint8_t> &scr, int w, int h)
{
    uint64_t x = 0xcbf29ce484222325ULL;
    for (int y = h - 1; y >= 0; --y)
        for (int i = 0; i < w; ++i) {
            const uint8_t *p = &scr[(size_t(y) * w + i) * 4];
            x = (x ^ p[2]) * 0x100000001b3ULL;
            x = (x ^ p[1]) * 0x100000001b3ULL;
            x = (x ^ p[0]) * 0x100000001b3ULL;
            x = (x ^ p[3]) * 0x100000001b3ULL;
        }
    return x;
}

uint32_t crc32_update(uint32_t crc, const uint8_t *d, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}

void chunk(std::vector<uint8_t> &png, const char *type, const std::vector<uint8_t> &data)
{
    put32(png, (uint32_t)data.size());
    size_t at = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    put32(png, crc32_update(0, &png[at], png.size() - at));
}

// An RGBA screen (bottom row first) as a PNG, top row first: zlib with stored (uncompressed) blocks, so no library.
bool save_png(const std::string &path, const std::vector<uint8_t> &scr, int w, int h)
{
    std::vector<uint8_t> raw;
    raw.reserve(size_t(w) * h * 4 + h);
    for (int y = h - 1; y >= 0; --y) {
        raw.push_back(0);                                   // filter: none
        raw.insert(raw.end(), &scr[size_t(y) * w * 4], &scr[size_t(y) * w * 4] + w * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t c : raw) {
        s1 = (s1 + c) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    for (size_t at = 0; at < raw.size(); at += 65535) {
        size_t n = raw.size() - at < 65535 ? raw.size() - at : 65535;
        z.push_back(at + n >= raw.size());
        z.push_back((uint8_t)n);
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n);
        z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    }
    put32(z, (s2 << 16) | s1);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}, ihdr;
    put32(ihdr, (uint32_t)w);
    put32(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});               // 8 bit RGBA
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

// The game's newest picture of the board's screen (Hooks::screen; vr_picture as it comes without): the frame it is
// (0: none yet), its size in w and h.
uint32_t screen(const Hooks &h, std::vector<uint8_t> &scr, int &w, int &ht)
{
    if (h.screen) return h.screen(scr, w, ht);
    uint8_t none[4];
    int pw = 0, ph = 0;
    uint32_t f = h.plugin->picture(none, 0, 0, &pw, &ph);   // too small to copy: the frame and the size
    if (!f || pw <= 0 || ph <= 0) return 0;
    scr.resize(size_t(pw) * ph * 4);
    f = h.plugin->picture(scr.data(), (int)scr.size(), 0, &pw, &ph);
    if (!f || size_t(pw) * ph * 4 != scr.size()) return 0;     // it changed size between the calls: not copied
    w = pw;
    ht = ph;
    return f;
}

void save_screen(const Hooks &h, const std::vector<uint8_t> &scr, int w, int ht, uint32_t frame)
{
    char name[64];
    std::snprintf(name, sizeof name, "/picture_f%05u.png", frame);
    std::string path = a.dir + name;
    if (!save_png(path, scr, w, ht)) log("%s self test: could not write %s", h.tag, path.c_str());
}

// ---- the result ----

void write_result(const Hooks &h, const std::string &line)
{
    std::string path = a.dir + "/result.txt";
    if (std::FILE *f = std::fopen(path.c_str(), "w")) {
        std::fputs(line.c_str(), f);
        std::fclose(f);
    } else {
        log("%s self test: could not write %s", h.tag, path.c_str());
    }
}

void finish(const Hooks &h, bool ok, const std::string &what)
{
    if (t.done) return;
    t.done = true;
    double secs = now_s() - t.start;
    uint32_t fr = 0, ended = 0, resyncs = 0, late = 0;
    h.plugin->stats(&fr, &ended, &resyncs, &late);
    ok &= ended == 0 && h.plugin->state() == VR_RUNNING;
    audio::Stats au = audio::stats(false);
    std::string extra = h.extra ? ", " + h.extra() : "";
    char line[1024];
    std::snprintf(line, sizeof line,
                  "%s %s%s%.1f s, game frame %u (%.3f frames/s), %s %u %s %u %s %u%s, host %.1f fps, audio %d Hz, "
                  "callbacks %llu, blocks %llu\n",
                  ok ? "OK" : "FAIL", what.c_str(), what.empty() ? "" : "; ", secs, fr, fr / secs, h.counters[0], ended,
                  h.counters[1], resyncs, h.counters[2], late, extra.c_str(), t.updates / secs, au.hz,
                  (unsigned long long)au.callbacks, (unsigned long long)au.blocks);
    write_result(h, line);
    std::string shown = line;
    while (!shown.empty() && shown.back() == '\n') shown.pop_back();
    log("%s self test: %s", h.tag, shown.c_str());
    request_quit(ok ? 0 : 3);
}

}

int option(const Hooks &h, int argc, char **argv, int i)
{
    const char *o = argv[i];
    if (!std::strcmp(o, "--self-test")) {
        char *end = nullptr;
        float secs = i + 1 < argc ? std::strtof(argv[i + 1], &end) : 0.f;
        if (i + 2 >= argc || end == argv[i + 1] || *end || secs <= 0.f) {
            log("%s --self-test takes SECONDS (above 0) and a folder", h.tag);
            return -1;
        }
        a.seconds = secs;
        a.dir = argv[i + 2];
        return 3;
    }
    if (!std::strcmp(o, "--expect")) {
        if (i + 1 >= argc) {
            log("%s --expect needs a hash", h.tag);
            return -1;
        }
        char *end = nullptr;
        std::strtoull(argv[i + 1], &end, 16);
        if (end == argv[i + 1] || *end) {
            log("%s --expect takes a hex hash, not %s", h.tag, argv[i + 1]);
            return -1;
        }
        a.expect = argv[i + 1];
        return 2;
    }
    return 0;
}

bool on() { return a.seconds > 0; }

void prepare(const Hooks &h)
{
    if (!on()) return;
    SDL_CreateDirectory(a.dir.c_str());
    t.hold = h.held && h.held();
    if (!t.hold && h.prepare_timed) h.prepare_timed();      // the timed run's presses
}

bool nvram_off(const std::string &nvram) { return on() && nvram.empty(); }

void start(const Hooks &h)
{
    if (!on()) return;
    t = Test{false, t.hold, now_s(), 0, 0, 0};
    log("%s self test: %.0f s into %s, %s", h.tag, a.seconds, a.dir.c_str(), t.hold ? "held" : h.timed);
}

void fail(const Hooks &h, const std::string &why)
{
    if (!on() || t.done) return;
    t.done = true;
    log("%s self test: FAIL %s", h.tag, why.c_str());
    write_result(h, "FAIL " + why + "\n");
    request_quit(2);
}

void update(const Hooks &h)
{
    if (!on() || t.done) return;
    ++t.updates;
    double secs = now_s() - t.start;
    bool out = secs >= a.seconds;
    if (t.hold) {
        if (!h.holding || !h.holding()) {
            if (out) {
                char why[64];
                std::snprintf(why, sizeof why, "not held after %.1f s", secs);
                finish(h, false, why);
            }
            return;
        }
        std::vector<uint8_t> scr;
        int w = 0, ht = 0;
        uint32_t f = screen(h, scr, w, ht);
        if (!f) {
            if (out) finish(h, false, "held, but the plugin has no picture of the board's size");
            return;
        }
        uint64_t hash = screen_hash(scr, w, ht);
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)hash);
        save_screen(h, scr, w, ht, f);
        bool ok = a.expect.empty() || std::strtoull(a.expect.c_str(), nullptr, 16) == hash;   // 0x, case, leading zeros: as a number
        char what[160];
        std::snprintf(what, sizeof what, "held at frame %u: last screen hash %s%s%s%s", f, hex,
                      a.expect.empty() ? "" : " (expected ", a.expect.c_str(), a.expect.empty() ? "" : ")");
        finish(h, ok, what);
        return;
    }
    std::vector<uint8_t> scr;
    uint32_t f = 0;
    uint32_t fr = 0, c0, c1, c2;
    h.plugin->stats(&fr, &c0, &c1, &c2);
    if (fr >= t.next_shot && t.shots < kMaxShots) {         // by game frame
        t.next_shot = (fr / 300 + 1) * 300;                 // also when there is no picture yet: not asked every frame
        int w = 0, ht = 0;
        if ((f = screen(h, scr, w, ht))) {
            save_screen(h, scr, w, ht, f);
            ++t.shots;
        }
    }
    if (out) finish(h, true, "");
}

}
}
