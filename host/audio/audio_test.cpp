/*
 * audio_test.cpp -- the sound's device-free checks, run by `--audio-test <dir>` (Mac) before any host starts: a
 * made-up signal through the callback's own block path (audio::test_block: the pull, then Steam Audio or the
 * plain stereo) at set head poses. No device, no plugin, nothing played. One line a check in <dir>/audio_test.txt,
 * the renders as WAVs beside it (to listen to), a summary in the log.
 *
 * The speakers are a typical pair, left channel at (-0.50, -0.10, -0.50) m, right mirrored, unless a check says.
 */
#include "audio_test.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "../app/log.h"
#include "audio.h"

namespace vrcore::audio {

namespace {

constexpr int kHz = 48000;
constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.f;

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
        if (f) std::fprintf(f, "%s %s\n", ok ? "PASS" : "FAIL", buf);
        if (!ok) log("audio-test: FAIL %s", buf);
    }
};

// ---- the signal (the pull) ----

enum class Signal { Noise, Stereo, Sine };     // the same noise on both / each its own / a 440 Hz tone
struct Source {
    Signal kind = Signal::Noise;
    float gain = 0.25f;
    bool left = true, right = true;     // which channels carry it
    uint32_t seed = 12345;
    double phase = 0;
} g_src;

float noise()
{
    g_src.seed = g_src.seed * 1664525u + 1013904223u;
    return (float)((g_src.seed >> 8) * (1.0 / 8388608.0) - 1.0);      // -1..1, white
}

std::vector<float> g_pulled;            // every frame the pull gave (interleaved), for the checks
int g_width = 0;                        // the last pull's width
double g_music_phase = 0;               // 4 wide: channels 0-1 a 330 Hz tone (the direct ones), 2-3 the source

int pull(float *buf, int frames, int channels)
{
    g_width = channels;
    if (channels == 4) {
        for (int i = 0; i < frames; ++i) {
            float m = 0.2f * (float)std::sin(g_music_phase);
            g_music_phase += 2 * 3.14159265358979 * 330.0 / kHz;
            float v = noise() * g_src.gain;
            buf[i * 4] = m;
            buf[i * 4 + 1] = -m;
            buf[i * 4 + 2] = g_src.left ? v : 0.f;
            buf[i * 4 + 3] = g_src.right ? v : 0.f;
        }
        g_pulled.insert(g_pulled.end(), buf, buf + frames * channels);
        return frames;
    }
    for (int i = 0; i < frames; ++i) {
        float v, w;
        if (g_src.kind == Signal::Sine) {
            v = w = (float)std::sin(g_src.phase);
            g_src.phase += 2 * 3.14159265358979 * 440.0 / kHz;
        } else {
            v = noise();
            w = g_src.kind == Signal::Stereo ? noise() : v;
        }
        buf[i * channels] = g_src.left ? v * g_src.gain : 0.f;
        buf[i * channels + 1] = g_src.right ? w * g_src.gain : 0.f;
    }
    g_pulled.insert(g_pulled.end(), buf, buf + frames * channels);
    return frames;
}

Pose head_yaw(float deg, float pitch_deg = 0)
{
    Pose p;
    Quat y{0, std::sin(deg * kDeg / 2), 0, std::cos(deg * kDeg / 2)};      // + turns left
    Quat x{std::sin(pitch_deg * kDeg / 2), 0, 0, std::cos(pitch_deg * kDeg / 2)};
    p.rot = {y.w * x.x, y.y * x.w, -y.y * x.x, y.w * x.w};                 // y then x (yaw of the pitched head)
    return p;
}

// `blocks` blocks at a fixed head; the output appended to `out` (interleaved stereo).
void render(const Pose &head, int blocks, std::vector<float> &out)
{
    for (int b = 0; b < blocks; ++b) {
        const float *o = test_block(head);
        out.insert(out.end(), o, o + kBlock * kChannels);
    }
}

double energy(const std::vector<float> &v, int ch, size_t from = 0)
{
    double e = 0;
    for (size_t i = from * kChannels + ch; i < v.size(); i += kChannels) e += (double)v[i] * v[i];
    return e;
}

double db(double a, double b) { return 10 * std::log10((a + 1e-30) / (b + 1e-30)); }

void write_wav(const std::string &path, const std::vector<float> &v)
{
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    auto u32 = [f](uint32_t x) { std::fwrite(&x, 4, 1, f); };
    auto u16 = [f](uint16_t x) { std::fwrite(&x, 2, 1, f); };
    uint32_t bytes = (uint32_t)(v.size() * sizeof(float));
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(3); u16(kChannels); u32(kHz); u32(kHz * kChannels * 4);
    u16(kChannels * 4); u16(32);
    std::fwrite("data", 1, 4, f); u32(bytes);
    std::fwrite(v.data(), sizeof(float), v.size(), f);
    std::fclose(f);
}

// Steam Audio started with these speakers (the previous ones released).
bool restart(const Speaker *s, int n, const Layout &layout = Layout())
{
    close();
    return test_start(kHz, pull, layout, s, n);
}

}

int audio_test(const char *dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string base = dir;
    Report r;
    r.f = std::fopen((base + "/audio_test.txt").c_str(), "w");
    if (!r.f) {
        log("audio-test: cannot write %s/audio_test.txt", dir);
        return 1;
    }
    const Speaker pair[2] = {{{-0.50f, -0.10f, -0.50f}, 0}, {{0.50f, -0.10f, -0.50f}, 1}};
    const Speaker ahead[1] = {{{0.f, 0.f, -0.75f}, 0}};
    constexpr int kWarm = 4;                // blocks skipped before measuring (the effects' history)
    constexpr int kBlocks = 64;             // ~0.34 s a measurement

    // (1) the built-in HRTF dead ahead, noted (not ours to fix: its own asymmetry); the cabinet pair with
    // independent stereo, head straight: the ears within 1.5 dB (that asymmetry leans the pair ~1 dB right)
    if (!restart(ahead, 1)) {
        r.check(false, "Steam Audio started");
        std::fclose(r.f);
        close();
        return 1;
    }
    set_spatial(true);
    {
        std::vector<float> out;
        render(Pose(), kBlocks, out);
        double d = db(energy(out, 0, kWarm * kBlock), energy(out, 1, kWarm * kBlock));
        std::fprintf(r.f, "note: one source dead ahead, left - right %.2f dB (the built-in HRTF's own)\n", d);
        write_wav(base + "/1_ahead.wav", out);
    }
    // (2) the left speaker alone, head straight: the left ear louder. (3) head 90 left: both speakers on the right,
    // the right ear louder for each; head 180: (2) swapped.
    if (!restart(pair, 2)) {
        r.check(false, "Steam Audio started with the pair");
        std::fclose(r.f);
        close();
        return 1;
    }
    {
        g_src = Source();
        g_src.kind = Signal::Stereo;
        std::vector<float> out;
        render(Pose(), kBlocks, out);
        double d = db(energy(out, 0, kWarm * kBlock), energy(out, 1, kWarm * kBlock));
        r.check(std::fabs(d) < 1.5, "the pair, independent stereo, head straight: left - right %.2f dB (within 1.5)",
                d);
    }
    struct Case { const char *name; float yaw; bool left_ch; bool expect_left_louder; } cases[] = {
        {"left speaker, head straight", 0, true, true},
        {"right speaker, head straight", 0, false, false},
        {"left speaker, head 90 left", 90, true, false},
        {"right speaker, head 90 left", 90, false, false},
        {"left speaker, head 180", 180, true, false},
        {"right speaker, head 180", 180, false, true},
    };
    for (const Case &c : cases) {
        g_src = Source();
        g_src.left = c.left_ch;
        g_src.right = !c.left_ch;
        std::vector<float> out;
        render(head_yaw(c.yaw), kBlocks, out);
        double d = db(energy(out, 0, kWarm * kBlock), energy(out, 1, kWarm * kBlock));
        r.check(c.expect_left_louder ? d > 1.0 : d < -1.0, "%s: left - right %.2f dB (%s louder)", c.name, d,
                c.expect_left_louder ? "left" : "right");
    }

    // (4) a slow yaw sweep (90 degrees a second, a full turn), a 440 Hz tone on both: no step at a block edge
    // larger than twice the largest step inside the blocks
    {
        g_src = Source();
        g_src.kind = Signal::Sine;
        std::vector<float> out;
        int blocks = (int)(4.0 * kHz / kBlock);
        for (int b = 0; b < blocks; ++b) {
            float yaw = 360.f * b / blocks;
            render(head_yaw(yaw), 1, out);
        }
        double inner = 0, edge = 0;
        size_t frames = out.size() / kChannels;
        for (size_t i = (size_t)kWarm * kBlock + 1; i < frames; ++i)
            for (int c = 0; c < kChannels; ++c) {
                double step = std::fabs(out[i * kChannels + c] - out[(i - 1) * kChannels + c]);
                (i % kBlock == 0 ? edge : inner) = std::max(i % kBlock == 0 ? edge : inner, step);
            }
        r.check(edge <= 2 * inner, "sweep: largest step at a block edge %.4f, inside %.4f (edge <= 2x)", edge, inner);
        write_wav(base + "/4_sweep.wav", out);
    }

    // (5) spatial off: the pulled stereo out bit for bit
    {
        set_spatial(false);
        g_src = Source();
        g_pulled.clear();
        std::vector<float> out;
        render(head_yaw(37), kBlocks, out);
        bool same = out.size() == g_pulled.size() &&
                    std::equal(out.begin(), out.end(), g_pulled.begin(),
                               [](float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; });
        r.check(same, "spatial off: %zu frames out equal the pulled ones bit for bit", out.size() / kChannels);
        set_spatial(true);
    }

    // (6) the level: the binaural output's RMS within 3 dB of the input's at the poses facing the cabinet (the
    // same noise on both, and each its own); facing away noted only (the HRTF's rear shadow makes it quieter, as
    // the room would). (7) the sum held to +-1 by the clamp, with a loud input (0.5): how much reached it noted
    {
        const struct { const char *name; float yaw, pitch; } poses[] = {
            {"straight", 0, 0}, {"90 left", 90, 0}, {"90 right", -90, 0}, {"180", 180, 0}, {"up 45", 0, 45}};
        for (Signal kind : {Signal::Noise, Signal::Stereo})
            for (const auto &p : poses) {
                g_src = Source();
                g_src.kind = kind;
                g_pulled.clear();
                std::vector<float> out;
                render(head_yaw(p.yaw, p.pitch), kBlocks, out);
                double in = energy(g_pulled, 0, kWarm * kBlock) + energy(g_pulled, 1, kWarm * kBlock);
                double o = energy(out, 0, kWarm * kBlock) + energy(out, 1, kWarm * kBlock);
                double d = db(o, in);
                const char *what = kind == Signal::Noise ? "the same on both" : "independent stereo";
                if (p.yaw == 180)
                    std::fprintf(r.f, "note: level, %s, head %s (facing away): out - in %.2f dB\n", what, p.name, d);
                else
                    r.check(std::fabs(d) <= 3.0, "level, %s, head %s: out - in %.2f dB (within 3)", what, p.name, d);
                if (p.yaw == 0 && p.pitch == 0 && kind == Signal::Stereo)
                    write_wav(base + "/6_stereo_straight.wav", out);
            }
        g_src = Source();
        g_src.gain = 0.5f;
        std::vector<float> out;
        render(head_yaw(30), kBlocks, out);
        float peak = 0;
        size_t clamped = 0;
        for (float v : out) {
            peak = std::max(peak, std::fabs(v));
            clamped += std::fabs(v) >= 1.f;
        }
        r.check(peak <= 1.f, "peak at input 0.5 (the same white noise on both): %.3f, %zu of %zu samples at the clamp",
                peak, clamped, out.size());
    }

    // (8) direct channels (4 wide, music in 0-1 to the ears, the speakers on 2-3): with the speakers silent the
    // output equals channels 0-1 exactly (by value: a -0 in comes out +0) whatever the head does; (9) off pulls 2 wide (the game's own mix)
    {
        Layout wide;
        wide.channels = 4;
        wide.direct[0] = 0;
        wide.direct[1] = 1;
        const Speaker effects[2] = {{{-0.50f, -0.10f, -0.50f}, 2}, {{0.50f, -0.10f, -0.50f}, 3}};
        bool ok = restart(effects, 2, wide);
        r.check(ok, "Steam Audio started 4 wide, direct 0 / 1, speakers on 2 / 3");
        set_spatial(true);
        g_src = Source();
        g_src.left = g_src.right = false;   // the effects silent
        bool same = ok;
        double most = 0;
        for (float yaw : {0.f, 90.f, 180.f, -45.f}) {
            g_pulled.clear();
            std::vector<float> out;
            render(head_yaw(yaw), 8, out);
            for (size_t i = 0; i < out.size() / kChannels; ++i)
                for (int c = 0; c < 2; ++c) {
                    same &= out[i * 2 + c] == g_pulled[i * 4 + c];
                    most = std::max(most, (double)std::fabs(out[i * 2 + c] - g_pulled[i * 4 + c]));
                }
        }
        r.check(same && g_width == 4, "direct: channels 0-1 out exactly at four head yaws (pulled %d wide; most "
                "off %.3g)", g_width, most);
        g_src = Source();
        std::vector<float> out;
        render(head_yaw(30), kBlocks, out);
        float peak = 0;
        for (float v : out) peak = std::max(peak, std::fabs(v));
        r.check(peak <= 1.f, "direct + speakers summed, held to +-1: peak %.3f", peak);
        write_wav(base + "/8_direct_and_speakers.wav", out);
        set_spatial(false);
        out.clear();
        render(Pose(), 1, out);
        r.check(g_width == kChannels, "spatial off, 4-wide layout: pulled %d wide (the game's mix)", g_width);
        set_spatial(true);
    }

    Stats s = stats(false);
    std::fprintf(r.f, "spatial time per block: mean %.1f us, max %.1f us over %llu blocks (this Mac)\n",
                 s.spatial_mean_us, s.spatial_max_us, (unsigned long long)s.spatial_blocks);
    std::fprintf(r.f, "%d passed, %d failed\n", r.pass, r.fail);
    std::fclose(r.f);
    close();
    log("audio-test: %d passed, %d failed (%s/audio_test.txt); spatial %.1f us a block (max %.1f)", r.pass, r.fail,
        dir, s.spatial_mean_us, s.spatial_max_us);
    return r.fail ? 1 : 0;
}

}
