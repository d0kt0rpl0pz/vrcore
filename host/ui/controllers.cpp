/*
 * controllers.cpp -- the controllers and the ray drawn (controllers.h).
 */
#include "controllers.h"

#include <SDL3/SDL.h>

#include <cmath>

#include "cgltf.h"

#include "gpu/vmath.h"
#include "log.h"

namespace vrcore::ui {

namespace {

// the scene shader (shaders/scene.vert): a model matrix and a colour a draw, a zero normal is unlit
const uint32_t kVert[] = {
#include "controller.vert.h"
};
const uint32_t kFrag[] = {
#include "controller.frag.h"
};

// the APK's assets on Android (SDL reads them by a relative path), the source tree's on the Mac
#ifdef __ANDROID__
#define VRCORE_ASSET(name) name
#else
#define VRCORE_ASSET(name) VRCORE_ASSETS_DIR "/" name
#endif

struct Vertex {
    float p[3], n[3];
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
            v.push_back({{(c & 1) ? 0.5f : -0.5f, (c & 2) ? 0.5f : -0.5f, (c & 4) ? 0.5f : -0.5f}, {0, 0, 0}});
        }
    return v;
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
            const cgltf_accessor *pos = nullptr, *nrm = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                if (prim.attributes[a].type == cgltf_attribute_type_position) pos = prim.attributes[a].data;
                if (prim.attributes[a].type == cgltf_attribute_type_normal) nrm = prim.attributes[a].data;
            }
            if (!pos || !prim.indices || prim.type != cgltf_primitive_type_triangles) continue;
            const uint32_t base = (uint32_t)verts.size();
            std::vector<float> pv(pos->count * 3), nv(nrm && nrm->count == pos->count ? nrm->count * 3 : 0);
            cgltf_accessor_unpack_floats(pos, pv.data(), pv.size());
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
                verts.push_back(v);
            }
            Part part;
            part.first = (uint32_t)idx.size();
            if (prim.material && prim.material->has_pbr_metallic_roughness)
                for (int k = 0; k < 3; ++k) part.color[k] = prim.material->pbr_metallic_roughness.base_color_factor[k];
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
    m.vertices = r.create_buffer(gpu::BufferUsage::Vertex, verts.size() * sizeof(Vertex), verts.data());
    m.indices = r.create_buffer(gpu::BufferUsage::Index, idx.size() * sizeof(uint32_t), idx.data());
    log("ui: %s: %zu vertices, %zu triangles, %.3f x %.3f x %.3f m", file, verts.size(), idx.size() / 3,
        hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
    return m.vertices && m.indices;
}

bool ControllerModels::init(gpu::Renderer &r)
{
    const bool left = load(r, VRCORE_ASSET("controller_left.glb"), models_[Input::kLeft]);
    const bool right = load(r, VRCORE_ASSET("controller_right.glb"), models_[Input::kRight]);
    std::vector<Vertex> cube = cube_mesh();
    cube_count_ = (uint32_t)cube.size();
    cube_ = r.create_buffer(gpu::BufferUsage::Vertex, cube.size() * sizeof(Vertex), cube.data());
    static const gpu::VertexAttr attrs[2] = {{0, 0, gpu::Format::Float3}, {1, 12, gpu::Format::Float3}};
    gpu::PipelineDesc d;
    d.vert = {kVert, sizeof kVert};
    d.frag = {kFrag, sizeof kFrag};
    d.stride = sizeof(Vertex);
    d.attrs = attrs;
    d.attr_count = 2;
    pipe_ = r.create_pipeline(d);
    return left && right && cube_ && pipe_;
}

void ControllerModels::shutdown(gpu::Renderer &r)
{
    for (Model &m : models_) {
        if (m.vertices) r.destroy_buffer(m.vertices);
        if (m.indices) r.destroy_buffer(m.indices);
        m = Model();
    }
    if (cube_) r.destroy_buffer(cube_);
    if (pipe_) r.destroy_pipeline(pipe_);
    cube_ = nullptr;
    pipe_ = nullptr;
}

void ControllerModels::draw(gpu::Frame &f, const Draw hands[2]) const
{
    if (!pipe_) return;
    f.bind(pipe_);
    for (int h = 0; h < 2; ++h) {
        const Draw &d = hands[h];
        const Model &m = models_[h];
        if (!d.shown || !m.vertices || !m.indices) continue;
        Push p;
        p.model = mat4_mul(mat4_translate(d.grip.pos[0], d.grip.pos[1], d.grip.pos[2]), mat4_rotate(d.grip.rot));
        f.bind_vertices(m.vertices);
        f.bind_indices(m.indices, gpu::IndexType::U32);
        for (const Part &part : m.parts) {
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
        f.bind_vertices(cube_);
        f.push(&p, sizeof p);
        f.draw(cube_count_);
    }
}

}
