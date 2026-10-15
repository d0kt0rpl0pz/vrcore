#include "ktx2.h"

#include <cstring>
#include <utility>

#include "../app/log.h"

namespace vrcore::gpu {

namespace {

// VkFormat values (the spec's, fixed): the four this reader takes.
constexpr uint32_t kAstc4x4Unorm = 157, kAstc4x4Srgb = 158, kAstc6x6Unorm = 165, kAstc6x6Srgb = 166;

// Largest side taken: a file's header is not trusted to size an allocation.
constexpr uint32_t kMaxSide = 16384;

const uint8_t kIdentifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

constexpr size_t kHeaderBytes = 80;         // the identifier, 13 u32 fields and two u64 (sgdByteOffset, -Length)
constexpr size_t kLevelIndexEntry = 24;     // byteOffset, byteLength, uncompressedByteLength: u64 each

uint32_t rd32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint64_t rd64(const uint8_t *p)
{
    return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32;
}

bool format_of(uint32_t vk_format, TextureFormat &out)
{
    switch (vk_format) {
    case kAstc4x4Unorm: out = TextureFormat::ASTC_4x4; return true;
    case kAstc4x4Srgb: out = TextureFormat::ASTC_4x4_sRGB; return true;
    case kAstc6x6Unorm: out = TextureFormat::ASTC_6x6; return true;
    case kAstc6x6Srgb: out = TextureFormat::ASTC_6x6_sRGB; return true;
    }
    return false;
}

bool fail(const char **why, const char *message)
{
    if (why) *why = message;
    return false;
}

}

bool ktx2_parse(const void *file, size_t bytes, Ktx2Image &out, const char **why)
{
    const uint8_t *f = static_cast<const uint8_t *>(file);
    if (!f || bytes < kHeaderBytes || std::memcmp(f, kIdentifier, sizeof kIdentifier) != 0)
        return fail(why, "not a KTX2 file");

    uint32_t vk_format = rd32(f + 12);
    uint32_t width = rd32(f + 20), height = rd32(f + 24), depth = rd32(f + 28);
    uint32_t layers = rd32(f + 32), faces = rd32(f + 36), levels = rd32(f + 40), supercompression = rd32(f + 44);

    TextureFormat format;
    if (!format_of(vk_format, format)) return fail(why, "not ASTC 4x4 or 6x6");
    if (supercompression != 0) return fail(why, "supercompressed (only raw ASTC is read)");
    if (depth > 1 || layers > 1 || faces != 1) return fail(why, "not a single 2D image");
    if (!width || !height || width > kMaxSide || height > kMaxSide) return fail(why, "bad size");
    if (levels == 0) return fail(why, "no stored levels (generated mips are not supported)");

    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.mips = levels;
    if (texture_mip_count(desc) != levels) return fail(why, "more levels than the size has");

    size_t index_bytes = size_t(levels) * kLevelIndexEntry;
    if (bytes - kHeaderBytes < index_bytes) return fail(why, "truncated level index");
    const uint8_t *index = f + kHeaderBytes;

    // Each level is checked against the size the format and extent make of it, and against the file; the levels go
    // into `data` where texture_level_offset puts them (a file keeps the smallest level first, and aligned).
    std::vector<uint8_t> data(texture_bytes(desc));
    for (uint32_t i = 0; i < levels; ++i) {
        uint64_t offset = rd64(index + i * kLevelIndexEntry), length = rd64(index + i * kLevelIndexEntry + 8);
        uint64_t end = offset + length;
        if (end < offset || end > bytes) return fail(why, "a level lies outside the file");
        TextureDesc to_i = desc;
        to_i.mips = i + 1;                  // the chain through level i ends where level i does
        size_t from = texture_level_offset(desc, i);
        size_t want = texture_bytes(to_i) - from;
        if (length != want) return fail(why, "a level's size does not match its extent");
        std::memcpy(data.data() + from, f + offset, want);
    }

    out.desc = desc;
    out.data = std::move(data);
    return true;
}

Texture *ktx2_create_texture(Renderer &r, const void *file, size_t bytes, Filter filter, Address address,
                             const char *name)
{
    Ktx2Image img;
    const char *why = nullptr;
    if (!ktx2_parse(file, bytes, img, &why)) {
        log("%s: %s", name, why ? why : "unreadable");
        return nullptr;
    }
    img.desc.filter = filter;
    img.desc.address = address;
    return r.create_texture(img.desc, img.data.data());
}

}
