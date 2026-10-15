/*
 * scene_front.cpp -- the M1 test scene, as a front: lit cubes in a ring at 1.5-30 m (slowly turning),
 * a floor 1.3 m below the start pose and a wall 6 m ahead, each a grid of 2 mm-wide quads every 5 cm.
 * The grids are thin triangles rather than LINE primitives so that 4x MSAA visibly smooths them, and
 * they fill the field of view so that the foveation levels show in the periphery.
 *
 * And render API v1 (M2), all of it in one panel 2 m ahead: an offscreen target drawn each frame (a cyan
 * checker: an R8 texture, scrolled by the front's uniform block, its colour a specialization constant), shown
 * on a quad from a dynamic buffer that bobs, which writes stencil 1; then a larger quad tinted red, blended,
 * drawn only where the stencil is 1. Right: a cyan scrolling checker on the panel, tinted pink, nothing red
 * round it.
 *
 * And a quad layer (app/layers.h, M2), 1 x 0.5 m up and left of the panel, redrawn every 30 frames: the same
 * checker over a half-transparent green, straight alpha. On the headset the compositor draws it; in the sim a
 * quad over everything.
 *
 * And KTX2 (gpu/ktx2.h), in a row of quads under the panel: a 16 x 16 ASTC 4x4 sRGB texture with its five mip
 * levels, built in memory as a KTX2 file and read back by the reader, one quad a level (the shader's lod): level 0
 * is a 4 x 4 checker of green and blue with a red block at the top left (so the picture's orientation shows), the
 * levels after it checkers of their own colour, each half as many blocks. Then a 10 x 10 ASTC 6x6 linear texture
 * with two levels (partial blocks at the edge), a yellow and a cyan checker, at the end of the row. Where the
 * device has no ASTC the row is not drawn and the log says so.
 *
 * And the menu (ui/menu.h; Esc, Start or the left menu button) with the overlay (app/layers.h): while it is up, a
 * yellow cube 10 cm across turns 0.5 m ahead of where the head was as it opened, between the eyes and the panel
 * (1.1 m), drawn on the overlay with the scene's own lit pipeline. It must stand in front of the panel's text: in
 * the eye pass the panel's quad layer would cover it. The menu's row turns it off, to compare. --menu opens the
 * menu at the start (a headless --shot of it).
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "../app/app.h"
#include "../app/layers.h"
#include "../app/log.h"
#include "../app/input.h"
#include "../gpu/ktx2.h"
#include "../gpu/render.h"
#include "../ui/menu.h"

namespace vrcore {

namespace {

const uint32_t kSceneVert[] = {
#include "scene.vert.h"
};
const uint32_t kSceneFrag[] = {
#include "scene.frag.h"
};
const uint32_t kSceneFdmFrag[] = {
#include "scene_fdm.frag.h"
};
const uint32_t kPanelVert[] = {
#include "panel.vert.h"
};
const uint32_t kPanelFrag[] = {
#include "panel.frag.h"
};
const uint32_t kOffscreenVert[] = {
#include "offscreen.vert.h"
};
const uint32_t kOffscreenFrag[] = {
#include "offscreen.frag.h"
};

struct PanelVertex {
    float pos[3];
    float uv[2];
};

struct PanelPush {
    float color[4];
    float textured;
    float lod;                              // below 0: the level the screen picks
};

struct Vertex {
    float pos[3];
    float normal[3];
};

struct Push {
    Mat4 model;
    float color[4];
};

constexpr float kFloorY = -1.3f;
constexpr float kWallZ = -6.f;
constexpr float kLineWidth = 0.002f, kLineStep = 0.05f;

struct Scene {
    gpu::Buffer *cube = nullptr, *grid = nullptr;
    uint32_t cube_count = 0, grid_count = 0;
    gpu::Pipeline *lit = nullptr, *flat = nullptr;
    // render API v1's panel
    gpu::Texture *checker = nullptr;
    gpu::Target *target = nullptr;
    const gpu::Texture *target_tex = nullptr;
    gpu::DynamicBuffer *panel = nullptr;
    gpu::Pipeline *offscreen = nullptr, *panel_pipe = nullptr, *tint = nullptr;
    // KTX2: the row of quads, their two ASTC textures and the pipeline (no stencil, so the panel's tint leaves them)
    gpu::Texture *ktx_4x4 = nullptr, *ktx_6x6 = nullptr;
    gpu::Buffer *ktx_quads = nullptr;
    gpu::Pipeline *ktx_pipe = nullptr;
    Layer *layer = nullptr;
    // the menu and the overlay's test cube
    ui::Menu menu;
    ui::Page top;
    bool cube_on = true;
    bool was_shown = false;
    bool open_at_start = false;             // --menu
    float cube_at[3] = {0, 0, 0};           // 0.5 m ahead of the head as the menu opened, 5 cm under the eyes
} scene;

constexpr float kCubeAhead = 0.5f, kCubeSize = 0.1f;
constexpr uint32_t kKtxQuads = 7;

void menu_build()
{
    ui::Item resume;
    resume.label = "Resume";
    resume.change = [](int) { scene.menu.close(); };
    ui::Item cube;
    cube.label = "Overlay test cube";
    cube.value = [] { return std::string(scene.cube_on ? "On" : "Off"); };
    cube.change = [](int) { scene.cube_on = !scene.cube_on; };
    cube.help = "A cube on the overlay, between you and this panel: it must stand in front of the text.";
    scene.top = {resume, cube};
    scene.menu.set_top(&scene.top, "SCENE");
}

int scene_option(int, char **argv, int i)
{
    if (std::string(argv[i]) != "--menu") return 0;
    scene.open_at_start = true;
    return 1;
}

void scene_update(const Tick &t)
{
    const Input &in = input();
    if (scene.open_at_start && in.head_tracked) {
        scene.open_at_start = false;
        scene.menu.open();
    }
    scene.menu.update(t, in);
    bool shown = scene.menu.shown();
    if (shown && !scene.was_shown) {
        // the head's forward, level, as the menu places its panel
        const Quat &q = in.head.rot;
        float fx = -(2.f * (q.x * q.z + q.w * q.y)), fz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
        float n = std::sqrt(fx * fx + fz * fz);
        if (n < 1e-4f) fx = 0, fz = -1, n = 1;
        scene.cube_at[0] = in.head.pos[0] + fx / n * kCubeAhead;
        scene.cube_at[1] = in.head.pos[1] - 0.05f;
        scene.cube_at[2] = in.head.pos[2] + fz / n * kCubeAhead;
    }
    scene.was_shown = shown;
}

void cube_overlay_draw(gpu::Frame &f)
{
    float spin = (float)f.time;
    Push p;
    p.model = mat4_translate(scene.cube_at[0], scene.cube_at[1], scene.cube_at[2]);
    p.model = mat4_mul(p.model, mat4_rotate(quat_mul(quat_axis_angle(0, 1, 0, spin),
                                                     quat_axis_angle(1, 0, 0, 0.6f))));
    p.model = mat4_mul(p.model, mat4_scale(kCubeSize, kCubeSize, kCubeSize));
    p.color[0] = 1.f;
    p.color[1] = 0.9f;
    p.color[2] = 0.1f;
    p.color[3] = 1.f;
    f.bind(scene.lit);
    f.bind_vertices(scene.cube);
    f.push(&p, sizeof p);
    f.draw(scene.cube_count);
}

void panel_quad(PanelVertex *v, float cx, float cy, float z, float half)
{
    const float c[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) {
        const float *k = c[order[i]];
        v[i] = {{cx + k[0] * half, cy + k[1] * half, z}, {0.5f + 0.5f * k[0], 0.5f - 0.5f * k[1]}};
    }
}

// A KTX2 file of ASTC void-extent blocks (one colour a block: a test needs no encoder). `colour(level, bx, by,
// rgba)` gives each block's colour as four 16-bit values. Written as toktx does: the level index in order, the
// levels smallest first, each on a 16-byte boundary; no data descriptor or key/value data.
template <typename F>
std::vector<uint8_t> ktx2_file(uint32_t vk_format, uint32_t block, uint32_t width, uint32_t height, uint32_t levels,
                               F colour)
{
    std::vector<uint8_t> file(80 + 24 * levels, 0);
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) file[at + i] = uint8_t(v >> (8 * i));
    };
    auto put64 = [&](size_t at, uint64_t v) {
        for (int i = 0; i < 8; ++i) file[at + i] = uint8_t(v >> (8 * i));
    };
    static const uint8_t id[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    std::copy(id, id + 12, file.begin());
    put32(12, vk_format);
    put32(16, 1);                                           // typeSize
    put32(20, width);
    put32(24, height);
    put32(36, 1);                                           // faceCount
    put32(40, levels);

    for (uint32_t l = levels; l-- > 0;) {                   // smallest first
        uint32_t w = std::max(width >> l, 1u), h = std::max(height >> l, 1u);
        uint32_t bx = (w + block - 1) / block, by = (h + block - 1) / block;
        file.resize((file.size() + 15) & ~size_t(15), 0);
        size_t at = file.size();
        put64(80 + 24 * l, at);
        put64(80 + 24 * l + 8, 16ull * bx * by);
        put64(80 + 24 * l + 16, 16ull * bx * by);
        file.resize(at + 16 * size_t(bx) * by, 0);
        for (uint32_t y = 0; y < by; ++y)
            for (uint32_t x = 0; x < bx; ++x) {
                uint16_t rgba[4];
                colour(l, x, y, rgba);
                uint8_t *b = &file[at + 16 * (size_t(y) * bx + x)];
                static const uint8_t head[8] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
                std::copy(head, head + 8, b);               // a void-extent block, LDR, no extent
                for (int c = 0; c < 4; ++c) {
                    b[8 + 2 * c] = uint8_t(rgba[c]);
                    b[9 + 2 * c] = uint8_t(rgba[c] >> 8);
                }
            }
    }
    return file;
}

bool ktx_init(gpu::Renderer &r)
{
    if (!r.astc()) {
        log("scene: no ASTC on this device; the KTX2 row is not drawn");
        return true;
    }
    // 4x4 sRGB, 16 x 16, 5 levels: level 0 green / blue with a red corner, then a colour a level
    static const uint16_t tint[5][3] = {{0, 0xc000, 0}, {0xffff, 0xc000, 0}, {0xffff, 0, 0xffff},
                                        {0xffff, 0xffff, 0}, {0xffff, 0xffff, 0xffff}};
    auto four = [](uint32_t l, uint32_t x, uint32_t y, uint16_t rgba[4]) {
        bool odd = (x + y) & 1;
        rgba[3] = 0xffff;
        if (l == 0 && !x && !y) {
            rgba[0] = 0xffff, rgba[1] = rgba[2] = 0;
        } else if (l == 0) {
            rgba[0] = 0, rgba[1] = odd ? 0 : 0xc000, rgba[2] = odd ? 0xc000 : 0;
        } else {
            for (int c = 0; c < 3; ++c) rgba[c] = odd ? tint[l][c] / 3 : tint[l][c];
        }
    };
    std::vector<uint8_t> a = ktx2_file(158, 4, 16, 16, 5, four);        // VK_FORMAT_ASTC_4x4_SRGB_BLOCK
    scene.ktx_4x4 = gpu::ktx2_create_texture(r, a.data(), a.size(), gpu::Filter::Linear, gpu::Address::Clamp, "scene");

    // 6x6 linear, 10 x 10: 2 x 2 blocks, the right and bottom ones partial; level 1 (5 x 5) is one block
    auto six = [](uint32_t l, uint32_t x, uint32_t y, uint16_t rgba[4]) {
        bool odd = (x + y) & 1;
        rgba[0] = odd ? 0xffff : 0;
        rgba[1] = 0xffff;
        rgba[2] = odd ? 0 : 0xffff;
        rgba[3] = 0xffff;
        if (l) rgba[0] = rgba[1] = rgba[2] = 0x8000;
    };
    std::vector<uint8_t> b = ktx2_file(165, 6, 10, 10, 2, six);         // VK_FORMAT_ASTC_6x6_UNORM_BLOCK
    scene.ktx_6x6 = gpu::ktx2_create_texture(r, b.data(), b.size(), gpu::Filter::Nearest, gpu::Address::Clamp, "scene");

    // seven quads 14 cm across, 18 cm apart, under the panel (which bobs 0.1 m about y = 0.3, z = -2): the five
    // levels of the 4x4 texture, then the two of the 6x6
    std::vector<PanelVertex> v(6 * kKtxQuads);
    for (uint32_t i = 0; i < kKtxQuads; ++i)
        panel_quad(&v[6 * i], -0.54f + 0.18f * i, kFloorY + 1.6f - 0.6f, -2.f, 0.07f);
    scene.ktx_quads = r.create_buffer(gpu::BufferUsage::Vertex, v.size() * sizeof(PanelVertex), v.data());

    static const gpu::VertexAttr attrs[2] = {{0, 0, gpu::Format::Float3}, {1, 12, gpu::Format::Float2}};
    gpu::PipelineDesc p;
    p.vert = {kPanelVert, sizeof kPanelVert};
    p.frag = {kPanelFrag, sizeof kPanelFrag};
    p.stride = sizeof(PanelVertex);
    p.attrs = attrs;
    p.attr_count = 2;
    p.cull = gpu::Cull::None;
    scene.ktx_pipe = r.create_pipeline(p);
    return scene.ktx_4x4 && scene.ktx_6x6 && scene.ktx_quads && scene.ktx_pipe;
}

bool panel_init(gpu::Renderer &r)
{
    const uint8_t checker[16] = {255, 0, 255, 0, 0, 255, 0, 255, 255, 0, 255, 0, 0, 255, 0, 255};
    scene.checker = r.create_texture({4, 4, gpu::TextureFormat::R8, gpu::Filter::Nearest, gpu::Address::Repeat},
                                     checker);
    scene.target = r.create_target(256, 256, gpu::TextureFormat::RGBA8_sRGB, gpu::Filter::Linear);
    if (scene.target) scene.target_tex = r.target_texture(scene.target);
    scene.panel = r.create_dynamic_buffer(gpu::BufferUsage::Vertex, 12 * sizeof(PanelVertex));

    static const gpu::SpecConstant cyan = {0, 1};
    gpu::PipelineDesc o;
    o.vert = {kOffscreenVert, sizeof kOffscreenVert};
    o.frag = {kOffscreenFrag, sizeof kOffscreenFrag};
    o.cull = gpu::Cull::None;
    o.depth_test = o.depth_write = false;
    o.spec = &cyan;
    o.spec_count = 1;
    o.pass = gpu::Pass::Offscreen;
    o.offscreen_format = gpu::TextureFormat::RGBA8_sRGB;
    scene.offscreen = r.create_pipeline(o);

    static const gpu::VertexAttr attrs[2] = {{0, 0, gpu::Format::Float3}, {1, 12, gpu::Format::Float2}};
    gpu::PipelineDesc p;
    p.vert = {kPanelVert, sizeof kPanelVert};
    p.frag = {kPanelFrag, sizeof kPanelFrag};
    p.stride = sizeof(PanelVertex);
    p.attrs = attrs;
    p.attr_count = 2;
    p.cull = gpu::Cull::None;
    p.stencil = {true, gpu::Compare::Always, gpu::StencilOp::Replace, 1};
    scene.panel_pipe = r.create_pipeline(p);
    p.depth_test = p.depth_write = false;
    p.blend = gpu::Blend::Alpha;
    p.stencil = {true, gpu::Compare::Equal, gpu::StencilOp::Keep, 1};
    scene.tint = r.create_pipeline(p);
    scene.layer = layer_create(r, {512, 256, false});
    return scene.checker && scene.target && scene.panel && scene.offscreen && scene.panel_pipe && scene.tint &&
           scene.layer && ktx_init(r);
}

void scene_prepare(gpu::Frame &f)
{
    scene.menu.prepare(f);
    if (scene.menu.shown() && scene.cube_on) overlay_draw(cube_overlay_draw);

    float scroll[4] = {(float)f.time * 2.f, (float)f.time, 0, 0};
    f.uniforms(scroll, sizeof scroll);
    auto *v = static_cast<PanelVertex *>(f.write(scene.panel, 12 * sizeof(PanelVertex)));
    if (v) {
        float y = kFloorY + 1.6f + 0.1f * std::sin((float)f.time * 2.f);
        panel_quad(v, 0.f, y, -2.f, 0.4f);               // the panel
        panel_quad(v + 6, 0.f, y, -1.9f, 1.5f);          // the tint, larger: the stencil keeps it to the panel
    }
    static const float black[4] = {0, 0, 0, 1};
    f.offscreen(scene.target, black);
    f.bind(scene.offscreen);
    const gpu::Texture *t = scene.checker;
    f.bind_textures(&t, 1);
    f.draw(3);
    f.end_offscreen();

    const float at[3] = {-1.2f, kFloorY + 2.3f, -2.5f};
    layer_place(scene.layer, at, Quat(), 1.f, 0.5f, true);
    if (f.number % 30 == 0)
        if (gpu::Target *lt = layer_target(f, scene.layer)) {
            static const float green[4] = {0.f, 0.3f, 0.f, 0.5f};
            f.offscreen(lt, green);
            f.bind(scene.offscreen);
            f.bind_textures(&t, 1);
            f.draw(3);
            f.end_offscreen();
        }
}

void panel_draw(gpu::Frame &f)
{
    const gpu::Texture *t = scene.target_tex;
    f.bind(scene.panel_pipe);
    f.bind_vertices(scene.panel);
    f.bind_textures(&t, 1);
    PanelPush p = {{1, 1, 1, 1}, 1.f, -1.f};
    f.push(&p, sizeof p);
    f.draw(6);
    f.bind(scene.tint);
    p = {{1, 0, 0, 0.35f}, 0.f, -1.f};
    f.push(&p, sizeof p);
    f.draw(6, 6);

    if (!scene.ktx_pipe) return;
    f.bind(scene.ktx_pipe);
    f.bind_vertices(scene.ktx_quads);
    for (uint32_t i = 0; i < kKtxQuads; ++i) {
        const gpu::Texture *t = i < 5 ? scene.ktx_4x4 : scene.ktx_6x6;
        f.bind_textures(&t, 1);
        p = {{1, 1, 1, 1}, 1.f, i < 5 ? float(i) : float(i - 5)};
        f.push(&p, sizeof p);
        f.draw(6, 6 * i);
    }
}

void quad(std::vector<Vertex> &v, const float a[3], const float b[3], const float c[3], const float d[3],
          const float n[3])
{
    const float *corners[6] = {a, b, c, a, c, d};      // a b c d counter-clockwise seen from the front
    for (const float *p : corners) v.push_back({{p[0], p[1], p[2]}, {n[0], n[1], n[2]}});
}

std::vector<Vertex> cube_mesh()
{
    std::vector<Vertex> v;
    for (int axis = 0; axis < 3; ++axis)
        for (int sign = -1; sign <= 1; sign += 2) {
            float n[3] = {0, 0, 0};
            n[axis] = (float)sign;
            int u = (axis + 1) % 3, w = (axis + 2) % 3;     // u x w = axis
            float c[4][3];
            const float uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (int k = 0; k < 4; ++k) {
                c[k][axis] = 0.5f * sign;
                c[k][u] = 0.5f * uv[k][0] * sign;           // flip the winding on the negative face
                c[k][w] = 0.5f * uv[k][1];
            }
            quad(v, c[0], c[1], c[2], c[3], n);
        }
    return v;
}

// Thin quads on a plane through `origin` spanned by unit axes `ax` and `ay` (both lengths in metres).
void grid_plane(std::vector<Vertex> &v, const float origin[3], const float ax[3], const float ay[3], float len_x,
                float len_y)
{
    const float zero[3] = {0, 0, 0};                    // unlit
    auto at = [&](float x, float y, float out[3]) {
        for (int i = 0; i < 3; ++i) out[i] = origin[i] + ax[i] * x + ay[i] * y;
    };
    float h = 0.5f * kLineWidth;
    int nx = (int)std::lround(len_x / kLineStep), ny = (int)std::lround(len_y / kLineStep);
    for (int i = 0; i <= nx; ++i) {                     // lines along y
        float x = -0.5f * len_x + i * kLineStep;
        float a[3], b[3], c[3], d[3];
        at(x - h, -0.5f * len_y, a); at(x + h, -0.5f * len_y, b);
        at(x + h, 0.5f * len_y, c); at(x - h, 0.5f * len_y, d);
        quad(v, a, b, c, d, zero);
    }
    for (int j = 0; j <= ny; ++j) {                     // lines along x
        float y = -0.5f * len_y + j * kLineStep;
        float a[3], b[3], c[3], d[3];
        at(-0.5f * len_x, y - h, a); at(0.5f * len_x, y - h, b);
        at(0.5f * len_x, y + h, c); at(-0.5f * len_x, y + h, d);
        quad(v, a, b, c, d, zero);
    }
}

bool scene_init(gpu::Renderer &r, const Options &o)
{
    std::vector<Vertex> cube = cube_mesh();
    std::vector<Vertex> grid;
    const float floor_o[3] = {0, kFloorY, 0}, x[3] = {1, 0, 0}, mz[3] = {0, 0, -1}, y[3] = {0, 1, 0};
    grid_plane(grid, floor_o, x, mz, 20.f, 20.f);
    const float wall_o[3] = {0, kFloorY + 3.f, kWallZ};
    grid_plane(grid, wall_o, x, y, 12.f, 6.f);
    scene.cube_count = (uint32_t)cube.size();
    scene.grid_count = (uint32_t)grid.size();
    scene.cube = r.create_buffer(gpu::BufferUsage::Vertex, cube.size() * sizeof(Vertex), cube.data());
    scene.grid = r.create_buffer(gpu::BufferUsage::Vertex, grid.size() * sizeof(Vertex), grid.data());

    bool fdm_view = o.fdm_view && r.fdm();
    if (o.fdm_view && !fdm_view) log("scene: --fdm-view needs the fragment density map; not on this device");
    static const gpu::VertexAttr attrs[2] = {{0, 0, gpu::Format::Float3}, {1, 12, gpu::Format::Float3}};
    gpu::PipelineDesc d;
    d.vert = {kSceneVert, sizeof kSceneVert};
    d.frag = fdm_view ? gpu::ShaderCode{kSceneFdmFrag, sizeof kSceneFdmFrag} : gpu::ShaderCode{kSceneFrag, sizeof kSceneFrag};
    d.stride = sizeof(Vertex);
    d.attrs = attrs;
    d.attr_count = 2;
    scene.lit = r.create_pipeline(d);
    d.cull = gpu::Cull::None;                           // the grids are seen from both sides
    scene.flat = r.create_pipeline(d);
    log("scene: %u cube vertices, %u grid vertices (%u quads)%s", scene.cube_count, scene.grid_count,
        scene.grid_count / 6, fdm_view ? ", density view" : "");
    if (!scene.menu.init(r, {})) return false;
    menu_build();
    return scene.cube && scene.grid && scene.lit && scene.flat && panel_init(r);
}

void scene_draw(gpu::Frame &f)
{
    Push p;
    f.bind(scene.flat);
    f.bind_vertices(scene.grid);
    p.model = mat4_identity();
    p.color[0] = p.color[1] = p.color[2] = 0.8f;
    p.color[3] = 1.f;
    f.push(&p, sizeof p);
    f.draw(scene.grid_count);

    // 24 cubes on a ring, at seven distances from 1.5 to 30 m; their size grows with the distance.
    static const float kDistance[7] = {1.5f, 2.5f, 4.f, 7.f, 12.f, 20.f, 30.f};
    static const float kColor[6][3] = {{0.9f, 0.2f, 0.15f}, {0.95f, 0.6f, 0.1f}, {0.2f, 0.75f, 0.25f},
                                       {0.15f, 0.45f, 0.95f}, {0.6f, 0.25f, 0.85f}, {0.9f, 0.9f, 0.9f}};
    f.bind(scene.lit);
    f.bind_vertices(scene.cube);
    for (int i = 0; i < 24; ++i) {
        float angle = i * (2.f * 3.14159265f / 24.f);
        float dist = kDistance[i % 7];
        float size = 0.25f * std::pow(dist, 0.6f);
        float spin = (float)f.time * (0.2f + 0.05f * (i % 5));
        Mat4 m = mat4_translate(std::sin(angle) * dist, kFloorY + 0.5f * size + 0.6f, -std::cos(angle) * dist);
        m = mat4_mul(m, mat4_rotate(quat_mul(quat_axis_angle(0, 1, 0, spin), quat_axis_angle(1, 0, 0, 0.5f * spin))));
        m = mat4_mul(m, mat4_scale(size, size, size));
        p.model = m;
        p.color[0] = kColor[i % 6][0];
        p.color[1] = kColor[i % 6][1];
        p.color[2] = kColor[i % 6][2];
        f.push(&p, sizeof p);
        f.draw(scene.cube_count);
    }
    panel_draw(f);
}

void scene_shutdown(gpu::Renderer &r)
{
    r.destroy_pipeline(scene.lit);
    r.destroy_pipeline(scene.flat);
    r.destroy_buffer(scene.cube);
    r.destroy_buffer(scene.grid);
    r.destroy_pipeline(scene.offscreen);
    r.destroy_pipeline(scene.panel_pipe);
    r.destroy_pipeline(scene.tint);
    r.destroy_texture(scene.checker);
    r.destroy_texture(scene.ktx_4x4);
    r.destroy_texture(scene.ktx_6x6);
    r.destroy_buffer(scene.ktx_quads);
    r.destroy_pipeline(scene.ktx_pipe);
    r.destroy_target(scene.target);
    r.destroy_dynamic_buffer(scene.panel);
    layer_destroy(r, scene.layer);
    scene.menu.shutdown(r);
    scene = Scene();
}

}

const AppInfo &front_info()
{
    static const AppInfo info = {"vrcore scene", scene_init, scene_draw, scene_shutdown, scene_option, scene_update,
                                 scene_prepare};
    return info;
}

}
