/*
 * controllers.cpp -- the controllers and the ray drawn (controllers.h).
 */
#include "controllers.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <algorithm>
#include <string>

#include "cgltf.h"

#include "gpu/ktx2.h"
#include "gpu/vmath.h"
#include "log.h"

namespace vrcore::ui {

namespace {

// the scene shader (shaders/scene.vert): a model matrix and a colour a draw, a zero normal is unlit; and its
// TEXTURED build, which also takes the UV and multiplies the colour by the base colour texture (set 2, binding 0)
const uint32_t kVert[] = {
#include "controller.vert.h"
};
const uint32_t kFrag[] = {
#include "controller.frag.h"
};
const uint32_t kVertTex[] = {
#include "controller_tex.vert.h"
};
const uint32_t kFragTex[] = {
#include "controller_tex.frag.h"
};

struct Vertex {
    float p[3], n[3], uv[2];
};

struct Push {
    Mat4 model;
    float color[4];
};

const float kRayWidth = 0.003f;
const float kWhite[4] = {1, 1, 1, 1}, kYellow[4] = {1, 0.85f, 0.1f, 1};

// A unit cube about the origin, zero normals (unlit): the ray's beam.
std::vector<Vertex> cube_mesh()
{
    static const int kFace[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    std::vector<Vertex> v;
    for (const auto &f : kFace)
        for (int k : {0, 1, 2, 0, 2, 3}) {
            int c = f[k];
            v.push_back({{(c & 1) ? 0.5f : -0.5f, (c & 2) ? 0.5f : -0.5f, (c & 4) ? 0.5f : -0.5f}, {0, 0, 0}, {0, 0}});
        }
    return v;
}


// The KTX2 file a glTF image's uri names, beside the GLB `file`: a texture sampled linear, mipped and repeating.
// Null (logged) where the file is missing or not one this reader takes, and where the device has no ASTC.
gpu::Texture *load_texture(gpu::Renderer &r, const std::string &file, const char *uri)
{
    std::string name = uri;
    name.resize(cgltf_decode_uri(&name[0]));
    const size_t slash = file.find_last_of('/');
    const std::string path = (slash == std::string::npos ? std::string() : file.substr(0, slash + 1)) + name;
    size_t size = 0;
    void *bytes = SDL_LoadFile(path.c_str(), &size);
    if (!bytes) {
        log("ui: no %s (%s): its parts are drawn in their colour", path.c_str(), SDL_GetError());
        return nullptr;
    }
    gpu::Texture *t = gpu::ktx2_create_texture(r, bytes, size, gpu::Filter::Linear, gpu::Address::Repeat,
                                               path.c_str());
    SDL_free(bytes);
    return t;
}


// THE TABLE: an interaction profile (without "/interaction_profiles/") to the GLBs of its models, under assets/. A
// null `right` is one file for both hands (Vive's). The last row is the fallback.
struct Family {
    const char *profile, *left, *right;
};
const Family kFamilies[] = {
    {"oculus/touch_controller", "controllers/oculus-touch-v3/left.glb", "controllers/oculus-touch-v3/right.glb"},
    {"meta/touch_controller_plus", "controllers/meta-quest-touch-plus/left.glb",
     "controllers/meta-quest-touch-plus/right.glb"},
    {"valve/index_controller", "controllers/valve-index/left.glb", "controllers/valve-index/right.glb"},
    {"htc/vive_controller", "controllers/htc-vive/none.glb", nullptr},
    {"bytedance/pico4_controller", "controllers/pico-4/left.glb", "controllers/pico-4/right.glb"},
    {"bytedance/pico_neo3_controller", "controllers/pico-neo3/left.glb", "controllers/pico-neo3/right.glb"},
    {"", "controller_left.glb", "controller_right.glb"},
};

const Family &family(const char *profile)
{
    for (const Family &f : kFamilies)
        if (f.profile[0] && SDL_strcmp(f.profile, profile) == 0) return f;
    return kFamilies[sizeof kFamilies / sizeof kFamilies[0] - 1];
}

// the full path of a table entry (the APK's relative one on Android)
std::string asset_path(const char *name)
{
#ifdef __ANDROID__
    return name;
#else
    return std::string(VRCORE_ASSETS_DIR "/") + name;
#endif
}

}

int controller_family_count() { return (int)(sizeof kFamilies / sizeof kFamilies[0]); }

const char *controller_family_profile(int i)
{
    const int n = controller_family_count();
    return kFamilies[i < 0 ? 0 : i >= n ? n - 1 : i].profile;
}

void ControllerModels::destroy_model(gpu::Renderer &r, Model &m)
{
    if (m.vertices) r.destroy_buffer(m.vertices);
    if (m.indices) r.destroy_buffer(m.indices);
    for (gpu::Texture *t : m.textures)
        if (t) r.destroy_texture(t);
    m = Model();
}

void ControllerModels::set_profile(gpu::Renderer &r, int hand, const char *profile)
{
    if (hand < 0 || hand > 1 || !pipe_) return;
    if (loaded_[hand] && profile_[hand] == profile) return;
    // The old model may still be read by frames in flight: it waits kRetireTicks ticks before it is freed.
    if (models_[hand].vertices) retired_.push_back({std::move(models_[hand]), kRetireTicks});
    models_[hand] = Model();
    const Family &f = family(profile);
    const char *file = hand == Input::kRight && f.right ? f.right : f.left;
    profile_[hand] = profile;
    loaded_[hand] = true;
    const bool ok = load(r, asset_path(file).c_str(), models_[hand]);
    log("ui: %s hand profile \"%s\": %s (%s)", hand ? "right" : "left", profile, file, ok ? "loaded" : "not drawn");
}

void ControllerModels::tick(gpu::Renderer &r)
{
    for (size_t i = 0; i < retired_.size();) {
        if (--retired_[i].ticks > 0) {
            ++i;
            continue;
        }
        destroy_model(r, retired_[i].model);
        retired_.erase(retired_.begin() + (long)i);
    }
}

bool ControllerModels::load(gpu::Renderer &r, const char *file, Model &m)
{
    size_t size = 0;
    void *bytes = SDL_LoadFile(file, &size);
    if (!bytes) {
        log("ui: no %s (%s): that controller is not drawn", file, SDL_GetError());
        return false;
    }
    cgltf_options opt = {};
    cgltf_data *data = nullptr;
    bool ok = cgltf_parse(&opt, bytes, size, &data) == cgltf_result_success &&
              cgltf_load_buffers(&opt, data, nullptr) == cgltf_result_success;
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    std::vector<std::string> uris;              // the images read, parallel to m.textures (a null: not loaded)
    for (cgltf_size n = 0; ok && n < data->nodes_count; ++n) {
        const cgltf_node &node = data->nodes[n];
        if (!node.mesh) continue;
        float w[16];
        cgltf_node_transform_world(&node, w);
        // a mirrored node (a negative determinant) turns its triangles round
        const float det = w[0] * (w[5] * w[10] - w[6] * w[9]) - w[1] * (w[4] * w[10] - w[6] * w[8]) +
                          w[2] * (w[4] * w[9] - w[5] * w[8]);
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive &prim = node.mesh->primitives[p];
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uvs = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                if (prim.attributes[a].type == cgltf_attribute_type_position) pos = prim.attributes[a].data;
                if (prim.attributes[a].type == cgltf_attribute_type_normal) nrm = prim.attributes[a].data;
                if (prim.attributes[a].type == cgltf_attribute_type_texcoord && prim.attributes[a].index == 0)
                    uvs = prim.attributes[a].data;
            }
            if (!pos || !prim.indices || prim.type != cgltf_primitive_type_triangles) continue;
            const uint32_t base = (uint32_t)verts.size();
            std::vector<float> pv(pos->count * 3), nv(nrm && nrm->count == pos->count ? nrm->count * 3 : 0),
                tv(uvs && uvs->count == pos->count ? uvs->count * 2 : 0);
            cgltf_accessor_unpack_floats(pos, pv.data(), pv.size());
            if (!tv.empty()) cgltf_accessor_unpack_floats(uvs, tv.data(), tv.size());
            if (!nv.empty()) cgltf_accessor_unpack_floats(nrm, nv.data(), nv.size());
            for (cgltf_size i = 0; i < pos->count; ++i) {
                Vertex v = {};
                const float *q = &pv[i * 3];
                for (int k = 0; k < 3; ++k) v.p[k] = w[k] * q[0] + w[4 + k] * q[1] + w[8 + k] * q[2] + w[12 + k];
                if (!nv.empty()) {
                    const float *s = &nv[i * 3];
                    float len = 0;
                    for (int k = 0; k < 3; ++k) {
                        v.n[k] = w[k] * s[0] + w[4 + k] * s[1] + w[8 + k] * s[2];
                        len += v.n[k] * v.n[k];
                    }
                    len = std::sqrt(len);
                    for (int k = 0; k < 3 && len > 1e-6f; ++k) v.n[k] /= len;
                }
                if (!tv.empty()) {
                    v.uv[0] = tv[i * 2];
                    v.uv[1] = tv[i * 2 + 1];
                }
                verts.push_back(v);
            }
            Part part;
            part.first = (uint32_t)idx.size();
            if (prim.material && prim.material->has_pbr_metallic_roughness)
                for (int k = 0; k < 3; ++k) part.color[k] = prim.material->pbr_metallic_roughness.base_color_factor[k];
            // the base colour texture, once a file: a part of a material with one but no UVs has none to sample
            const cgltf_image *img = nullptr;
            if (!tv.empty() && prim.material && prim.material->has_pbr_metallic_roughness)
                img = prim.material->pbr_metallic_roughness.base_color_texture.texture
                          ? prim.material->pbr_metallic_roughness.base_color_texture.texture->image
                          : nullptr;
            if (img && img->uri && !img->buffer_view) {
                const std::string uri = img->uri;
                auto it = std::find(uris.begin(), uris.end(), uri);
                if (it == uris.end()) {
                    uris.push_back(uri);
                    m.textures.push_back(load_texture(r, file, img->uri));
                    it = uris.end() - 1;
                }
                gpu::Texture *t = m.textures[it - uris.begin()];
                if (t) part.texture = (int)(it - uris.begin());
            }
            for (cgltf_size i = 0; i + 2 < prim.indices->count; i += 3) {
                const uint32_t a = base + (uint32_t)cgltf_accessor_read_index(prim.indices, i);
                const uint32_t b = base + (uint32_t)cgltf_accessor_read_index(prim.indices, i + 1);
                const uint32_t c = base + (uint32_t)cgltf_accessor_read_index(prim.indices, i + 2);
                idx.push_back(a);
                idx.push_back(det < 0 ? c : b);
                idx.push_back(det < 0 ? b : c);
            }
            part.count = (uint32_t)idx.size() - part.first;
            m.parts.push_back(part);
        }
    }
    if (data) cgltf_free(data);
    SDL_free(bytes);
    if (!ok || verts.empty() || idx.empty()) {
        log("ui: %s holds no triangles: that controller is not drawn", file);
        m.parts.clear();
        return false;
    }
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const Vertex &v : verts)
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::fmin(lo[k], v.p[k]);
            hi[k] = std::fmax(hi[k], v.p[k]);
        }
    size_t textured = 0;
    for (const Part &part : m.parts) textured += part.texture >= 0;
    m.vertices = r.create_buffer(gpu::BufferUsage::Vertex, verts.size() * sizeof(Vertex), verts.data());
    m.indices = r.create_buffer(gpu::BufferUsage::Index, idx.size() * sizeof(uint32_t), idx.data());
    log("ui: %s: %zu vertices, %zu triangles, %.3f x %.3f x %.3f m, %zu of %zu parts textured", file, verts.size(),
        idx.size() / 3, hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], textured, m.parts.size());
    return m.vertices && m.indices;
}

bool ControllerModels::init(gpu::Renderer &r)
{
    std::vector<Vertex> cube = cube_mesh();
    cube_count_ = (uint32_t)cube.size();
    cube_ = r.create_buffer(gpu::BufferUsage::Vertex, cube.size() * sizeof(Vertex), cube.data());
    static const gpu::VertexAttr attrs[3] = {
        {0, 0, gpu::Format::Float3}, {1, 12, gpu::Format::Float3}, {2, 24, gpu::Format::Float2}};
    gpu::PipelineDesc d;
    d.vert = {kVert, sizeof kVert};
    d.frag = {kFrag, sizeof kFrag};
    d.stride = sizeof(Vertex);
    d.attrs = attrs;
    d.attr_count = 2;
    pipe_ = r.create_pipeline(d);
    d.vert = {kVertTex, sizeof kVertTex};
    d.frag = {kFragTex, sizeof kFragTex};
    d.attr_count = 3;
    pipe_tex_ = r.create_pipeline(d);
    // the models are loaded by set_profile as the runtime reports a profile; until then (and in the sim) the generic
    // pair, so a hand is never drawn bare
    set_profile(r, Input::kLeft, "");
    set_profile(r, Input::kRight, "");
    return models_[Input::kLeft].vertices && models_[Input::kRight].vertices && cube_ && pipe_;
}

void ControllerModels::shutdown(gpu::Renderer &r)
{
    for (Model &m : models_) destroy_model(r, m);
    for (Retired &x : retired_) destroy_model(r, x.model);
    retired_.clear();
    for (int h = 0; h < 2; ++h) {
        profile_[h].clear();
        loaded_[h] = false;
    }
    if (cube_) r.destroy_buffer(cube_);
    if (pipe_) r.destroy_pipeline(pipe_);
    if (pipe_tex_) r.destroy_pipeline(pipe_tex_);
    cube_ = nullptr;
    pipe_ = nullptr;
    pipe_tex_ = nullptr;
}

void ControllerModels::draw(gpu::Frame &f, const Draw hands[2]) const
{
    if (!pipe_) return;
    for (int h = 0; h < 2; ++h) {
        const Draw &d = hands[h];
        const Model &m = models_[h];
        if (!d.shown || !m.vertices || !m.indices) continue;
        Push p;
        p.model = mat4_mul(mat4_translate(d.grip.pos[0], d.grip.pos[1], d.grip.pos[2]), mat4_rotate(d.grip.rot));
        f.bind_vertices(m.vertices);
        f.bind_indices(m.indices, gpu::IndexType::U32);
        // the pipeline and the texture are bound as the parts change them; none is bound at the start of a hand
        int bound = -2;
        for (const Part &part : m.parts) {
            const bool tex = part.texture >= 0 && pipe_tex_;
            const int want = tex ? part.texture : -1;
            if (want != bound) {
                if (tex) {
                    if (bound < 0) f.bind(pipe_tex_);
                    const gpu::Texture *t = m.textures[part.texture];
                    f.bind_textures(&t, 1);
                } else if (bound != -1) {
                    f.bind(pipe_);
                }
                bound = want;
            }
            for (int k = 0; k < 4; ++k) p.color[k] = part.color[k];
            f.push(&p, sizeof p);
            f.draw_indexed(part.count, part.first);
        }
    }
    if (!cube_) return;
    for (int h = 0; h < 2; ++h) {
        const Draw &d = hands[h];
        if (!d.shown || !d.ray) continue;
        // the beam: the unit cube stretched along -z from the aim, its middle half way out
        Mat4 mm = mat4_mul(mat4_translate(d.aim.pos[0], d.aim.pos[1], d.aim.pos[2]), mat4_rotate(d.aim.rot));
        mm = mat4_mul(mm, mat4_translate(0, 0, -d.ray_length / 2));
        Push p;
        p.model = mat4_mul(mm, mat4_scale(kRayWidth, kRayWidth, d.ray_length));
        const float *c = d.clickable ? kYellow : kWhite;
        for (int k = 0; k < 4; ++k) p.color[k] = c[k];
        f.bind(pipe_);
        f.bind_vertices(cube_);
        f.push(&p, sizeof p);
        f.draw(cube_count_);
    }
}

}
