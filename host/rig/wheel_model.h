/*
 * wheel_model.h -- the hand wheel drawn: assets/wheel.glb (SM_Wheel.fbx
 * converted once, the "MOMO STEERING WHEEL" model, CC BY 4.0: THIRD_PARTY.md), loaded with cgltf.
 *
 * THE MEASURE (LoadModel): the grip circle is the outer radius less half the rim's thickness (the mesh's
 * half-depth), so the hands sit on the rim; drawn `kDiameter` across that circle.
 * THE POSE: centred between the hands, facing you (its +Z, the logo side, towards you), leaning back by the
 * wheel's pitch, turned with the hands but never past ShowLock; hands closer than MinSpan keep the last pose.
 * Shown while the hands steer, both are tracked and the game says Driving. Colour 0.175 grey (sRGB,
 * taken to linear), unlit, back faces culled.
 *
 * The placement is worked in the wheel's space (left-handed, z forward; x mirrored), turned into
 * the room's (right-handed, -z forward) at the end.
 */
#pragma once

#include "../gpu/render.h"
#include "wheel.h"

namespace vrcore::rig {

class WheelModel {
public:
    static constexpr float kDiameter = 0.30f;       // m across the grip circle

    bool init(gpu::Renderer &r);                     // false (logged): no model, the wheel is not drawn
    void shutdown(gpu::Renderer &r);
    // Once an update, after the controls read the wheel: shown or not, and where.
    void update(Wheel &w, bool driving);
    // In the eye pass.
    void draw(gpu::Frame &f) const;

private:
    gpu::Buffer *vertices_ = nullptr, *indices_ = nullptr;
    gpu::Pipeline *pipe_ = nullptr;
    uint32_t index_count_ = 0;
    float unit_scale_ = 1.f;                        // 1 / the grip circle's diameter, model units
    Vec3 centre_;                                   // left-handed, z 0
    bool shown_ = false, posed_ = false;
    Mat4 model_;
};

}
