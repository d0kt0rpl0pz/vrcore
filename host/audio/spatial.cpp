/*
 * spatial.cpp -- Steam Audio's binaural effects for the virtual speakers (spatial.h).
 *
 * A binaural effect renders all of its input from one direction, so each speaker has its own, fed its channel as
 * mono; the effects' stereo outputs are summed. The direction is the speaker seen from the head: the seat-space
 * vector turned by the head's inverse rotation, normalised (the head's frame is Steam Audio's listener space as
 * it is: x right, y up, -z ahead), so nothing of Steam Audio but Apply runs on the audio thread.
 */
#include "spatial.h"

#include <phonon.h>

#include <algorithm>
#include <cmath>

#include "../app/log.h"

namespace vrcore::audio {

struct Spatial::Impl {
    IPLContext context = nullptr;
    IPLHRTF hrtf = nullptr;
    int n = 0;
    int width = kChannels;                      // the input's channels
    Speaker speakers[kMaxSpeakers];
    IPLBinauralEffect effects[kMaxSpeakers] = {};
    float mono[kBlock];                         // one speaker's channel
    float ears[2][kBlock];                      // one effect's output
    float sum[2][kBlock];                       // all of them
};

namespace {

// The level against the plain stereo (--audio-test's level check): RMS-normalised, the built-in HRTF still gives a
// source 1.6 dB (ahead) to 3.3 dB (at the side) more than its input over both ears, and the cabinet pair adds both
// speakers at each ear (a centred, correlated sound sums in phase). -4 dB puts correlated stereo and independent
// stereo either side of the input's level.
const float kTrim = 0.631f;                 // -4 dB

void IPLCALL steam_log(IPLLogLevel level, const char *message)
{
    if (level != IPL_LOGLEVEL_DEBUG) log("audio: Steam Audio: %s", message);
}

// v turned by q's inverse
void unrotate(const Quat &q, const float v[3], float out[3])
{
    // q* v q, for unit q: v + 2 w (u x v)' + 2 u x (u x v)', u = -q.xyz
    float ux = -q.x, uy = -q.y, uz = -q.z, w = q.w;
    float tx = 2 * (uy * v[2] - uz * v[1]);
    float ty = 2 * (uz * v[0] - ux * v[2]);
    float tz = 2 * (ux * v[1] - uy * v[0]);
    out[0] = v[0] + w * tx + (uy * tz - uz * ty);
    out[1] = v[1] + w * ty + (uz * tx - ux * tz);
    out[2] = v[2] + w * tz + (ux * ty - uy * tx);
}

}

bool Spatial::init(int hz, int channels, const Speaker *speakers, int n)
{
    shutdown();
    if (hz <= 0 || n <= 0 || n > kMaxSpeakers || channels < 1 || channels > kMaxIn) return false;
    Impl *m = new Impl();
    impl_ = m;
    m->n = n;
    m->width = channels;
    for (int i = 0; i < n; ++i) {
        m->speakers[i] = speakers[i];
        m->speakers[i].channel = std::clamp(speakers[i].channel, 0, channels - 1);
    }

    IPLContextSettings cs{};
    cs.version = STEAMAUDIO_VERSION;
    cs.logCallback = steam_log;
    cs.simdLevel = IPL_SIMDLEVEL_AVX2;          // a cap (x86 only; NEON on the Quest)
    IPLerror e = iplContextCreate(&cs, &m->context);
    if (e != IPL_STATUS_SUCCESS) {
        log("audio: Steam Audio's context did not start (%d)", (int)e);
        shutdown();
        return false;
    }
    IPLAudioSettings as{hz, kBlock};
    IPLHRTFSettings hs{};
    hs.type = IPL_HRTFTYPE_DEFAULT;
    hs.volume = kTrim;
    hs.normType = IPL_HRTFNORMTYPE_RMS;
    e = iplHRTFCreate(m->context, &as, &hs, &m->hrtf);
    if (e != IPL_STATUS_SUCCESS) {
        log("audio: Steam Audio's HRTF did not load (%d)", (int)e);
        shutdown();
        return false;
    }
    for (int i = 0; i < n; ++i) {
        IPLBinauralEffectSettings es{m->hrtf};
        e = iplBinauralEffectCreate(m->context, &as, &es, &m->effects[i]);
        if (e != IPL_STATUS_SUCCESS) {
            log("audio: Steam Audio's binaural effect %d did not start (%d)", i, (int)e);
            shutdown();
            return false;
        }
    }
    ready_ = true;
    for (int i = 0; i < n; ++i)
        log("audio: speaker %d (channel %d) at %.2f %.2f %.2f m", i, m->speakers[i].channel, m->speakers[i].pos[0],
            m->speakers[i].pos[1], m->speakers[i].pos[2]);
    log("audio: Steam Audio %d.%d.%d, %d Hz, %d frames, default HRTF (RMS, -4 dB), bilinear", STEAMAUDIO_VERSION_MAJOR,
        STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH, hz, kBlock);
    return true;
}

void Spatial::process(const float *in, float *out, const Pose &head)
{
    Impl *m = impl_;
    std::fill(&m->sum[0][0], &m->sum[0][0] + 2 * kBlock, 0.f);
    float *mono = m->mono;
    float *ears[2] = {m->ears[0], m->ears[1]};
    IPLAudioBuffer ib{1, kBlock, &mono};
    IPLAudioBuffer ob{2, kBlock, ears};
    for (int s = 0; s < m->n; ++s) {
        const Speaker &sp = m->speakers[s];
        for (int i = 0; i < kBlock; ++i) mono[i] = in[i * m->width + sp.channel];
        float rel[3] = {sp.pos[0] - head.pos[0], sp.pos[1] - head.pos[1], sp.pos[2] - head.pos[2]};
        float d[3];
        unrotate(head.rot, rel, d);
        float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        IPLBinauralEffectParams p{};
        if (len > 1e-6f) p.direction = {d[0] / len, d[1] / len, d[2] / len};
        else p.direction = {0, 0, -1};          // in the head: ahead
        p.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
        p.spatialBlend = 1.f;
        p.hrtf = m->hrtf;
        iplBinauralEffectApply(m->effects[s], &p, &ib, &ob);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i) m->sum[c][i] += ears[c][i];
    }
    for (int i = 0; i < kBlock; ++i) {
        out[i * kChannels] = m->sum[0][i];
        out[i * kChannels + 1] = m->sum[1][i];
    }
}

void Spatial::reset()
{
    if (!ready_) return;
    for (int i = 0; i < impl_->n; ++i) iplBinauralEffectReset(impl_->effects[i]);
}

void Spatial::shutdown()
{
    ready_ = false;
    Impl *m = impl_;
    if (!m) return;
    for (IPLBinauralEffect &fx : m->effects)
        if (fx) iplBinauralEffectRelease(&fx);
    if (m->hrtf) iplHRTFRelease(&m->hrtf);
    if (m->context) iplContextRelease(&m->context);
    delete m;
    impl_ = nullptr;
}

}
