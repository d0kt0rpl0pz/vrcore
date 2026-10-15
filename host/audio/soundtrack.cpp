/*
 * soundtrack.cpp -- the streaming thread, the ring and the audio thread's mix (soundtrack.h).
 *
 * THE RING holds stereo float frames at the device's rate, kRing of them (~1.4 s at 48 kHz). Its indices only
 * grow (64 bits: they never wrap in practice): the streaming thread writes at w, the audio thread reads at r, and
 * w - r frames are waiting.
 *
 * A SKIP is a mark in the ring: the streaming thread stores `start`, the index its new track's first frame will
 * have, and writes on behind the old samples. The audio thread, seeing start ahead of r, fades out on what is
 * left of the old track, then jumps r to start and fades in. A stop is the `playing` flag: the audio thread fades
 * out, then drops whatever is in the ring (and goes on dropping until a play marks a new start). A track that ends
 * on its own sets no mark: the next one follows on in the ring.
 *
 * THE TRACK HEARD. The ring runs ahead of the ears, so the streaming thread notes where its newest track begins
 * (track_from) and the one before; status() compares that with r. Both are indices into the list the streaming
 * thread has taken (list_heard): a new list forgets the one before (its index meant the old list), and a retarget
 * moves both to their places in the new list.
 */
#include "soundtrack.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "../app/log.h"
#include "decode.h"

namespace vrcore::audio {

bool decodable_name(const char *path)
{
    const char *dot = std::strrchr(path, '.');
    if (!dot || std::strchr(dot, '/')) return false;
    char ext[8] = {};
    for (size_t i = 0; dot[i]; ++i) {
        if (i + 1 >= sizeof ext) return false;
        ext[i] = (char)std::tolower((unsigned char)dot[i]);
    }
    for (const char *e : {".mp3", ".m4a", ".aac", ".flac", ".ogg", ".wav"})
        if (!std::strcmp(ext, e)) return true;
    return false;
}

namespace soundtrack {

namespace {

constexpr uint64_t kRing = 1u << 16;        // frames
constexpr int kChunk = 1024;                // frames a decode or a ring write
constexpr auto kPoll = std::chrono::milliseconds(10);   // the streaming thread's wait when the ring is full

struct State {
    // the main thread's commands to the streaming thread, under m
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> tracks;
    std::vector<std::string> failures;
    int want = -1;                          // the track asked for
    bool run = false;                       // playing asked for
    bool quit = false;
    std::atomic<uint32_t> gen{0};           // one more each command (written under m)
    uint32_t list = 0;                      // one more each set_tracks and retarget
    bool retargeted = false, commanded = false;     // since the streaming thread last looked
    std::thread thread;
    int thread_hz = 0;                      // the rate the streaming thread resamples to
    std::atomic<int> hz{0};

    // the ring
    float ring[kRing * 2];
    std::atomic<uint64_t> w{0}, r{0};
    std::atomic<uint64_t> start{0};         // the newest skip's first frame
    std::atomic<bool> playing{false};
    std::atomic<float> volume{1.f};         // the soundtrack's volume times the master, 0..1
    std::atomic<int> track_prev{-1}, track_now{-1};
    std::atomic<uint64_t> track_from{0};    // where track_now begins in the ring
    std::atomic<uint32_t> list_heard{0};    // the list track_now and track_prev index (State::list)

    // the stats
    std::atomic<uint64_t> frames{0}, dry{0}, decoded{0};
    std::atomic<int> failed{0};

    // the audio thread's
    float fade = 0.f;                       // the skip / stop fade, 0..1
    float gain = 0.f;                       // the volume, eased towards `volume`
    bool skipping = false;                  // fading out to a skip's mark
} g;

// The newest track begins at ring index `from` (-1: none); `new_list`: the one before was in another list.
void heard_from(int track, uint64_t from, bool new_list = false)
{
    g.track_prev.store(new_list ? -1 : g.track_now.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g.track_from.store(from, std::memory_order_relaxed);
    g.track_now.store(track, std::memory_order_release);
}

// The streaming thread: the commands, then the ring kept full.
class Streamer {
public:
    explicit Streamer(int hz) : hz_(hz) {}

    void run()
    {
        const SDL_AudioSpec out{SDL_AUDIO_F32, 2, hz_};
        rs_ = SDL_CreateAudioStream(&out, &out);
        if (!rs_) {
            log("soundtrack: no resampler (%s): no soundtrack", SDL_GetError());
            g.playing = false;
            return;
        }
        uint32_t seen = g.gen.load() - 1;   // the first pass takes the current command
        bool first = true;
        for (;;) {
            bool fresh, want_run, keep = false, new_list = false;
            int want;
            std::vector<std::string> old;
            {
                std::unique_lock<std::mutex> lk(g.m);
                g.cv.wait_for(lk, kPoll, [&] { return g.quit || g.gen.load() != seen; });
                if (g.quit) break;
                fresh = g.gen.load() != seen;
                seen = g.gen.load();
                want_run = g.run;
                want = g.want;
                if (fresh) {
                    keep = g.retargeted && !g.commanded;   // a play with it: that play, in the new list
                    g.retargeted = g.commanded = false;
                    new_list = g.list != list_n_ || first;
                    list_n_ = g.list;
                    old.swap(list_);
                    list_ = g.tracks;
                }
            }
            first = false;
            gen_ = seen;
            if (fresh) {
                if (keep && !list_.empty()) retarget(old);
                else command(want_run, want, new_list);
                g.list_heard.store(list_n_, std::memory_order_release);
            }
            if (dec_ || ended_) fill();
        }
        dec_.reset();
        SDL_DestroyAudioStream(rs_);
    }

private:
    // A play, next, stop or a new list: what plays is dropped, and a skip marked at the ring's end.
    void command(bool want_run, int want, bool new_list)
    {
        dec_.reset();
        ended_ = false;
        SDL_ClearAudioStream(rs_);
        uint64_t w = g.w.load(std::memory_order_relaxed);
        g.start.store(w, std::memory_order_release);
        cur_ = want_run ? open_from(want) : -1;
        heard_from(cur_, w, new_list);
        g.playing.store(cur_ >= 0, std::memory_order_release);
    }

    // A retarget: `old` was the list, list_ is the new one. What is decoding goes on, renumbered. A file no longer
    // listed plays to its end as the one heard before it (so the one after that follows); nothing else is skipped.
    void retarget(const std::vector<std::string> &old)
    {
        auto at = [&](int i) {
            if (i < 0 || i >= (int)old.size()) return -1;
            auto it = std::find(list_.begin(), list_.end(), old[i]);
            return it == list_.end() ? -1 : (int)(it - list_.begin());
        };
        const int prev = at(g.track_prev.load(std::memory_order_relaxed));
        int now = at(g.track_now.load(std::memory_order_relaxed));
        if (cur_ >= 0) {
            cur_ = at(cur_);
            if (cur_ < 0) cur_ = now = prev >= 0 ? prev : (int)list_.size() - 1;
        }
        g.track_prev.store(prev, std::memory_order_relaxed);
        g.track_now.store(now, std::memory_order_release);
    }

    // The first track from i on (wrapping round) that opens; -1 when none does.
    int open_from(int i)
    {
        int n = (int)list_.size();
        if (i < 0 || i >= n) return -1;
        for (int k = 0; k < n; ++k) {
            int t = (i + k) % n;
            std::string why;
            dec_ = open_decoder(list_[t].c_str(), &why);
            if (dec_) {
                src_ = SDL_AudioSpec{SDL_AUDIO_F32, dec_->channels(), dec_->hz()};
                SDL_SetAudioStreamFormat(rs_, &src_, nullptr);
                log("soundtrack: track %d, %s (%d Hz, %d ch)", t, list_[t].c_str(), dec_->hz(), dec_->channels());
                return t;
            }
            failed(t, why);
        }
        log("soundtrack: none of the %d tracks can be played", n);
        return -1;
    }

    // Track t is skipped (logged, and kept for failure()), unless the list has changed since.
    void failed(int t, const std::string &why)
    {
        log("soundtrack: skipped track %d, %s: %s", t, list_[t].c_str(), why.c_str());
        std::lock_guard<std::mutex> lk(g.m);
        if (g.gen.load() != gen_ || t >= (int)g.failures.size()) return;
        if (g.failures[t].empty()) g.failed.fetch_add(1, std::memory_order_relaxed);
        g.failures[t] = why;
    }

    // Decoded, resampled frames into the ring until it is full, the track list runs out or a command comes.
    void fill()
    {
        while (g.gen.load(std::memory_order_acquire) == gen_) {
            uint64_t w = g.w.load(std::memory_order_relaxed);
            uint64_t space = kRing - (w - g.r.load(std::memory_order_acquire));
            if (space < (uint64_t)kChunk) return;           // full: the next poll
            int ready = SDL_GetAudioStreamAvailable(rs_) / (int)sizeof stereo_[0];
            if (ready >= kChunk || (ended_ && ready > 0)) {
                int n = SDL_GetAudioStreamData(rs_, stereo_, std::min(ready, kChunk) * (int)sizeof stereo_[0]);
                n = std::max(0, n) / (int)sizeof stereo_[0];
                for (int i = 0; i < n; ++i) {
                    uint64_t at = (w + i) & (kRing - 1);
                    g.ring[at * 2] = stereo_[i][0];
                    g.ring[at * 2 + 1] = stereo_[i][1];
                }
                g.w.store(w + n, std::memory_order_release);
                g.decoded.fetch_add(n, std::memory_order_relaxed);
                if (n > 0) continue;
            }
            if (ended_) {                   // the track is all in the ring: the next follows on
                ended_ = false;
                cur_ = open_from((cur_ + 1) % (int)list_.size());
                heard_from(cur_, w);
                if (cur_ < 0) {
                    g.playing.store(false, std::memory_order_release);
                    return;
                }
                continue;
            }
            int got = dec_->read(in_, kChunk);
            if (got < 0) failed(cur_, dec_->why());
            if (got <= 0) {
                dec_.reset();
                ended_ = true;
                SDL_FlushAudioStream(rs_);
                continue;
            }
            if (dec_->hz() != src_.freq || dec_->channels() != src_.channels) {
                src_ = SDL_AudioSpec{SDL_AUDIO_F32, dec_->channels(), dec_->hz()};
                SDL_SetAudioStreamFormat(rs_, &src_, nullptr);
            }
            SDL_PutAudioStreamData(rs_, in_, got * src_.channels * (int)sizeof(float));
        }
    }

    int hz_;
    SDL_AudioStream *rs_ = nullptr;
    SDL_AudioSpec src_{};
    std::unique_ptr<Decoder> dec_;
    bool ended_ = false;                    // the decoder is done: the resampler's last frames to the ring
    int cur_ = -1;
    uint32_t gen_ = 0;                      // the command this work is for
    uint32_t list_n_ = 0;                   // the list list_ is (State::list)
    std::vector<std::string> list_;
    float in_[kChunk * kMaxDecodeChannels];
    float stereo_[kChunk][2];
};

void stream_main(int hz)
{
    std::unique_ptr<Streamer> s(new Streamer(hz));      // ~40 KB of buffers: off the thread's stack
    s->run();
}

// A command for the streaming thread (under g.m), and the thread started if it is not running.
void command_locked(bool run, int want)
{
    g.commanded = true;
    g.run = run;
    g.want = want;
    g.gen.fetch_add(1);
    int hz = g.hz.load();
    if (run && !g.thread.joinable() && hz > 0) {
        g.quit = false;
        g.thread_hz = hz;
        g.thread = std::thread(stream_main, hz);
    }
}

}

void set_rate(int hz)
{
    g.hz = hz;
}

uint32_t set_tracks(std::vector<std::string> paths)
{
    g.playing = false;
    uint32_t list;
    {
        std::lock_guard<std::mutex> lk(g.m);
        g.tracks = std::move(paths);
        g.failures.assign(g.tracks.size(), std::string());
        g.failed = 0;
        list = ++g.list;
        command_locked(false, -1);
    }
    g.cv.notify_all();
    return list;
}

uint32_t retarget(std::vector<std::string> paths)
{
    uint32_t list;
    {
        std::lock_guard<std::mutex> lk(g.m);
        if (g.want >= 0 && g.want < (int)g.tracks.size()) {
            // a play the streaming thread has not taken yet: the same file, in the new list
            auto it = std::find(paths.begin(), paths.end(), g.tracks[g.want]);
            g.want = it == paths.end() ? -1 : (int)(it - paths.begin());
        }
        g.tracks = std::move(paths);
        g.failures.assign(g.tracks.size(), std::string());
        g.failed = 0;
        list = ++g.list;
        g.retargeted = true;
        g.gen.fetch_add(1);
    }
    g.cv.notify_all();
    return list;
}

bool play(int i)
{
    if (g.thread.joinable() && g.thread_hz != g.hz.load()) shutdown();     // the device's rate has changed
    {
        std::lock_guard<std::mutex> lk(g.m);
        if (i < 0 || i >= (int)g.tracks.size() || g.hz.load() <= 0) return false;
        command_locked(true, i);
    }
    g.cv.notify_all();
    return true;
}

void next()
{
    int now = status().track;
    {
        std::lock_guard<std::mutex> lk(g.m);
        if (!g.run || g.tracks.empty()) return;
        command_locked(true, ((now >= 0 ? now : g.want) + 1) % (int)g.tracks.size());
    }
    g.cv.notify_all();
}

void stop()
{
    g.playing = false;                      // the fade starts now
    {
        std::lock_guard<std::mutex> lk(g.m);
        command_locked(false, -1);
    }
    g.cv.notify_all();
}

void set_volume(int volume, int master)
{
    g.volume = std::clamp(volume, 0, 100) * std::clamp(master, 0, 100) * 1e-4f;
}

Status status()
{
    Status s;
    s.list = g.list_heard.load(std::memory_order_acquire);
    uint64_t r = g.r.load(std::memory_order_acquire);
    int now = g.track_now.load(std::memory_order_acquire);
    s.playing = g.playing.load();
    if (s.playing)
        s.track = r >= g.track_from.load(std::memory_order_relaxed) ? now : g.track_prev.load(std::memory_order_relaxed);
    s.failed = g.failed.load();
    s.frames = g.frames.load(std::memory_order_relaxed);
    s.dry_frames = g.dry.load(std::memory_order_relaxed);
    s.decoded = g.decoded.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g.m);
    s.tracks = (int)g.tracks.size();
    return s;
}

std::string failure(int i)
{
    std::lock_guard<std::mutex> lk(g.m);
    return i >= 0 && i < (int)g.failures.size() ? g.failures[i] : std::string();
}

bool mix(float *out, int frames)
{
    uint64_t r = g.r.load(std::memory_order_relaxed);   // ours
    const uint64_t w = g.w.load(std::memory_order_acquire);
    const uint64_t start = g.start.load(std::memory_order_acquire);
    const bool on = g.playing.load(std::memory_order_acquire);
    const float volume = g.volume.load(std::memory_order_relaxed);
    const int hz = g.hz.load(std::memory_order_relaxed);
    if (hz <= 0 || (!on && g.fade == 0.f)) {
        if (r != w) g.r.store(w, std::memory_order_release);     // stopped: drop what came
        return false;
    }
    if (g.fade == 0.f) g.gain = volume;     // nothing audible yet: no need to ease
    const float step = 1000.f / (kFadeMs * hz);
    uint64_t dry = 0;
    for (int i = 0; i < frames; ++i) {
        if (start > r) g.skipping = true;
        if (g.skipping && g.fade == 0.f) {  // faded out: the new track from its first frame
            r = std::max(r, start);
            g.skipping = false;
        }
        const float target = on && !g.skipping ? 1.f : 0.f;
        g.fade = g.fade < target ? std::min(target, g.fade + step) : std::max(target, g.fade - step);
        g.gain = g.gain < volume ? std::min(volume, g.gain + step) : std::max(volume, g.gain - step);
        float l = 0.f, rt = 0.f;
        if (r < w && !(g.skipping && r >= start)) {
            uint64_t at = r & (kRing - 1);
            l = g.ring[at * 2];
            rt = g.ring[at * 2 + 1];
            ++r;
        } else if (on && !g.skipping) {
            ++dry;
        }
        const float k = g.fade * g.gain;
        out[i * 2] = std::clamp(out[i * 2] + l * k, -1.f, 1.f);
        out[i * 2 + 1] = std::clamp(out[i * 2 + 1] + rt * k, -1.f, 1.f);
    }
    if (!on && g.fade == 0.f) r = w;
    g.r.store(r, std::memory_order_release);
    g.frames.fetch_add(frames, std::memory_order_relaxed);
    if (dry) g.dry.fetch_add(dry, std::memory_order_relaxed);
    return true;
}

void shutdown()
{
    {
        std::lock_guard<std::mutex> lk(g.m);
        g.quit = true;
        g.run = false;
        g.gen.fetch_add(1);
    }
    g.cv.notify_all();
    if (g.thread.joinable()) g.thread.join();
    g.quit = false;
    g.playing = false;
    g.w = g.r = g.start = 0;
    g.track_prev = g.track_now = -1;
    g.track_from = 0;
    g.fade = g.gain = 0.f;
    g.skipping = false;
}

}

}
