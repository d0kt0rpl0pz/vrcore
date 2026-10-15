/*
 * render_host.h -- the host's side of render.h: the renderer's Vulkan state and the frame sequence.
 * Two frames in flight; each slot has a command buffer, a fence, the Views and front uniform buffers and
 * their descriptor sets, a descriptor pool for the frame's texture sets, a staging buffer for its uploads, and
 * two timestamps (the frame's GPU time, every pass up to the submit, read back when the slot comes round).
 *
 *   Frame &f = renderer_begin(r, eyes);                 // waits for the slot, opens its command buffer
 *   front.prepare(f);                                   // uploads, dynamic buffers, offscreen passes
 *   renderer_begin_eye_pass(r, image, clear);           // the multiview eye pass on `image`
 *   front.draw(f);
 *   renderer_end_pass(r);
 *   [renderer_begin_eye_pass(r, image, clear, &overlay) ... renderer_end_pass(r)]    // the headset's overlay
 *   ... host commands on renderer_cmd(r) (the sim's blit) ...
 *   renderer_submit(r, wait, wait_stage, signal);
 *
 * Host-internal.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "device.h"
#include "eye_target.h"
#include "render.h"

namespace vrcore::gpu {

constexpr uint32_t kFramesInFlight = 2;

// The eye pass's clear colour (linear), the same on the headset and in the sim (the planes: render.h).
constexpr float kClear[4] = {0.02f, 0.025f, 0.03f, 1.f};
constexpr uint32_t kPushBytes = 128;
constexpr uint32_t kTextureSetsPerFrame = 512;     // bind_textures calls a frame

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation mem = VK_NULL_HANDLE;
    void *mapped = nullptr;
    size_t bytes = 0;
};

struct DynamicBuffer {
    VkBufferUsageFlags usage = 0;
    Buffer ring[kFramesInFlight];
    uint32_t current = 0;                    // the copy draws bind
    uint64_t written = UINT64_MAX;           // the display frame of the last write
};

struct Texture {
    TextureDesc desc;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};

struct Target {
    Texture tex;                             // its image, sampled once a pass has ended
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkRenderPass clear_pass = VK_NULL_HANDLE, load_pass = VK_NULL_HANDLE;
    bool external = false;                   // the image is someone else's (an XR swapchain's): only the view
                                             //   and framebuffer are ours, and it is not sampled
};

struct Pipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
};

// std140, as shaders/vrcore_views.glsl declares it.
struct ViewsUniform {
    Mat4 view_proj[2];
    Mat4 view[2];
    Mat4 proj[2];
    float eye_pos[2][4];
    float time[4];
};

struct RendererImpl {
    Device *dev = nullptr;
    EyeTarget *target = nullptr;

    VkDescriptorSetLayout set_layouts[3] = {};   // 0 Views, 1 the front's uniforms, 2 textures
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE; // sets 0 and 1 of every slot

    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;      // signalled: free
        Buffer views, uniforms;
        VkDescriptorSet sets[2] = {};        // sets 0 and 1
        VkDescriptorPool texture_pool = VK_NULL_HANDLE;   // set 2's, reset as the slot begins
        Buffer staging;                      // the frame's uploads, grown on demand
        size_t staging_used = 0;
        std::vector<Buffer> retired;         // buffers grown out of while this slot's frame used them: freed
                                             //   when the slot comes round
        bool timed = false;                  // its timestamps were written by the last submit
    } slots[kFramesInFlight];
    uint32_t slot = 0;                       // the slot of the frame being recorded / next begun
    uint64_t number = 0;
    uint64_t start_ns = 0;

    struct OffscreenPasses {
        VkFormat format;
        VkImageLayout rest;                  // the image's layout outside the pass
        VkRenderPass clear, load;
    };
    std::vector<OffscreenPasses> offscreen_passes;
    Texture *black = nullptr;                // set 2's unbound bindings

    VkQueryPool queries = VK_NULL_HANDLE;    // 2 per slot; null when the queue has no timestamps
    double gpu_ms = 0;                       // the last read-back frame
    enum class InPass { None, Offscreen, Eye } in_pass = InPass::None;
    EyeTarget *eye_on = nullptr;             // the eye pass's target (the renderer's, or another's)

    Renderer renderer;                       // what the front sees
    Frame frame;
};

bool renderer_create(RendererImpl &r, Device &dev, EyeTarget &target);
void renderer_destroy(RendererImpl &r);          // waits for the device first

// The slot the next renderer_begin will use (the sim picks its eye image by it).
inline uint32_t renderer_next_slot(const RendererImpl &r) { return r.slot; }

Frame &renderer_begin(RendererImpl &r, const Eye eyes[2]);
// `on`: another eye target than the renderer's (the sim's view shots), the same formats and samples (so every
// eye pipeline fits); null: the renderer's.
void renderer_begin_eye_pass(RendererImpl &r, uint32_t image, const float clear_rgba[4], EyeTarget *on = nullptr);
void renderer_end_pass(RendererImpl &r);
// In the eye pass: its depth (and stencil) cleared as the pass began, so what is drawn next stands over all that
// was drawn before (the sim's overlay: app/layers.h).
void renderer_clear_depth(RendererImpl &r);
VkCommandBuffer renderer_cmd(const RendererImpl &r);
bool renderer_submit(RendererImpl &r, VkSemaphore wait = VK_NULL_HANDLE, VkPipelineStageFlags wait_stage = 0,
                     VkSemaphore signal = VK_NULL_HANDLE);

// The render passes for offscreen targets of `format` (made on first use): CLEAR and LOAD, the image in `rest`
// outside the pass (SHADER_READ_ONLY_OPTIMAL for a target; COLOR_ATTACHMENT_OPTIMAL for an XR swapchain's
// image, as the runtime hands it over and takes it back). Null on failure (logged).
const RendererImpl::OffscreenPasses *renderer_offscreen_passes(RendererImpl &r, VkFormat format,
                                                               VkImageLayout rest = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

// A target on someone else's 2D colour image (an XR swapchain's: COLOR_ATTACHMENT_OPTIMAL at rest), drawn by
// Frame::offscreen as any target; not sampled. renderer_destroy_target frees what is ours. Null on failure.
Target *renderer_wrap_target(RendererImpl &r, VkImage image, uint32_t width, uint32_t height, VkFormat format,
                             VkImageLayout rest);

}
