/*
 * decode.h -- a sound file decoded to float, through the platform's own decoders: AudioToolbox's ExtAudioFile
 * on the Mac (decode_apple.cpp), the NDK's AMediaExtractor + AMediaCodec on Android (decode_android.cpp). No
 * decoder of our own and no third-party one: what the platform plays is what plays. Used by the soundtrack's
 * streaming thread (soundtrack.h), never on the audio thread: it allocates, blocks on the file and logs.
 *
 * The samples come out interleaved at the file's own rate and width; the soundtrack resamples them to the
 * device's rate and folds them to stereo.
 *
 * A PATH here is a storage.h ref: a file's path, or on Android a picked folder's document (content://), opened by
 * descriptor.
 */
#pragma once

#include <memory>
#include <string>

namespace vrcore::audio {

// The file types offered, by extension (lower case, with the dot): .mp3 .m4a .aac .flac .ogg .wav.
bool decodable_name(const char *path);

class Decoder {
public:
    virtual ~Decoder() = default;
    // Up to `frames` frames into buf (interleaved float, channels() wide); the frames written, 0 at the end, -1 on
    // an error (why() says what). The rate and width may change between calls (Android reports its output format
    // once the codec has seen the stream): read them after each call.
    virtual int read(float *buf, int frames) = 0;
    virtual int hz() const = 0;
    virtual int channels() const = 0;
    const std::string &why() const { return why_; }

protected:
    std::string why_;
};

// The file opened for decoding; null, and *why saying what was wrong, when it cannot be.
std::unique_ptr<Decoder> open_decoder(const char *path, std::string *why);

// The file's title and artist tags (each empty when it has none, or the file cannot be read). For the library's
// scan, on its own thread.
void read_tags(const char *path, std::string *title, std::string *artist);

// The widest a decoder hands out; wider files are refused.
constexpr int kMaxDecodeChannels = 8;

}
