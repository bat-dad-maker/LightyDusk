// Vulkan part of the probe: what a driver offers against what runtime/nvn/vulkan/device.c asks for, then the same
// vkCreateDevice the backend does, and a host-memory import of the guest heap (exec.c gmem_import).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <string.h>
#include <sys/mman.h>

#include "guest_window.h"
#include "sb.h"

#define VK_FN(name) PFN_##name name
typedef struct {
    VkInstance inst;
    VK_FN(vkGetInstanceProcAddr);
    VK_FN(vkEnumerateInstanceVersion);
    VK_FN(vkCreateInstance);
    VK_FN(vkDestroyInstance);
    VK_FN(vkEnumeratePhysicalDevices);
    VK_FN(vkGetPhysicalDeviceProperties);
    VK_FN(vkGetPhysicalDeviceProperties2);
    VK_FN(vkGetPhysicalDeviceFeatures2);
    VK_FN(vkGetPhysicalDeviceFormatProperties);
    VK_FN(vkGetPhysicalDeviceMemoryProperties);
    VK_FN(vkGetPhysicalDeviceQueueFamilyProperties);
    VK_FN(vkEnumerateDeviceExtensionProperties);
    VK_FN(vkCreateDevice);
    VK_FN(vkGetDeviceProcAddr);
} Vk;

static const char *vk_result(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    default: return "?";
    }
}

static void ver(Sb *b, uint32_t v) {
    sb_printf(b, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

static bool has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

#define YN(x) ((x) ? "yes" : "NO")

// ------------------------------------------------------------------ formats the backend uses (vulkan/textures.c)
typedef struct { VkFormat f; const char *name; } Fmt;
#define F(x) {VK_FORMAT_##x, #x}
static const Fmt k_formats[] = {
    F(R8G8B8A8_UNORM), F(R8G8B8A8_SRGB), F(B8G8R8A8_UNORM), F(B8G8R8A8_SRGB), F(R8G8B8A8_SNORM), F(R8G8B8A8_UINT),
    F(R8_UNORM), F(R8G8_UNORM), F(R16_UNORM), F(R16G16_UNORM), F(R16G16B16A16_UNORM), F(R16_SFLOAT),
    F(R16G16_SFLOAT), F(R16G16B16A16_SFLOAT), F(R32_SFLOAT), F(R32G32_SFLOAT), F(R32G32B32A32_SFLOAT), F(R32_UINT),
    F(R5G6B5_UNORM_PACK16), F(R5G5B5A1_UNORM_PACK16), F(R4G4B4A4_UNORM_PACK16), F(A2B10G10R10_UNORM_PACK32),
    F(A2B10G10R10_UINT_PACK32), F(B10G11R11_UFLOAT_PACK32), F(E5B9G9R9_UFLOAT_PACK32),
    F(D16_UNORM), F(D32_SFLOAT), F(D24_UNORM_S8_UINT), F(D32_SFLOAT_S8_UINT), F(S8_UINT),
    F(BC1_RGBA_UNORM_BLOCK), F(BC1_RGBA_SRGB_BLOCK), F(BC2_UNORM_BLOCK), F(BC2_SRGB_BLOCK), F(BC3_UNORM_BLOCK),
    F(BC3_SRGB_BLOCK), F(BC4_UNORM_BLOCK), F(BC4_SNORM_BLOCK), F(BC5_UNORM_BLOCK), F(BC5_SNORM_BLOCK),
    F(BC6H_UFLOAT_BLOCK), F(BC6H_SFLOAT_BLOCK), F(BC7_UNORM_BLOCK), F(BC7_SRGB_BLOCK),
    F(ASTC_4x4_UNORM_BLOCK), F(ASTC_4x4_SRGB_BLOCK), F(ASTC_8x8_SRGB_BLOCK), F(ASTC_12x12_SRGB_BLOCK),
};
// vertex formats the backend picks (3-component ones are often missing on mobile GPUs)
static const Fmt k_vertex_formats[] = {
    F(R8G8B8_UNORM), F(R8G8B8_UINT), F(R8G8B8_SNORM), F(R8G8B8A8_UNORM), F(R16G16B16_UNORM), F(R16G16B16_SNORM),
    F(R16G16B16_SFLOAT), F(R16G16B16_UINT), F(R16G16_SFLOAT), F(R16G16B16A16_SFLOAT), F(R32G32B32_SFLOAT),
    F(R32G32B32_UINT), F(R32G32B32A32_SFLOAT), F(A2B10G10R10_UNORM_PACK32), F(A2B10G10R10_SNORM_PACK32),
};
#undef F

static void formats(Sb *b, Vk *v, VkPhysicalDevice pd) {
    sb_printf(b, "\nformatos (tiling otimo: S amostra, L filtro linear, C cor, B blend, D profundidade, T copia):\n");
    for (size_t i = 0; i < sizeof k_formats / sizeof *k_formats; i++) {
        VkFormatProperties p;
        v->vkGetPhysicalDeviceFormatProperties(pd, k_formats[i].f, &p);
        VkFormatFeatureFlags o = p.optimalTilingFeatures;
        sb_printf(b, "  %-26s %s%s%s%s%s%s%s\n", k_formats[i].name, o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? "S" : "-",
                  o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? "L" : "-",
                  o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? "C" : "-",
                  o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT ? "B" : "-",
                  o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? "D" : "-",
                  o & VK_FORMAT_FEATURE_TRANSFER_DST_BIT ? "T" : "-", o ? "" : "   <- NOT SUPPORTED");
    }
    sb_printf(b, "\nvertex formats:\n");
    for (size_t i = 0; i < sizeof k_vertex_formats / sizeof *k_vertex_formats; i++) {
        VkFormatProperties p;
        v->vkGetPhysicalDeviceFormatProperties(pd, k_vertex_formats[i].f, &p);
        sb_printf(b, "  %-26s %s\n", k_vertex_formats[i].name,
                  p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT ? "yes" : "NO");
    }
}

// ------------------------------------------------------------------ guest heap as a GPU buffer (exec.c gmem_import)
static void host_import(Sb *b, Vk *v, VkDevice dev, uint64_t align) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)v->vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
    PFN_vkAllocateMemory alloc = (PFN_vkAllocateMemory)v->vkGetDeviceProcAddr(dev, "vkAllocateMemory");
    PFN_vkFreeMemory free_mem = (PFN_vkFreeMemory)v->vkGetDeviceProcAddr(dev, "vkFreeMemory");
    uint64_t base = gw_reserve(GW_SIZE, GW_ALIGN, GW_FLOOR, GW_VA_LIMIT);
    if (!props || !alloc || !free_mem || !base) {
        sb_printf(b, "  heap import: not tested (%s)\n", base ? "functions missing" : "no guest window");
        if (base) munmap((void *)(uintptr_t)base, GW_SIZE);
        return;
    }
    void *heap = (void *)(uintptr_t)(base + GW_HEAP_OFF);
    uint64_t size = 64ull << 20;
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, heap, &mp);
    sb_printf(b, "  heap import (64 MB at %p, minimum alignment %#llx): properties %s, types %#x\n", heap,
              (unsigned long long)align, vk_result(r), mp.memoryTypeBits);
    if (r == VK_SUCCESS && mp.memoryTypeBits) {
        VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                .pHostPointer = heap};
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                                   .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
        VkDeviceMemory mem = VK_NULL_HANDLE;
        r = alloc(dev, &ai, NULL, &mem);
        sb_printf(b, "  heap import: vkAllocateMemory %s\n", vk_result(r));
        if (r == VK_SUCCESS) free_mem(dev, mem, NULL);
    }
    munmap((void *)(uintptr_t)base, GW_SIZE);
}

// ------------------------------------------------------------------ one physical device
static void device(Sb *b, Vk *v, VkPhysicalDevice pd) {
    uint32_t ne = 0;
    v->vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    v->vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    bool e_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    bool e_divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    bool e_host = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    bool e_fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);

    // properties
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
                                              .pNext = e_host ? &hostp : NULL};
    VkPhysicalDeviceVulkan11Properties p11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, .pNext = &p13};
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = &p11};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
    v->vkGetPhysicalDeviceProperties2(pd, &p2);
    VkPhysicalDeviceProperties *p = &p2.properties;
    sb_printf(b, "GPU: %s (vendor %#x, device %#x)\n", p->deviceName, p->vendorID, p->deviceID);
    sb_printf(b, "API: ");
    ver(b, p->apiVersion);
    sb_printf(b, ", driverVersion %#x\n", p->driverVersion);
    sb_printf(b, "driver: %s / %s (conformance %u.%u.%u.%u)\n", drv.driverName, drv.driverInfo, drv.conformanceVersion.major,
              drv.conformanceVersion.minor, drv.conformanceVersion.subminor, drv.conformanceVersion.patch);
    if (p->apiVersion < VK_API_VERSION_1_3) sb_printf(b, "AVISO: API < 1.3: device.c ignora esta GPU\n");

    // features
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    void **tail = &f13.pNext;
    if (e_border) { *tail = &cbc; tail = &cbc.pNext; }
    if (e_divisor) { *tail = &div; tail = &div.pNext; }
    if (e_fault) { *tail = &ff; tail = &ff.pNext; }
    v->vkGetPhysicalDeviceFeatures2(pd, &have);
    VkPhysicalDeviceFeatures *h = &have.features;

    sb_printf(b, "\nextensoes que o backend usa (%u no total):\n", ne);
    sb_printf(b, "  VK_KHR_swapchain                  %s (obrigatoria)\n", YN(has_ext(ep, ne, VK_KHR_SWAPCHAIN_EXTENSION_NAME)));
    sb_printf(b, "  VK_EXT_custom_border_color        %s\n", YN(e_border));
    sb_printf(b, "  VK_EXT_vertex_attribute_divisor   %s (KHR: %s)\n", YN(e_divisor),
              YN(has_ext(ep, ne, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)));
    sb_printf(b, "  VK_EXT_external_memory_host       %s  <- the shaders' global memory (exec.c)\n", YN(e_host));
    sb_printf(b, "  VK_EXT_device_fault               %s\n", YN(e_fault));

    // what device.c enables unconditionally: a missing one makes vkCreateDevice fail
    struct { const char *name; VkBool32 v; } req[] = {
        {"storageBuffer16BitAccess", f11.storageBuffer16BitAccess},
        {"timelineSemaphore", f12.timelineSemaphore},
        {"shaderInt8", f12.shaderInt8},
        {"samplerMirrorClampToEdge", f12.samplerMirrorClampToEdge},
        {"uniformAndStorageBuffer8BitAccess", f12.uniformAndStorageBuffer8BitAccess},
        {"storageBuffer8BitAccess", f12.storageBuffer8BitAccess},
        {"scalarBlockLayout", f12.scalarBlockLayout},
        {"dynamicRendering", f13.dynamicRendering},
        {"synchronization2", f13.synchronization2},
        {"shaderDemoteToHelperInvocation", f13.shaderDemoteToHelperInvocation},
        {"shaderTerminateInvocation", f13.shaderTerminateInvocation},
        {"maintenance4", f13.maintenance4},
        {"customBorderColors (se a ext existe)", !e_border || cbc.customBorderColors},
        {"customBorderColorWithoutFormat (se a ext existe)", !e_border || cbc.customBorderColorWithoutFormat},
        {"vertexAttributeInstanceRateDivisor (se a ext existe)", !e_divisor || div.vertexAttributeInstanceRateDivisor},
        {"deviceFault (se a ext existe)", !e_fault || ff.deviceFault},
    };
    sb_printf(b, "\nfeatures que device.c exige:\n");
    for (size_t i = 0; i < sizeof req / sizeof *req; i++)
        sb_printf(b, "  %-52s %s\n", req[i].name, req[i].v ? "ok" : "FALTA");

    // what device.c copies from the driver (used when present)
    struct { const char *name; VkBool32 v; } opt[] = {
        {"textureCompressionBC", h->textureCompressionBC},
        {"textureCompressionASTC_LDR", h->textureCompressionASTC_LDR},
        {"shaderInt64", h->shaderInt64},
        {"shaderInt16", h->shaderInt16},
        {"shaderFloat64", h->shaderFloat64},
        {"shaderFloat16 (1.2)", f12.shaderFloat16},
        {"dualSrcBlend", h->dualSrcBlend},
        {"independentBlend", h->independentBlend},
        {"depthClamp", h->depthClamp},
        {"depthBiasClamp", h->depthBiasClamp},
        {"fillModeNonSolid", h->fillModeNonSolid},
        {"wideLines", h->wideLines},
        {"largePoints", h->largePoints},
        {"samplerAnisotropy", h->samplerAnisotropy},
        {"imageCubeArray", h->imageCubeArray},
        {"sampleRateShading", h->sampleRateShading},
        {"fragmentStoresAndAtomics", h->fragmentStoresAndAtomics},
        {"vertexPipelineStoresAndAtomics", h->vertexPipelineStoresAndAtomics},
        {"robustBufferAccess", h->robustBufferAccess},
        {"shaderClipDistance", h->shaderClipDistance},
        {"geometryShader", h->geometryShader},
        {"tessellationShader", h->tessellationShader},
        {"multiViewport", h->multiViewport},
        {"shaderStorageImageWriteWithoutFormat", h->shaderStorageImageWriteWithoutFormat},
        {"shaderImageGatherExtended", h->shaderImageGatherExtended},
        {"bufferDeviceAddress (1.2)", f12.bufferDeviceAddress},
    };
    sb_printf(b, "\noptional features:\n");
    for (size_t i = 0; i < sizeof opt / sizeof *opt; i++) sb_printf(b, "  %-40s %s\n", opt[i].name, YN(opt[i].v));

    // subgroups: the shader translator emits subgroupShuffle* (translate.c)
    VkSubgroupFeatureFlags so = p11.subgroupSupportedOperations;
    sb_printf(b, "\nsubgroup: size %u (min %u, max %u), stages %#x, ops:%s%s%s%s%s%s%s%s\n", p11.subgroupSize,
              p13.minSubgroupSize, p13.maxSubgroupSize, p11.subgroupSupportedStages,
              so & VK_SUBGROUP_FEATURE_BASIC_BIT ? " basic" : "", so & VK_SUBGROUP_FEATURE_VOTE_BIT ? " vote" : "",
              so & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT ? " arithmetic" : "", so & VK_SUBGROUP_FEATURE_BALLOT_BIT ? " ballot" : "",
              so & VK_SUBGROUP_FEATURE_SHUFFLE_BIT ? " shuffle" : " (NO shuffle)",
              so & VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT ? " shuffle_relative" : " (NO shuffle_relative)",
              so & VK_SUBGROUP_FEATURE_CLUSTERED_BIT ? " clustered" : "", so & VK_SUBGROUP_FEATURE_QUAD_BIT ? " quad" : "");

    VkPhysicalDeviceLimits *l = &p->limits;
    sb_printf(b, "\nlimites: maxImageDimension2D %u, maxUniformBufferRange %u, maxStorageBufferRange %u,\n"
                 "  minUniformBufferOffsetAlignment %llu, minStorageBufferOffsetAlignment %llu, maxPushConstantsSize %u,\n"
                 "  maxBoundDescriptorSets %u, maxPerStageDescriptorSampledImages %u, maxPerStageDescriptorStorageBuffers %u,\n"
                 "  maxComputeSharedMemorySize %u, maxColorAttachments %u, maxSamplerAnisotropy %.0f, maxVertexInputAttributes %u\n",
              l->maxImageDimension2D, l->maxUniformBufferRange, l->maxStorageBufferRange,
              (unsigned long long)l->minUniformBufferOffsetAlignment, (unsigned long long)l->minStorageBufferOffsetAlignment,
              l->maxPushConstantsSize, l->maxBoundDescriptorSets, l->maxPerStageDescriptorSampledImages,
              l->maxPerStageDescriptorStorageBuffers, l->maxComputeSharedMemorySize, l->maxColorAttachments,
              l->maxSamplerAnisotropy, l->maxVertexInputAttributes);

    VkPhysicalDeviceMemoryProperties mp;
    v->vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    sb_printf(b, "\nmemory: %u heaps, %u types\n", mp.memoryHeapCount, mp.memoryTypeCount);
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        sb_printf(b, "  heap %u: %.2f GB%s\n", i, (double)mp.memoryHeaps[i].size / (1ull << 30),
                  mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " device-local" : "");
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        sb_printf(b, "  tipo %u (heap %u):%s%s%s%s\n", i, mp.memoryTypes[i].heapIndex,
                  f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " device-local" : "",
                  f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host-visible" : "",
                  f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " coherent" : "",
                  f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " cached" : "");
    }

    formats(b, v, pd);

    // The backend's own vkCreateDevice (device.c create_device), with the same extensions and features
    const char *ext[8];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (e_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    if (e_divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    if (e_host) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    if (e_fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT rdiv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT rcbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = e_divisor ? &rdiv : NULL};
    VkPhysicalDeviceVulkan13Features r13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE,
                                            .pNext = e_border ? (void *)&rcbc : e_divisor ? (void *)&rdiv : NULL};
    VkPhysicalDeviceVulkan12Features r12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .pNext = &r13};
    VkPhysicalDeviceVulkan11Features r11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &r12};
    VkPhysicalDeviceFaultFeaturesEXT rff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                            .pNext = &r11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = e_fault ? (void *)&rff : (void *)&r11};
    VkPhysicalDeviceFeatures *rb = &f.features;
    rb->independentBlend = h->independentBlend;
    rb->dualSrcBlend = h->dualSrcBlend;
    rb->fillModeNonSolid = h->fillModeNonSolid;
    rb->samplerAnisotropy = h->samplerAnisotropy;
    rb->textureCompressionBC = h->textureCompressionBC;
    rb->shaderInt64 = h->shaderInt64;
    rb->depthBiasClamp = h->depthBiasClamp;
    rb->depthClamp = h->depthClamp;
    rb->largePoints = h->largePoints;
    rb->wideLines = h->wideLines;
    rb->imageCubeArray = h->imageCubeArray;
    rb->sampleRateShading = h->sampleRateShading;
    rb->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    rb->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    rb->robustBufferAccess = h->robustBufferAccess;
    rb->shaderClipDistance = h->shaderClipDistance;

    uint32_t nq = 0;
    v->vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    v->vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfam = 0;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r = v->vkCreateDevice(pd, &di, NULL, &dev);
    sb_printf(b, "\nvkCreateDevice as in device.c: %s\n", vk_result(r));
    if (r == VK_ERROR_FEATURE_NOT_PRESENT) {
        // The Android adjustment: 8-bit features only when the driver has them (no shader uses 8-bit uniform buffers)
        r12.shaderInt8 = f12.shaderInt8;
        r12.uniformAndStorageBuffer8BitAccess = f12.uniformAndStorageBuffer8BitAccess;
        r12.storageBuffer8BitAccess = f12.storageBuffer8BitAccess;
        r = v->vkCreateDevice(pd, &di, NULL, &dev);
        sb_printf(b, "vkCreateDevice with the optional 8-bit features (Android adjustment): %s\n", vk_result(r));
    }
    if (r == VK_SUCCESS) {
        if (e_host) host_import(b, v, dev, hostp.minImportedHostPointerAlignment);
        PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)v->vkGetDeviceProcAddr(dev, "vkDestroyDevice");
        if (destroy) destroy(dev, NULL);
    }

    sb_printf(b, "\ntodas as extensoes:\n");
    for (uint32_t i = 0; i < ne; i++) sb_printf(b, "  %s (v%u)\n", ep[i].extensionName, ep[i].specVersion);
    free(ep);
}

void vk_probe(Sb *b, void *lib) {
    Vk v = {0};
    v.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!v.vkGetInstanceProcAddr) {
        sb_printf(b, "ERROR: vkGetInstanceProcAddr missing\n");
        return;
    }
#define LOAD_GLOBAL(name) v.name = (PFN_##name)v.vkGetInstanceProcAddr(VK_NULL_HANDLE, #name)
    LOAD_GLOBAL(vkEnumerateInstanceVersion);
    LOAD_GLOBAL(vkCreateInstance);
    uint32_t iv = VK_API_VERSION_1_0;
    if (v.vkEnumerateInstanceVersion) v.vkEnumerateInstanceVersion(&iv);
    sb_printf(b, "instancia: ");
    ver(b, iv);
    sb_printf(b, "\n");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LASProbe",
                             .pEngineName = "LARecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkResult r = v.vkCreateInstance ? v.vkCreateInstance(&ci, NULL, &v.inst) : VK_ERROR_INITIALIZATION_FAILED;
    if (r != VK_SUCCESS) {
        sb_printf(b, "ERROR: vkCreateInstance: %s\n", vk_result(r));
        return;
    }
#define LOAD(name) v.name = (PFN_##name)v.vkGetInstanceProcAddr(v.inst, #name)
    LOAD(vkDestroyInstance);
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkGetPhysicalDeviceProperties2);
    LOAD(vkGetPhysicalDeviceFeatures2);
    LOAD(vkGetPhysicalDeviceFormatProperties);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkEnumerateDeviceExtensionProperties);
    LOAD(vkCreateDevice);
    LOAD(vkGetDeviceProcAddr);
    uint32_t n = 0;
    v.vkEnumeratePhysicalDevices(v.inst, &n, NULL);
    VkPhysicalDevice pds[4];
    if (n > 4) n = 4;
    v.vkEnumeratePhysicalDevices(v.inst, &n, pds);
    if (!n) sb_printf(b, "ERROR: no GPU (with adrenotools: hooks outside nativeLibraryDir?)\n");
    for (uint32_t i = 0; i < n; i++) device(b, &v, pds[i]);
    v.vkDestroyInstance(v.inst, NULL);
}
