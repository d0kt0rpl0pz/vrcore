/*
 * sim_layers.cpp -- a front's quad layers in the sim (app/layers.h): there is no compositor, so each is an
 * offscreen target the front draws into, shown as a textured rectangle in the eye pass after the front's draw,
 * over everything (no depth test), in the order the layers were made: straight alpha blended, or opaque. An
 * underlay is drawn by layer_punch instead, where the front punches it: with the depth test, as the headset's
 * compositor shows it through the hole.
 */
#include "sim_layers.h"

#include <algorithm>
#include <vector>

#include "../app/layers.h"
#include "../app/log.h"

namespace vrcore {

struct Layer {
    LayerDesc desc;
    gpu::Target *target = nullptr;
    const gpu::Texture *tex = nullptr;
    float pos[3] = {0, 0, 0};
    Quat rot;
    float width_m = 1, height_m = 1;
    bool up = false;
    bool drawn = false;                     // the front has drawn a picture into it
    float crop[2] = {1, 1};                 // the shown part, a fraction of the picture (layer_crop)
};

namespace sim {

namespace {

const uint32_t kLayerVert[] = {
#include "layer_quad.vert.h"
};
const uint32_t kLayerFrag[] = {
#include "layer_quad.frag.h"
};

std::vector<Layer *> g_layers;
gpu::Pipeline *g_alpha = nullptr, *g_opaque = nullptr;
gpu::Pipeline *g_under_alpha = nullptr, *g_under_opaque = nullptr;     // layer_punch's: depth tested and written

void draw_layer(gpu::Frame &f, const Layer *l, gpu::Pipeline *p)
{
    struct {
        Mat4 model;
        int32_t eye;
        int32_t pad;
        float crop[2];
    } push = {mat4_mul(mat4_translate(l->pos[0], l->pos[1], l->pos[2]),
                       mat4_mul(mat4_rotate(l->rot), mat4_scale(l->width_m, l->height_m, 1.f))),
              l->desc.eye, 0, {l->crop[0], l->crop[1]}};
    f.bind(p);
    f.bind_textures(&l->tex, 1);
    f.push(&push, sizeof push);
    f.draw(4);
}

}

bool layers_init(gpu::Renderer &r)
{
    gpu::PipelineDesc d;
    d.vert = {kLayerVert, sizeof kLayerVert};
    d.frag = {kLayerFrag, sizeof kLayerFrag};
    d.topology = gpu::Topology::TriangleStrip;
    d.cull = gpu::Cull::None;
    d.depth_test = d.depth_write = false;
    g_opaque = r.create_pipeline(d);
    d.blend = gpu::Blend::Alpha;
    g_alpha = r.create_pipeline(d);
    d.depth_test = d.depth_write = true;
    g_under_alpha = r.create_pipeline(d);
    d.blend = gpu::Blend::Opaque;
    g_under_opaque = r.create_pipeline(d);
    return g_opaque && g_alpha && g_under_alpha && g_under_opaque;
}

void layers_draw(gpu::Frame &f)
{
    for (const Layer *l : g_layers)
        if (l->up && l->drawn && !l->desc.underlay) draw_layer(f, l, l->desc.opaque ? g_opaque : g_alpha);
}

void layers_shutdown(gpu::Renderer &r)
{
    r.destroy_pipeline(g_alpha);
    r.destroy_pipeline(g_opaque);
    r.destroy_pipeline(g_under_alpha);
    r.destroy_pipeline(g_under_opaque);
    g_alpha = g_opaque = g_under_alpha = g_under_opaque = nullptr;
}

}

Layer *layer_create(gpu::Renderer &r, const LayerDesc &d)
{
    Layer *l = new Layer;
    l->desc = d;
    l->target = r.create_target(d.width, d.height, gpu::TextureFormat::RGBA8_sRGB, gpu::Filter::Linear);
    if (!l->target) {
        delete l;
        return nullptr;
    }
    l->tex = r.target_texture(l->target);
    sim::g_layers.push_back(l);
    log("sim: layer %ux%u, %s (a quad in the eye pass)", d.width, d.height, d.opaque ? "opaque" : "straight alpha");
    return l;
}

void layer_destroy(gpu::Renderer &r, Layer *l)
{
    if (!l) return;
    sim::g_layers.erase(std::remove(sim::g_layers.begin(), sim::g_layers.end(), l), sim::g_layers.end());
    r.destroy_target(l->target);
    delete l;
}

gpu::Target *layer_target(gpu::Frame &, Layer *l)
{
    l->drawn = true;
    return l->target;
}

void layer_place(Layer *l, const float pos[3], const Quat &rot, float width_m, float height_m, bool up)
{
    l->pos[0] = pos[0];
    l->pos[1] = pos[1];
    l->pos[2] = pos[2];
    l->rot = rot;
    l->width_m = width_m;
    l->height_m = height_m;
    l->up = up;
}

void layer_crop(Layer *l, uint32_t width, uint32_t height)
{
    l->crop[0] = width ? std::min(1.f, (float)width / (float)l->desc.width) : 1.f;
    l->crop[1] = height ? std::min(1.f, (float)height / (float)l->desc.height) : 1.f;
}

void layer_punch(gpu::Frame &f, Layer *l)
{
    if (!l || !l->desc.underlay || !l->up || !l->drawn) return;
    sim::draw_layer(f, l, l->desc.opaque ? sim::g_under_opaque : sim::g_under_alpha);
}

}
