/*
 * controllers.h -- the controllers and the pointing ray, drawn while a menu is up (ui/menu.h), on the overlay layer
 * (app/layers.h) so they stand in front of the panel and the boxes, which are quad layers.
 *
 * THE MODELS: assets/controller_left.glb and controller_right.glb, the generic trigger / squeeze / thumbstick
 * controller of immersive-web/webxr-input-profiles (MIT: THIRD_PARTY.md), loaded with cgltf as the wheel is. Their
 * origin is the grip pose (-z forward, y up, metres), so a hand is drawn at its grip as it is. Every node's mesh is
 * baked into one static pose (no button animation); a left-handed (mirrored) node turns its triangles round. Lit
 * with the scene shader's one directional light, in the material's base colour (its textures are not used).
 * THE RAY: a thin white box from the aim along -z, yellow over something clickable.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "app/input.h"
#include "gpu/render.h"

namespace vrcore::ui {

class ControllerModels {
public:
    static constexpr float kRayMiss = 2.f;          // m the ray runs when it hits nothing

    // One hand as the overlay's draw needs it: a copy, since the draw runs after prepare returns.
    struct Draw {
        bool shown = false;
        Pose grip;
        bool ray = false;                           // this hand points
        Pose aim;
        float ray_length = kRayMiss;
        bool clickable = false;
    };

    bool init(gpu::Renderer &r);                    // false (logged): nothing to draw, the menu carries on
    void shutdown(gpu::Renderer &r);
    bool ready() const { return pipe_ != nullptr; }
    // In the overlay's pass: `hands` indexed by Input::kLeft / kRight.
    void draw(gpu::Frame &f, const Draw hands[2]) const;

private:
    struct Part {
        uint32_t first = 0, count = 0;
        float color[4] = {0.5f, 0.5f, 0.5f, 1};
    };
    struct Model {
        gpu::Buffer *vertices = nullptr, *indices = nullptr;
        std::vector<Part> parts;
    };
    bool load(gpu::Renderer &r, const char *file, Model &m);

    Model models_[2];
    gpu::Buffer *cube_ = nullptr;
    uint32_t cube_count_ = 0;
    gpu::Pipeline *pipe_ = nullptr;
};

}
