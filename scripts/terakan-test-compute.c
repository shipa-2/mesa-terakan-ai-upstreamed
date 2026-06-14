/*
 * Minimal compute dispatch test: write invocation index + 1 into SSBO.
 * Build: cc -O2 -o terakan-test-compute terakan-test-compute.c -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define COUNT 4

#define STEP(msg) do { fprintf(stderr, "STEP: %s\n", msg); fflush(stderr); } while (0)

#define CHECK(call)                                                                                \
   do {                                                                                            \
      VkResult _r = (call);                                                                        \
      if (_r != VK_SUCCESS) {                                                                      \
         fprintf(stderr, "FAIL: %s -> %d\n", #call, _r);                                           \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static const uint32_t cs_spirv[] = {
   0x07230203, 0x00010000, 0x0008000b, 0x0000001b,
   0x00000000, 0x00020011, 0x00000001, 0x0006000b,
   0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
   0x00000000, 0x0003000e, 0x00000000, 0x00000001,
   0x0006000f, 0x00000005, 0x00000004, 0x6e69616d,
   0x00000000, 0x0000000f, 0x00060010, 0x00000004,
   0x00000011, 0x00000001, 0x00000001, 0x00000001,
   0x00030003, 0x00000002, 0x000001c2, 0x00040005,
   0x00000004, 0x6e69616d, 0x00000000, 0x00030005,
   0x00000008, 0x0074754f, 0x00050006, 0x00000008,
   0x00000000, 0x61746164, 0x00000000, 0x00040005,
   0x0000000a, 0x5f74756f, 0x00667562, 0x00080005,
   0x0000000f, 0x475f6c67, 0x61626f6c, 0x766e496c,
   0x7461636f, 0x496e6f69, 0x00000044, 0x00040047,
   0x00000007, 0x00000006, 0x00000004, 0x00030047,
   0x00000008, 0x00000003, 0x00050048, 0x00000008,
   0x00000000, 0x00000023, 0x00000000, 0x00040047,
   0x0000000a, 0x00000021, 0x00000000, 0x00040047,
   0x0000000a, 0x00000022, 0x00000000, 0x00040047,
   0x0000000f, 0x0000000b, 0x0000001c, 0x00040047,
   0x0000001a, 0x0000000b, 0x00000019, 0x00020013,
   0x00000002, 0x00030021, 0x00000003, 0x00000002,
   0x00040015, 0x00000006, 0x00000020, 0x00000000,
   0x0003001d, 0x00000007, 0x00000006, 0x0003001e,
   0x00000008, 0x00000007, 0x00040020, 0x00000009,
   0x00000002, 0x00000008, 0x0004003b, 0x00000009,
   0x0000000a, 0x00000002, 0x00040015, 0x0000000b,
   0x00000020, 0x00000001, 0x0004002b, 0x0000000b,
   0x0000000c, 0x00000000, 0x00040017, 0x0000000d,
   0x00000006, 0x00000003, 0x00040020, 0x0000000e,
   0x00000001, 0x0000000d, 0x0004003b, 0x0000000e,
   0x0000000f, 0x00000001, 0x0004002b, 0x00000006,
   0x00000010, 0x00000000, 0x00040020, 0x00000011,
   0x00000001, 0x00000006, 0x0004002b, 0x00000006,
   0x00000016, 0x00000001, 0x00040020, 0x00000018,
   0x00000002, 0x00000006, 0x0006002c, 0x0000000d,
   0x0000001a, 0x00000016, 0x00000016, 0x00000016,
   0x00050036, 0x00000002, 0x00000004, 0x00000000,
   0x00000003, 0x000200f8, 0x00000005, 0x00050041,
   0x00000011, 0x00000012, 0x0000000f, 0x00000010,
   0x0004003d, 0x00000006, 0x00000013, 0x00000012,
   0x00050041, 0x00000011, 0x00000014, 0x0000000f,
   0x00000010, 0x0004003d, 0x00000006, 0x00000015,
   0x00000014, 0x00050080, 0x00000006, 0x00000017,
   0x00000015, 0x00000016, 0x00060041, 0x00000018,
   0x00000019, 0x0000000a, 0x0000000c, 0x00000013,
   0x0003003e, 0x00000019, 0x00000017, 0x000100fd,
   0x00010038,
};

int
main(void)
{
   STEP("start");
   VkInstance instance;
   {
      VkApplicationInfo app = {
         .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
         .pApplicationName = "terakan-test-compute",
         .apiVersion = VK_API_VERSION_1_1,
      };
      VkInstanceCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo = &app,
      };
      STEP("CreateInstance"); CHECK(vkCreateInstance(&ici, NULL, &instance));
   }

   uint32_t pd_count = 0;
   vkEnumeratePhysicalDevices(instance, &pd_count, NULL);
   if (pd_count == 0) {
      fprintf(stderr, "no physical devices\n");
      return 1;
   }
   VkPhysicalDevice pd;
   vkEnumeratePhysicalDevices(instance, &pd_count, &pd);

   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);
   printf("GPU: %s\n", props.deviceName);

   uint32_t qf = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf, NULL);
   VkQueueFamilyProperties * qfp = calloc(qf, sizeof(*qfp));
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf, qfp);
   uint32_t compute_qfi = UINT32_MAX;
   for (uint32_t i = 0; i < qf; ++i) {
      if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
         compute_qfi = i;
         break;
      }
   }
   free(qfp);
   if (compute_qfi == UINT32_MAX) {
      fprintf(stderr, "no compute queue\n");
      return 1;
   }

   VkDevice device;
   {
      float prio = 1.0f;
      VkDeviceQueueCreateInfo qci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = compute_qfi,
         .queueCount = 1,
         .pQueuePriorities = &prio,
      };
      VkDeviceCreateInfo dci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos = &qci,
      };
      STEP("CreateDevice"); CHECK(vkCreateDevice(pd, &dci, NULL, &device));
   }

   VkQueue queue;
   vkGetDeviceQueue(device, compute_qfi, 0, &queue);

   VkBuffer buffer;
   VkDeviceMemory memory;
   {
      VkBufferCreateInfo bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = COUNT * sizeof(uint32_t),
         .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      CHECK(vkCreateBuffer(device, &bci, NULL, &buffer));

      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(device, buffer, &req);
      VkPhysicalDeviceMemoryProperties mp;
      vkGetPhysicalDeviceMemoryProperties(pd, &mp);
      uint32_t type = UINT32_MAX;
      for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
         if ((req.memoryTypeBits & (1u << i)) &&
             (mp.memoryTypes[i].propertyFlags &
              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
         }
      }
      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size,
         .memoryTypeIndex = type,
      };
      CHECK(vkAllocateMemory(device, &mai, NULL, &memory));
      CHECK(vkBindBufferMemory(device, buffer, memory, 0));
   }

   void * mapped = NULL;
   CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
   memset(mapped, 0, COUNT * sizeof(uint32_t));

   VkDescriptorSetLayout set_layout;
   {
      VkDescriptorSetLayoutBinding binding = {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      };
      VkDescriptorSetLayoutCreateInfo ci = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 1,
         .pBindings = &binding,
      };
      CHECK(vkCreateDescriptorSetLayout(device, &ci, NULL, &set_layout));
   }

   VkPipelineLayout pipeline_layout;
   {
      VkPipelineLayoutCreateInfo ci = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         .setLayoutCount = 1,
         .pSetLayouts = &set_layout,
      };
      CHECK(vkCreatePipelineLayout(device, &ci, NULL, &pipeline_layout));
   }

   VkDescriptorPool pool;
   VkDescriptorSet set;
   {
      VkDescriptorPoolSize ps = {
         .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
      };
      VkDescriptorPoolCreateInfo pci = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
         .maxSets = 1,
         .poolSizeCount = 1,
         .pPoolSizes = &ps,
      };
      CHECK(vkCreateDescriptorPool(device, &pci, NULL, &pool));
      VkDescriptorSetAllocateInfo ai = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &set_layout,
      };
      CHECK(vkAllocateDescriptorSets(device, &ai, &set));
      VkDescriptorBufferInfo bi = {
         .buffer = buffer,
         .offset = 0,
         .range = COUNT * sizeof(uint32_t),
      };
      VkWriteDescriptorSet w = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &bi,
      };
      vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
   }

   VkShaderModule sm;
   {
      VkShaderModuleCreateInfo ci = {
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(cs_spirv),
         .pCode = cs_spirv,
      };
      CHECK(vkCreateShaderModule(device, &ci, NULL, &sm));
   }

   VkPipeline pipeline;
   {
      VkPipelineShaderStageCreateInfo stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = sm,
         .pName = "main",
      };
      VkComputePipelineCreateInfo ci = {
         .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
         .stage = stage,
         .layout = pipeline_layout,
      };
      STEP("CreateComputePipelines"); CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
   }

   VkCommandPool cmd_pool;
   VkCommandBuffer cmd;
   {
      VkCommandPoolCreateInfo pci = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = compute_qfi,
      };
      CHECK(vkCreateCommandPool(device, &pci, NULL, &cmd_pool));
      VkCommandBufferAllocateInfo ai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = cmd_pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
   }

   VkFence fence;
   CHECK(vkCreateFence(device,
                       &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL,
                       &fence));

   {
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CHECK(vkBeginCommandBuffer(cmd, &bi));
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0,
                                NULL);
      vkCmdDispatch(cmd, COUNT, 1, 1);
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                           &(VkMemoryBarrier){
                              .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                           },
                           0, NULL, 0, NULL);
      CHECK(vkEndCommandBuffer(cmd));
      CHECK(vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){
                             .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cmd,
                          },
                          fence));
      STEP("WaitForFences"); CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
   }

   uint32_t expected[COUNT] = {1, 2, 3, 4};
   int pass = 1;
   for (uint32_t i = 0; i < COUNT; ++i) {
      uint32_t v = ((uint32_t *)mapped)[i];
      printf("data[%u] = %u (expected %u)\n", i, v, expected[i]);
      if (v != expected[i]) {
         pass = 0;
      }
   }

   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, cmd_pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, sm, NULL);
   vkDestroyDescriptorPool(device, pool, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL);
   vkUnmapMemory(device, memory);
   vkFreeMemory(device, memory, NULL);
   vkDestroyBuffer(device, buffer, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("%s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
