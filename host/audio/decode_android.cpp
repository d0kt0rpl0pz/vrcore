/*
 * decode_android.cpp -- the Quest's decoder (decode.h): the NDK's AMediaExtractor splits the file into its audio
 * track's packets, and AMediaCodec decodes them with whatever codec the system has (MP3, AAC, FLAC, Vorbis, raw
 * PCM for WAV). The file is opened by descriptor (storage.h's open_fd), a path and a picked folder's document alike.
 * Its tags are the extractor's file format's (the title and artist keys, API 29).
 *
 * The codec is asked for float PCM; one that ignores the request gives 16-bit, converted here. The rate and width
 * are the output format's, known once the codec reports it (until then the track's own): read() stops early when
 * they change, so one call's frames are always one format.
 */
#include "decode.h"

#include <sys/stat.h>
#include <unistd.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#include "storage.h"

namespace vrcore::audio {

namespace {

constexpr int32_t kPcm16 = 2;              // android.media.AudioFormat's ENCODING_PCM_16BIT
constexpr int32_t kPcmFloat = 4;           // ENCODING_PCM_FLOAT
constexpr int64_t kWaitUs = 10000;         // a dequeue's longest wait
constexpr int kStallTries = 200;           // ~2 s of a codec giving nothing: an error

class AndroidDecoder : public Decoder {
public:
    ~AndroidDecoder() override
    {
        if (codec_) {
            AMediaCodec_stop(codec_);
            AMediaCodec_delete(codec_);
        }
        if (ex_) AMediaExtractor_delete(ex_);
        if (fd_ >= 0) ::close(fd_);
    }

    bool open(const char *path)
    {
        fd_ = storage::open_fd(path, &why_);
        if (fd_ < 0) return false;
        struct stat st;
        if (fstat(fd_, &st) != 0) {
            why_ = std::string("cannot read it (") + std::strerror(errno) + ")";
            return false;
        }
        ex_ = AMediaExtractor_new();
        media_status_t s = AMediaExtractor_setDataSourceFd(ex_, fd_, 0, st.st_size);
        if (s != AMEDIA_OK) {
            why_ = "the system cannot open it (" + std::to_string((int)s) + ")";
            return false;
        }
        size_t tracks = AMediaExtractor_getTrackCount(ex_);
        for (size_t i = 0; i < tracks && !codec_; ++i) {
            AMediaFormat *f = AMediaExtractor_getTrackFormat(ex_, i);
            const char *mime = nullptr;
            if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && mime && !std::strncmp(mime, "audio/", 6)) {
                int32_t hz = 0, ch = 0;
                AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &hz);
                AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
                AMediaCodec *c = AMediaCodec_createDecoderByType(mime);
                if (!c) {
                    why_ = std::string("the system has no decoder for ") + mime;
                } else {
                    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_PCM_ENCODING, kPcmFloat);
                    if (AMediaCodec_configure(c, f, nullptr, nullptr, 0) != AMEDIA_OK ||
                        AMediaCodec_start(c) != AMEDIA_OK) {
                        why_ = std::string("the ") + mime + " decoder did not start";
                        AMediaCodec_delete(c);
                    } else {
                        codec_ = c;
                        AMediaExtractor_selectTrack(ex_, i);
                        hz_ = hz;
                        channels_ = ch;
                    }
                }
            }
            AMediaFormat_delete(f);
        }
        if (!codec_) {
            if (why_.empty()) why_ = "no audio track";
            return false;
        }
        if (hz_ <= 0 || channels_ < 1 || channels_ > kMaxDecodeChannels) {
            why_ = "an audio format it cannot play (" + std::to_string(hz_) + " Hz, " + std::to_string(channels_) +
                   " channels)";
            return false;
        }
        return true;
    }

    int read(float *buf, int frames) override
    {
        int done = 0;
        while (done < frames) {
            if (pos_ == pending_.size()) {
                if (out_eos_) break;
                int hz = hz_, ch = channels_;
                if (!pump()) return -1;
                if (done > 0 && (hz != hz_ || ch != channels_)) break;     // the next call's, in the new format
                continue;
            }
            size_t n = std::min((pending_.size() - pos_) / channels_, (size_t)(frames - done));
            std::memcpy(buf + done * channels_, pending_.data() + pos_, n * channels_ * sizeof(float));
            pos_ += n * channels_;
            done += (int)n;
        }
        return done;
    }

    int hz() const override { return hz_; }
    int channels() const override { return channels_; }

private:
    // Packets in until the codec gives one buffer of samples (or reports a format, or ends); false on an error.
    bool pump()
    {
        pending_.clear();
        pos_ = 0;
        for (int tries = 0; tries < kStallTries; ++tries) {
            if (!in_eos_) {
                ssize_t i = AMediaCodec_dequeueInputBuffer(codec_, 0);
                if (i >= 0) {
                    size_t cap = 0;
                    uint8_t *p = AMediaCodec_getInputBuffer(codec_, (size_t)i, &cap);
                    ssize_t n = p ? AMediaExtractor_readSampleData(ex_, p, cap) : -1;
                    if (n < 0) {
                        AMediaCodec_queueInputBuffer(codec_, (size_t)i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                        in_eos_ = true;
                    } else {
                        AMediaCodec_queueInputBuffer(codec_, (size_t)i, 0, (size_t)n,
                                                     (uint64_t)AMediaExtractor_getSampleTime(ex_), 0);
                        AMediaExtractor_advance(ex_);
                    }
                }
            }
            AMediaCodecBufferInfo info;
            ssize_t o = AMediaCodec_dequeueOutputBuffer(codec_, &info, kWaitUs);
            if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) return take_format();
            if (o < 0) continue;                // try again later, buffers changed
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) out_eos_ = true;
            size_t cap = 0;
            uint8_t *p = AMediaCodec_getOutputBuffer(codec_, (size_t)o, &cap);
            if (p && info.size > 0 && info.offset >= 0 && (size_t)(info.offset + info.size) <= cap) {
                const uint8_t *src = p + info.offset;
                if (encoding_ == kPcmFloat) {
                    size_t n = (size_t)info.size / sizeof(float);
                    pending_.resize(n - n % channels_);
                    std::memcpy(pending_.data(), src, pending_.size() * sizeof(float));
                } else {
                    size_t n = (size_t)info.size / sizeof(int16_t);
                    pending_.resize(n - n % channels_);
                    for (size_t k = 0; k < pending_.size(); ++k) {
                        int16_t v;
                        std::memcpy(&v, src + k * sizeof v, sizeof v);
                        pending_[k] = v * (1.f / 32768.f);
                    }
                }
            }
            AMediaCodec_releaseOutputBuffer(codec_, (size_t)o, false);
            if (!pending_.empty() || out_eos_) return true;
        }
        why_ = "the decoder stalled";
        return false;
    }

    // The codec's output format: its rate, width and sample type.
    bool take_format()
    {
        AMediaFormat *f = AMediaCodec_getOutputFormat(codec_);
        int32_t hz = 0, ch = 0, enc = kPcm16;
        AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &hz);
        AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
        AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_PCM_ENCODING, &enc);
        AMediaFormat_delete(f);
        if (hz <= 0 || ch < 1 || ch > kMaxDecodeChannels || (enc != kPcm16 && enc != kPcmFloat)) {
            why_ = "the decoder's output cannot be played (" + std::to_string(hz) + " Hz, " + std::to_string(ch) +
                   " channels, encoding " + std::to_string(enc) + ")";
            return false;
        }
        hz_ = hz;
        channels_ = ch;
        encoding_ = enc;
        return true;
    }

    int fd_ = -1;
    AMediaExtractor *ex_ = nullptr;
    AMediaCodec *codec_ = nullptr;
    int hz_ = 0, channels_ = 0;
    int32_t encoding_ = kPcm16;
    bool in_eos_ = false, out_eos_ = false;
    std::vector<float> pending_;
    size_t pos_ = 0;
};

}

void read_tags(const char *path, std::string *title, std::string *artist)
{
    title->clear();
    artist->clear();
    std::string why;
    int fd = storage::open_fd(path, &why);
    struct stat st;
    if (fd < 0) return;
    AMediaExtractor *ex = AMediaExtractor_new();
    if (fstat(fd, &st) == 0 && AMediaExtractor_setDataSourceFd(ex, fd, 0, st.st_size) == AMEDIA_OK) {
        if (AMediaFormat *f = AMediaExtractor_getFileFormat(ex)) {
            const char *s = nullptr;
            if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_TITLE, &s) && s) *title = s;
            s = nullptr;
            if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_ARTIST, &s) && s) *artist = s;
            AMediaFormat_delete(f);
        }
    }
    AMediaExtractor_delete(ex);
    ::close(fd);
}

std::unique_ptr<Decoder> open_decoder(const char *path, std::string *why)
{
    std::unique_ptr<AndroidDecoder> d(new AndroidDecoder);
    if (!d->open(path)) {
        *why = d->why();
        return nullptr;
    }
    return d;
}

}
