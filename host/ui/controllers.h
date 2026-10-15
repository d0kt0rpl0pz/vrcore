/*
 * controllers.h -- the controllers and the pointing ray, drawn while a menu is up (ui/menu.h), on the overlay layer
 * (app/layers.h) so they stand in front of the panel and the boxes, which are quad layers.
 *
 * THE MODELS: one set a hand, picked by the interaction profile the runtime reports for it (set_profile; the table
 * is in controllers.cpp): Touch, Touch Plus, Index, Vive, Pico 4 and Neo 3 models under assets/controllers/, and the
 * generic trigger / squeeze / thumbstick pair (assets/controller_{left,right}.glb) for any other profile and before
 * one is known, all of immersive-web/webxr-input-profiles (MIT: THIRD_PARTY.md), loaded with cgltf as the wheel
 * is. The menu asks for a hand's profile only while it is up, so a profile change in the game loads nothing on the
 * render thread. Up to kCache models stay loaded, keyed by file, so a profile that comes back (a controller that slept,
 * hands and back) is a lookup; the one least recently used, and not on the other hand, makes room, and is freed after
 * a few ticks (frames in flight may still read it). A model that fails to load frees what it made. Their origin is
 * the grip pose (-z forward, y up, metres), so a hand is drawn at its grip as it is. Every node's mesh is baked into
 * one static pose (no button animation); a left-handed (mirrored) node turns its triangles round. Lit
 * with the scene shader's one directional light, in the material's base colour factor, times the base colour
 * texture where the material has one. A texture is a KTX2 file (ASTC, gpu/ktx2.h) that the glTF image's `uri`
 * names, beside the GLB (assets/controllers/); it is sampled repeating, since Index's UVs run outside 0..1. No
 * ASTC on the device, or a file that does not load: the part is drawn in its factor alone.
 * THE RAY: a thin white box from the aim along -z, yellow over something clickable.
 */
#pragma once

#include <cstdint>
#include <string>
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
    // The profile now on a hand ("oculus/touch_controller"; "" none): its model, from the cache or loaded (THE
    // MODELS). The same profile again does nothing. Logged.
    void set_profile(gpu::Renderer &r, int hand, const char *profile);
    void tick(gpu::Renderer &r);                    // once a frame: retired models past their wait are freed
    void shutdown(gpu::Renderer &r);
    bool ready() const { return pipe_ != nullptr; }
    // In the overlay's pass: `hands` indexed by Input::kLeft / kRight.
    void draw(gpu::Frame &f, const Draw hands[2]) const;

private:
    struct Part {
        uint32_t first = 0, count = 0;
        float color[4] = {0.5f, 0.5f, 0.5f, 1};
        int texture = -1;                           // into Model::textures; -1 the factor alone
    };
    struct Model {
        gpu::Buffer *vertices = nullptr, *indices = nullptr;
        std::vector<Part> parts;
        std::vector<gpu::Texture *> textures;       // one a distinct image uri
    };
    struct Retired {
        Model model;
        int ticks;
    };
    struct Cached {
        std::string file;                           // "": a free slot
        Model model;                                // empty when the file did not load (it is not tried again)
        uint64_t used = 0;                          // set_profile's count when a hand last took it
    };
    static constexpr int kRetireTicks = 8;
    static constexpr int kCache = 6;                // both hands' models of three families
    bool load(gpu::Renderer &r, const char *file, Model &m);
    static void destroy_model(gpu::Renderer &r, Model &m);
    const Model *model(int hand) const { return slot_[hand] >= 0 ? &cache_[slot_[hand]].model : nullptr; }

    Cached cache_[kCache];
    int slot_[2] = {-1, -1};                        // each hand's model in cache_ (-1 none yet)
    uint64_t uses_ = 0;
    std::string profile_[2];
    bool loaded_[2] = {false, false};
    std::vector<Retired> retired_;
    gpu::Buffer *cube_ = nullptr;
    uint32_t cube_count_ = 0;
    gpu::Pipeline *pipe_ = nullptr, *pipe_tex_ = nullptr;
};

// The families the table knows, for the sim's key that cycles them (host/sim): `i` in [0, count) is a profile string
// ("oculus/touch_controller"), the last one "" (the generic pair).
int controller_family_count();
const char *controller_family_profile(int i);

// What a profile binds of the buttons the menu's hint names, as xr/xr_actions.cpp suggests them: B / Y (`secondary`)
// and the left hand's menu button. A profile the table does not know, and "" (none yet, the sim), says both.
void controller_buttons(const char *profile, bool &secondary, bool &menu);

}
