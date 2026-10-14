/*
 * decode_apple.cpp -- the Mac sim's decoder (decode.h): AudioToolbox's ExtAudioFile, which reads every file type
 * Core Audio knows (MP3, AAC in .m4a or ADTS, FLAC, WAV; Ogg Vorbis only where the system has it) and converts
 * to the client format we ask for: float, interleaved, at the file's own rate and width. The tags are AudioFile's
 * info dictionary (its title and artist).
 */
#include "decode.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <cstring>

namespace vrcore::audio {

namespace {

// An OSStatus as Core Audio prints them: four characters when it is a code, else the number.
std::string status_text(OSStatus s)
{
    char c[4] = {(char)((uint32_t)s >> 24), (char)((uint32_t)s >> 16), (char)((uint32_t)s >> 8), (char)s};
    bool printable = true;
    for (char ch : c) printable = printable && ch >= 32 && ch < 127;
    char buf[32];
    if (printable) std::snprintf(buf, sizeof buf, "'%.4s'", c);
    else std::snprintf(buf, sizeof buf, "%d", (int)s);
    return buf;
}

class AppleDecoder : public Decoder {
public:
    ~AppleDecoder() override
    {
        if (file_) ExtAudioFileDispose(file_);
    }

    bool open(const char *path)
    {
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path,
                                                               (CFIndex)std::strlen(path), false);
        if (!url) {
            why_ = "not a path";
            return false;
        }
        OSStatus s = ExtAudioFileOpenURL(url, &file_);
        CFRelease(url);
        if (s != noErr) {
            file_ = nullptr;
            why_ = "the system cannot open it (" + status_text(s) + ")";
            return false;
        }
        AudioStreamBasicDescription in{};
        UInt32 size = sizeof in;
        s = ExtAudioFileGetProperty(file_, kExtAudioFileProperty_FileDataFormat, &size, &in);
        if (s != noErr || in.mSampleRate <= 0 || in.mChannelsPerFrame < 1) {
            why_ = "no audio format (" + status_text(s) + ")";
            return false;
        }
        if ((int)in.mChannelsPerFrame > kMaxDecodeChannels) {
            why_ = std::to_string(in.mChannelsPerFrame) + " channels (at most " +
                   std::to_string(kMaxDecodeChannels) + ")";
            return false;
        }
        hz_ = (int)in.mSampleRate;
        channels_ = (int)in.mChannelsPerFrame;
        AudioStreamBasicDescription out{};
        out.mSampleRate = in.mSampleRate;
        out.mFormatID = kAudioFormatLinearPCM;
        out.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        out.mChannelsPerFrame = in.mChannelsPerFrame;
        out.mBitsPerChannel = 32;
        out.mFramesPerPacket = 1;
        out.mBytesPerFrame = out.mBytesPerPacket = 4 * in.mChannelsPerFrame;
        s = ExtAudioFileSetProperty(file_, kExtAudioFileProperty_ClientDataFormat, sizeof out, &out);
        if (s != noErr) {
            why_ = "the system has no decoder for it (" + status_text(s) + ")";
            return false;
        }
        return true;
    }

    int read(float *buf, int frames) override
    {
        AudioBufferList list;
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = (UInt32)channels_;
        list.mBuffers[0].mDataByteSize = (UInt32)(frames * channels_ * sizeof(float));
        list.mBuffers[0].mData = buf;
        UInt32 n = (UInt32)frames;
        OSStatus s = ExtAudioFileRead(file_, &n, &list);
        if (s != noErr) {
            why_ = "decoding failed (" + status_text(s) + ")";
            return -1;
        }
        return (int)n;
    }

    int hz() const override { return hz_; }
    int channels() const override { return channels_; }

private:
    ExtAudioFileRef file_ = nullptr;
    int hz_ = 0;
    int channels_ = 0;
};

}

namespace {

// A dictionary's string value as UTF-8 (empty: none).
std::string info_string(CFDictionaryRef info, const char *key)
{
    CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
    const void *v = k ? CFDictionaryGetValue(info, k) : nullptr;
    if (k) CFRelease(k);
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return std::string();
    CFStringRef s = static_cast<CFStringRef>(v);
    CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::string out((size_t)size, '\0');
    if (!CFStringGetCString(s, &out[0], size, kCFStringEncodingUTF8)) return std::string();
    out.resize(std::strlen(out.c_str()));
    return out;
}

}

void read_tags(const char *path, std::string *title, std::string *artist)
{
    title->clear();
    artist->clear();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path, (CFIndex)std::strlen(path),
                                                           false);
    if (!url) return;
    AudioFileID file = nullptr;
    OSStatus s = AudioFileOpenURL(url, kAudioFileReadPermission, 0, &file);
    CFRelease(url);
    if (s != noErr) return;
    CFDictionaryRef info = nullptr;
    UInt32 size = sizeof info;
    if (AudioFileGetProperty(file, kAudioFilePropertyInfoDictionary, &size, &info) == noErr && info) {
        *title = info_string(info, kAFInfoDictionary_Title);
        *artist = info_string(info, kAFInfoDictionary_Artist);
        CFRelease(info);
    }
    AudioFileClose(file);
}

std::unique_ptr<Decoder> open_decoder(const char *path, std::string *why)
{
    std::unique_ptr<AppleDecoder> d(new AppleDecoder);
    if (!d->open(path)) {
        *why = d->why();
        return nullptr;
    }
    return d;
}

}
