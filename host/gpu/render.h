/*
 * render.h -- the render API a front draws with (v1). No Vulkan here: buffers, textures, targets and
 * pipelines are opaque. A display frame is recorded in two parts: the front's prepare (uploads, dynamic
 * buffers, its uniforms and offscreen passes into targets), then its draw inside the one multiview eye pass.
 *
 * The shader contract (shaders/vrcore_views.glsl, include it):
 *   set 0, binding 0: the Views block, per frame, both eyes: view_proj / view / proj [2], eye_pos [2],
 *                     time (x = seconds since start). gl_ViewIndex picks the eye (eye pass only).
 *   set 1, binding 0: the front's uniform block, its own layout, per frame (Frame::uniforms; <= kUniformBytes).
 *   set 2, binding 0..kMaxTextures-1: combined image samplers, per draw (Frame::bind_textures); a binding not
 *                     given holds a 1x1 black texture.
 *   push constants:   up to 128 bytes, vertex and fragment stages, the front's own layout.
 *   specialization:   constant_id k takes PipelineDesc::spec's value for k (32 bits), both stages.
 *   depth:            reverse-Z (1 near, 0 far, cleared to 0); a pipeline's depth_compare is the Vulkan one
 *                     (Less on a reversed-Z target is Greater, LEqual is GreaterOrEqual).
 *   stencil:          the eye pass has 8 bits, cleared to 0 (offscreen targets have none).
 *   colour:           linear; sRGB images (the eyes, RGBA8_sRGB and ASTC_*_sRGB textures, targets) encode and decode.
 *   blending:         "Blend A B": the same factors for colour and alpha.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "vmath.h"

namespace vrcore::gpu {

struct Buffer;
struct DynamicBuffer;
struct Texture;
struct Target;
struct Pipeline;
struct RendererImpl;      // render_host.h

constexpr uint32_t kMaxTextures = 8;        // set 2's bindings
// The eye projection's planes, metres (0.1 to 1000). A
// shader that writes its own depth (reverse-Z near / w) or clips at the near plane takes kEyeNear.
constexpr float kEyeNear = 0.1f, kEyeFar = 1000.f;
constexpr uint32_t kUniformBytes = 4096;    // set 1's block
constexpr uint32_t kMaxMips = 16;           // a texture's mip levels, at most

enum class BufferUsage { Vertex, Index };
enum class IndexType { U16, U32 };
enum class Format { Float1, Float2, Float3, Float4, UNorm8x4 };
enum class Topology { Triangles, TriangleStrip, Lines };
enum class Cull { None, Back, Front };
// Opaque: off. Alpha: SrcAlpha, OneMinusSrcAlpha. Premultiplied: One, OneMinusSrcAlpha. Additive: One, One.
enum class Blend { Opaque, Alpha, Premultiplied, Additive };
enum class Compare { Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };
enum class StencilOp { Keep, Zero, Replace, IncrementWrap, DecrementWrap };
// ASTC: LDR, 16-byte blocks of 4x4 or 6x6 texels, where Renderer::astc() says the device has it (Quest 3, Apple
// silicon). A compressed texture is sampled only: no target and no offscreen pipeline is made of one.
enum class TextureFormat { R8, RGBA8, RGBA8_sRGB, ASTC_4x4, ASTC_4x4_sRGB, ASTC_6x6, ASTC_6x6_sRGB };
enum class Filter { Nearest, Linear };
enum class Address { Clamp, Repeat, RepeatU };   // RepeatU: across repeated, down clamped (a panorama)
enum class Pass { Eye, Offscreen };

struct VertexAttr {
    uint32_t location;
    uint32_t offset;
    Format format;
};

struct ShaderCode {
    const uint32_t *words = nullptr;
    size_t bytes = 0;
};

struct SpecConstant {
    uint32_t id;
    uint32_t value;                         // an int, a bool (0 / 1) or a float's bits
};

struct StencilDesc {
    bool enable = false;
    Compare compare = Compare::Always;
    StencilOp pass = StencilOp::Keep;       // on depth and stencil passing; the stencil failing keeps
    uint8_t ref = 0;
    StencilOp depth_fail = StencilOp::Keep; // on the stencil passing and depth failing (z-fail shadow volumes)
};

struct PipelineDesc {
    ShaderCode vert, frag;
    uint32_t stride = 0;                    // one vertex buffer, per vertex (0: none)
    const VertexAttr *attrs = nullptr;
    uint32_t attr_count = 0;
    Topology topology = Topology::Triangles;
    Cull cull = Cull::Back;                 // counter-clockwise is front
    Blend blend = Blend::Opaque;
    bool color_write = true;                // false: depth and stencil only
    bool depth_test = true;                 // eye pass only
    bool depth_write = true;
    Compare depth_compare = Compare::GreaterOrEqual;
    StencilDesc stencil;                    // eye pass only
    const SpecConstant *spec = nullptr;
    uint32_t spec_count = 0;
    Pass pass = Pass::Eye;                  // Offscreen: for targets of offscreen_format (1 sample, one view)
    TextureFormat offscreen_format = TextureFormat::RGBA8_sRGB;
};

struct TextureDesc {
    uint32_t width = 0, height = 0;
    TextureFormat format = TextureFormat::RGBA8;
    Filter filter = Filter::Nearest;
    Address address = Address::Clamp;
    uint32_t mips = 1;                      // levels (level 0 is width x height, each next one half, down to 1x1);
                                            //   more than the chain has, or kMaxMips, is cut. Linear filtering
                                            //   blends between levels, Nearest picks one.
};

// The data create_texture, update_texture and Frame::upload take for `d`: the mip levels one after the other,
// level 0 first, each level's rows tightly packed (a block row for ASTC, with partial blocks at the right and
// bottom edges) and each level starting on a 4-byte boundary (a no-op for all but small R8 levels).
uint32_t texture_mip_count(const TextureDesc &d);                   // d.mips, cut to what the texture can have
size_t texture_level_offset(const TextureDesc &d, uint32_t level);  // where `level` starts
size_t texture_bytes(const TextureDesc &d);                         // the whole of it

// One eye's view this frame (from OpenXR's located views, or the sim's fake head).
struct Eye {
    Mat4 view;                              // world to eye
    Mat4 proj;                              // reverse-Z
    float pos[3];                           // world position
};

// The calls a front makes while a frame is recorded. In prepare: upload, write, uniforms and offscreen passes;
// in a pass (an offscreen one, or the eye pass in draw): binds and draws.
class Frame {
public:
    // ---- prepare (outside any pass) ----
    // A texture's whole contents (every mip level, laid out as texture_bytes counts), from this frame on.
    void upload(Texture *t, const void *data);
    // This frame's contents of `b`: `bytes` to write at the pointer returned (null when it could not grow). The
    // buffer keeps them for every later frame until the next write; one write a frame (a second one in the
    // same frame overwrites the first's).
    void *write(DynamicBuffer *b, size_t bytes);
    // Set 1's block for every draw of this frame (<= kUniformBytes).
    void uniforms(const void *data, uint32_t bytes);
    // An offscreen pass into `t` (cleared to `clear_rgba`, linear; null: its contents kept), then draws, then
    // end_offscreen. After it the target samples as a texture (target_texture).
    void offscreen(Target *t, const float *clear_rgba);
    void end_offscreen();

    // ---- in a pass ----
    void bind(const Pipeline *p);
    void bind_vertices(const Buffer *b, size_t offset = 0);
    void bind_vertices(const DynamicBuffer *b, size_t offset = 0);
    void bind_indices(const Buffer *b, IndexType type, size_t offset = 0);
    void bind_indices(const DynamicBuffer *b, IndexType type, size_t offset = 0);
    void bind_textures(const Texture *const *t, uint32_t count);      // set 2, bindings 0..count-1
    void push(const void *data, uint32_t bytes);                       // <= 128
    void draw(uint32_t vertices, uint32_t first = 0, uint32_t instances = 1);
    void draw_indexed(uint32_t indices, uint32_t first = 0, int32_t vertex_offset = 0, uint32_t instances = 1);

    double time = 0;                        // seconds since the renderer started
    uint64_t number = 0;                    // display frames so far
    Eye eyes[2];

    RendererImpl *impl = nullptr;           // the host's
};

// What a front builds its resources with (at init) and frees them with (at shutdown).
class Renderer {
public:
    Buffer *create_buffer(BufferUsage usage, size_t bytes, const void *data);
    void update_buffer(Buffer *b, size_t offset, size_t bytes, const void *data);   // host-visible
    void destroy_buffer(Buffer *b);

    // A buffer rewritten while frames are in flight (Frame::write): one copy per frame in flight, grown on
    // demand from `initial_bytes`.
    DynamicBuffer *create_dynamic_buffer(BufferUsage usage, size_t initial_bytes);
    void destroy_dynamic_buffer(DynamicBuffer *b);

    // A 2D texture; `data` (null: black) as Frame::upload takes it. Null when it cannot be made, and for an ASTC
    // format when astc() is false (logged): the caller falls back to an uncompressed texture. update_texture waits
    // for the GPU: for init, not for every frame (Frame::upload).
    Texture *create_texture(const TextureDesc &desc, const void *data);
    void update_texture(Texture *t, const void *data);
    void destroy_texture(Texture *t);

    // An offscreen colour image: no MSAA, no depth (RGBA8 or RGBA8_sRGB), cleared to 0 at creation.
    Target *create_target(uint32_t width, uint32_t height, TextureFormat format, Filter filter);
    const Texture *target_texture(const Target *t) const;
    void destroy_target(Target *t);

    Pipeline *create_pipeline(const PipelineDesc &desc);
    void destroy_pipeline(Pipeline *p);

    bool fdm() const;                       // foveation's density map is on (FragmentDensityEXT usable)
    bool astc() const;                      // ASTC textures can be made (textureCompressionASTC_LDR)

    RendererImpl *impl = nullptr;           // the host's
};

}
