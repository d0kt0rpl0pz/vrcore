/*
 * ktx2.h -- a KTX2 reader for ASTC textures, so a model's or a front's textures are stored compressed and go to
 * the GPU as they are. Only what needs no transcoder is read: one 2D image (no array, cube or depth), ASTC 4x4
 * or 6x6 (LDR, sRGB or linear: the file's vkFormat says which), its mip levels all present, and no
 * supercompression (no Basis, no zstd, no zlib). Anything else is refused with a reason; a front falls back to an
 * uncompressed texture, as it does where the device has no ASTC (Renderer::astc()).
 *
 * The file's data descriptor and key/value data are not read: the vkFormat and the level index are all the
 * renderer needs, and the reader checks each level's size against what the format makes of its extent.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "render.h"

namespace vrcore::gpu {

struct Ktx2Image {
    TextureDesc desc;                       // size, ASTC format, mips; filter Nearest and address Clamp (the caller's)
    std::vector<uint8_t> data;              // the levels as texture_bytes counts them: create_texture's data
};

// Reads `bytes` of a .ktx2 file. False, with `why` (if not null) set to a static message, when it is not a file
// this reader takes.
bool ktx2_parse(const void *file, size_t bytes, Ktx2Image &out, const char **why = nullptr);

// ktx2_parse and create_texture, with the filter and address mode the texture is sampled with. Null on a file the
// reader refuses and where Renderer::astc() is false (both logged, `name` in the message).
Texture *ktx2_create_texture(Renderer &r, const void *file, size_t bytes, Filter filter, Address address,
                             const char *name = "ktx2");

}
