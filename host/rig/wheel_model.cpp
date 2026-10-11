/*
 * wheel_model.cpp -- the hand wheel drawn (wheel_model.h).
 */
#include "wheel_model.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "cgltf.h"

#include "../app/log.h"

namespace vrcore::rig {

namespace {

const uint32_t kVert[] = {
#include "wheel.vert.h"
};
const uint32_t kFrag[] = {
#include "wheel.frag.h"
};

// assets/wheel.glb: the APK's assets on Android (SDL reads them by a relative path), the source tree's on the Mac
#ifdef __ANDROID__
const char *const kPath = "wheel.glb";
#else
const char *const kPath = VRCORE_ASSETS_DIR "/wheel.glb";
#endif

float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// A look rotation as a matrix's columns: z the forward, x = up x z, y = z x x
void look_rotation(Vec3 forward, Vec3 up, Vec3 &x, Vec3 &y, Vec3 &z)
{
    z = forward.normalized();
    x = Vec3::cross(up, z).normalized();
    y = Vec3::cross(z, x);
}

struct Push {
    Mat4 model;
    float color[4];
};

}

bool WheelModel::init(gpu::Renderer &r)
{
    size_t size = 0;
    void *bytes = SDL_LoadFile(kPath, &size);
    if (!bytes) {
        log("rig: no %s (%s): the hand wheel is not drawn", kPath, SDL_GetError());
        return false;
    }
    cgltf_options opt = {};
    cgltf_data *data = nullptr;
    bool ok = cgltf_parse(&opt, bytes, size, &data) == cgltf_result_success &&
              cgltf_load_buffers(&opt, data, nullptr) == cgltf_result_success;
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    // every node's mesh in the model's space (its node transforms applied)
    for (cgltf_size n = 0; ok && n < data->nodes_count; ++n) {
        const cgltf_node &node = data->nodes[n];
        if (!node.mesh) continue;
        float m[16];
        cgltf_node_transform_world(&node, m);
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive &prim = node.mesh->primitives[p];
            const cgltf_accessor *at = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a)
                if (prim.attributes[a].type == cgltf_attribute_type_position) at = prim.attributes[a].data;
            if (!at || !prim.indices || prim.type != cgltf_primitive_type_triangles) continue;
            uint32_t base = (uint32_t)(pos.size() / 3);
            std::vector<float> v(at->count * 3);
            cgltf_accessor_unpack_floats(at, v.data(), v.size());
            for (cgltf_size i = 0; i < at->count; ++i) {
                const float *q = &v[i * 3];
                for (int k = 0; k < 3; ++k) pos.push_back(m[k] * q[0] + m[4 + k] * q[1] + m[8 + k] * q[2] + m[12 + k]);
            }
            for (cgltf_size i = 0; i < prim.indices->count; ++i)
                idx.push_back(base + (uint32_t)cgltf_accessor_read_index(prim.indices, i));
        }
    }
    if (data) cgltf_free(data);
    SDL_free(bytes);
    if (!ok || pos.empty() || idx.empty()) {
        log("rig: %s holds no triangles: the hand wheel is not drawn", kPath);
        return false;
    }

    // THE MEASURE, in the wheel's space (left-handed: x mirrored)
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < pos.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            float c = k == 0 ? -pos[i] : pos[i + k];
            lo[k] = std::min(lo[k], c);
            hi[k] = std::max(hi[k], c);
        }
    float ext[3], ctr[3];
    for (int k = 0; k < 3; ++k) {
        ext[k] = 0.5f * (hi[k] - lo[k]);
        ctr[k] = 0.5f * (hi[k] + lo[k]);
    }
    float grip = std::max(ext[0], ext[1]) - ext[2];     // one mesh: it is the rim
    unit_scale_ = grip > 1e-4f ? 1.f / (2.f * grip) : 1.f;
    centre_ = {ctr[0], ctr[1], 0.f};

    vertices_ = r.create_buffer(gpu::BufferUsage::Vertex, pos.size() * sizeof(float), pos.data());
    indices_ = r.create_buffer(gpu::BufferUsage::Index, idx.size() * sizeof(uint32_t), idx.data());
    index_count_ = (uint32_t)idx.size();
    static const gpu::VertexAttr attrs[1] = {{0, 0, gpu::Format::Float3}};
    gpu::PipelineDesc d;
    d.vert = {kVert, sizeof kVert};
    d.frag = {kFrag, sizeof kFrag};
    d.stride = 3 * sizeof(float);
    d.attrs = attrs;
    d.attr_count = 1;
    d.cull = gpu::Cull::Back;
    d.depth_compare = gpu::Compare::Greater;            // ZTest LessEqual's nearest on reversed Z, writing
    pipe_ = r.create_pipeline(d);
    log("rig: hand wheel: %zu vertices, %u triangles, model %.3f x %.3f x %.3f, grip radius %.3f", pos.size() / 3,
        index_count_ / 3, 2 * ext[0], 2 * ext[1], 2 * ext[2], grip);
    return vertices_ && indices_ && pipe_;
}

void WheelModel::shutdown(gpu::Renderer &r)
{
    if (pipe_) r.destroy_pipeline(pipe_);
    if (vertices_) r.destroy_buffer(vertices_);
    if (indices_) r.destroy_buffer(indices_);
    pipe_ = nullptr;
    vertices_ = indices_ = nullptr;
}

void WheelModel::update(Wheel &w, bool driving)
{
    bool read = w.take_read();
    shown_ = pipe_ && w.hands && w.show && w.tracked() && read && driving;
    if (!shown_) return;
    Vec3 right, up, axis;
    Wheel::plane(w.forward(), (float)w.pitchDeg, right, up, axis);
    if (w.span().magnitude() >= Wheel::MinSpan) {      // hands together: the last pose stays
        // the hands' line is right cos(theta) + up sin(theta) in the wheel plane: the same line, theta held to
        // ShowLock
        float t = std::clamp(w.state().theta, -Wheel::ShowLock, Wheel::ShowLock) * 3.14159265f / 180.f;
        Vec3 line = right * std::cos(t) + up * std::sin(t);
        Vec3 x, y, z;
        look_rotation(-axis, Vec3::cross(axis, line), x, y, z);     // +Z (the logo) towards you
        float s = kDiameter * unit_scale_;
        Vec3 c = x * (centre_.x * s) + y * (centre_.y * s) + z * (centre_.z * s);
        Vec3 p = w.mid() - c;
        // the room's: F (z turned) * [R s X] (X the import's mirror) and F p
        Vec3 col[3] = {x * -s, y * s, z * s};
        model_ = mat4_identity();
        for (int k = 0; k < 3; ++k) {
            model_.m[k * 4 + 0] = col[k].x;
            model_.m[k * 4 + 1] = col[k].y;
            model_.m[k * 4 + 2] = -col[k].z;
        }
        model_.m[12] = p.x;
        model_.m[13] = p.y;
        model_.m[14] = -p.z;
        posed_ = true;
    }
}

void WheelModel::draw(gpu::Frame &f) const
{
    if (!shown_ || !posed_) return;
    Push push;
    push.model = model_;
    float g = srgb_to_linear(0.175f);
    push.color[0] = push.color[1] = push.color[2] = g;
    push.color[3] = 1.f;
    f.bind(pipe_);
    f.bind_vertices(vertices_);
    f.bind_indices(indices_, gpu::IndexType::U32);
    f.push(&push, sizeof push);
    f.draw_indexed(index_count_);
}

}
