/*
 * audio.cpp -- the SDL stream, the block loop on SDL's audio thread, the listener's seqlock and the stats
 * (audio.h). Each block ends with the soundtrack's music added (soundtrack.h), spatial or not.
 */
#include "audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif

#include "../app/log.h"
#include "soundtrack.h"
#include "spatial.h"

namespace vrcore::audio {

namespace {

struct State {
    SDL_AudioStream *stream = nullptr;
    bool sdl_up = false;
    int hz = 0;
    int device_frames = 0;
    std::atomic<Pull> pull{nullptr};
    Layout layout;                              // set before the device runs
    std::atomic<bool> spatial_on{true};
    Spatial fx;                                 // ready once start made it
    bool was_on = false;                        // the audio thread's: spatialised the block before
    // the listener's seqlock: pos[3], rot x y z w (odd seq: a write is under way)
    std::atomic<uint32_t> seq{0};
    std::atomic<float> head[7];
    Pose last_head;                             // the audio thread's: the last whole read
    // the stats
    std::atomic<uint64_t> callbacks{0}, blocks{0}, short_pulls{0};
    std::atomic<uint64_t> fx_blocks{0}, fx_ns{0}, fx_max_ns{0};
    // the audio thread's blocks (one in, one out)
    float in[kBlock * kMaxIn];
    float out[kBlock * kChannels];
} g;

int64_t steady_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef __ANDROID__
// The output's own rate, from AAudio (SDL's AAudio backend does not know it before opening, and falls back to
// 44.1 kHz, which Android then resamples to the mixer's 48 kHz). 0: not known.
int native_rate()
{
    AAudioStreamBuilder *b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return 0;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStream *st = nullptr;
    int hz = 0;
    if (AAudioStreamBuilder_openStream(b, &st) == AAUDIO_OK) {
        hz = AAudioStream_getSampleRate(st);
        AAudioStream_close(st);
    }
    AAudioStreamBuilder_delete(b);
    return hz;
}
#endif

// The newest whole listener; a write caught half-done is retried a few times, then the last good one stands.
const Pose &read_listener()
{
    for (int tries = 0; tries < 16; ++tries) {
        uint32_t s0 = g.seq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        float v[7];
        for (int i = 0; i < 7; ++i) v[i] = g.head[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g.seq.load(std::memory_order_relaxed) != s0) continue;
        if (s0 == 0) break;                     // never written: the default head
        Pose &p = g.last_head;
        std::memcpy(p.pos, v, sizeof p.pos);
        p.rot = {v[3], v[4], v[5], v[6]};
        break;
    }
    return g.last_head;
}

// One block as the callback makes it: off, the game's stereo pulled as it is; on, the layout's channels pulled, the
// speakers through Steam Audio and the direct channels added, clamped to +-1. Then the soundtrack added, locked to
// the head like the direct channels (when it is silent the block is left as it was). The audio thread's (or a
// test's).
const float *next_block(Pull pull, bool on, const Pose &head)
{
    const int width = on ? g.layout.channels : kChannels;
    int got = std::max(0, pull(g.in, kBlock, width));
    if (got < kBlock) {
        std::fill(g.in + got * width, g.in + kBlock * width, 0.f);
        g.short_pulls.fetch_add(1, std::memory_order_relaxed);
    }
    g.blocks.fetch_add(1, std::memory_order_relaxed);
    if (!on) {
        soundtrack::mix(g.in, kBlock);
        return g.in;
    }
    int64_t t0 = steady_ns();
    g.fx.process(g.in, g.out, head);
    uint64_t ns = (uint64_t)(steady_ns() - t0);
    for (int ear = 0; ear < kChannels; ++ear) {
        int c = g.layout.direct[ear];
        for (int i = 0; i < kBlock; ++i) {
            float v = g.out[i * kChannels + ear] + (c >= 0 ? g.in[i * width + c] : 0.f);
            g.out[i * kChannels + ear] = std::clamp(v, -1.f, 1.f);
        }
    }
    g.fx_blocks.fetch_add(1, std::memory_order_relaxed);
    g.fx_ns.fetch_add(ns, std::memory_order_relaxed);
    // a max, not a store: stats(true) may have just reset it
    for (uint64_t mx = g.fx_max_ns.load(std::memory_order_relaxed);
         ns > mx && !g.fx_max_ns.compare_exchange_weak(mx, ns, std::memory_order_relaxed);) {
    }
    soundtrack::mix(g.out, kBlock);
    return g.out;
}

// The layout kept, its channels checked (a bad one: the plain stereo's).
void set_layout(const Layout &l)
{
    g.layout = l;
    if (l.channels < 1 || l.channels > kMaxIn) g.layout = Layout();
    for (int &c : g.layout.direct)
        if (c >= g.layout.channels) c = -1;
}

// Spatialising this block: on, and Steam Audio started (its history cleared when it comes back on).
bool spatial_now()
{
    bool on = g.spatial_on.load(std::memory_order_relaxed) && g.fx.ready();
    if (on && !g.was_on) g.fx.reset();
    g.was_on = on;
    return on;
}

// SDL's audio thread: whole blocks until SDL has what it asked for (the rest waits in the stream).
void SDLCALL feed(void *, SDL_AudioStream *stream, int additional, int)
{
    g.callbacks.fetch_add(1, std::memory_order_relaxed);
    Pull pull = g.pull.load(std::memory_order_acquire);
    if (!pull) return;                          // SDL plays silence
    bool on = spatial_now();
    const Pose &head = read_listener();
    constexpr int kBytes = (int)(kBlock * kChannels * sizeof(float));     // one block of the output (stereo)
    for (int left = additional; left > 0; left -= kBytes)
        SDL_PutAudioStreamData(stream, next_block(pull, on, head), kBytes);
}

}

bool open(int *hz)
{
    *hz = 0;
    if (g.stream) {
        *hz = g.hz;
        return true;
    }
    // The device's buffer: 1024 frames (~21 ms), four blocks a callback. 256 (one block) was
    // too tight for AAudio's low-latency path on the Quest 3: the sound stuttered with the game's ring full.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log("audio: SDL's audio did not start (%s): no sound", SDL_GetError());
        return false;
    }
    g.sdl_up = true;
    SDL_AudioSpec dev{};
    int frames = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev, &frames) || dev.freq <= 0) {
        log("audio: no playback device (%s): no sound", SDL_GetError());
        close();
        return false;
    }
    // the device's own rate (the game resamples to it), stereo float
    int rate = dev.freq;
#ifdef __ANDROID__
    if (int native = native_rate()) rate = native;
#endif
    SDL_AudioSpec spec{SDL_AUDIO_F32, kChannels, rate};
    g.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, nullptr);
    if (!g.stream) {
        log("audio: the playback stream did not open (%s): no sound", SDL_GetError());
        close();
        return false;
    }
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g.stream), &dev, &frames);
    g.hz = spec.freq;
    g.device_frames = frames;
    soundtrack::set_rate(g.hz);
    *hz = g.hz;
    log("audio: %s, %d Hz (device %d Hz, %d ch, %d frames a buffer), blocks of %d", SDL_GetCurrentAudioDriver(),
        g.hz, dev.freq, dev.channels, frames, kBlock);
    return true;
}

bool start(Pull pull, const Layout &layout, const Speaker *speakers, int n)
{
    if (!g.stream || !pull) return false;
    if (g.pull.load()) {                        // the callback reads the layout and Steam Audio: once per open
        log("audio: already started (close first)");
        return false;
    }
    set_layout(layout);
    if (!g.fx.init(g.hz, g.layout.channels, speakers, std::min(n, kMaxSpeakers)))
        log("audio: no spatial sound (Steam Audio did not start): the plain stereo");
    g.pull.store(pull, std::memory_order_release);
    if (!SDL_ResumeAudioStreamDevice(g.stream)) {
        log("audio: the device did not resume (%s)", SDL_GetError());
        return false;
    }
    log("audio: started, spatial %s, %d channels: %d speakers, direct %d / %d", spatial() ? "on" : "off",
        g.layout.channels, n, g.layout.direct[0], g.layout.direct[1]);
    return true;
}

void listener(const Pose &p)
{
    uint32_t s = g.seq.load(std::memory_order_relaxed);
    g.seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const float v[7] = {p.pos[0], p.pos[1], p.pos[2], p.rot.x, p.rot.y, p.rot.z, p.rot.w};
    for (int i = 0; i < 7; ++i) g.head[i].store(v[i], std::memory_order_relaxed);
    g.seq.store(s + 2, std::memory_order_release);
}

void set_spatial(bool on)
{
    g.spatial_on = on;
}

bool spatial()
{
    return g.spatial_on && g.fx.ready();
}

Stats stats(bool reset_window)
{
    Stats s;
    s.hz = g.hz;
    s.device_frames = g.device_frames;
    s.callbacks = g.callbacks.load(std::memory_order_relaxed);
    s.blocks = g.blocks.load(std::memory_order_relaxed);
    s.short_pulls = g.short_pulls.load(std::memory_order_relaxed);
    uint64_t n = reset_window ? g.fx_blocks.exchange(0) : g.fx_blocks.load();
    uint64_t ns = reset_window ? g.fx_ns.exchange(0) : g.fx_ns.load();
    uint64_t mx = reset_window ? g.fx_max_ns.exchange(0) : g.fx_max_ns.load();
    s.spatial_blocks = n;
    s.spatial_mean_us = n ? ns * 1e-3 / n : 0.0;
    s.spatial_max_us = mx * 1e-3;
    s.spatial = spatial();
    return s;
}

bool test_start(int hz, Pull pull, const Layout &layout, const Speaker *speakers, int n)
{
    g.hz = hz;
    g.pull = pull;
    soundtrack::set_rate(hz);
    set_layout(layout);
    return g.fx.ready() || g.fx.init(hz, g.layout.channels, speakers, std::min(n, kMaxSpeakers));
}

const float *test_block(const Pose &head)
{
    g.callbacks.fetch_add(1, std::memory_order_relaxed);
    return next_block(g.pull.load(), spatial_now(), head);
}

void close()
{
    if (g.stream) {
        SDL_DestroyAudioStream(g.stream);       // the device closed with it: the callback has returned
        g.stream = nullptr;
    }
    soundtrack::shutdown();                     // after the device: mix can no longer run
    soundtrack::set_rate(0);
    g.pull = nullptr;
    g.fx.shutdown();
    if (g.sdl_up) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    g.sdl_up = false;
}

}
