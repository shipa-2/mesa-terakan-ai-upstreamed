/*
 * Terakan mipmap blit probe — STK-style vkCmdBlitImage mip chain + readback checksums.
 *
 * Build: cc -O2 -o terakan-test-mipmap-blitz terakan-test-mipmap-blitz.c -lvulkan -lm
 * Reference (RADV):  ./terakan-test-mipmap-blitz
 * Terakan:           VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/terascale_icd.x86_64.json ./terakan-test-mipmap-blitz
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <vulkan/vulkan.h>

#define WIDTH 64
#define HEIGHT 64
#define MIP_LEVELS 7

static uint32_t const reference_checksums[MIP_LEVELS] = {
   0xa29b9504u, 0xed3ebf10u, 0xa3314b30u, 0x07050530u,
   0x7b5b6000u, 0x08059078u, 0x110372b4u,
};
#define CHECK(call)                                                                                \
   do {                                                                                            \
      VkResult const _r = (call);                                                                  \
      if (_r != VK_SUCCESS) {                                                                      \
         fprintf(stderr, "FAIL %s:%d: %s -> %d\n", __FILE__, __LINE__, #call, (int)_r);            \
         exit(1);                                                                                  \
      }                                                                                            \
   } while (0)

static uint32_t
checksum_rgba8(uint8_t const *data, size_t bytes)
{
   uint32_t sum = 0;
   for (size_t i = 0; i < bytes; ++i)
      sum = sum * 131u + data[i];
   return sum;
}

static void
fill_mip0_gradient(uint8_t *pixels, uint32_t w, uint32_t h)
{
   for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
         uint8_t *p = pixels + (y * w + x) * 4;
         p[0] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
         p[1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
         p[2] = (uint8_t)((x + y) * 255 / (w + h > 2 ? w + h - 2 : 1));
         p[3] = 255;
      }
   }
}

static uint32_t
mip_width(uint32_t level)
{
   uint32_t w = WIDTH >> level;
   return w ? w : 1;
}

static uint32_t
mip_height(uint32_t level)
{
   uint32_t h = HEIGHT >> level;
   return h ? h : 1;
}

int
main(void)
{
   VkInstance instance = VK_NULL_HANDLE;
   VkPhysicalDevice phys = VK_NULL_HANDLE;
   VkDevice device = VK_NULL_HANDLE;
   VkQueue queue = VK_NULL_HANDLE;
   uint32_t queue_family = 0;
   VkCommandPool cmd_pool = VK_NULL_HANDLE;
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   VkBuffer staging = VK_NULL_HANDLE;
   VkDeviceMemory staging_mem = VK_NULL_HANDLE;
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory image_mem = VK_NULL_HANDLE;
   VkImageView view = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;

   {
      VkApplicationInfo app = {
         .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
         .pApplicationName = "terakan-test-mipmap-blitz",
         .apiVersion = VK_API_VERSION_1_1,
      };
      VkInstanceCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo = &app,
      };
      CHECK(vkCreateInstance(&ici, NULL, &instance));
   }

   {
      uint32_t count = 1;
      CHECK(vkEnumeratePhysicalDevices(instance, &count, &phys));
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(phys, &props);
      printf("deviceName: %s\n", props.deviceName);
      printf("apiVersion: %u.%u.%u\n", VK_VERSION_MAJOR(props.apiVersion),
             VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

      VkFormatProperties fp;
      vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R8G8B8A8_UNORM, &fp);
      printf("R8G8B8A8_UNORM optimal: blit_src=%d blit_dst=%d linear_filter=%d sampled=%d\n",
             (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0,
             (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0,
             (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0,
             (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0);
      printf("STK supportsRGBA8Blit (linear only): %s\n",
             (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                ? "true"
                : "false");
   }

   {
      float qprio = 0.f;
      VkDeviceQueueCreateInfo qci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = queue_family,
         .queueCount = 1,
         .pQueuePriorities = &qprio,
      };
      VkDeviceCreateInfo dci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos = &qci,
      };
      CHECK(vkCreateDevice(phys, &dci, NULL, &device));
      vkGetDeviceQueue(device, queue_family, 0, &queue);
   }

   {
      VkCommandPoolCreateInfo cpi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         .queueFamilyIndex = queue_family,
      };
      CHECK(vkCreateCommandPool(device, &cpi, NULL, &cmd_pool));
      VkCommandBufferAllocateInfo cbai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = cmd_pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));
   }

   {
      VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
      CHECK(vkCreateFence(device, &fci, NULL, &fence));
   }

   size_t const staging_bytes = (size_t)WIDTH * HEIGHT * 4;
   {
      VkBufferCreateInfo bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = staging_bytes,
         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      };
      CHECK(vkCreateBuffer(device, &bci, NULL, &staging));
      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(device, staging, &req);
      VkPhysicalDeviceMemoryProperties mem_props;
      vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
      uint32_t type = UINT32_MAX;
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
         if ((req.memoryTypeBits & (1u << i)) &&
             (mem_props.memoryTypes[i].propertyFlags &
              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
         }
      }
      if (type == UINT32_MAX) {
         fprintf(stderr, "host-visible memory not found\n");
         return 1;
      }
      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size,
         .memoryTypeIndex = type,
      };
      CHECK(vkAllocateMemory(device, &mai, NULL, &staging_mem));
      CHECK(vkBindBufferMemory(device, staging, staging_mem, 0));
   }

   {
      VkImageCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = { WIDTH, HEIGHT, 1 },
         .mipLevels = MIP_LEVELS,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      CHECK(vkCreateImage(device, &ici, NULL, &image));
      VkMemoryRequirements req;
      vkGetImageMemoryRequirements(device, image, &req);
      VkPhysicalDeviceMemoryProperties mem_props;
      vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
      uint32_t type = UINT32_MAX;
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
         if (req.memoryTypeBits & (1u << i)) {
            type = i;
            break;
         }
      }
      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size,
         .memoryTypeIndex = type,
      };
      CHECK(vkAllocateMemory(device, &mai, NULL, &image_mem));
      CHECK(vkBindImageMemory(device, image, image_mem, 0));
   }

   {
      VkImageViewCreateInfo vci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .subresourceRange =
            {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .levelCount = MIP_LEVELS,
               .layerCount = 1,
            },
      };
      CHECK(vkCreateImageView(device, &vci, NULL, &view));
   }

   void *mapped = NULL;
   CHECK(vkMapMemory(device, staging_mem, 0, staging_bytes, 0, &mapped));
   fill_mip0_gradient(mapped, WIDTH, HEIGHT);

   VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK(vkBeginCommandBuffer(cmd, &bi));

   VkImageMemoryBarrier to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                        0, NULL, 0, NULL, 1, &to_dst);

   VkBufferImageCopy copy = {
      .bufferOffset = 0,
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageExtent = { WIDTH, HEIGHT, 1 },
   };
   vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

   int mip_w = WIDTH, mip_h = HEIGHT;
   for (uint32_t i = 1; i < MIP_LEVELS; ++i) {
      VkImageMemoryBarrier barrier = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = image,
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 1 },
      };
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, NULL, 0, NULL, 1, &barrier);

      int const dst_w = mip_w > 1 ? mip_w / 2 : 1;
      int const dst_h = mip_h > 1 ? mip_h / 2 : 1;
      VkImageBlit blit = {
         .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 },
         .srcOffsets = { { 0, 0, 0 }, { mip_w, mip_h, 1 } },
         .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 },
         .dstOffsets = { { 0, 0, 0 }, { dst_w, dst_h, 1 } },
      };
      vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.subresourceRange.baseMipLevel = i - 1;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, NULL, 0, NULL, 1, &barrier);

      barrier.subresourceRange.baseMipLevel = i;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, NULL, 0, NULL, 1, &barrier);

      mip_w = dst_w;
      mip_h = dst_h;
   }

   VkImageMemoryBarrier to_read = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, MIP_LEVELS, 0, 1 },
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                        NULL, 0, NULL, 1, &to_read);
   CHECK(vkEndCommandBuffer(cmd));

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(vkQueueSubmit(queue, 1, &si, fence));
   CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 30000000000ULL));

   printf("mip_checksums:\n");
   unsigned mismatches = 0;
   for (uint32_t level = 0; level < MIP_LEVELS; ++level) {
      uint32_t const w = mip_width(level);
      uint32_t const h = mip_height(level);
      size_t const bytes = (size_t)w * h * 4;

      VkBufferImageCopy rd = {
         .bufferOffset = 0,
         .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
         .imageExtent = { w, h, 1 },
      };
      CHECK(vkResetCommandBuffer(cmd, 0));
      CHECK(vkBeginCommandBuffer(cmd, &bi));
      vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &rd);
      CHECK(vkEndCommandBuffer(cmd));
      CHECK(vkResetFences(device, 1, &fence));
      CHECK(vkQueueSubmit(queue, 1, &si, fence));
      CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 30000000000ULL));

      uint32_t const cs = checksum_rgba8(mapped, bytes);
      bool const match = cs == reference_checksums[level];
      if (!match)
         mismatches++;
      printf("  level %u (%ux%u): checksum=0x%08x  ref=0x%08x  %s\n", level, w, h, cs,
             reference_checksums[level], match ? "MATCH" : "MISMATCH");
   }

   if (mismatches == 0) {
      printf("result: PASS (all mip levels match RADV reference)\n");
   } else {
      printf("result: FAIL (%u/%u mip levels differ from RADV reference)\n", mismatches,
             MIP_LEVELS);
      printf("hint: STK uses the same blit mipgen path; broken UI textures often mean mip1+ never written\n");
   }

   vkUnmapMemory(device, staging_mem);
   vkDestroyImageView(device, view, NULL);
   vkDestroyImage(device, image, NULL);
   vkFreeMemory(device, image_mem, NULL);
   vkDestroyBuffer(device, staging, NULL);
   vkFreeMemory(device, staging_mem, NULL);
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, cmd_pool, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return mismatches == 0 ? 0 : 2;
}
