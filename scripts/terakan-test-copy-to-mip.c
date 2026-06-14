/*
 * Probe: can meta copy write mip level 1? (1:1 copy 32x32 from mip0 -> mip1)
 * Build: cc -O2 -o terakan-test-copy-to-mip terakan-test-copy-to-mip.c -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define W 64
#define H 64

#define CHECK(call)                                                                                \
   do {                                                                                            \
      if ((call) != VK_SUCCESS) {                                                                  \
         fprintf(stderr, "FAIL: %s\n", #call);                                                     \
         exit(1);                                                                                  \
      }                                                                                            \
   } while (0)

static uint32_t checksum(uint8_t const *d, size_t n)
{
   uint32_t s = 0;
   for (size_t i = 0; i < n; ++i)
      s = s * 131u + d[i];
   return s;
}

int main(void)
{
   VkInstance inst = VK_NULL_HANDLE;
   VkPhysicalDevice phys;
   VkDevice dev = VK_NULL_HANDLE;
   VkQueue queue;
   uint32_t qf = 0;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkBuffer staging;
   VkDeviceMemory staging_mem, img_mem;
   VkImage img;
   VkFence fence;

   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t nc = 1;
   CHECK(vkEnumeratePhysicalDevices(inst, &nc, &phys));
   float qp = 0.f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &qp };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
   CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
   vkGetDeviceQueue(dev, qf, 0, &queue);

   VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
   CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
   CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
   CHECK(vkCreateFence(dev, &(VkFenceCreateInfo){ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }, NULL, &fence));

   size_t const sb = (size_t)W * H * 4;
   CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sb, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT }, NULL, &staging));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, staging, &req);
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(phys, &mp);
   uint32_t mt = 0;
   for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         mt = i;
   CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = mt }, NULL, &staging_mem));
   CHECK(vkBindBufferMemory(dev, staging, staging_mem, 0));

   CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { W, H, 1 },
      .mipLevels = 2,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
   }, NULL, &img));
   vkGetImageMemoryRequirements(dev, img, &req);
   for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if (req.memoryTypeBits & (1u << i)) { mt = i; break; }
   CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = mt }, NULL, &img_mem));
   CHECK(vkBindImageMemory(dev, img, img_mem, 0));

   void *map = NULL;
   CHECK(vkMapMemory(dev, staging_mem, 0, sb, 0, &map));
   for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
         uint8_t *p = (uint8_t *)map + (y * W + x) * 4;
         p[0] = (uint8_t)x * 4; p[1] = (uint8_t)y * 4; p[2] = 128; p[3] = 255;
      }

   VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
   CHECK(vkBeginCommandBuffer(cmd, &bi));

   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .image = img,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   vkCmdCopyBufferToImage(cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
      &(VkBufferImageCopy){ .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } });

   b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

   /* 1:1 copy top-left 32x32 from mip0 to mip1 */
   VkImageCopy cp = {
      .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1 },
      .extent = { 32, 32, 1 },
   };
   b.subresourceRange.baseMipLevel = 1;
   b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   vkCmdCopyImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);

   b.subresourceRange.baseMipLevel = 0;
   b.subresourceRange.levelCount = 2;
   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
   CHECK(vkEndCommandBuffer(cmd));
   CHECK(vkQueueSubmit(queue, 1, &si, fence));
   CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

   CHECK(vkResetCommandBuffer(cmd, 0));
   CHECK(vkBeginCommandBuffer(cmd, &bi));
   vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
      &(VkBufferImageCopy){ .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1 }, .imageExtent = { 32, 32, 1 } });
   CHECK(vkEndCommandBuffer(cmd));
   CHECK(vkResetFences(dev, 1, &fence));
   CHECK(vkQueueSubmit(queue, 1, &si, fence));
   CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

   uint32_t cs = checksum(map, 32u * 32u * 4u);
   uint32_t ref = checksum(map, 32u * 32u * 4u); /* expected = top-left of mip0 pattern */
   /* Recompute expected from original gradient at 32x32 */
   uint32_t exp = 0;
   for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 0; x < 32; ++x) {
         uint8_t px[4] = { (uint8_t)x * 4, (uint8_t)y * 4, 128, 255 };
         for (int i = 0; i < 4; ++i)
            exp = exp * 131u + px[i];
      }
   printf("mip1 checksum=0x%08x expected=0x%08x %s\n", cs, exp, cs == exp ? "MATCH" : "MISMATCH");
   return cs == exp ? 0 : 2;
}
