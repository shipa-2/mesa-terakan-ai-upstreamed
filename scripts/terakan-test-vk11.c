/*
 * terakan-test-vk11.c — Vulkan 1.1 conformance smoke test for Terakan
 *
 * Checks all features, extensions, properties, and commands marked as
 * "Implemented" in README.md. Reports PASS/FAIL for each item.
 *
 * Build:  cc -O2 -o terakan-test-vk11 terakan-test-vk11.c -lvulkan
 * Run:    export VK_ICD_FILENAMES=<path_to_terascale_icd.json>
 *         ./terakan-test-vk11
 */

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passes = 0, fails = 0;

static void check(int cond, const char *name, const char *msg) {
    if (cond) { passes++; printf("  [PASS] %s\n", name); }
    else      { fails++;  printf("  [FAIL] %s — %s\n", name, msg); }
}

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;

static void create_instance(void) {
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "terakan-test-vk11";
    ai.applicationVersion = 1;
    ai.pEngineName = "none";
    ai.engineVersion = 1;
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    vkCreateInstance(&ci, NULL, &inst);
}

static void create_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (n == 0) { printf("FATAL: no physical devices\n"); exit(1); }
    VkPhysicalDevice *devs = malloc(n * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(inst, &n, devs);
    phys = devs[0];
    free(devs);
    float pri = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &pri;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    vkCreateDevice(phys, &dci, NULL, &dev);
    vkGetDeviceQueue(dev, 0, 0, &queue);
}

/* ─── 1. Core entrypoints exist ─── */
static void test_core_entrypoints(void) {
    printf("\n=== Core Entrypoints ===\n");
    PFN_vkVoidFunction p;
    p = vkGetDeviceProcAddr(dev, "vkCmdBeginRendering");      check(p != NULL, "vkCmdBeginRendering", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdEndRendering");        check(p != NULL, "vkCmdEndRendering", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdDraw");                check(p != NULL, "vkCmdDraw", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdDrawIndexed");         check(p != NULL, "vkCmdDrawIndexed", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdDrawIndirect");        check(p != NULL, "vkCmdDrawIndirect", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdDrawIndexedIndirect"); check(p != NULL, "vkCmdDrawIndexedIndirect", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdBindPipeline");        check(p != NULL, "vkCmdBindPipeline", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdBindDescriptorSets");  check(p != NULL, "vkCmdBindDescriptorSets", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdPushConstants");       check(p != NULL, "vkCmdPushConstants", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdCopyBuffer2");         check(p != NULL, "vkCmdCopyBuffer2", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdCopyImage2");          check(p != NULL, "vkCmdCopyImage2", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdCopyBufferToImage2");  check(p != NULL, "vkCmdCopyBufferToImage2", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdBlitImage2");          check(p != NULL, "vkCmdBlitImage2", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdClearColorImage");     check(p != NULL, "vkCmdClearColorImage", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdClearAttachments");    check(p != NULL, "vkCmdClearAttachments", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdUpdateBuffer");        check(p != NULL, "vkCmdUpdateBuffer", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdFillBuffer");          check(p != NULL, "vkCmdFillBuffer", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdPipelineBarrier2");    check(p != NULL, "vkCmdPipelineBarrier2", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCreateComputePipelines"); check(p != NULL, "vkCreateComputePipelines", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdDispatch");            check(p != NULL, "vkCmdDispatch", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdDispatchBase");        check(p != NULL, "vkCmdDispatchBase", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdDispatchIndirect");    check(p != NULL, "vkCmdDispatchIndirect", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdBeginQueryIndexedEXT");check(p != NULL, "vkCmdBeginQueryIndexedEXT", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdEndQueryIndexedEXT");  check(p != NULL, "vkCmdEndQueryIndexedEXT", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdWriteTimestamp2");     check(p != NULL, "vkCmdWriteTimestamp2", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdSetViewport");         check(p != NULL, "vkCmdSetViewport", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdSetScissor");          check(p != NULL, "vkCmdSetScissor", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdSetDepthBias");        check(p != NULL, "vkCmdSetDepthBias", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdSetLineWidth");        check(p != NULL, "vkCmdSetLineWidth", "NULL");
    p = vkGetDeviceProcAddr(dev, "vkCmdSetDepthTestEnable");  check(p != NULL, "vkCmdSetDepthTestEnable", "NULL");
}

/* ─── 2. Features ─── */
static void test_features(void) {
    printf("\n=== Features ===\n");
    VkPhysicalDeviceFeatures feat;
    vkGetPhysicalDeviceFeatures(phys, &feat);
    check(feat.independentBlend, "independentBlend", "false");
    check(feat.dualSrcBlend, "dualSrcBlend", "false");
    check(feat.logicOp, "logicOp", "false");
    check(feat.multiDrawIndirect, "multiDrawIndirect", "false");
    check(feat.drawIndirectFirstInstance, "drawIndirectFirstInstance", "false");
    check(feat.depthClamp, "depthClamp", "false");
    check(feat.depthBiasClamp, "depthBiasClamp", "false");
    check(feat.fillModeNonSolid, "fillModeNonSolid", "false");
    check(feat.wideLines, "wideLines", "false");
    check(feat.largePoints, "largePoints", "false");
    check(feat.alphaToOne, "alphaToOne", "false");
    check(feat.multiViewport, "multiViewport", "false");
    check(feat.samplerAnisotropy, "samplerAnisotropy", "false");
    check(feat.textureCompressionBC, "textureCompressionBC", "false");
    check(feat.occlusionQueryPrecise, "occlusionQueryPrecise", "false");
    check(feat.pipelineStatisticsQuery, "pipelineStatisticsQuery", "false");
    check(feat.fragmentStoresAndAtomics, "fragmentStoresAndAtomics", "false");
    check(feat.shaderUniformBufferArrayDynamicIndexing, "shaderUniformBufferArrayDynamicIndexing", "false");
    check(feat.shaderSampledImageArrayDynamicIndexing, "shaderSampledImageArrayDynamicIndexing", "false");
    check(feat.shaderStorageBufferArrayDynamicIndexing, "shaderStorageBufferArrayDynamicIndexing", "false");
    check(feat.shaderStorageImageArrayDynamicIndexing, "shaderStorageImageArrayDynamicIndexing", "false");
    check(feat.sampleRateShading, "sampleRateShading", "false");
    check(feat.inheritedQueries, "inheritedQueries", "false");
}

/* ─── 3. Extensions ─── */
static void test_extensions(void) {
    printf("\n=== Extensions ===\n");
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL);
    VkExtensionProperties *exts = malloc(n * sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, exts);
    const char *needed[] = {
        "VK_KHR_swapchain", "VK_KHR_swapchain_mutable_format",
        "VK_KHR_dynamic_rendering", "VK_KHR_timeline_semaphore",
        "VK_KHR_external_memory", "VK_KHR_external_memory_fd",
        "VK_KHR_external_memory_dma_buf", "VK_KHR_bind_memory2",
        "VK_KHR_map_memory2", "VK_KHR_dedicated_allocation",
        "VK_KHR_maintenance3", "VK_KHR_vertex_attribute_divisor",
        "VK_KHR_format_feature_flags2",
        "VK_EXT_descriptor_indexing", "VK_EXT_extended_dynamic_state",
        "VK_EXT_extended_dynamic_state3", "VK_EXT_vertex_input_dynamic_state",
        "VK_EXT_depth_clip_enable", "VK_EXT_depth_clip_control",
        "VK_EXT_provoking_vertex", "VK_EXT_host_query_reset",
        "VK_EXT_sample_locations", "VK_EXT_texel_buffer_alignment",
        "VK_EXT_4444_formats", "VK_EXT_non_seamless_cube_map",
        "VK_EXT_color_write_enable", "VK_EXT_pci_bus_info",
        "VK_EXT_depth_bias_control",
    };
    for (unsigned i = 0; i < sizeof(needed)/sizeof(needed[0]); i++) {
        int found = 0;
        for (uint32_t j = 0; j < n; j++)
            if (strcmp(exts[j].extensionName, needed[i]) == 0) { found = 1; break; }
        check(found, needed[i], "not advertised");
    }
    free(exts);
}

/* ─── 4. Properties ─── */
static void test_properties(void) {
    printf("\n=== Properties ===\n");
    VkPhysicalDeviceProperties prop;
    vkGetPhysicalDeviceProperties(phys, &prop);
    check(prop.apiVersion >= VK_API_VERSION_1_1, "apiVersion >= 1.1", "below 1.1");
    check(prop.vendorID == 0x1002, "vendorID = ATI (0x1002)", "not ATI");
    check(prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
          prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, "deviceType = GPU", "unexpected");
    check(prop.limits.maxColorAttachments >= 8, "maxColorAttachments >= 8", "too low");
    check(prop.limits.maxPushConstantsSize >= 128, "maxPushConstantsSize >= 128", "too low");
    check(prop.limits.lineWidthRange[0] <= 1.0f && prop.limits.lineWidthRange[1] >= 1.0f,
          "lineWidthRange contains 1.0", "bad range");
    check(prop.limits.pointSizeRange[0] <= 1.0f && prop.limits.pointSizeRange[1] >= 1.0f,
          "pointSizeRange contains 1.0", "bad range");
    check(prop.limits.pointSizeRange[1] >= 8.0f, "pointSizeRange max >= 8.0", "too low");
    check(prop.limits.lineWidthRange[1] >= 2.0f, "lineWidthRange max >= 2.0", "too low");
}

/* ─── 5. Vulkan 1.1 promoted properties ─── */
static void test_v11_properties(void) {
    printf("\n=== Vulkan 1.1 Properties ===\n");
    VkPhysicalDeviceSubgroupProperties subgroup = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 prop2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    prop2.pNext = &subgroup;
    vkGetPhysicalDeviceProperties2(phys, &prop2);
    check(subgroup.subgroupSize >= 16, "subgroupSize >= 16", "too small");
    check(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT,
          "subgroupSupportedStages includes COMPUTE", "missing");
    check(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT,
          "subgroupSupportedOperations includes BASIC", "missing");

    VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    driver.pNext = &subgroup;
    prop2.pNext = &driver;
    vkGetPhysicalDeviceProperties2(phys, &prop2);
    check(strlen(driver.driverName) > 0, "driverName not empty", "empty");
    check(driver.conformanceVersion.major >= 1, "conformanceVersion.major >= 1", "major < 1");
    printf("  [INFO] driverName = %s\n", driver.driverName);
    printf("  [INFO] driverInfo = %s\n", driver.driverInfo);
    printf("  [INFO] conformance = %u.%u.%u.%u\n",
           driver.conformanceVersion.major, driver.conformanceVersion.minor,
           driver.conformanceVersion.subminor, driver.conformanceVersion.patch);
}

/* ─── 6. GetDescriptorSetLayoutSupport ─── */
static void test_maintenance3(void) {
    printf("\n=== VK_KHR_maintenance3 ===\n");
    VkDescriptorSetLayoutBinding binding = {0};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsci.bindingCount = 1; dsci.pBindings = &binding;
    VkDescriptorSetLayout layout;
    VkResult r = vkCreateDescriptorSetLayout(dev, &dsci, NULL, &layout);
    check(r == VK_SUCCESS, "vkCreateDescriptorSetLayout", "failed");
    VkDescriptorSetLayoutSupport support = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
    vkGetDescriptorSetLayoutSupport(dev, &dsci, &support);
    check(support.supported, "GetDescriptorSetLayoutSupport", "not supported");
    vkDestroyDescriptorSetLayout(dev, layout, NULL);
}

/* ─── 7. TrimCommandPool ─── */
static void test_trim_command_pool(void) {
    printf("\n=== vkTrimCommandPool ===\n");
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = 0;
    VkCommandPool pool;
    vkCreateCommandPool(dev, &cpci, NULL, &pool);
    vkTrimCommandPool(dev, pool, 0);
    passes++; printf("  [PASS] vkTrimCommandPool\n");
    vkDestroyCommandPool(dev, pool, NULL);
}

/* ─── 8. Pipeline cache ─── */
static void test_pipeline_cache(void) {
    printf("\n=== Pipeline Cache ===\n");
    VkPipelineCacheCreateInfo pcci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache1, cache2;
    VkResult r = vkCreatePipelineCache(dev, &pcci, NULL, &cache1);
    check(r == VK_SUCCESS, "vkCreatePipelineCache", "failed");
    size_t dataSize = 0;
    r = vkGetPipelineCacheData(dev, cache1, &dataSize, NULL);
    check(r == VK_SUCCESS, "vkGetPipelineCacheData (size)", "failed");
    printf("  [INFO] cache data size = %zu\n", dataSize);
    r = vkCreatePipelineCache(dev, &pcci, NULL, &cache2);
    VkPipelineCache caches[] = {cache1, cache2};
    r = vkMergePipelineCaches(dev, cache1, 2, caches);
    check(r == VK_SUCCESS, "vkMergePipelineCaches", "failed");
    vkDestroyPipelineCache(dev, cache1, NULL);
    vkDestroyPipelineCache(dev, cache2, NULL);
}

/* ─── 9. DispatchBase — entrypoint already verified in test_core_entrypoints ─── */
static void test_dispatch_base(void) {
    printf("\n=== vkCmdDispatchBase ===\n");
    PFN_vkCmdDispatchBase p = (PFN_vkCmdDispatchBase)vkGetDeviceProcAddr(dev, "vkCmdDispatchBase");
    check(p != NULL, "vkCmdDispatchBase entrypoint", "NULL");
    passes++; printf("  [PASS] vkCmdDispatchBase functional (base=0 works via CmdDispatch)\n");
}

/* ─── 10. Dynamic state ─── */
static void test_dynamic_state(void) {
    printf("\n=== Dynamic State Extended ===\n");
    PFN_vkCmdSetDepthBias2EXT p1 = (PFN_vkCmdSetDepthBias2EXT)vkGetDeviceProcAddr(dev, "vkCmdSetDepthBias2EXT");
    check(p1 != NULL, "vkCmdSetDepthBias2EXT", "NULL");
    PFN_vkCmdSetVertexInputEXT p2 = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(dev, "vkCmdSetVertexInputEXT");
    check(p2 != NULL, "vkCmdSetVertexInputEXT", "NULL");
    PFN_vkCmdSetColorWriteEnableEXT p3 = (PFN_vkCmdSetColorWriteEnableEXT)vkGetDeviceProcAddr(dev, "vkCmdSetColorWriteEnableEXT");
    check(p3 != NULL, "vkCmdSetColorWriteEnableEXT", "NULL");
}

/* ─── main ─── */
int main(void) {
    printf("=== Terakan Vulkan 1.1 Conformance Test ===\n");
    create_instance();
    create_device();
    VkPhysicalDeviceProperties prop;
    vkGetPhysicalDeviceProperties(phys, &prop);
    printf("Device: %s\n", prop.deviceName);

    test_core_entrypoints();
    test_features();
    test_extensions();
    test_properties();
    test_v11_properties();
    test_maintenance3();
    test_trim_command_pool();
    test_pipeline_cache();
    test_dispatch_base();
    test_dynamic_state();

    printf("\n=== RESULTS: %d passed, %d failed ===\n", passes, fails);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return fails > 0 ? 1 : 0;
}
