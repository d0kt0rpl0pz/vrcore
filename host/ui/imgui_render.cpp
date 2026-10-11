#include "imgui_render.h"

#include <cstddef>
#include <cstring>

#include <imgui.h>

#include "../app/log.h"

namespace vrcore::ui {

namespace {

const uint32_t kVert[] = {
#include "imgui.vert.h"
};
const uint32_t kFrag[] = {
#include "imgui.frag.h"
};

static_assert(sizeof(ImDrawIdx) == 2, "imgui_render binds 16-bit indices");

struct Push {
    float scale[2];
    float offset[2];
    float clip[4];
};

const gpu::Texture *texture_of(ImTextureID id)
{
    return reinterpret_cast<const gpu::Texture *>(static_cast<uintptr_t>(id));
}

}

bool ImguiRender::init(gpu::Renderer &r)
{
    static const gpu::VertexAttr attrs[] = {
        {0, offsetof(ImDrawVert, pos), gpu::Format::Float2},
        {1, offsetof(ImDrawVert, uv), gpu::Format::Float2},
        {2, offsetof(ImDrawVert, col), gpu::Format::UNorm8x4},
    };
    gpu::PipelineDesc d;
    d.vert = {kVert, sizeof kVert};
    d.frag = {kFrag, sizeof kFrag};
    d.stride = sizeof(ImDrawVert);
    d.attrs = attrs;
    d.attr_count = 3;
    d.cull = gpu::Cull::None;
    d.blend = gpu::Blend::Premultiplied;
    d.depth_test = d.depth_write = false;
    d.pass = gpu::Pass::Offscreen;
    d.offscreen_format = gpu::TextureFormat::RGBA8_sRGB;     // the layers' (app/layers.h)
    pipeline_ = r.create_pipeline(d);
    vertices_ = r.create_dynamic_buffer(gpu::BufferUsage::Vertex, 256 * 1024);
    indices_ = r.create_dynamic_buffer(gpu::BufferUsage::Index, 64 * 1024);

    ImGuiIO &io = ImGui::GetIO();
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    gpu::TextureDesc td;
    td.width = (uint32_t)w;
    td.height = (uint32_t)h;
    td.format = gpu::TextureFormat::RGBA8;
    td.filter = gpu::Filter::Linear;
    font_ = px ? r.create_texture(td, px) : nullptr;
    if (font_) io.Fonts->SetTexID((ImTextureID)(uintptr_t)font_);
    if (!pipeline_ || !vertices_ || !indices_ || !font_) {
        log("ui: the ImGui renderer could not be made");
        return false;
    }
    log("ui: ImGui %s, font atlas %dx%d", IMGUI_VERSION, w, h);
    return true;
}

void ImguiRender::shutdown(gpu::Renderer &r)
{
    r.destroy_pipeline(pipeline_);
    r.destroy_dynamic_buffer(vertices_);
    r.destroy_dynamic_buffer(indices_);
    r.destroy_texture(font_);
    pipeline_ = nullptr;
    vertices_ = indices_ = nullptr;
    font_ = nullptr;
    placed_.clear();
}

void ImguiRender::upload(gpu::Frame &f, const ImDrawData *dd)
{
    placed_.clear();
    if (!dd || dd->TotalVtxCount == 0) return;
    auto *v = static_cast<ImDrawVert *>(f.write(vertices_, (size_t)dd->TotalVtxCount * sizeof(ImDrawVert)));
    auto *i = static_cast<ImDrawIdx *>(f.write(indices_, (size_t)dd->TotalIdxCount * sizeof(ImDrawIdx)));
    if (!v || !i) {
        log("ui: no room for %d ImGui vertices", dd->TotalVtxCount);
        return;
    }
    int32_t vn = 0;
    uint32_t in = 0;
    for (const ImDrawList *l : dd->CmdLists) {
        std::memcpy(v + vn, l->VtxBuffer.Data, (size_t)l->VtxBuffer.Size * sizeof(ImDrawVert));
        std::memcpy(i + in, l->IdxBuffer.Data, (size_t)l->IdxBuffer.Size * sizeof(ImDrawIdx));
        placed_.push_back({l, vn, in});
        vn += l->VtxBuffer.Size;
        in += (uint32_t)l->IdxBuffer.Size;
    }
}

void ImguiRender::draw_list(gpu::Frame &f, const ImDrawList *list, float x, float y, uint32_t width, uint32_t height)
{
    const Placed *p = nullptr;
    for (const Placed &q : placed_)
        if (q.list == list) p = &q;
    if (!p) return;
    f.bind(pipeline_);
    f.bind_vertices(vertices_);
    f.bind_indices(indices_, gpu::IndexType::U16);
    Push push = {{2.f / (float)width, 2.f / (float)height}, {-x, -y}, {}};
    for (const ImDrawCmd &c : list->CmdBuffer) {
        if (c.UserCallback || c.ElemCount == 0) continue;
        push.clip[0] = c.ClipRect.x - x;
        push.clip[1] = c.ClipRect.y - y;
        push.clip[2] = c.ClipRect.z - x;
        push.clip[3] = c.ClipRect.w - y;
        if (push.clip[2] <= 0 || push.clip[3] <= 0 || push.clip[0] >= (float)width || push.clip[1] >= (float)height)
            continue;
        const gpu::Texture *t = texture_of(c.GetTexID());
        f.bind_textures(&t, 1);
        f.push(&push, sizeof push);
        f.draw_indexed(c.ElemCount, p->index + c.IdxOffset, p->vertex + (int32_t)c.VtxOffset);
    }
}

}
