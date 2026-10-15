#include "render_host.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace vrcore::gpu {

namespace {

uint64_t now_ns()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

VkFormat vk_format(Format f)
{
    switch (f) {
    case Format::Float1: return VK_FORMAT_R32_SFLOAT;
    case Format::Float2: return VK_FORMAT_R32G32_SFLOAT;
    case Format::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::UNorm8x4: return VK_FORMAT_R8G8B8A8_UNORM;
    }
    return VK_FORMAT_UNDEFINED;
}

VkFormat vk_format(TextureFormat f)
{
    switch (f) {
    case TextureFormat::R8: return VK_FORMAT_R8_UNORM;
    case TextureFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::RGBA8_sRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    }
    return VK_FORMAT_UNDEFINED;
}

uint32_t texel_bytes(TextureFormat f)
{
    return f == TextureFormat::R8 ? 1 : 4;
}

VkCompareOp vk_compare(Compare c)
{
    switch (c) {
    case Compare::Never: return VK_COMPARE_OP_NEVER;
    case Compare::Less: return VK_COMPARE_OP_LESS;
    case Compare::Equal: return VK_COMPARE_OP_EQUAL;
    case Compare::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case Compare::Greater: return VK_COMPARE_OP_GREATER;
    case Compare::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case Compare::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case Compare::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkStencilOp vk_stencil_op(StencilOp o)
{
    switch (o) {
    case StencilOp::Keep: return VK_STENCIL_OP_KEEP;
    case StencilOp::Zero: return VK_STENCIL_OP_ZERO;
    case StencilOp::Replace: return VK_STENCIL_OP_REPLACE;
    case StencilOp::IncrementWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case StencilOp::DecrementWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}

VkShaderModule shader_module(VkDevice device, const ShaderCode &code)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.bytes;
    ci.pCode = code.words;
    VkShaderModule m = VK_NULL_HANDLE;
    VRCORE_VK(vkCreateShaderModule(device, &ci, nullptr, &m), VK_NULL_HANDLE);
    return m;
}

// Host-visible, coherent and mapped: written by the CPU (meshes, uniforms, dynamic buffers, staging).
bool make_buffer(Device &dev, Buffer &b, VkBufferUsageFlags usage, size_t bytes)
{
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = bytes;
    ci.usage = usage;
    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VmaAllocationInfo info = {};
    VRCORE_VK(vmaCreateBuffer(dev.vma, &ci, &aci, &b.buffer, &b.mem, &info), false);
    b.mapped = info.pMappedData;
    b.bytes = bytes;
    return true;
}

void free_buffer(Device &dev, Buffer &b)
{
    if (b.buffer) vmaDestroyBuffer(dev.vma, b.buffer, b.mem);
    b = Buffer();
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                   VkAccessFlags dst_access)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

constexpr VkPipelineStageFlags kShaderStages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

// Records `t`'s contents from `staging` at `offset`: the image from (`from`) to TRANSFER_DST, the copy, back to
// SHADER_READ_ONLY for the shaders after it. An earlier frame's reads are ordered before the copy (queue order).
void record_copy(VkCommandBuffer cmd, Texture &t, VkBuffer staging, VkDeviceSize offset, VkImageLayout from)
{
    image_barrier(cmd, t.image, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kShaderStages, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = {};
    copy.bufferOffset = offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {t.desc.width, t.desc.height, 1};
    vkCmdCopyBufferToImage(cmd, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, kShaderStages,
                  VK_ACCESS_SHADER_READ_BIT);
}

// An image, its view and sampler; `usage` on top of SAMPLED.
bool make_texture(Device &dev, Texture &t, const TextureDesc &desc, VkImageUsageFlags usage)
{
    t.desc = desc;
    t.format = vk_format(desc.format);
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = t.format;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VRCORE_VK(vmaCreateImage(dev.vma, &ici, &aci, &t.image, &t.mem, nullptr), false);

    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = t.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VRCORE_VK(vkCreateImageView(dev.device, &vci, nullptr, &t.view), false);

    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = desc.filter == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = desc.address != Address::Clamp ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = sci.addressModeW =
        desc.address == Address::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.f;
    VRCORE_VK(vkCreateSampler(dev.device, &sci, nullptr, &t.sampler), false);
    return true;
}

void free_texture(Device &dev, Texture &t)
{
    if (t.sampler) vkDestroySampler(dev.device, t.sampler, nullptr);
    if (t.view) vkDestroyImageView(dev.device, t.view, nullptr);
    if (t.image) vmaDestroyImage(dev.vma, t.image, t.mem);
    t = Texture();
}

// A texture's contents through a one-off command buffer and a staging buffer (null data: zeros), waited for.
bool upload_once(Device &dev, Texture &t, const void *data, VkImageLayout from)
{
    size_t bytes = size_t(t.desc.width) * t.desc.height * texel_bytes(t.desc.format);
    Buffer staging;
    if (!make_buffer(dev, staging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, bytes)) return false;
    if (data) std::memcpy(staging.mapped, data, bytes);
    else std::memset(staging.mapped, 0, bytes);
    VkCommandBuffer cmd = device_begin_once(dev);
    bool ok = cmd != VK_NULL_HANDLE;
    if (ok) {
        record_copy(cmd, t, staging.buffer, 0, from);
        ok = device_end_once(dev, cmd);
    }
    free_buffer(dev, staging);
    return ok;
}

VkDescriptorSetLayout set_layout(VkDevice device, VkDescriptorType type, uint32_t count)
{
    std::vector<VkDescriptorSetLayoutBinding> b(count);
    for (uint32_t i = 0; i < count; ++i) {
        b[i].binding = i;
        b[i].descriptorType = type;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = count;
    ci.pBindings = b.data();
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    VRCORE_VK(vkCreateDescriptorSetLayout(device, &ci, nullptr, &l), VK_NULL_HANDLE);
    return l;
}

void write_uniform_set(VkDevice device, VkDescriptorSet set, VkBuffer buffer, size_t bytes)
{
    VkDescriptorBufferInfo bi = {buffer, 0, bytes};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

}

const RendererImpl::OffscreenPasses *renderer_offscreen_passes(RendererImpl &r, VkFormat format, VkImageLayout rest)
{
    for (const RendererImpl::OffscreenPasses &p : r.offscreen_passes)
        if (p.format == format && p.rest == rest) return &p;
    if (r.offscreen_passes.size() == r.offscreen_passes.capacity()) {
        log("render: more than %zu offscreen pass kinds", r.offscreen_passes.size());   // pointers are handed out
        return nullptr;
    }

    RendererImpl::OffscreenPasses p = {format, rest, VK_NULL_HANDLE, VK_NULL_HANDLE};
    for (int load = 0; load < 2; ++load) {
        VkAttachmentDescription att = {};
        att.format = format;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = rest;
        att.finalLayout = rest;
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;

        // In: after the earlier passes that drew it, sampled it or copied it. Out: before the later ones that
        // sample or copy it (another pass, the eye pass, a readback or a layer's blit).
        VkSubpassDependency dep[2] = {};
        dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dep[0].dstSubpass = 0;
        dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kShaderStages | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass = 0;
        dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstStageMask = kShaderStages | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

        VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1;
        ci.pAttachments = &att;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        ci.dependencyCount = 2;
        ci.pDependencies = dep;
        VRCORE_VK(vkCreateRenderPass(r.dev->device, &ci, nullptr, load ? &p.load : &p.clear), nullptr);
    }
    r.offscreen_passes.push_back(p);
    return &r.offscreen_passes.back();
}

bool renderer_create(RendererImpl &r, Device &dev, EyeTarget &target)
{
    r.dev = &dev;
    r.target = &target;
    r.renderer.impl = &r;
    r.frame.impl = &r;
    r.offscreen_passes.reserve(8);                  // pointers into it are handed out

    VkDevice d = dev.device;
    r.set_layouts[0] = set_layout(d, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1);
    r.set_layouts[1] = set_layout(d, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1);
    r.set_layouts[2] = set_layout(d, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures);
    if (!r.set_layouts[0] || !r.set_layouts[1] || !r.set_layouts[2]) return false;

    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 3;
    plci.pSetLayouts = r.set_layouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    VRCORE_VK(vkCreatePipelineLayout(d, &plci, nullptr, &r.layout), false);

    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kFramesInFlight};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 2 * kFramesInFlight;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VRCORE_VK(vkCreateDescriptorPool(d, &dpci, nullptr, &r.desc_pool), false);

    for (RendererImpl::Slot &s : r.slots) {
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = dev.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VRCORE_VK(vkAllocateCommandBuffers(d, &cai, &s.cmd), false);
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VRCORE_VK(vkCreateFence(d, &fci, nullptr, &s.fence), false);
        if (!make_buffer(dev, s.views, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, sizeof(ViewsUniform)) ||
            !make_buffer(dev, s.uniforms, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kUniformBytes))
            return false;
        std::memset(s.uniforms.mapped, 0, kUniformBytes);

        VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = r.desc_pool;
        dsai.descriptorSetCount = 2;
        dsai.pSetLayouts = r.set_layouts;
        VRCORE_VK(vkAllocateDescriptorSets(d, &dsai, s.sets), false);
        write_uniform_set(d, s.sets[0], s.views.buffer, sizeof(ViewsUniform));
        write_uniform_set(d, s.sets[1], s.uniforms.buffer, kUniformBytes);

        VkDescriptorPoolSize tps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTextureSetsPerFrame * kMaxTextures};
        VkDescriptorPoolCreateInfo tpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        tpci.maxSets = kTextureSetsPerFrame;
        tpci.poolSizeCount = 1;
        tpci.pPoolSizes = &tps;
        VRCORE_VK(vkCreateDescriptorPool(d, &tpci, nullptr, &s.texture_pool), false);
    }

    TextureDesc black = {1, 1, TextureFormat::RGBA8, Filter::Nearest, Address::Clamp};
    r.black = r.renderer.create_texture(black, nullptr);
    if (!r.black) return false;

    // The frame's GPU time, where the queue has timestamps.
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev.phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.phys, &n, families.data());
    if (families[dev.queue_family].timestampValidBits && dev.props.limits.timestampPeriod > 0) {
        VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2 * kFramesInFlight;
        VRCORE_VK(vkCreateQueryPool(d, &qci, nullptr, &r.queries), false);
    }
    r.start_ns = now_ns();
    return true;
}

void renderer_destroy(RendererImpl &r)
{
    if (!r.dev) return;
    VkDevice d = r.dev->device;
    vkDeviceWaitIdle(d);
    if (r.black) r.renderer.destroy_texture(r.black);
    for (RendererImpl::Slot &s : r.slots) {
        if (s.fence) vkDestroyFence(d, s.fence, nullptr);
        if (s.cmd) vkFreeCommandBuffers(d, r.dev->pool, 1, &s.cmd);
        if (s.texture_pool) vkDestroyDescriptorPool(d, s.texture_pool, nullptr);
        free_buffer(*r.dev, s.views);
        free_buffer(*r.dev, s.uniforms);
        free_buffer(*r.dev, s.staging);
        for (Buffer &b : s.retired) free_buffer(*r.dev, b);
    }
    for (RendererImpl::OffscreenPasses &p : r.offscreen_passes) {
        if (p.clear) vkDestroyRenderPass(d, p.clear, nullptr);
        if (p.load) vkDestroyRenderPass(d, p.load, nullptr);
    }
    if (r.queries) vkDestroyQueryPool(d, r.queries, nullptr);
    if (r.desc_pool) vkDestroyDescriptorPool(d, r.desc_pool, nullptr);
    if (r.layout) vkDestroyPipelineLayout(d, r.layout, nullptr);
    for (VkDescriptorSetLayout l : r.set_layouts)
        if (l) vkDestroyDescriptorSetLayout(d, l, nullptr);
    r = RendererImpl();
}

Frame &renderer_begin(RendererImpl &r, const Eye eyes[2])
{
    Device &dev = *r.dev;
    RendererImpl::Slot &s = r.slots[r.slot];
    vkWaitForFences(dev.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
    if (s.timed) {
        uint64_t ts[2];
        if (vkGetQueryPoolResults(dev.device, r.queries, 2 * r.slot, 2, sizeof ts, ts, sizeof ts[0],
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            r.gpu_ms = (double)(ts[1] - ts[0]) * dev.props.limits.timestampPeriod * 1e-6;
        s.timed = false;
    }
    vkResetFences(dev.device, 1, &s.fence);
    for (Buffer &b : s.retired) free_buffer(dev, b);
    s.retired.clear();
    vkResetCommandBuffer(s.cmd, 0);
    vkResetDescriptorPool(dev.device, s.texture_pool, 0);
    s.staging_used = 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
    if (r.queries) {
        vkCmdResetQueryPool(s.cmd, r.queries, 2 * r.slot, 2);
        vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, r.queries, 2 * r.slot);
    }

    Frame &f = r.frame;
    f.time = (double)(now_ns() - r.start_ns) * 1e-9;
    f.number = r.number;
    ViewsUniform u;
    for (int e = 0; e < 2; ++e) {
        f.eyes[e] = eyes[e];
        u.view[e] = eyes[e].view;
        u.proj[e] = eyes[e].proj;
        u.view_proj[e] = mat4_mul(eyes[e].proj, eyes[e].view);
        u.eye_pos[e][0] = eyes[e].pos[0];
        u.eye_pos[e][1] = eyes[e].pos[1];
        u.eye_pos[e][2] = eyes[e].pos[2];
        u.eye_pos[e][3] = 1.f;
    }
    u.time[0] = (float)f.time;
    u.time[1] = u.time[2] = u.time[3] = 0.f;
    std::memcpy(s.views.mapped, &u, sizeof u);

    // Sets 0 and 1 stay bound across the frame's passes (every pipeline has the one layout).
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout, 0, 2, s.sets, 0, nullptr);
    r.in_pass = RendererImpl::InPass::None;
    return f;
}

void renderer_begin_eye_pass(RendererImpl &r, uint32_t image, const float clear_rgba[4], EyeTarget *on)
{
    EyeTarget &t = on ? *on : *r.target;
    RendererImpl::Slot &s = r.slots[r.slot];
    if (r.in_pass != RendererImpl::InPass::None) {
        log("render: the eye pass begun inside another pass; that one ended first");
        renderer_end_pass(r);
    }
    VkClearValue clear[2];
    std::memcpy(clear[0].color.float32, clear_rgba, 4 * sizeof(float));
    clear[1].depthStencil = {0.f, 0};                   // reverse-Z: far; stencil 0
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = t.pass;
    rbi.framebuffer = t.framebuffers[image];
    rbi.renderArea.extent = t.extent;
    rbi.clearValueCount = 2;
    rbi.pClearValues = clear;
    vkCmdBeginRenderPass(s.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, (float)t.extent.width, (float)t.extent.height, 0.f, 1.f};
    VkRect2D sc = {{0, 0}, t.extent};
    vkCmdSetViewport(s.cmd, 0, 1, &vp);
    vkCmdSetScissor(s.cmd, 0, 1, &sc);
    r.in_pass = RendererImpl::InPass::Eye;
    r.eye_on = &t;
}

void renderer_end_pass(RendererImpl &r)
{
    RendererImpl::Slot &s = r.slots[r.slot];
    if (r.in_pass == RendererImpl::InPass::None) return;
    vkCmdEndRenderPass(s.cmd);
    r.in_pass = RendererImpl::InPass::None;
}

void renderer_clear_depth(RendererImpl &r)
{
    if (r.in_pass != RendererImpl::InPass::Eye) return;
    VkClearAttachment a = {};
    a.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (r.eye_on->stencil) a.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    a.clearValue.depthStencil = {0.f, 0};               // reverse-Z: far; stencil 0, as the pass began
    VkClearRect rect = {{{0, 0}, r.eye_on->extent}, 0, 1};  // multiview: layer 0 stands for every view
    VkCommandBuffer cmd = r.slots[r.slot].cmd;
    vkCmdClearAttachments(cmd, 1, &a, 1, &rect);
    // MoltenVK clears with a draw of its own and the frame's sets 0 and 1 were lost after it (the next draws had no
    // Views: nothing showed; Apple M5, 2026-10-15): bound again, with the viewport and scissor
    VkViewport vp = {0, 0, (float)r.eye_on->extent.width, (float)r.eye_on->extent.height, 0.f, 1.f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &rect.rect);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout, 0, 2, r.slots[r.slot].sets, 0, nullptr);
}

VkCommandBuffer renderer_cmd(const RendererImpl &r)
{
    return r.slots[r.slot].cmd;
}

bool renderer_submit(RendererImpl &r, VkSemaphore wait, VkPipelineStageFlags wait_stage, VkSemaphore signal)
{
    RendererImpl::Slot &s = r.slots[r.slot];
    if (r.in_pass != RendererImpl::InPass::None) renderer_end_pass(r);
    // the frame's end, after every pass (the eye pass, and the overlay's when it has one)
    if (r.queries) vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, r.queries, 2 * r.slot + 1);
    VRCORE_VK(vkEndCommandBuffer(s.cmd), false);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &wait_stage;
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    VRCORE_VK(vkQueueSubmit(r.dev->queue, 1, &si, s.fence), false);
    s.timed = r.queries != VK_NULL_HANDLE;
    r.slot = (r.slot + 1) % kFramesInFlight;
    ++r.number;
    return true;
}

// ---- the front's calls: prepare ----

void Frame::upload(Texture *t, const void *data)
{
    RendererImpl &r = *impl;
    if (r.in_pass != RendererImpl::InPass::None) {
        log("render: upload inside a pass; ignored");
        return;
    }
    RendererImpl::Slot &s = r.slots[r.slot];
    size_t bytes = size_t(t->desc.width) * t->desc.height * texel_bytes(t->desc.format);
    size_t offset = (s.staging_used + 15) & ~size_t(15);
    if (offset + bytes > s.staging.bytes) {
        // Grown: the copies this frame already recorded read the old buffer, which is retired until the slot
        // comes round; the new one starts empty.
        size_t want = std::max(bytes, s.staging.bytes * 2);
        Buffer grown;
        if (!make_buffer(*r.dev, grown, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, want)) {
            log("render: staging buffer of %zu bytes failed; upload dropped", want);
            return;
        }
        if (s.staging.buffer) s.retired.push_back(s.staging);
        s.staging = grown;
        offset = 0;
    }
    std::memcpy((char *)s.staging.mapped + offset, data, bytes);
    s.staging_used = offset + bytes;
    record_copy(s.cmd, *t, s.staging.buffer, offset, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void *Frame::write(DynamicBuffer *b, size_t bytes)
{
    RendererImpl &r = *impl;
    if (b->written != r.number) b->current = (b->current + 1) % kFramesInFlight;
    b->written = r.number;
    Buffer &buf = b->ring[b->current];
    if (bytes > buf.bytes) {
        // Retired, not freed: a second write in the same frame may follow a draw that bound it.
        size_t want = std::max(bytes, buf.bytes + buf.bytes / 2);
        if (buf.buffer) r.slots[r.slot].retired.push_back(buf);
        buf = Buffer();
        if (!make_buffer(*r.dev, buf, b->usage, want)) {
            log("render: dynamic buffer of %zu bytes failed", want);
            return nullptr;
        }
    }
    return buf.mapped;
}

void Frame::uniforms(const void *data, uint32_t bytes)
{
    RendererImpl::Slot &s = impl->slots[impl->slot];
    if (bytes > kUniformBytes) {
        log("render: uniforms of %u bytes, more than %u; cut", bytes, kUniformBytes);
        bytes = kUniformBytes;
    }
    std::memcpy(s.uniforms.mapped, data, bytes);
}

void Frame::offscreen(Target *t, const float *clear_rgba)
{
    RendererImpl &r = *impl;
    if (r.in_pass != RendererImpl::InPass::None) {
        log("render: an offscreen pass begun inside another pass; that one ended first");
        renderer_end_pass(r);
    }
    VkClearValue clear = {};
    if (clear_rgba) std::memcpy(clear.color.float32, clear_rgba, 4 * sizeof(float));
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = clear_rgba ? t->clear_pass : t->load_pass;
    rbi.framebuffer = t->framebuffer;
    rbi.renderArea.extent = {t->tex.desc.width, t->tex.desc.height};
    rbi.clearValueCount = clear_rgba ? 1 : 0;
    rbi.pClearValues = &clear;
    VkCommandBuffer cmd = renderer_cmd(r);
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, (float)t->tex.desc.width, (float)t->tex.desc.height, 0.f, 1.f};
    VkRect2D sc = {{0, 0}, rbi.renderArea.extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    r.in_pass = RendererImpl::InPass::Offscreen;
}

void Frame::end_offscreen()
{
    if (impl->in_pass != RendererImpl::InPass::Offscreen) {
        log("render: end_offscreen outside an offscreen pass; ignored");
        return;
    }
    renderer_end_pass(*impl);
}

// ---- the front's calls: in a pass ----

void Frame::bind(const Pipeline *p)
{
    vkCmdBindPipeline(renderer_cmd(*impl), VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
}

void Frame::bind_vertices(const Buffer *b, size_t offset)
{
    VkDeviceSize off = offset;
    vkCmdBindVertexBuffers(renderer_cmd(*impl), 0, 1, &b->buffer, &off);
}

void Frame::bind_vertices(const DynamicBuffer *b, size_t offset)
{
    bind_vertices(&b->ring[b->current], offset);
}

void Frame::bind_indices(const Buffer *b, IndexType type, size_t offset)
{
    vkCmdBindIndexBuffer(renderer_cmd(*impl), b->buffer, offset,
                         type == IndexType::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void Frame::bind_indices(const DynamicBuffer *b, IndexType type, size_t offset)
{
    bind_indices(&b->ring[b->current], type, offset);
}

void Frame::bind_textures(const Texture *const *t, uint32_t count)
{
    RendererImpl &r = *impl;
    RendererImpl::Slot &s = r.slots[r.slot];
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.texture_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &r.set_layouts[2];
    if (vkAllocateDescriptorSets(r.dev->device, &ai, &set) != VK_SUCCESS) {
        log("render: more than %u texture sets in a frame; bind_textures ignored", kTextureSetsPerFrame);
        return;
    }
    VkDescriptorImageInfo ii[kMaxTextures];
    for (uint32_t i = 0; i < kMaxTextures; ++i) {
        const Texture *x = i < count && t[i] ? t[i] : r.black;
        ii[i] = {x->sampler, x->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = kMaxTextures;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = ii;
    vkUpdateDescriptorSets(r.dev->device, 1, &w, 0, nullptr);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout, 2, 1, &set, 0, nullptr);
}

void Frame::push(const void *data, uint32_t bytes)
{
    if (bytes > kPushBytes) bytes = kPushBytes;
    vkCmdPushConstants(renderer_cmd(*impl), impl->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       bytes, data);
}

void Frame::draw(uint32_t vertices, uint32_t first, uint32_t instances)
{
    vkCmdDraw(renderer_cmd(*impl), vertices, instances, first, 0);
}

void Frame::draw_indexed(uint32_t indices, uint32_t first, int32_t vertex_offset, uint32_t instances)
{
    vkCmdDrawIndexed(renderer_cmd(*impl), indices, instances, first, vertex_offset, 0);
}

// ---- the front's resources ----

Buffer *Renderer::create_buffer(BufferUsage usage, size_t bytes, const void *data)
{
    Buffer *b = new Buffer;
    VkBufferUsageFlags u = usage == BufferUsage::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                                        : VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (!make_buffer(*impl->dev, *b, u, bytes)) {
        delete b;
        return nullptr;
    }
    if (data) update_buffer(b, 0, bytes, data);
    return b;
}

void Renderer::update_buffer(Buffer *b, size_t offset, size_t bytes, const void *data)
{
    std::memcpy((char *)b->mapped + offset, data, bytes);
}

void Renderer::destroy_buffer(Buffer *b)
{
    if (!b) return;
    free_buffer(*impl->dev, *b);
    delete b;
}

DynamicBuffer *Renderer::create_dynamic_buffer(BufferUsage usage, size_t initial_bytes)
{
    DynamicBuffer *b = new DynamicBuffer;
    b->usage = usage == BufferUsage::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    for (Buffer &x : b->ring)
        if (!make_buffer(*impl->dev, x, b->usage, std::max<size_t>(initial_bytes, 256))) {
            destroy_dynamic_buffer(b);
            return nullptr;
        }
    return b;
}

void Renderer::destroy_dynamic_buffer(DynamicBuffer *b)
{
    if (!b) return;
    for (Buffer &x : b->ring) free_buffer(*impl->dev, x);
    delete b;
}

Texture *Renderer::create_texture(const TextureDesc &desc, const void *data)
{
    Texture *t = new Texture;
    if (!desc.width || !desc.height || !make_texture(*impl->dev, *t, desc, VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !upload_once(*impl->dev, *t, data, VK_IMAGE_LAYOUT_UNDEFINED)) {
        free_texture(*impl->dev, *t);
        delete t;
        return nullptr;
    }
    return t;
}

void Renderer::update_texture(Texture *t, const void *data)
{
    vkDeviceWaitIdle(impl->dev->device);
    upload_once(*impl->dev, *t, data, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Renderer::destroy_texture(Texture *t)
{
    if (!t) return;
    free_texture(*impl->dev, *t);
    delete t;
}

Target *Renderer::create_target(uint32_t width, uint32_t height, TextureFormat format, Filter filter)
{
    Device &dev = *impl->dev;
    Target *t = new Target;
    TextureDesc desc = {width, height, format, filter, Address::Clamp};
    const RendererImpl::OffscreenPasses *passes =
        format == TextureFormat::R8 ? nullptr : renderer_offscreen_passes(*impl, vk_format(format));
    bool ok = passes && make_texture(dev, t->tex, desc,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (ok) {
        t->clear_pass = passes->clear;
        t->load_pass = passes->load;
        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = t->clear_pass;
        fci.attachmentCount = 1;
        fci.pAttachments = &t->tex.view;
        fci.width = width;
        fci.height = height;
        fci.layers = 1;
        ok = vkCreateFramebuffer(dev.device, &fci, nullptr, &t->framebuffer) == VK_SUCCESS &&
             upload_once(dev, t->tex, nullptr, VK_IMAGE_LAYOUT_UNDEFINED);
    }
    if (!ok) {
        log("render: target %ux%u format %d failed", width, height, (int)format);
        destroy_target(t);
        return nullptr;
    }
    return t;
}

const Texture *Renderer::target_texture(const Target *t) const
{
    return &t->tex;
}

void Renderer::destroy_target(Target *t)
{
    if (!t) return;
    if (t->framebuffer) vkDestroyFramebuffer(impl->dev->device, t->framebuffer, nullptr);
    if (t->external) {
        if (t->tex.view) vkDestroyImageView(impl->dev->device, t->tex.view, nullptr);
    } else {
        free_texture(*impl->dev, t->tex);
    }
    delete t;
}

Target *renderer_wrap_target(RendererImpl &r, VkImage image, uint32_t width, uint32_t height, VkFormat format,
                             VkImageLayout rest)
{
    Device &dev = *r.dev;
    const RendererImpl::OffscreenPasses *passes = renderer_offscreen_passes(r, format, rest);
    if (!passes) return nullptr;
    Target *t = new Target;
    t->external = true;
    t->tex.desc = {width, height, TextureFormat::RGBA8_sRGB, Filter::Nearest, Address::Clamp};
    t->tex.format = format;
    t->tex.image = image;
    t->clear_pass = passes->clear;
    t->load_pass = passes->load;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bool ok = vkCreateImageView(dev.device, &vci, nullptr, &t->tex.view) == VK_SUCCESS;
    if (ok) {
        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = t->clear_pass;
        fci.attachmentCount = 1;
        fci.pAttachments = &t->tex.view;
        fci.width = width;
        fci.height = height;
        fci.layers = 1;
        ok = vkCreateFramebuffer(dev.device, &fci, nullptr, &t->framebuffer) == VK_SUCCESS;
    }
    if (!ok) {
        log("render: a target on an image of %ux%u, format %d, failed", width, height, (int)format);
        r.renderer.destroy_target(t);
        return nullptr;
    }
    return t;
}

Pipeline *Renderer::create_pipeline(const PipelineDesc &d)
{
    VkDevice device = impl->dev->device;
    bool eye = d.pass == Pass::Eye;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (eye) {
        pass = impl->target->pass;
    } else if (d.offscreen_format != TextureFormat::R8) {
        const RendererImpl::OffscreenPasses *p = renderer_offscreen_passes(*impl, vk_format(d.offscreen_format));
        if (p) pass = p->clear;
    }
    if (!pass) {
        log("render: no render pass for the pipeline (offscreen format %d)", (int)d.offscreen_format);
        return nullptr;
    }
    if (eye && d.stencil.enable && !impl->target->stencil) log("render: stencil asked for; the eye pass has none");

    VkShaderModule vs = shader_module(device, d.vert), fs = shader_module(device, d.frag);
    Pipeline *p = nullptr;
    if (vs && fs) {
        std::vector<VkSpecializationMapEntry> entries(d.spec_count);
        std::vector<uint32_t> values(d.spec_count);
        for (uint32_t i = 0; i < d.spec_count; ++i) {
            entries[i] = {d.spec[i].id, uint32_t(i * sizeof(uint32_t)), sizeof(uint32_t)};
            values[i] = d.spec[i].value;
        }
        VkSpecializationInfo spec = {d.spec_count, entries.data(), values.size() * sizeof(uint32_t), values.data()};

        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";
        if (d.spec_count) stages[0].pSpecializationInfo = stages[1].pSpecializationInfo = &spec;

        VkVertexInputBindingDescription vb = {0, d.stride, VK_VERTEX_INPUT_RATE_VERTEX};
        std::vector<VkVertexInputAttributeDescription> attrs;
        for (uint32_t i = 0; i < d.attr_count; ++i)
            attrs.push_back({d.attrs[i].location, 0, vk_format(d.attrs[i].format), d.attrs[i].offset});
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = d.stride ? 1 : 0;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size();
        vi.pVertexAttributeDescriptions = attrs.data();

        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = d.topology == Topology::Lines           ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                      : d.topology == Topology::TriangleStrip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                                              : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;

        // The projection flips y for Vulkan, so the picture is upright and the on-screen winding is the
        // world's: counter-clockwise is front.
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = d.cull == Cull::None ? VK_CULL_MODE_NONE
                      : d.cull == Cull::Front ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.f;

        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = eye ? impl->target->samples : VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = eye && d.depth_test;
        ds.depthWriteEnable = eye && d.depth_write;
        ds.depthCompareOp = vk_compare(d.depth_compare);
        if (eye && d.stencil.enable && impl->target->stencil) {
            ds.stencilTestEnable = VK_TRUE;
            VkStencilOpState so = {};
            so.failOp = VK_STENCIL_OP_KEEP;
            so.passOp = vk_stencil_op(d.stencil.pass);
            so.depthFailOp = vk_stencil_op(d.stencil.depth_fail);
            so.compareOp = vk_compare(d.stencil.compare);
            so.compareMask = 0xff;
            so.writeMask = 0xff;
            so.reference = d.stencil.ref;
            ds.front = ds.back = so;
        }

        VkPipelineColorBlendAttachmentState ba = {};
        ba.colorWriteMask = d.color_write ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
                                          : 0;
        if (d.blend != Blend::Opaque) {
            VkBlendFactor src = d.blend == Blend::Alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
            VkBlendFactor dst = d.blend == Blend::Additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = src;
            ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = dst;
            ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;

        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vps;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = impl->layout;
        ci.renderPass = pass;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkResult vr = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
        if (vr == VK_SUCCESS) p = new Pipeline{pipe};
        else log("vulkan: vkCreateGraphicsPipelines failed: %d", (int)vr);
    }
    if (vs) vkDestroyShaderModule(device, vs, nullptr);
    if (fs) vkDestroyShaderModule(device, fs, nullptr);
    return p;
}

void Renderer::destroy_pipeline(Pipeline *p)
{
    if (!p) return;
    vkDestroyPipeline(impl->dev->device, p->pipeline, nullptr);
    delete p;
}

bool Renderer::fdm() const
{
    return impl->target->fdm;
}

}
