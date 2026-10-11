/*
 * imgui_render.h -- Dear ImGui's draw data through vrcore's render API (render.h), into offscreen targets (a
 * layer's picture, app/layers.h). One pipeline: ImGui's vertices, a texture per command (the font atlas, or a
 * front's image: an ImTextureID is a `const gpu::Texture *`), the command's clip rectangle; no Vulkan here, and
 * none of ImGui's own backends.
 *
 * THE FONTS are baked once at init (ImGui's legacy atlas: no texture requests later), each at the pixel size it
 * is drawn at, so nothing is scaled. The atlas is RGBA8 (white, the glyphs' coverage in alpha).
 *
 * A FRAME: after ImGui::Render, upload(f, draw data) puts every list's vertices and indices into one dynamic
 * buffer each (one write a frame); then, inside an offscreen pass into a target, draw_list(f, list, ...) draws
 * one of the lists with ImGui's pixel (x, y) at the target's (0, 0).
 */
#pragma once

#include <vector>

#include "gpu/render.h"

struct ImDrawData;
struct ImDrawList;

namespace vrcore::ui {

class ImguiRender {
public:
    // The pipeline, buffers and the font atlas (ImGui's context exists and its fonts are added).
    bool init(gpu::Renderer &r);
    void shutdown(gpu::Renderer &r);

    void upload(gpu::Frame &f, const ImDrawData *dd);
    // In an offscreen pass into a `width` x `height` target: `list` (one of the last upload's) drawn with
    // ImGui's pixel (x, y) at the target's top left.
    void draw_list(gpu::Frame &f, const ImDrawList *list, float x, float y, uint32_t width, uint32_t height);

private:
    gpu::Pipeline *pipeline_ = nullptr;
    gpu::DynamicBuffer *vertices_ = nullptr, *indices_ = nullptr;
    gpu::Texture *font_ = nullptr;
    struct Placed {
        const ImDrawList *list;
        int32_t vertex;         // its first vertex in the frame's buffer
        uint32_t index;         // its first index
    };
    std::vector<Placed> placed_;
};

}
