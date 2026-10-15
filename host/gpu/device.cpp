#include "device.h"

#include <cstring>

namespace vrcore::gpu {

namespace {

bool has_ext(const std::vector<VkExtensionProperties> &list, const char *name)
{
    for (const VkExtensionProperties &e : list)
        if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}

void add_ext(std::vector<const char *> &list, const char *name)
{
    for (const char *e : list)
        if (!std::strcmp(e, name)) return;
    list.push_back(name);
}

}

bool device_create(Device &dev, const DeviceHooks &hooks, const char *app_name)
{
    if (hooks.get_instance_proc_addr) {
        volkInitializeCustom(hooks.get_instance_proc_addr);
    } else if (volkInitialize() != VK_SUCCESS) {
        log("vulkan: no Vulkan loader");
        return false;
    }

    // Instance. MoltenVK offers portability enumeration; without asking for it a loader would hide it.
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> inst_exts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, inst_exts.data());

    std::vector<const char *> iexts = hooks.instance_extensions;
    for (const char *e : iexts)
        if (!has_ext(inst_exts, e)) {
            log("vulkan: instance extension %s missing", e);
            return false;
        }
    VkInstanceCreateFlags iflags = 0;
    if (has_ext(inst_exts, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        add_ext(iexts, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        iflags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = app_name;
    ai.pEngineName = "vrcore";
    ai.apiVersion = kApiVersion;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.flags = iflags;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = (uint32_t)iexts.size();
    ici.ppEnabledExtensionNames = iexts.data();
    if (hooks.create_instance)
        VRCORE_VK(hooks.create_instance(hooks.user, &ici, &dev.instance), false);
    else
        VRCORE_VK(vkCreateInstance(&ici, nullptr, &dev.instance), false);
    volkLoadInstanceOnly(dev.instance);

    // Physical device: the caller's (OpenXR names the one the headset drives), else the first.
    if (hooks.pick_physical_device) {
        VRCORE_VK(hooks.pick_physical_device(hooks.user, dev.instance, &dev.phys), false);
    } else {
        uint32_t count = 1;
        VkResult r = vkEnumeratePhysicalDevices(dev.instance, &count, &dev.phys);
        if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !count) {
            log("vulkan: no physical device");
            return false;
        }
    }
    vkGetPhysicalDeviceProperties(dev.phys, &dev.props);
    if (dev.props.apiVersion < kApiVersion) {
        log("vulkan: %s has Vulkan %u.%u, needs 1.1", dev.props.deviceName, VK_API_VERSION_MAJOR(dev.props.apiVersion),
            VK_API_VERSION_MINOR(dev.props.apiVersion));
        return false;
    }

    // One graphics queue (it also presents on the desktop; sim/ checks).
    vkGetPhysicalDeviceQueueFamilyProperties(dev.phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.phys, &n, families.data());
    dev.queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < n && dev.queue_family == UINT32_MAX; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) dev.queue_family = i;
    if (dev.queue_family == UINT32_MAX) {
        log("vulkan: no graphics queue");
        return false;
    }

    vkEnumerateDeviceExtensionProperties(dev.phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> dev_exts(n);
    vkEnumerateDeviceExtensionProperties(dev.phys, nullptr, &n, dev_exts.data());
    std::vector<const char *> dexts = hooks.device_extensions;
    for (const char *e : dexts)
        if (!has_ext(dev_exts, e)) {
            log("vulkan: device extension %s missing", e);
            return false;
        }
    if (has_ext(dev_exts, "VK_KHR_portability_subset")) {     // must be enabled where offered
        add_ext(dexts, "VK_KHR_portability_subset");
        dev.portability = true;
    }

    // Features: multiview (core 1.1) always; the fragment density map when asked and fully there.
    bool fdm_ext = has_ext(dev_exts, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT fdm_have = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    VkPhysicalDeviceMultiviewFeatures mv_have = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    mv_have.pNext = fdm_ext ? &fdm_have : nullptr;
    VkPhysicalDeviceFeatures2 have = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &mv_have;
    vkGetPhysicalDeviceFeatures2(dev.phys, &have);
    if (!mv_have.multiview) {
        log("vulkan: %s has no multiview", dev.props.deviceName);
        return false;
    }

    VkPhysicalDeviceFragmentDensityMapFeaturesEXT fdm_on = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    VkPhysicalDeviceMultiviewFeatures mv_on = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    mv_on.multiview = VK_TRUE;
    if (hooks.want_fdm) {
        log("vulkan: fragment density map: extension %d, fragmentDensityMap %d, nonSubsampledImages %d, dynamic %d",
            fdm_ext, fdm_have.fragmentDensityMap, fdm_have.fragmentDensityMapNonSubsampledImages,
            fdm_have.fragmentDensityMapDynamic);
        if (fdm_ext && fdm_have.fragmentDensityMap && fdm_have.fragmentDensityMapNonSubsampledImages) {
            add_ext(dexts, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
            fdm_on.fragmentDensityMap = VK_TRUE;
            fdm_on.fragmentDensityMapNonSubsampledImages = VK_TRUE;
            mv_on.pNext = &fdm_on;
            dev.fdm = true;
        }
    }
    VkPhysicalDeviceFeatures2 on = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    on.pNext = &mv_on;
    // Core features a front's shaders may use, where offered: clip distances (a vertex dropped by its
    // clip distance) and textureGatherOffset with offsets known only at run time. And ASTC textures: without
    // them create_texture returns null for an ASTC format and the caller falls back.
    on.features.shaderClipDistance = have.features.shaderClipDistance;
    on.features.shaderImageGatherExtended = have.features.shaderImageGatherExtended;
    on.features.textureCompressionASTC_LDR = have.features.textureCompressionASTC_LDR;
    dev.clip_distance = have.features.shaderClipDistance;
    dev.gather_extended = have.features.shaderImageGatherExtended;
    dev.astc_ldr = have.features.textureCompressionASTC_LDR;
    log("vulkan: shaderClipDistance %d, shaderImageGatherExtended %d, textureCompressionASTC_LDR %d",
        dev.clip_distance, dev.gather_extended, dev.astc_ldr);

    float priority = 1.f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = dev.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &on;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)dexts.size();
    dci.ppEnabledExtensionNames = dexts.data();
    if (hooks.create_device)
        VRCORE_VK(hooks.create_device(hooks.user, dev.phys, &dci, &dev.device), false);
    else
        VRCORE_VK(vkCreateDevice(dev.phys, &dci, nullptr, &dev.device), false);
    volkLoadDevice(dev.device);
    vkGetDeviceQueue(dev.device, dev.queue_family, 0, &dev.queue);

    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(dev.phys, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) dev.lazy_memory = true;

    VmaVulkanFunctions vf = {};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci = {};
    aci.vulkanApiVersion = kApiVersion;
    aci.physicalDevice = dev.phys;
    aci.device = dev.device;
    aci.instance = dev.instance;
    aci.pVulkanFunctions = &vf;
    VRCORE_VK(vmaCreateAllocator(&aci, &dev.vma), false);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = dev.queue_family;
    VRCORE_VK(vkCreateCommandPool(dev.device, &pci, nullptr, &dev.pool), false);

    log("vulkan: %s, Vulkan %u.%u.%u, driver 0x%x; fdm %d, lazy memory %d, portability %d, %u device extensions",
        dev.props.deviceName, VK_API_VERSION_MAJOR(dev.props.apiVersion), VK_API_VERSION_MINOR(dev.props.apiVersion),
        VK_API_VERSION_PATCH(dev.props.apiVersion), dev.props.driverVersion, dev.fdm, dev.lazy_memory,
        dev.portability, (unsigned)dexts.size());
    return true;
}

void device_destroy(Device &dev)
{
    if (dev.device) {
        vkDeviceWaitIdle(dev.device);
        if (dev.pool) vkDestroyCommandPool(dev.device, dev.pool, nullptr);
        if (dev.vma) vmaDestroyAllocator(dev.vma);
        vkDestroyDevice(dev.device, nullptr);
    }
    if (dev.instance) vkDestroyInstance(dev.instance, nullptr);
    dev = Device();
}

VkCommandBuffer device_begin_once(Device &dev)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = dev.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VRCORE_VK(vkAllocateCommandBuffers(dev.device, &ai, &cmd), VK_NULL_HANDLE);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

bool device_end_once(Device &dev, VkCommandBuffer cmd)
{
    bool ok = false;
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkEndCommandBuffer(cmd) == VK_SUCCESS && vkCreateFence(dev.device, &fci, nullptr, &fence) == VK_SUCCESS &&
        vkQueueSubmit(dev.queue, 1, &si, fence) == VK_SUCCESS)
        ok = vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    if (!ok) log("vulkan: one-off command buffer failed");
    if (fence) vkDestroyFence(dev.device, fence, nullptr);
    vkFreeCommandBuffers(dev.device, dev.pool, 1, &cmd);
    return ok;
}

}
