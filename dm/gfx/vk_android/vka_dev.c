// The Vulkan device of the replay: instance, the one queue, the timeline that fences submissions, the completion thread that runs their CPU-side effects, and scratch memory.
// Feature and extension choices follow the probe of android/spike (results/honor-pnm-n49.md): placed memory maps, buffer device addresses, update-after-bind arrays, display timing
#if defined(__ANDROID__)
#include "vka.h"
#include <dlfcn.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "vka_fd_alias.h"

// One placed map of a 64 KiB host-visible allocation at an address reserved for it (vka_dev.c, unadvertised placed maps): 1 when the driver maps it exactly there and
// the memory reads back what was written through it
static int vka_placed_probe(void) {
    VkPhysicalDeviceMemoryProperties mp; vk.vkGetPhysicalDeviceMemoryProperties(V.pd, &mp);
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == ~0u; i++)
        if ((mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) type = i;
    if (type == ~0u) return 0;
    const VkDeviceSize n = 64 << 10;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = n, .memoryTypeIndex = type};
    VkDeviceMemory mem; if (vk.vkAllocateMemory(V.dev, &mai, NULL, &mem)) return 0;
    void *at = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    int ok = 0; void *q = NULL; VkResult r = VK_ERROR_UNKNOWN;
    if (at != MAP_FAILED) {
        VkMemoryMapPlacedInfoEXT pi = {VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = at};
        VkMemoryMapInfoKHR mi = {VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = mem, .offset = 0, .size = VK_WHOLE_SIZE};
        r = vk.vkMapMemory2KHR(V.dev, &mi, &q);
        if (!r && q == at) { volatile uint32_t *w = q; w[0] = 0x5eed1234u; w[n / 4 - 1] = 0xc0ffee00u; ok = w[0] == 0x5eed1234u && w[n / 4 - 1] == 0xc0ffee00u; }
        if (!r) { VkMemoryUnmapInfoKHR ui = {VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = mem}; vk.vkUnmapMemory2KHR(V.dev, &ui); }
        munmap(at, n);
    }
    vk.vkFreeMemory(V.dev, mem, NULL);
    VLOG("placed maps: not advertised by the driver; a placed map %s (vkMapMemory2 %d, asked %p, got %p)%s", ok ? "works" : "does not work", r, at, q,
         ok ? ": used without the extension (DM_VK_PLACED_PROBE=0: off)" : "");
    return ok;
}

vka_fns vk;
vka_dev V = {.qmu = PTHREAD_MUTEX_INITIALIZER};
static int g_state;   // 0 not tried, 1 up, -1 failed
static pthread_mutex_t g_init_mu = PTHREAD_MUTEX_INITIALIZER;


uint32_t vka_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < V.memp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (V.memp.memoryTypes[i].propertyFlags & want) == want && !(V.memp.memoryTypes[i].propertyFlags & avoid)) return i;
    return UINT32_MAX;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }

static void *completion_main(void *arg);
// ---------- The host's floating-point environment (vka_fpenv.h) ----------
int vka_fp_guest(void) {
    static _Atomic int v = -1;
    int g = atomic_load_explicit(&v, memory_order_relaxed);
    if (g < 0) {
        g = vka_fp_guest_env(getenv("DM_VK_GUEST_FPCR"));
        if (atomic_exchange(&v, g) < 0 && g) VLOG("DM_VK_GUEST_FPCR=1: the replay's threads and the driver's compiles keep the FPCR they inherit from the game; the pipeline cache keeps no file");
    }
    return g;
}
uint64_t vka_fp_host(const char *who) {
    uint64_t was = vka_fp_enter(vka_fp_guest());
    static _Atomic int n;
    if (who && was != VKA_HOST_FPCR && atomic_fetch_add(&n, 1) < 8)
        VLOG("%s: started with the game's FPCR %#llx%s", who, (unsigned long long)was, vka_fp_guest() ? ", kept (DM_VK_GUEST_FPCR=1)" : ", the host's (0) set");
    return was;
}
// The calls of vk.* that compile go through these, so they compile with the host's FPCR on whatever thread calls them (the replay, presents, the NaN scan, NDSR,
// a replay run inline on the game's thread), and the caller's FPCR is put back afterwards
static PFN_vkCreateShaderModule g_csm; static PFN_vkCreateGraphicsPipelines g_cgp; static PFN_vkCreateComputePipelines g_ccp;
static VkResult VKAPI_CALL fp_csm(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *m) {
    uint64_t f = vka_fp_host(NULL); VkResult r = g_csm(d, ci, a, m); vka_fp_back(f); return r; }
static VkResult VKAPI_CALL fp_cgp(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    uint64_t f = vka_fp_host(NULL); VkResult r = g_cgp(d, c, n, ci, a, p); vka_fp_back(f); return r; }
static VkResult VKAPI_CALL fp_ccp(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    uint64_t f = vka_fp_host(NULL); VkResult r = g_ccp(d, c, n, ci, a, p); vka_fp_back(f); return r; }
static void fp_wrap_compiles(void) {
    if (vka_fp_guest()) return;
    if ((g_csm = vk.vkCreateShaderModule)) vk.vkCreateShaderModule = fp_csm;
    if ((g_cgp = vk.vkCreateGraphicsPipelines)) vk.vkCreateGraphicsPipelines = fp_cgp;
    if ((g_ccp = vk.vkCreateComputePipelines)) vk.vkCreateComputePipelines = fp_ccp;
}
static int init_once(void) {
    void *lib = vka_driver_open();   // DM_VK_DRIVER: a custom driver through libadrenotools (vka_driver.c); NULL = the system's
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib || !(vk.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr"))) { VRED("no libvulkan.so: %s", dlerror()); return 0; }
#define G(f) vk.f = (PFN_##f)vk.vkGetInstanceProcAddr(NULL, #f);
    VKA_GLOBAL_FNS(G)
#undef G
    uint32_t api = VK_API_VERSION_1_0; if (vk.vkEnumerateInstanceVersion) vk.vkEnumerateInstanceVersion(&api);
    if (api < VK_API_VERSION_1_3) { VRED("Vulkan instance %u.%u: the replay needs 1.3", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api)); return 0; }
    uint32_t ne = 0; vk.vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
    VkExtensionProperties *ie = calloc(ne ? ne : 1, sizeof *ie); vk.vkEnumerateInstanceExtensionProperties(NULL, &ne, ie);
    const char *iext[4]; uint32_t niext = 0;
    if (has_ext(ie, ne, VK_KHR_SURFACE_EXTENSION_NAME) && has_ext(ie, ne, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) { iext[niext++] = VK_KHR_SURFACE_EXTENSION_NAME; iext[niext++] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME; V.has_android_surface = 1; }
    free(ie);
    const char *layer = "VK_LAYER_KHRONOS_validation"; uint32_t nlayer = 0;
    const char *ve = getenv("DM_VK_VALIDATE");
    if (ve && atoi(ve)) {
        uint32_t nl = 0; vk.vkEnumerateInstanceLayerProperties(&nl, NULL); VkLayerProperties *lp = calloc(nl ? nl : 1, sizeof *lp); vk.vkEnumerateInstanceLayerProperties(&nl, lp);
        for (uint32_t i = 0; i < nl; i++) if (!strcmp(lp[i].layerName, layer)) nlayer = 1;
        free(lp); VLOG("DM_VK_VALIDATE: validation layer %s", nlayer ? "on" : "not installed");
    }
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative", .pEngineName = "nxnative", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = niext, .ppEnabledExtensionNames = iext,
                                .enabledLayerCount = nlayer, .ppEnabledLayerNames = &layer};
    VkResult r = vk.vkCreateInstance(&ici, NULL, &V.inst);
    if (r) { VRED("vkCreateInstance %d", r); return 0; }
#define I(f) vk.f = (PFN_##f)vk.vkGetInstanceProcAddr(V.inst, #f);
    VKA_INSTANCE_FNS(I)
#undef I
    uint32_t npd = 1; VkPhysicalDevice pd[1];
    if (vk.vkEnumeratePhysicalDevices(V.inst, &npd, pd) < 0 || !npd) { VRED("no Vulkan physical device"); return 0; }
    V.pd = pd[0];
    vk.vkGetPhysicalDeviceProperties(V.pd, &V.props); vk.vkGetPhysicalDeviceMemoryProperties(V.pd, &V.memp);
    if (V.props.apiVersion < VK_API_VERSION_1_3) { VRED("device %s is Vulkan %u.%u: the replay needs 1.3", V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion)); return 0; }
    uint32_t nq = 0; vk.vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp); vk.vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, qp);
    V.qf = UINT32_MAX; for (uint32_t i = 0; i < nq && V.qf == UINT32_MAX; i++) if ((qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) V.qf = i;
    { VkPhysicalDeviceFeatures pf; vk.vkGetPhysicalDeviceFeatures(V.pd, &pf); const VkPhysicalDeviceSparseProperties *sp = &V.props.sparseProperties;   // sparse residency: for sparse NVN
      VLOG("sparse: binding %d, residency buffer %d image2D %d image3D %d aliased %d, standard 2D block shape %d, non-resident strict %d, address space %llu MiB, sparse binding on queue family %u: %s; "
           "buffer / image granularity %llu",   // pools' textures (docs/reference/nvn-objects.md "Sparse texture arrays made at their full size")
           pf.sparseBinding, pf.sparseResidencyBuffer, pf.sparseResidencyImage2D, pf.sparseResidencyImage3D, pf.sparseResidencyAliased, sp->residencyStandard2DBlockShape, sp->residencyNonResidentStrict,
           (unsigned long long)(V.props.limits.sparseAddressSpaceSize >> 20), V.qf, V.qf < nq && (qp[V.qf].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) ? "yes" : "no",
           (unsigned long long)V.props.limits.bufferImageGranularity);
      // Sparse residency images (vka_sparse.c) need it all: binding on the replay's queue, 2D residency, the standard block shape (NVN's 64 KB tiles) and strict non-resident reads
      // (an unbound tile reads 0, as an unmapped NVN page). The features are enabled with every other supported one (vkGetPhysicalDeviceFeatures2 below)
      V.sparse_res = pf.sparseBinding && pf.sparseResidencyImage2D && sp->residencyStandard2DBlockShape && sp->residencyNonResidentStrict && V.qf < nq && (qp[V.qf].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT); }
    free(qp);
    if (V.qf == UINT32_MAX) { VRED("no graphics + compute queue family"); return 0; }

    uint32_t nde = 0; vk.vkEnumerateDeviceExtensionProperties(V.pd, NULL, &nde, NULL);
    VkExtensionProperties *de = calloc(nde ? nde : 1, sizeof *de); vk.vkEnumerateDeviceExtensionProperties(V.pd, NULL, &nde, de);
    const char *dext[24]; uint32_t ndext = 0;
    { char q[700]; size_t qn = 0; q[0] = 0;   // the vendor's own extensions (tile properties, render pass transforms ...): what a tiler-specific path could use
      for (uint32_t i = 0; i < nde && qn < sizeof q; i++) if (strstr(de[i].extensionName, "_QCOM_")) qn += (size_t)snprintf(q + qn, sizeof q - qn, " %s", de[i].extensionName + 3);
      if (qn) VLOG("device extensions of the vendor:%s", q); }
    int swap = has_ext(de, nde, VK_KHR_SWAPCHAIN_EXTENSION_NAME), map2 = has_ext(de, nde, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME), placed = map2 && has_ext(de, nde, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME);
    V.display_timing = swap && has_ext(de, nde, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    if (swap) dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (map2) dext[ndext++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
    if (placed) dext[ndext++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
    int x_fd = has_ext(de, nde, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    if (x_fd) dext[ndext++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    if (V.display_timing) dext[ndext++] = VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME;
    // The draw path (vka_draw.c): each one optional, with a fallback when absent
    int x_push = has_ext(de, nde, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME), x_vin = has_ext(de, nde, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    const char *x_rob = has_ext(de, nde, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME) ? VK_KHR_ROBUSTNESS_2_EXTENSION_NAME : has_ext(de, nde, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) ? VK_EXT_ROBUSTNESS_2_EXTENSION_NAME : NULL;
    const char *x_i8 = has_ext(de, nde, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME) ? VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME : has_ext(de, nde, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME) ? VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME : NULL;
    const char *x_div = has_ext(de, nde, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) ? VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME : has_ext(de, nde, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) ? VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME : NULL;
    if (x_push) dext[ndext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    if (x_vin) dext[ndext++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    if (x_rob) dext[ndext++] = x_rob;
    if (x_i8) dext[ndext++] = x_i8;
    if (x_div) dext[ndext++] = x_div;
    int x_gpl = has_ext(de, nde, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) && has_ext(de, nde, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    if (x_gpl) { dext[ndext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME; dext[ndext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME; }
    int mesh_ext = has_ext(de, nde, VK_EXT_MESH_SHADER_EXTENSION_NAME); const char *mesh_env = getenv("DM_VK_MESH");   // restart only: the device's extensions
    int x_mesh = mesh_ext && !(mesh_env && !strcmp(mesh_env, "0"));
    if (x_mesh) dext[ndext++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
    int x_ph = has_ext(de, nde, "VK_QCOM_queue_perf_hint");
    if (x_ph) dext[ndext++] = "VK_QCOM_queue_perf_hint";
    V.mem_budget = has_ext(de, nde, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);   // the image heap's budget, for the texture cache's limit (vka_tex.c img_limit_update)
    if (V.mem_budget) dext[ndext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    free(de);

    // Every feature the device has, but robustness and capture-replay (they cost and nothing needs them); the shader side takes what it needs from the same device
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT fmp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvin = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesKHR frob = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR};
    VkPhysicalDeviceIndexTypeUint8Features fi8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES};
    VkPhysicalDeviceVertexAttributeDivisorFeatures fdiv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceMeshShaderFeaturesEXT fmesh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    void *chain = placed ? &fmp : NULL;
#define LINK(on, st) if (on) { (st).pNext = chain; chain = &(st); }
    struct { VkStructureType sType; void *pNext; VkBool32 queuePerfHint; } fph = {(VkStructureType)1000302001};   // VkPhysicalDeviceQueuePerfHintFeaturesQCOM (not in the NDK's headers yet)
    LINK(x_vin, fvin) LINK(x_rob, frob) LINK(x_i8, fi8) LINK(x_div, fdiv) LINK(x_ph, fph) LINK(x_gpl, fgpl) LINK(x_mesh, fmesh)
#undef LINK
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = chain};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vk.vkGetPhysicalDeviceFeatures2(V.pd, &f2);
    f2.features.robustBufferAccess = VK_FALSE; f13.robustImageAccess = VK_FALSE;
    f12.bufferDeviceAddressCaptureReplay = VK_FALSE; f12.bufferDeviceAddressMultiDevice = VK_FALSE;
    frob.robustBufferAccess2 = VK_FALSE; frob.robustImageAccess2 = VK_FALSE;   // only nullDescriptor: unbound slots read a null descriptor
    // Mesh shading: the task and mesh stages only (multiview, shading-rate and query support each need more of the device than the replay enables or uses)
    fmesh.multiviewMeshShader = VK_FALSE; fmesh.primitiveFragmentShadingRateMeshShader = VK_FALSE; fmesh.meshShaderQueries = VK_FALSE;
    if (x_mesh && !fmesh.meshShader) fmesh.taskShader = VK_FALSE;   // (task shading is of no use without mesh shading)
    V.push_desc = x_push; V.vin_dyn = x_vin && fvin.vertexInputDynamicState; V.null_desc = x_rob && frob.nullDescriptor; V.idx8 = x_i8 && fi8.indexTypeUint8;
    V.divisor = x_div && fdiv.vertexAttributeInstanceRateDivisor; V.logic_op = f2.features.logicOp; V.dual_src = f2.features.dualSrcBlend; V.geom = f2.features.geometryShader;
    V.tess = f2.features.tessellationShader; V.depth_clamp = f2.features.depthClamp; V.fill_lines = f2.features.fillModeNonSolid; V.depth_bias_clamp = f2.features.depthBiasClamp;
    V.multi_vp = f2.features.multiViewport; V.a2c = 1; V.mesh = x_mesh && fmesh.meshShader; V.task = V.mesh && fmesh.taskShader;
    if (!f12.timelineSemaphore || !f13.dynamicRendering) { VRED("the device lacks timeline semaphores (%d) or dynamic rendering (%d)", f12.timelineSemaphore, f13.dynamicRendering); return 0; }
    V.imageless = f12.imagelessFramebuffer;   // (enabled with the rest of f12)
    V.bda = f12.bufferDeviceAddress; V.uab = f12.descriptorBindingSampledImageUpdateAfterBind && f12.descriptorBindingPartiallyBound && f12.runtimeDescriptorArray;
    V.placed = placed && fmp.memoryMapPlaced; V.placed_range = placed && fmp.memoryMapRangePlaced; V.unmap_reserve = placed && fmp.memoryUnmapReserve;
    float prio = 1.f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V.qf, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = ndext, .ppEnabledExtensionNames = dext};
    void vka_kgsl_before(int has_ext), vka_kgsl_after(int has_hint); vka_kgsl_before(x_ph);   // the kernel contexts the device's queue gets (the KGSL floor below)
    if ((r = vk.vkCreateDevice(V.pd, &dci, NULL, &V.dev))) { VRED("vkCreateDevice %d", r); return 0; }
#define D(f) vk.f = (PFN_##f)vk.vkGetDeviceProcAddr(V.dev, #f);
    VKA_DEVICE_FNS(D)
#undef D
    fp_wrap_compiles();
    if (!vk.vkMapMemory2KHR) vk.vkMapMemory2KHR = (PFN_vkMapMemory2KHR)vk.vkGetDeviceProcAddr(V.dev, "vkMapMemory2");   // core in 1.4
    if (!vk.vkUnmapMemory2KHR) vk.vkUnmapMemory2KHR = (PFN_vkUnmapMemory2KHR)vk.vkGetDeviceProcAddr(V.dev, "vkUnmapMemory2");
    if (!vk.vkMapMemory2KHR || !vk.vkUnmapMemory2KHR) V.placed = V.placed_range = 0;
    // A driver that has the placed map but does not advertise it (Qualcomm 512.863 on the Adreno 740: the strings and vkMapMemory2 are in the driver, the extension is
    // not reported and cannot be enabled): try one placed map of a small allocation at a reserved address; when it lands there, use placed maps without enabling the
    // extension (whole allocations only, no unmap-reserve). DM_VK_PLACED_PROBE=0 turns the try off
    if (!V.placed && vk.vkMapMemory2KHR && vk.vkUnmapMemory2KHR) {
        const char *e = getenv("DM_VK_PLACED_PROBE");
        if (!e || strcmp(e, "0")) V.placed = vka_placed_probe();
    }
    if (!V.placed && x_fd && vka_fd_enabled()) {
        char why[160]; V.fd_alias = vka_fd_probe(&vk, V.pd, V.dev, &V.memp, V.bda, why, sizeof why);
        VLOG("exported guest memory alias: %s (%s; DM_VK_FD_ALIAS=0: off)", V.fd_alias ? "on" : "unavailable", why);
    }
    vk.vkGetDeviceQueue(V.dev, V.qf, 0, &V.q);
    if (x_ph && fph.queuePerfHint) V.perf_hint = (void *)vk.vkGetDeviceProcAddr(V.dev, "vkQueueSetPerfHintQCOM");
    VLOG("GPU clock floor (VK_QCOM_queue_perf_hint): %s", V.perf_hint ? "available (DM_VK_GPU_FLOOR)" : x_ph ? "extension without the feature" : "not on this device");
    vka_kgsl_after(V.perf_hint != NULL);
#if defined(__ANDROID__)
    { int vka_floor_adpf(void), vka_floor_kgsl(void); if (!V.perf_hint && !vka_floor_kgsl()) VLOG("GPU clock floor through ADPF: %s", vka_floor_adpf() ? "each frame's span and GPU time reported against the game's shortest period (DM_VK_GPU_FLOOR_ADPF)" : "off (DM_VK_GPU_FLOOR=0 or DM_VK_GPU_FLOOR_ADPF=0)"); }
#endif

    VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pmp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan13Properties p13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, .pNext = placed ? &pmp : NULL};
    VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, .pNext = &p13};
    VkPhysicalDevicePushDescriptorPropertiesKHR ppd = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR, .pNext = &p11};
    VkPhysicalDeviceMaintenance3Properties pm3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, .pNext = V.push_desc ? (void *)&ppd : (void *)&p11};
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT pgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT, .pNext = &pm3};
    VkPhysicalDeviceDriverProperties pdr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = x_gpl ? (void *)&pgpl : (void *)&pm3};
    VkPhysicalDeviceMeshShaderPropertiesEXT pmesh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT, .pNext = &pdr};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = x_mesh ? (void *)&pmesh : (void *)&pdr};
    vk.vkGetPhysicalDeviceProperties2(V.pd, &p2);
    VLOG("driver: %s (%s), driver ID %d, conformance %u.%u.%u.%u, driver version %#x; %s", pdr.driverName, pdr.driverInfo, pdr.driverID, pdr.conformanceVersion.major,
         pdr.conformanceVersion.minor, pdr.conformanceVersion.subminor, pdr.conformanceVersion.patch, V.props.driverVersion, vka_driver_tag() ? "custom (DM_VK_DRIVER)" : "the system's");
    V.driver_id = (int)pdr.driverID;
    { const char *e = getenv("DM_VK_GPL"); int want = !(e && !strcmp(e, "0"));   // restart only: the libraries are made with the device's pipelines
      V.gpl = want && x_gpl && fgpl.graphicsPipelineLibrary && pgpl.graphicsPipelineLibraryFastLinking;
      VLOG("graphics pipeline libraries: %s", V.gpl ? "on (a pipeline the cache lacks is linked from stage libraries, the optimized one is made in the background; DM_VK_GPL=0 off)"
           : !x_gpl ? "not on this device" : !fgpl.graphicsPipelineLibrary ? "extension without the feature" : !pgpl.graphicsPipelineLibraryFastLinking ? "no fast linking" : "off (DM_VK_GPL=0)"); }
    V.placed_align = pmp.minPlacedMemoryMapAlignment ? pmp.minPlacedMemoryMapAlignment : 4096; V.max_alloc = pm3.maxMemoryAllocationSize;
    V.max_push = V.push_desc ? ppd.maxPushDescriptors : 0;
    if (V.mesh && !vk.vkCmdDrawMeshTasksEXT) V.mesh = V.task = 0;
    if (V.mesh) { V.mesh_inv = pmesh.maxMeshWorkGroupInvocations; V.task_inv = pmesh.maxTaskWorkGroupInvocations; V.mesh_out_v = pmesh.maxMeshOutputVertices;
                  V.mesh_out_p = pmesh.maxMeshOutputPrimitives; V.task_payload = pmesh.maxTaskPayloadSize;
                  V.task_groups = pmesh.maxTaskWorkGroupCount[0]; V.mesh_groups = pmesh.maxMeshWorkGroupCount[0]; }
    VLOG("mesh shading (NVN2 task and mesh programs): %s", V.task ? "on" : V.mesh ? "mesh shaders only (no taskShader)" : !mesh_ext ? "not on this device" : !x_mesh ? "off (DM_VK_MESH=0)" : "extension without the mesh stage");
    if (V.mesh) VLOG("mesh shading limits: workgroups mesh %u / task %u invocations, mesh output %u vertices / %u primitives / %u components / %u bytes, task payload %u bytes, mesh shared %u bytes",
                     V.mesh_inv, V.task_inv, V.mesh_out_v, V.mesh_out_p, pmesh.maxMeshOutputComponents, pmesh.maxMeshOutputMemorySize, V.task_payload, pmesh.maxMeshSharedMemorySize);
    VLOG("subgroups: size %u, stages %#x (geometry %s, fragment %s, vertex %s), operations %#x, quad operations in all stages %s, required size stages %#x", p11.subgroupSize, p11.subgroupSupportedStages,
         p11.subgroupSupportedStages & VK_SHADER_STAGE_GEOMETRY_BIT ? "yes" : "NO", p11.subgroupSupportedStages & VK_SHADER_STAGE_FRAGMENT_BIT ? "yes" : "NO", p11.subgroupSupportedStages & VK_SHADER_STAGE_VERTEX_BIT ? "yes" : "NO",
         p11.subgroupSupportedOperations, p11.subgroupQuadOperationsInAllStages ? "yes" : "no", p13.requiredSubgroupSizeStages);
    V.subgroup_stages = p11.subgroupSupportedStages;
    V.subgroup_req_stages = f13.subgroupSizeControl ? p13.requiredSubgroupSizeStages : 0;
    V.subgroup_size = p11.subgroupSize; V.subgroup_min = p13.minSubgroupSize ? p13.minSubgroupSize : p11.subgroupSize; V.subgroup_max = p13.maxSubgroupSize ? p13.maxSubgroupSize : p11.subgroupSize;
    if (!vk.vkCmdPushDescriptorSetKHR) vk.vkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vk.vkGetDeviceProcAddr(V.dev, "vkCmdPushDescriptorSet");   // core in 1.4
    if (!vk.vkCmdPushDescriptorSetKHR) V.push_desc = 0, V.max_push = 0;
    if (!vk.vkCmdSetVertexInputEXT) V.vin_dyn = 0;
    vka_enc_install();
    const VkPhysicalDeviceLimits *lim = &V.props.limits;
    V.ub_align = lim->minUniformBufferOffsetAlignment ? lim->minUniformBufferOffsetAlignment : 256; V.sb_align = lim->minStorageBufferOffsetAlignment ? lim->minStorageBufferOffsetAlignment : 256;
    V.ub_range = lim->maxUniformBufferRange; V.sb_range = lim->maxStorageBufferRange; V.max_vin_bindings = lim->maxVertexInputBindings;

    VkMemoryPropertyFlags HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    V.guest_type = vka_mem_type(~0u, HV | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (V.guest_type == UINT32_MAX) V.guest_type = vka_mem_type(~0u, HV | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (V.guest_type == UINT32_MAX) V.guest_type = vka_mem_type(~0u, HV, 0);
    V.upload_type = V.guest_type;
    V.local_type = vka_mem_type(~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);

    VkSemaphoreTypeCreateInfo st = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    if ((r = vk.vkCreateSemaphore(V.dev, &sci, NULL, &V.tl))) { VRED("timeline semaphore %d", r); return 0; }
    pthread_t th; if (pthread_create(&th, NULL, completion_main, NULL)) { VRED("cannot start the completion thread"); return 0; }
    pthread_detach(th);
    VLOG("device %s, Vulkan %u.%u.%u, driver %#x; placed maps %s (range %s, alignment %llu), buffer device address %s, update-after-bind %s, display timing %s; guest memory type %u (%#x), max allocation %llu MiB, at most %u allocations",
         V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), VK_API_VERSION_PATCH(V.props.apiVersion), V.props.driverVersion,
         V.placed ? "yes" : "no", V.placed_range ? "yes" : "no", (unsigned long long)V.placed_align, V.bda ? "yes" : "no", V.uab ? "yes" : "no", V.display_timing ? "yes" : "no",
         V.guest_type, V.guest_type < 32 ? V.memp.memoryTypes[V.guest_type].propertyFlags : 0, (unsigned long long)(V.max_alloc >> 20), V.props.limits.maxMemoryAllocationCount);   // (images take one allocation each: Mario Kart World ran out of them)
    VLOG("draw path: push descriptors %s (%u), vertex input dynamic state %s, null descriptors %s, 8-bit indices %s, instance divisors %s, logic op %s, dual source %s, geometry %s, tessellation %s; "
         "uniform alignment %llu (range %llu), storage alignment %llu, subgroups %u (%u-%u)",
         V.push_desc ? "yes" : "no", V.max_push, V.vin_dyn ? "yes" : "no", V.null_desc ? "yes" : "no", V.idx8 ? "yes" : "no", V.divisor ? "yes" : "no", V.logic_op ? "yes" : "no",
         V.dual_src ? "yes" : "no", V.geom ? "yes" : "no", V.tess ? "yes" : "no", (unsigned long long)V.ub_align, (unsigned long long)V.ub_range, (unsigned long long)V.sb_align, V.subgroup_size, V.subgroup_min, V.subgroup_max);
    if (!V.placed && !V.fd_alias) VRED("no placed maps or exported guest memory alias: guest memory stays anonymous and the GPU sees no memory pool");
    if (!swap) VRED("no VK_KHR_swapchain: nothing is presented");
    vka_present_init();
    return 1;
}
int vka_init(void) {
    if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE)) return g_state > 0;
    pthread_mutex_lock(&g_init_mu);
    // With the host's FPCR: the driver compiles at device creation, and the threads it makes here inherit this thread's FPCR (often a game thread's)
    if (!g_state) { uint64_t f = vka_fp_host(NULL); __atomic_store_n(&g_state, init_once() ? 1 : -1, __ATOMIC_RELEASE); vka_fp_back(f); }
    pthread_mutex_unlock(&g_init_mu);
    return g_state > 0;
}
int vka_ok(void) { return __atomic_load_n(&g_state, __ATOMIC_ACQUIRE) > 0; }

VkResult vka_submit(VkCommandBuffer cb, uint64_t signal, uint32_t nwait, const VkSemaphore *wait, const uint64_t *wait_vals, const VkPipelineStageFlags *stages, VkSemaphore bin_signal) {
    VkSemaphore sig[2]; uint64_t sv[2]; uint32_t ns = 0;
    if (signal) { sig[ns] = V.tl; sv[ns++] = signal; }
    if (bin_signal) { sig[ns] = bin_signal; sv[ns++] = 0; }
    // The waits, and one more for the sparse binds queued before it (vks_queue_locked)
    VkSemaphore ws[9]; uint64_t wv[9]; VkPipelineStageFlags wst[9];
    if (nwait > 8) { VRED_ONCE("vka_submit: %u waits, at most 8 taken: the rest are dropped", nwait); nwait = 8; }
    for (uint32_t i = 0; i < nwait; i++) { ws[i] = wait[i]; wv[i] = wait_vals[i]; wst[i] = stages[i]; }
    pthread_mutex_lock(&V.qmu);
    vks_queue_locked(&nwait, ws, wv, wst);
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = nwait, .pWaitSemaphoreValues = wv, .signalSemaphoreValueCount = ns, .pSignalSemaphoreValues = sv};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ti, .waitSemaphoreCount = nwait, .pWaitSemaphores = ws, .pWaitDstStageMask = wst,
                       .commandBufferCount = cb ? 1 : 0, .pCommandBuffers = &cb, .signalSemaphoreCount = ns, .pSignalSemaphores = sig};
    VkResult r = vka_hung() ? VK_ERROR_DEVICE_LOST : vk.vkQueueSubmit(V.q, 1, &si, VK_NULL_HANDLE);
    if (!r && signal > V.tl_queued) __atomic_store_n(&V.tl_queued, signal, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&V.qmu);
    return r;
}
// The queue idle, bounded by the hang time: the timeline up to the last value handed to it (the present's copies wait for no more), then vkQueueWaitIdle for the rest
int vka_queue_idle(void) {
    if (vka_hung()) return 0;
    uint64_t q = vka_fence_queued();
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &V.tl, .pValues = &q};
    VkResult r = q ? vk.vkWaitSemaphores(V.dev, &wi, vka_hang_ns()) : VK_SUCCESS;
    if (r) { VRED("queue idle: fence %llu not reached in %.1f s (%d): not waited for", (unsigned long long)q, vka_hang_ns() / 1e9, r); return 0; }
    pthread_mutex_lock(&V.qmu); vk.vkQueueWaitIdle(V.q); pthread_mutex_unlock(&V.qmu);
    return 1;
}
uint64_t vka_fence_queued(void) { return __atomic_load_n(&V.tl_queued, __ATOMIC_ACQUIRE); }

// ---------- Fences ----------
// Fence n is reserved by the submission that signals timeline value n. The completion thread waits for the timeline, then runs that submission's CPU-side effects in order
// and only then publishes n as done: a SyncWait that returns has seen the events and counters of everything before it, as nvn_mtl.m's post_complete does
enum { POST_RING = 4096 };
typedef struct { uint64_t fence; void *ops; int n; void (*run)(void *, int); } post_rec;
static post_rec g_post[POST_RING];
typedef struct after_rec { struct after_rec *next; uint64_t fence; void (*fn)(void *); void *arg; } after_rec;
static after_rec *g_after;
static uint64_t g_fence, g_submitted, g_done;
static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fcv = PTHREAD_COND_INITIALIZER, g_scv = PTHREAD_COND_INITIALIZER;

uint64_t vka_fence_last(void) { pthread_mutex_lock(&g_fmu); uint64_t f = g_fence; pthread_mutex_unlock(&g_fmu); return f; }
uint64_t vka_fence_next(void) { pthread_mutex_lock(&g_fmu); uint64_t f = ++g_fence; pthread_mutex_unlock(&g_fmu); return f; }
void vka_fence_post(uint64_t fence, void *ops, int n, void (*run)(void *, int)) {
    pthread_mutex_lock(&g_fmu);
    while (g_post[fence % POST_RING].fence && g_post[fence % POST_RING].fence != fence) pthread_cond_wait(&g_fcv, &g_fmu);   // that slot's earlier fence has not completed yet
    g_post[fence % POST_RING] = (post_rec){fence, ops, n, run};
    if (fence > g_submitted) g_submitted = fence;
    pthread_cond_broadcast(&g_scv); pthread_mutex_unlock(&g_fmu);
}
void vka_fence_cpu(uint64_t fence, void *ops, int n, void (*run)(void *, int)) {
    if (run) run(ops, n); else free(ops);
    pthread_mutex_lock(&g_fmu);
    if (fence > g_submitted) g_submitted = fence;
    if (fence > g_done) __atomic_store_n(&g_done, fence, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_fcv); pthread_mutex_unlock(&g_fmu);
}
uint64_t vka_done(void) { return __atomic_load_n(&g_done, __ATOMIC_ACQUIRE); }
int vka_wait_done(uint64_t fence, uint64_t timeout_ns) {
    if (vka_done() >= fence) return 1;
    if (!timeout_ns) return 0;
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000ull); dl.tv_nsec = (long)(ns % 1000000000ull);
    pthread_mutex_lock(&g_fmu);
    while (g_done < fence) if (pthread_cond_timedwait(&g_fcv, &g_fmu, &dl)) break;
    int ok = g_done >= fence; pthread_mutex_unlock(&g_fmu);
    return ok;
}
void vka_after(uint64_t fence, void (*fn)(void *), void *arg) {
    pthread_mutex_lock(&g_fmu);
    if (g_done >= fence) { pthread_mutex_unlock(&g_fmu); fn(arg); return; }
    after_rec *a = malloc(sizeof *a); *a = (after_rec){g_after, fence, fn, arg}; g_after = a;
    pthread_mutex_unlock(&g_fmu);
}
// ---------- GPU hangs ----------
// A fence that makes no progress for DM_VK_HANG_S seconds (default 6) is a hang: the GPU, or the driver's queue, will not get there. Then nothing more is submitted (mtl_sub_begin
// records no command buffer, vka_submit refuses, the present drops its frames), every fence reads as done with its CPU-side effects run, pool memory the GPU may still read is
// kept instead of freed, and the host exits (dm_host_shutdown) unless DM_VK_HANG_EXIT=0. Before this, the completion thread logged the stall every 2 s and went on waiting, a pool
// given back was freed anyway after 2 s, and the present waited on the queue: on the Adreno 840 phone the system server restarted (a soft reboot) a few seconds later
static _Atomic int g_hung;
int vka_hung(void) { return atomic_load_explicit(&g_hung, memory_order_acquire); }
uint64_t vka_hang_ns(void) {
    double s = VKA_ENVF("DM_VK_HANG_S", 6.0); if (!(s > 0.5)) s = 6.0;
    return (uint64_t)(s * 1e9);
}
static void *hang_exit_main(void *arg) { (void)arg; pthread_setname_np(pthread_self(), "vk-hang"); dm_host_shutdown("GPU hang"); return NULL; }
void vka_exec_hang_report(uint64_t fence);   // vka_exec.c: what the stuck submission held
static void hang_declare(uint64_t want, uint64_t at, double s, const char *what) {
    if (atomic_exchange(&g_hung, 1)) return;
    VRED("GPU hang: %s; fence %llu not reached in %.1f s (the timeline at %llu, %llu handed to the queue, %llu reserved): nothing more is submitted, every fence reads as done",
         what, (unsigned long long)want, s, (unsigned long long)at, (unsigned long long)vka_fence_queued(), (unsigned long long)vka_fence_last());
    vka_exec_hang_report(at + 1);
    if (!VKA_ENV("DM_VK_HANG_EXIT", 1)) { VRED("GPU hang: DM_VK_HANG_EXIT=0, the game runs on without the GPU"); return; }
    VRED("GPU hang: the host exits (DM_VK_HANG_EXIT=0 runs on without the GPU)");
    pthread_t th; if (!pthread_create(&th, NULL, hang_exit_main, NULL)) pthread_detach(th);
}

static void *completion_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-complete"); vka_fp_host("vk-complete"); void vka_perf_thread(int nice); vka_perf_thread(-8);   // vka_perf.c
    uint64_t stall_t0 = 0, stall_at = 0; int stall_logged = 0;
    for (;;) {
        pthread_mutex_lock(&g_fmu);
        while (g_done >= g_submitted) pthread_cond_wait(&g_scv, &g_fmu);
        // Wait for the next fence, not the last one handed to the queue: each fence is published (and its waiters woken) when the GPU reaches it. Waiting for the last one
        // held every earlier fence back until all work queued at that moment had run: Donkey Kong Bananza waits for its previous frame's last fence after queueing two
        // submissions of the next frame, so its wait ended only when those were done too. DM_VK_DONE_LAST=1 (live) waits for the last one, as before, for A/B runs
        uint64_t sub = g_submitted, done = g_done, want = VKA_ENV("DM_VK_DONE_LAST", 0) ? sub : done + 1; pthread_mutex_unlock(&g_fmu);
        uint64_t v = 0;
        if (!vka_hung()) {
            VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &V.tl, .pValues = &want};
            VkResult r = vk.vkWaitSemaphores(V.dev, &wi, 500000000ull);
            vk.vkGetSemaphoreCounterValue(V.dev, V.tl, &v);
            uint64_t t = now_ns();
            if (r == VK_TIMEOUT && v <= done) {   // no progress in this wait
                if (!stall_t0 || stall_at != done) { stall_t0 = t - 500000000ull; stall_at = done; stall_logged = 0; }
                double st = (t - stall_t0) / 1e9;
                if (st >= 2 && !stall_logged) { stall_logged = 1; VRED("GPU: fence %llu not reached after %.1f s (at %llu, %llu handed to the queue)", (unsigned long long)want, st, (unsigned long long)v, (unsigned long long)vka_fence_queued()); }
                if (t - stall_t0 < vka_hang_ns()) continue;
                hang_declare(want, v, st, "no progress");
            } else if (r && r != VK_TIMEOUT) {
                char w[64]; snprintf(w, sizeof w, "vkWaitSemaphores %d (the device is lost)", r);
                hang_declare(want, v > done ? v : done, 0, w);   // (a lost device reads its timeline as 0: the last value published done instead)
            } else stall_t0 = 0;
        }
        if (vka_hung()) v = sub;   // every fence submitted so far is taken as done: its CPU-side effects run, the guest's waits return
        if (v > sub) v = sub;
        for (uint64_t f = done + 1; f <= v; f++) {
            post_rec *p = &g_post[f % POST_RING];
            if (p->fence == f) { if (p->run) p->run(p->ops, p->n); else free(p->ops); }
        }
        pthread_mutex_lock(&g_fmu);
        for (uint64_t f = done + 1; f <= v; f++) if (g_post[f % POST_RING].fence == f) g_post[f % POST_RING].fence = 0;
        if (v > g_done) __atomic_store_n(&g_done, v, __ATOMIC_RELEASE);
        after_rec *run = NULL, **pp = &g_after;
        while (*pp) { after_rec *a = *pp; if (a->fence <= g_done) { *pp = a->next; a->next = run; run = a; } else pp = &a->next; }
        pthread_cond_broadcast(&g_fcv); pthread_mutex_unlock(&g_fmu);
        while (run) { after_rec *a = run; run = a->next; a->fn(a->arg); free(a); }
    }
    return NULL;
}

// ---------- Scratch memory ----------
// Blocks of host-visible memory handed out linearly. Two owners: the submission being recorded (class 0: the blocks retire at its fence, vka_scratch_retire) and the
// open upload batch (class 1, while vka_scr_up is set on the calling thread: the blocks retire at the batch's value on the upload timeline, vka_up_flush). A block goes
// back to use once that value is reached
typedef struct sblk { struct sblk *next; VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size, used; uint64_t fence; int up; } sblk;
static sblk *g_open[2], *g_busy;
static _Atomic uint64_t g_scratch_bytes; static _Atomic uint32_t g_scratch_n;
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
_Thread_local int vka_scr_up;
static VkSemaphore g_utl;   // the upload timeline: value n = upload batch n has completed
static VkDeviceSize g_ubytes;   // staging taken by the open upload batch (under g_smu)
static uint64_t up_done(void) {   // the upload timeline's value (every batch counts as done after a GPU hang)
    if (vka_hung()) return UINT64_MAX;
    uint64_t v = 0; if (g_utl) vk.vkGetSemaphoreCounterValue(V.dev, g_utl, &v); return v;
}
static int sblk_idle(const sblk *b, uint64_t done, uint64_t udone) { return b->fence <= (b->up ? udone : done); }
static sblk *sblk_new(VkDeviceSize n) {
    sblk *b = calloc(1, sizeof *b); b->size = n;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};   // indirect: the {count, 1, 1} copies of indirect mesh draws (vka_exec.c mdx_copy)
    if (vk.vkCreateBuffer(V.dev, &bi, NULL, &b->buf)) { free(b); return NULL; }
    VkMemoryRequirements mr; vk.vkGetBufferMemoryRequirements(V.dev, b->buf, &mr);
    uint32_t t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (t == UINT32_MAX) t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    void *p = NULL;
    if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &b->mem) || vk.vkBindBufferMemory(V.dev, b->buf, b->mem, 0) || vk.vkMapMemory(V.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &p)) {
        VRED("scratch block of %llu bytes not allocated", (unsigned long long)n); if (b->mem) vk.vkFreeMemory(V.dev, b->mem, NULL); vk.vkDestroyBuffer(V.dev, b->buf, NULL); free(b); return NULL; }
    b->ptr = p; atomic_fetch_add(&g_scratch_bytes, n); atomic_fetch_add(&g_scratch_n, 1); vka_mem_grew(n); return b;
}
uint64_t vka_scratch_bytes(uint32_t *blocks) { if (blocks) *blocks = atomic_load(&g_scratch_n); return atomic_load(&g_scratch_bytes); }
void vka_mem_grew(uint64_t bytes) {
    static _Atomic uint64_t acc, step = 1;
    uint64_t s = atomic_load_explicit(&step, memory_order_relaxed);
    if (s == 1) { const char *e = getenv("DM_VK_MEMLOG"); s = (e ? strtoull(e, NULL, 10) : 512) << 20; atomic_store_explicit(&step, s, memory_order_relaxed); }
    if (!s || atomic_fetch_add(&acc, bytes) + bytes < s) return;
    atomic_store(&acc, 0);
    int ni; uint32_t ns; uint64_t img = vka_tex_mem_nolock(&ni), scr = vka_scratch_bytes(&ns), pl = vka_mem_placed_nolock(), sp = vks_bytes_nolock();
    VLOG("memory: placed pools %.0f MiB, images %.0f MiB (%d), sparse tile blocks %.0f MiB, scratch %.0f MiB (%u blocks): %.0f MiB", pl / 1048576.0, img / 1048576.0, ni, sp / 1048576.0,
         scr / 1048576.0, ns, (pl + img + sp + scr) / 1048576.0);
}
// Idle blocks (their value reached) larger than the standard 16 MiB, or beyond 64 MiB of idle blocks in all, are freed (under g_smu): at every miss and at every retire.
// Kept, every block stayed for the process's life: Mario Kart World 1.8.0's boot uploads (levels of up to 144 MiB, each in a block of its size) left 1.4 GB of scratch
// before its first frame, and the phone's low-memory killer stopped it
static void sblk_trim(uint64_t done, uint64_t udone) {
    VkDeviceSize idle = 0;
    for (sblk **pp = &g_busy; *pp;) {
        sblk *b = *pp;
        int id = sblk_idle(b, done, udone);
        if (id && (b->size > (16u << 20) || idle + b->size > (64u << 20))) {
            *pp = b->next; vk.vkUnmapMemory(V.dev, b->mem); vk.vkDestroyBuffer(V.dev, b->buf, NULL); vk.vkFreeMemory(V.dev, b->mem, NULL);
            atomic_fetch_sub(&g_scratch_bytes, b->size); atomic_fetch_sub(&g_scratch_n, 1); free(b);
        } else { if (id) idle += b->size; pp = &b->next; }
    }
}
int vka_scratch(VkDeviceSize n, VkDeviceSize align, vka_span *out) {
    if (!align) align = 16;
    int c = vka_scr_up ? 1 : 0;
    pthread_mutex_lock(&g_smu);
    sblk *b = g_open[c];
    VkDeviceSize o = b ? (b->used + align - 1) / align * align : 0;
    if (!b || o + n > b->size) {
        // Best fit among the idle blocks (a 144 MiB block kept for a 1 KB request stays alive), then the idle ones beyond the budget freed
        uint64_t done = vka_done(), udone = up_done(); sblk **best = NULL;
        for (sblk **pp = &g_busy; *pp; pp = &(*pp)->next) if ((*pp)->size >= n && sblk_idle(*pp, done, udone) && (!best || (*pp)->size < (*best)->size)) best = pp;
        b = NULL; if (best) { b = *best; *best = b->next; }
        sblk_trim(done, udone);
        if (!b) b = sblk_new(n > (16u << 20) ? (n + 0xFFFF) & ~(VkDeviceSize)0xFFFF : 16u << 20);
        if (!b) { pthread_mutex_unlock(&g_smu); return 0; }
        b->used = 0; b->up = c; b->next = g_open[c]; g_open[c] = b; o = 0;
    }
    b->used = o + n; *out = (vka_span){b->buf, b->ptr + o, o};
    if (c) g_ubytes += n;
    pthread_mutex_unlock(&g_smu);
    return 1;
}
static void retire(int c, uint64_t value) {
    pthread_mutex_lock(&g_smu);
    while (g_open[c]) { sblk *b = g_open[c]; g_open[c] = b->next; b->fence = value; b->next = g_busy; g_busy = b; }
    if (c) { g_ubytes = 0; sblk_trim(vka_done(), up_done()); }   // (the submission's blocks are trimmed at a miss only: a game that takes more than 16 MiB a frame would make and free them every frame)
    pthread_mutex_unlock(&g_smu);
}
void vka_scratch_retire(uint64_t fence) { retire(0, fence); }

// ---------- Upload batches ----------
// The first content of an image the replay makes (its upload from guest memory, or its clear) is recorded into a command buffer of its own, an upload batch, instead of
// the submission being recorded. The batch goes to the queue when its staging passes DM_VK_UPLOAD_BATCH_MB (live, default 64) and always before the replay's next part
// or submission goes (vka_exec.c: chunk, vka_rp_sub_commit), so the queue runs it before every command that uses the image; it ends with a full barrier, whose second
// scope covers every later submission. Its staging retires when the GPU reaches the batch's value on the upload timeline, not at the fence of the submission it was made
// in: one submission that makes hundreds of images (Mario Kart World 1.8.0 at boot: 29,000 images, 1.5 GB of staging in one submission) no longer holds all of it.
// When the batches in flight hold more than DM_VK_UPLOAD_INFLIGHT_MB (live, default 256) of staging, the replay thread waits for the oldest. Only a new image is safe here:
// nothing recorded earlier in the submission reads it (a re-upload of a live image stays in the submission, in order with the draws before it). DM_VK_UPLOAD_BATCH=0 (live)
// records first contents into the submission, as before
enum { NUP = 16 };
static struct { VkCommandBuffer cb; uint64_t val; } g_ub[NUP];
static VkCommandPool g_upool; static uint64_t g_uval; static int g_ui = -1; static unsigned g_unext;   // g_ui: the open batch's slot (-1: none); replay thread only
static uint64_t g_n_ubatch, g_n_uwait, g_uwait_ns;
static int up_wait(uint64_t v) {   // 1 = the upload timeline reached v
    if (up_done() >= v) return 1;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_utl, .pValues = &v};
    uint64_t t0 = now_ns(); VkResult r = vk.vkWaitSemaphores(V.dev, &wi, vka_hang_ns());
    g_n_uwait++; g_uwait_ns += now_ns() - t0;
    if (r && !vka_hung()) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) {
        if (r == VK_TIMEOUT) VRED("upload batch %llu not done in %.1f s", (unsigned long long)v, vka_hang_ns() / 1e9);
        else VRED("upload batch %llu: the wait failed after %.2f s (%d%s)", (unsigned long long)v, (now_ns() - t0) / 1e9, r, r == VK_ERROR_DEVICE_LOST ? ": the device is lost" : ""); } }
    return !r || vka_hung();
}
int vka_up_on(void) { return VKA_ENV("DM_VK_UPLOAD_BATCH", 1) != 0; }
VkCommandBuffer vka_up_begin(void) {
    if (g_ui < 0) {
        static int failed; if (failed) return VK_NULL_HANDLE;
        if (!g_upool) {
            VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = V.qf};
            VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = NUP};
            VkSemaphoreTypeCreateInfo st = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
            VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
            VkCommandBuffer cbs[NUP]; VkCommandPool p = VK_NULL_HANDLE;
            if (vk.vkCreateCommandPool(V.dev, &pi, NULL, &p) || ((ai.commandPool = p), vk.vkAllocateCommandBuffers(V.dev, &ai, cbs)) || vk.vkCreateSemaphore(V.dev, &sci, NULL, &g_utl)) {
                VRED_ONCE("upload batches not set up: first contents are recorded into the submission"); g_utl = VK_NULL_HANDLE; failed = 1; return VK_NULL_HANDLE; }
            for (int i = 0; i < NUP; i++) g_ub[i].cb = cbs[i];
            g_upool = p;
        }
        if (vka_hung()) return VK_NULL_HANDLE;
        unsigned k = g_unext % NUP;   // (16 slots: the backpressure below keeps them mostly done)
        if (g_ub[k].val && !up_wait(g_ub[k].val)) return VK_NULL_HANDLE;   // (its batch still runs: the caller records into the submission)
        g_unext++;
        vk.vkResetCommandBuffer(g_ub[k].cb, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if (vk.vkBeginCommandBuffer(g_ub[k].cb, &bi)) return VK_NULL_HANDLE;
        g_ui = (int)k;
    }
    vka_scr_up = 1;
    return g_ub[g_ui].cb;
}
void vka_up_end(void) { vka_scr_up = 0; }
static int is_batch(VkCommandBuffer cb) { for (int i = 0; i < NUP; i++) if (cb && g_ub[i].cb == cb) return 1; return 0; }
// No batch can be begun (a slot's batch still runs after the hang time): the upload goes on in the submission being recorded, with its staging (the batch before it was sent,
// so its image's first barrier and copies come before; an upload stopped half-way would leave a tracked image with part of its content for good)
static VkCommandBuffer up_fallback(void) { vka_scr_up = 0; return vka_rp_cb(); }
VkCommandBuffer vka_up_cur(VkCommandBuffer cb) {   // the open batch when cb was one (vka_up_point may have sent it), else cb
    if (!is_batch(cb)) return cb;
    if (g_ui >= 0) return g_ub[g_ui].cb;
    VkCommandBuffer n = vka_up_begin(); return n ? n : up_fallback();
}
VkCommandBuffer vka_up_point(VkCommandBuffer cb) {
    if (g_ui < 0 || cb != g_ub[g_ui].cb) return cb;
    pthread_mutex_lock(&g_smu); VkDeviceSize n = g_ubytes; pthread_mutex_unlock(&g_smu);
    double mb = VKA_ENVF("DM_VK_UPLOAD_BATCH_MB", 64.0);
    if ((double)n < mb * 1048576.0) return cb;
    vka_up_flush();
    VkCommandBuffer nb = vka_up_begin(); return nb ? nb : up_fallback();
}
void vka_up_flush(void) {
    if (g_ui < 0) return;
    unsigned k = (unsigned)g_ui; g_ui = -1;
    VkCommandBuffer cb = g_ub[k].cb;
    vka_barrier(cb);   // everything the batch wrote, for every later submission on the queue
    vka_xp_ts(cb, 2);
    VkResult r = vk.vkEndCommandBuffer(cb);
    uint64_t v = ++g_uval;
    VkSemaphore ws[1]; uint64_t wv[1]; VkPipelineStageFlags wst[1]; uint32_t nw = 0;   // the sparse binds queued before it (vks_queue_locked)
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &v};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ti, .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_utl};
    pthread_mutex_lock(&V.qmu);
    if (!r) {
        vks_queue_locked(&nw, ws, wv, wst);
        ti.waitSemaphoreValueCount = nw; ti.pWaitSemaphoreValues = wv; si.waitSemaphoreCount = nw; si.pWaitSemaphores = ws; si.pWaitDstStageMask = wst;
        r = vka_hung() ? VK_ERROR_DEVICE_LOST : vk.vkQueueSubmit(V.q, 1, &si, VK_NULL_HANDLE);
    }
    pthread_mutex_unlock(&V.qmu);
    // Nothing on the GPU will signal it: the host does, so later batches and the slot are not held; only once the batch before it is done (a host signal must be below
    // every value still pending on the queue)
    if (r && !vka_hung() && (v == 1 || up_wait(v - 1))) { VkSemaphoreSignalInfo ss = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = g_utl, .value = v}; vk.vkSignalSemaphore(V.dev, &ss); }
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("upload batch %llu: %d (its images keep what they held)", (unsigned long long)v, r); }
    g_ub[k].val = v; g_n_ubatch++;
    retire(1, v);
}
// Backpressure: the batches in flight hold at most DM_VK_UPLOAD_INFLIGHT_MB of staging (the oldest is waited for; the GPU runs them in queue order). Called where the
// replay thread holds no lock (vka_tex_get after the table lock, chunk, the commit): the present and other threads that take the image table do not wait for the GPU
void vka_up_throttle(void) {
    if (!g_utl) return;
    double cap = VKA_ENVF("DM_VK_UPLOAD_INFLIGHT_MB", 256.0) * 1048576.0;
    for (;;) {
        uint64_t udone = up_done(), oldest = UINT64_MAX; VkDeviceSize fl = 0;
        pthread_mutex_lock(&g_smu);
        for (sblk *b = g_busy; b; b = b->next) if (b->up && b->fence > udone) { fl += b->size; if (b->fence < oldest) oldest = b->fence; }
        pthread_mutex_unlock(&g_smu);
        if ((double)fl <= cap || oldest == UINT64_MAX || !up_wait(oldest)) break;
    }
}
void vka_up_stats(uint64_t *batches, uint64_t *waits, double *wait_ms) { if (batches) *batches = g_n_ubatch; if (waits) *waits = g_n_uwait; if (wait_ms) *wait_ms = g_uwait_ns / 1e6; }
#endif

#if defined(__ANDROID__)
// DM_VK_GPU_FLOOR=s (live; default 100): the queue's GPU clock floor through VK_QCOM_queue_perf_hint, s % of the highest clock (100: the highest; 0: the driver's default
// governor). The GPU's governor follows its busy time, and the replay keeps the GPU idle while it records the start of a frame, so it clocks down: the same replayed level
// frame took 21-28 ms of GPU time with the clock up and 93-99 ms after two seconds of idle GPU (Adreno 840), and in the live level the GPU finished 23-33 ms after the
// recording at the default governor, 6-10 ms with the floor at 100 (with DM_VK_CHUNK_PASS). The game's frame-rate controller measures frame start to GPU end, so that tail
// decides between its 20 and 30 fps: in one live session (CPU capped at 2.23 GHz) 85 % of the frames at 30 fps with the floor at 100, 0-16 % at 70 or with the
// driver's governor. The cost is power while a frame's GPU work runs; a lower floor or 0 gives it back
// A hint is the queue's state until changed; the kernel takes the highest of all queues' floors
// DM_VK_GPU_FLOOR_AUTO=1 (live; default 0 = the fixed floor): the floor follows what the frame needs, between 0 and DM_VK_GPU_FLOOR. Each frame's span from the start of the replay's
// recording of its first submission to the GPU end of its last (vka_exec.c, vka_floor_span: recording plus the GPU tail) is held against the period the frame aims at: the game's
// shortest period (vka_floor_period_ns), not the period it presents at now. Over the last 30 frames: the 90th percentile within 2 ms of the period raises the floor a step (within
// 0.5 ms: to DM_VK_GPU_FLOOR at once); more than 5 ms below it (DM_VK_GPU_FLOOR_LO; raise: DM_VK_GPU_FLOOR_HI, 2 ms) for 60 frames since the last change lowers it a step. Steps of 25 %
enum { FSPAN = 64 };
static _Atomic uint64_t g_fspan[FSPAN], g_fspan_f[FSPAN];   // per frame (mod FSPAN): its span in ns, and which frame it is
static uint64_t g_floor_frames[5];                            // frames at each step (statistics)
static double g_p90_sum; static uint64_t g_p90_n;
// The period the floor aims at, for every floor (the queue hint's DM_VK_GPU_FLOOR_AUTO=1 and the ADPF floor's reports, vka_perf.c): the game's shortest period, the shortest
// present period it has asked for in this session and at most one 60 Hz vsync (16.7 ms), not the period it presents at now. A game with a dynamic present interval decides
// its interval from how long its frames take: DKB's controller (main+0x204a18) steps from 3 vsyncs (20 fps) back to 2 only when its measure of the frame is below 20 ms. At
// 20 fps the GPU idles a third of the time, its governor lowers the clock, the frame's GPU time grows to 33-36 ms (Adreno 840) and the game stays at 20 fps for good; a floor
// aiming at the current period (50 ms, or the 33.3 ms cap the auto floor had) sees that frame on target and never raises the clock. Aiming at the shortest period keeps the
// clock where the game can step back up. DM_VK_GPU_FLOOR_PERIOD=<ms> (live; default 0 = the shortest period) sets it instead (33.3: the cap before 2026-11-05)
_Atomic int64_t vka_period_min_ns;   // the shortest present period the game asked for (vka_perf.c vka_perf_frame); 0 = none yet
int64_t vka_floor_period_ns(void) {
    double ms = VKA_ENVF("DM_VK_GPU_FLOOR_PERIOD", 0.0);
    if (ms >= 4 && ms <= 200) return (int64_t)(ms * 1e6);
    int64_t m = atomic_load_explicit(&vka_period_min_ns, memory_order_relaxed), v = 16666667;
    return m >= 4000000 && m < v ? m : v;
}
void vka_floor_span(uint64_t frame, uint64_t ns) {           // the completion thread: a submission of frame ended ns after the frame began
    unsigned k = frame % FSPAN;
    if (atomic_load(&g_fspan_f[k]) != frame) { atomic_store(&g_fspan[k], 0); atomic_store(&g_fspan_f[k], frame); }
    if (ns > atomic_load(&g_fspan[k])) atomic_store(&g_fspan[k], ns);
}
uint64_t vka_floor_span_of(uint64_t frame) { unsigned k = frame % FSPAN; return atomic_load(&g_fspan_f[k]) == frame ? atomic_load(&g_fspan[k]) : 0; }   // 0 = not in (vka_perf.c DM_VK_ADPF_SPAN)
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static int floor_auto(int top, uint64_t f) {
    static int cur = -1; static uint64_t last_f, changed_f;
    if (cur < 0 || cur > top) cur = top;
    if (f == last_f) return cur;
    last_f = f;
    uint64_t v[30]; int n = 0;
    for (uint64_t g = f > 3 ? f - 3 : 0; g > 0 && g + 34 > f && n < 30; g--) { unsigned k = g % FSPAN; if (atomic_load(&g_fspan_f[k]) == g && atomic_load(&g_fspan[k])) v[n++] = atomic_load(&g_fspan[k]); }
    if (n < 20) return cur;
    qsort(v, n, sizeof *v, cmp_u64);
    double p90 = v[n * 9 / 10] / 1e6, per = vka_floor_period_ns() / 1e6;
    int was = cur;
    g_p90_sum += p90; g_p90_n++;
    double hi = VKA_ENVF("DM_VK_GPU_FLOOR_HI", 2.0), lo = VKA_ENVF("DM_VK_GPU_FLOOR_LO", 5.0);   // ms below the period: raise above per - hi, lower below per - lo
    // A step down that has to be taken back within 60 frames is not tried again for a while (300 frames, doubling up to 4,800 each time it fails again): on the Adreno
    // 840 a floor of 75 instead of 100 added 5-15 ms to the level's span at once, and the controller went up and down every few frames
    static uint64_t hold_until, backoff = 300, lowered_f;
    if (p90 > per - 0.5) cur = top;
    else if (p90 > per - hi) cur = cur + 25 > top ? top : cur + 25;
    else if (p90 < per - lo && f - changed_f >= 60 && f >= hold_until) { cur = cur - 25 < 0 ? 0 : cur - 25; lowered_f = f; }
    if (cur > was && lowered_f && f - lowered_f < 60) { hold_until = f + backoff; backoff = backoff * 2 > 4800 ? 4800 : backoff * 2; lowered_f = 0; }
    if (lowered_f && f - lowered_f > 600) { backoff = 300; lowered_f = 0; }   // a step down that held
    if (cur != was) { changed_f = f; if (VKA_ENV("DM_VK_GPU_FLOOR_LOG", 0)) VLOG("GPU clock floor %d -> %d %%: frame %llu, span p90 %.1f ms of %.1f", was, cur, (unsigned long long)f, p90, per); }
    g_floor_frames[cur / 25 < 5 ? cur / 25 : 4]++;
    return cur;
}
void vka_floor_stats(char *b, size_t n) {   // the replay statistics: frames at each floor step since the last call
    uint64_t t = 0; for (int i = 0; i < 5; i++) t += g_floor_frames[i];
    if (!t) { if (n) b[0] = 0; return; }
    snprintf(b, n, "GPU clock floor (auto): frames at 0/25/50/75/100 %%: %.0f/%.0f/%.0f/%.0f/%.0f %%; recording start to GPU end p90 %.1f ms on average", 100.0 * g_floor_frames[0] / t, 100.0 * g_floor_frames[1] / t, 100.0 * g_floor_frames[2] / t,
             100.0 * g_floor_frames[3] / t, 100.0 * g_floor_frames[4] / t, g_p90_n ? g_p90_sum / g_p90_n : 0.0);
    g_p90_sum = 0; g_p90_n = 0;
    memset(g_floor_frames, 0, sizeof g_floor_frames);
}
// The floor through the kernel driver (KGSL) for a Vulkan driver without VK_QCOM_queue_perf_hint on an Adreno (Turnip): the power constraint of the queue's own kernel context.
// Qualcomm's hint maps onto the same kernel call: IOCTL_KGSL_SETPROPERTY KGSL_PROP_PWR_CONSTRAINT {KGSL_CONSTRAINT_PWRLEVEL, context, level} (graphics-kernel adreno.c
// adreno_set_constraint), level 1 = the highest clock or 2-100 = a floor at that % of it, the hint's two kinds. On GPUs whose clock the GMU decides (gen8, the Adreno 840:
// not host_based_dcvs) the kernel hands it to the GMU at once as the context's rule (adreno_gen8_hwsched.c gen8_hwsched_set_pwrconstraint, H2F_MSG_CONTEXT_RULE), which holds
// the clock while that context's work runs; on host-DCVS GPUs (older) it applies at dispatch of submissions flagged KGSL_CONTEXT_PWR_CONSTRAINT and there only "highest" (1)
// is taken. The rule only reaches the GMU while the context is registered with it (after its first submission; again after the GPU slept), and Turnip does not create its
// context with that flag, so the level is sent again every 30 frames. Which contexts: the kernel lets a file descriptor set the constraint only on contexts it owns
// (kgsl_context_get_owner), so the contexts the driver made for the device's queue are the ids this process's /dev/kgsl-3d0 descriptors own after vkCreateDevice and did
// not own before (Turnip: one per VkQueue, tu_knl_kgsl.cc kgsl_submitqueue_new). The ownership test is the same ioctl with KGSL_CONSTRAINT_NONE, which changes nothing on a
// context without a constraint. Before 2026-11-05 Turnip's floor was the ADPF session (vka_perf.c), which did not raise the GPU clock: 282-646 MHz (mean 380) in DKB's
// level where the system driver ran 1,100-1,200 MHz. DM_VK_GPU_FLOOR_KGSL=0 (restart) leaves it off (the ADPF floor then); DM_VK_GPU_FLOOR and DM_VK_GPU_FLOOR_AUTO work as
// with the queue hint
#include <fcntl.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
struct kgsl_dc { unsigned int type, context_id; void *data; size_t size; };
struct kgsl_gp { unsigned int type; void *value; size_t sizebytes; };
#define KGSL_IOCTL_SETPROPERTY _IOW(0x09, 0x32, struct kgsl_gp)
enum { KG_FDS = 4, KG_IDS = 1024, KG_CTX = 8 };
static int g_kg_fd[KG_FDS], g_kg_nfd; static uint8_t g_kg_own[KG_FDS][KG_IDS / 8];
static struct { int fd; unsigned id; } g_kg[KG_CTX]; static int g_kg_n, g_kg_host;   // the queue's contexts; host DCVS: only "highest"
static int kg_set(int fd, unsigned id, int type, unsigned level) {
    unsigned lv = level; struct kgsl_dc c = {(unsigned)type, id, &lv, sizeof lv}; struct kgsl_gp g = {0x12, &c, sizeof c};   // KGSL_PROP_PWR_CONSTRAINT
    int r; do r = ioctl(fd, KGSL_IOCTL_SETPROPERTY, &g); while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r < 0 ? -errno : 0;
}
static void kg_scan(int keep_fds) {   // this process's descriptors on /dev/kgsl-3d0 and the context ids each owns
    if (!keep_fds) {
        g_kg_nfd = 0; DIR *d = opendir("/proc/self/fd"); if (!d) return;
        struct dirent *de; char path[64], to[64];
        while ((de = readdir(d)) && g_kg_nfd < KG_FDS) {
            int fd = atoi(de->d_name); if (fd <= 2) continue;
            snprintf(path, sizeof path, "/proc/self/fd/%d", fd); ssize_t n = readlink(path, to, sizeof to - 1); if (n <= 0) continue; to[n] = 0;
            if (!strncmp(to, "/dev/kgsl-3d", 12)) g_kg_fd[g_kg_nfd++] = fd;
        }
        closedir(d);
    }
    for (int i = 0; i < g_kg_nfd; i++) { memset(g_kg_own[i], 0, sizeof g_kg_own[i]); for (unsigned id = 1; id < KG_IDS; id++) if (!kg_set(g_kg_fd[i], id, 0, 0)) g_kg_own[i][id / 8] |= (uint8_t)(1u << (id % 8)); }
}
static uint8_t g_kg_was[KG_FDS][KG_IDS / 8]; static int g_kg_wasfd[KG_FDS], g_kg_wasn, g_kg_tried;
void vka_kgsl_before(int has_ext) {   // (not with the queue hint's extension: its driver's contexts are left alone)
    g_kg_tried = !has_ext && VKA_ENV("DM_VK_GPU_FLOOR_KGSL", 1) != 0;
    if (!g_kg_tried) return;
    kg_scan(0); g_kg_wasn = g_kg_nfd; memcpy(g_kg_wasfd, g_kg_fd, sizeof g_kg_fd); memcpy(g_kg_was, g_kg_own, sizeof g_kg_own);
}
void vka_kgsl_after(int has_hint) {
    if (has_hint) return;   // the queue hint does it
    if (!g_kg_tried) { VLOG("GPU clock floor through KGSL: off (DM_VK_GPU_FLOOR_KGSL=0, or the queue hint's extension without its feature)"); return; }
    kg_scan(0);
    g_kg_n = 0;
    for (int i = 0; i < g_kg_nfd; i++) {
        int w = -1; for (int j = 0; j < g_kg_wasn; j++) if (g_kg_wasfd[j] == g_kg_fd[i]) w = j;
        for (unsigned id = 1; id < KG_IDS && g_kg_n < KG_CTX; id++)
            if ((g_kg_own[i][id / 8] >> (id % 8) & 1) && !(w >= 0 && (g_kg_was[w][id / 8] >> (id % 8) & 1))) { g_kg[g_kg_n].fd = g_kg_fd[i]; g_kg[g_kg_n].id = id; g_kg_n++; }
    }
    if (g_kg_n) {   // the level form the kernel takes: 2-100 % with GMU DCVS, else -EINVAL and only 1 (highest)
        int r = kg_set(g_kg[0].fd, g_kg[0].id, 1, 50); g_kg_host = r == -EINVAL; kg_set(g_kg[0].fd, g_kg[0].id, 0, 0);
    }
    char b[96]; size_t o = 0; b[0] = 0; for (int k = 0; k < g_kg_n && o < sizeof b; k++) o += (size_t)snprintf(b + o, sizeof b - o, "%s%u", k ? "," : "", g_kg[k].id);
    if (g_kg_n) VLOG("GPU clock floor through KGSL: the queue's kernel context%s %s (%d descriptor%s on /dev/kgsl-3d0; %s), DM_VK_GPU_FLOOR (DM_VK_GPU_FLOOR_KGSL)",
                     g_kg_n > 1 ? "s" : "", b, g_kg_nfd, g_kg_nfd == 1 ? "" : "s", g_kg_host ? "host DCVS: highest clock only" : "GMU DCVS: floor in % of the highest clock");
    else VLOG("GPU clock floor through KGSL: %s", g_kg_nfd ? "no kernel context of the device's own (the ADPF floor)" : "no /dev/kgsl-3d0 descriptor (not an Adreno with KGSL; the ADPF floor)");
}
int vka_floor_kgsl(void) { return !V.perf_hint && g_kg_n > 0; }
static VkResult kg_apply(int s) {   // the floor s % (0: none) on the queue's contexts
    int bad = 0;
    for (int k = 0; k < g_kg_n; k++) {
        int r = s <= 0 ? kg_set(g_kg[k].fd, g_kg[k].id, 0, 0) : kg_set(g_kg[k].fd, g_kg[k].id, 1, s >= 100 || g_kg_host ? 1u : (unsigned)(s < 2 ? 2 : s));
        if (r) { bad = r; static int n; if (n++ < 3) VRED("KGSL power constraint %d %% on context %u: %d", s, g_kg[k].id, r); }
    }
    return bad ? VK_ERROR_UNKNOWN : VK_SUCCESS;
}
static _Atomic int g_tail_n;   // DM_VK_GPU_FLOOR_AUTO=2: submissions with draws counted at their commit and not yet completed
// The queue's hint (any thread: the replay's, the completion thread's). tail: the floor is s while a counted submission is pending, else 0, decided under the queue lock, so
// a commit and a completion racing cannot apply their floors in the wrong order (before 2026-11-03 a completion's 0 could land after the next commit's floor)
static _Atomic int g_floor_cur = -1, g_floor_res;   // the floor last given to the queue and the driver's or kernel's answer (the pace line, vka_perf.c)
// The GPU clock floor for the pace line: its way ("queue hint", "KGSL", "ADPF", "off"), the % last given (-1: none yet) and that call's result (0 = taken)
const char *vka_floor_state(int *pct, int *res) {
    int vka_floor_adpf(void);
    if (pct) *pct = atomic_load(&g_floor_cur);
    if (res) *res = atomic_load(&g_floor_res);
    return V.perf_hint ? "queue hint" : g_kg_n ? "KGSL" : vka_floor_adpf() ? "ADPF" : "off";
}
static void floor_set_(int s, int tail, int again) {   // again: send it even if unchanged (the KGSL rule, see kg_apply)
    static _Atomic int cur = -1;
    if (!tail && !again && atomic_load(&cur) == s) return;
    pthread_mutex_lock(&V.qmu);
    if (tail) s = atomic_load(&g_tail_n) > 0 ? s : 0;
    if (!again && atomic_load(&cur) == s) { pthread_mutex_unlock(&V.qmu); return; }
    VkResult r;
    if (V.perf_hint) {
        struct { VkStructureType sType; void *pNext; uint32_t type, scale; } h = {(VkStructureType)1000302000, NULL, s >= 100 ? 2u : s > 0 ? 3u : 0u, s > 0 && s < 100 ? (uint32_t)s : 0u};
        r = ((VkResult (*)(VkQueue, const void *))V.perf_hint)(V.q, &h);
    } else r = kg_apply(s);
    int was = atomic_exchange(&cur, s); atomic_store(&g_floor_cur, s); atomic_store(&g_floor_res, (int)r);
    pthread_mutex_unlock(&V.qmu);
    if (was < 0 || (was != s && VKA_ENV("DM_VK_GPU_FLOOR_LOG", 0) > 1)) VLOG("GPU clock floor%s: %d %% (%s): %d", V.perf_hint ? "" : " (KGSL)", s, s >= 100 ? "highest" : s ? "scaled" : "default governor", r);
}
// DM_VK_GPU_FLOOR_AUTO=2: the floor only for the tail. The driver's governor keeps the clock while the frame is recorded (its parts reach the GPU as fast as they are
// recorded, so a faster GPU only waits longer); when a submission with draws is handed to the queue the floor goes to DM_VK_GPU_FLOOR, and back to the governor once
// every such submission handed over has completed. A submission is counted when its span op is posted (vka_floor_tail_add) and the count is given back by that op on
// completion, so the two always pair: before 2026-11-03 every completed submission with draws took one off, also those committed before a live switch to mode 2 or whose
// end failed, and the floor could drop with a counted submission still on the GPU
static int floor_mode(void) { return V.perf_hint || g_kg_n ? VKA_ENV("DM_VK_GPU_FLOOR_AUTO", 0) : -1; }
static int floor_top(void) { int s = VKA_ENV("DM_VK_GPU_FLOOR", 100); return s < 0 ? 0 : s > 100 ? 100 : s; }
// The floor without VK_QCOM_queue_perf_hint and without the KGSL constraint above (a driver that is not on KGSL, or DM_VK_GPU_FLOOR_KGSL=0): the ADPF session (vka_perf.c) reports each frame's span (recording start to the GPU end of
// its last submission) with its GPU time against the shortest period, so the system raises the clocks the frame's work runs on when the frame takes longer than the period
// the game could run at. Before 2026-11-05 there was no floor at all there: on Turnip DKB stayed at 20 fps with 33-36 ms of GPU time a frame, as the system driver did with
// DM_VK_GPU_FLOOR=0. It has no level: DM_VK_GPU_FLOOR > 0 turns it on, 0 off, as does DM_VK_GPU_FLOOR_ADPF=0 (live); DM_VK_ADPF_SPAN set explicitly decides the reports. On the Adreno 840 with Turnip it did not raise the GPU clock (282-646 MHz, mean 380; 2026-11-05)
int vka_floor_adpf(void) { return !V.perf_hint && !g_kg_n && floor_top() > 0 && VKA_ENV("DM_VK_GPU_FLOOR_ADPF", 1); }
int vka_floor_tail_add(void) { if (floor_mode() != 2) return 0; atomic_fetch_add(&g_tail_n, 1); return 1; }   // vka_exec.c, a submission with draws: 1 = counted
void vka_floor_commit(int counted) { if (counted) floor_set_(floor_top(), 1, 0); }   // before that submission is handed to the queue
void vka_floor_done(int counted) {     // the completion thread, that submission done
    if (!counted) return;
    atomic_fetch_sub(&g_tail_n, 1);
    if (floor_mode() == 2) floor_set_(floor_top(), 1, 0);
}
void vka_gpu_floor(void) {
    int mode = floor_mode(); if (mode < 0 || mode == 2) return;
    int s = VKA_ENV("DM_VK_GPU_FLOOR", 100); if (s < 0) s = 0; if (s > 100) s = 100;
    uint64_t vka_frame(void);
    uint64_t f = vka_frame();
    if (mode == 1) s = floor_auto(s, f);
    static uint64_t sent_f;   // the KGSL rule again every 30 frames (it is lost when the GMU registers the context again)
    int again = !V.perf_hint && (f >= sent_f + 30 || f < sent_f);
    if (again) sent_f = f;
    floor_set_(s, 0, again);
}
#endif
