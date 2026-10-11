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
 */
#include <cmath>
#include <cstdint>
#include <vector>

#include "../app/app.h"
#include "../app/layers.h"
#include "../app/log.h"
#include "../gpu/render.h"

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
    Layer *layer = nullptr;
} scene;

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
           scene.layer;
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

void scene_prepare(gpu::Frame &f)
{
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
    PanelPush p = {{1, 1, 1, 1}, 1.f};
    f.push(&p, sizeof p);
    f.draw(6);
    f.bind(scene.tint);
    p = {{1, 0, 0, 0.35f}, 0.f};
    f.push(&p, sizeof p);
    f.draw(6, 6);
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
    r.destroy_target(scene.target);
    r.destroy_dynamic_buffer(scene.panel);
    layer_destroy(r, scene.layer);
    scene = Scene();
}

}

const AppInfo &front_info()
{
    static const AppInfo info = {"vrcore scene", scene_init, scene_draw, scene_shutdown, nullptr, nullptr,
                                 scene_prepare};
    return info;
}

}
