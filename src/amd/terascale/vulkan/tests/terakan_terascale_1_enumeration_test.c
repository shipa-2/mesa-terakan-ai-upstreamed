/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* TeraScale 1 physical devices enumerate and report properties. On hardware-validated R600 and
 * R700, vkCreateDevice additionally performs the minimal logical-device bring-up. Queue submission
 * remains disabled on both until each generation's command stream is validated independently.
 *
 * Meaningful only on a machine with a TeraScale 1 card actually installed; there is no requirement
 * that one be present. When none is found, this reports that plainly and passes, since there is
 * nothing to check.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t const application_vertex_spirv[] = {
#include "terakan_vertex_fetch_bounds.vert.spv.h"
};
static uint32_t const application_fragment_spirv[] = {
#include "terakan_vertex_fetch_bounds.frag.spv.h"
};

#define VK_CHECK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_result = (expression);                                                  \
      if (check_result != VK_SUCCESS) {                                                            \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_result);               \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static uint32_t
first_memory_type(uint32_t const memory_type_bits)
{
   for (uint32_t memory_type = 0; memory_type < 32; ++memory_type) {
      if (memory_type_bits & ((uint32_t)1 << memory_type)) {
         return memory_type;
      }
   }
   return UINT32_MAX;
}

static bool
terascale_1_submit_opted_in(void)
{
   char const * const value = getenv("TERAKAN_DEBUG_TERASCALE_1_SUBMIT");
   return value != NULL && strcmp(value, "1") == 0;
}

static bool
terascale_1_signal_only_opted_in(void)
{
   char const * const value = getenv("TERAKAN_DEBUG_TERASCALE_1_SIGNAL_ONLY");
   return value != NULL && strcmp(value, "1") == 0;
}

static bool
terascale_1_cp_dma_copy_opted_in(void)
{
   char const * const value = getenv("TERAKAN_DEBUG_TERASCALE_1_CP_DMA_COPY");
   return value != NULL && strcmp(value, "1") == 0;
}

static bool
terascale_1_cp_dma_fill_opted_in(void)
{
   char const * const value = getenv("TERAKAN_DEBUG_TERASCALE_1_CP_DMA_FILL");
   return value != NULL && strcmp(value, "1") == 0;
}

static bool
terascale_1_cp_dma_unaligned_copy_opted_in(void)
{
   char const * const value = getenv("TERAKAN_DEBUG_TERASCALE_1_CP_DMA_UNALIGNED_COPY");
   return value != NULL && strcmp(value, "1") == 0;
}

static bool
terascale_1_linear_image_readback_opted_in(void)
{
   char const * const value = getenv("TERAKAN_DEBUG_TERASCALE_1_LINEAR_IMAGE_READBACK");
   return value != NULL && strcmp(value, "1") == 0;
}

static bool
terascale_1_linear_image_clear_opted_in(void)
{
   char const * const value = getenv("TERAKAN_DEBUG_TERASCALE_1_LINEAR_IMAGE_CLEAR");
   return value != NULL && strcmp(value, "1") == 0;
}

static bool
terascale_1_linear_buffer_upload_opted_in(void)
{
   char const * const value = getenv("TERAKAN_DEBUG_TERASCALE_1_LINEAR_BUFFER_UPLOAD");
   return value != NULL && strcmp(value, "1") == 0;
}

static bool
terascale_1_tiled_image_roundtrip_opted_in(void)
{
   char const * const value = getenv("TERAKAN_DEBUG_TERASCALE_1_TILED_IMAGE_ROUNDTRIP");
   return value != NULL && strcmp(value, "1") == 0;
}

static char const *
terascale_1_bc1_roundtrip_mode(void)
{
   char const * const value = getenv("TERAKAN_DEBUG_TERASCALE_1_BC1_ROUNDTRIP");
   return value != NULL && (!strcmp(value, "1") || !strcmp(value, "negative") ||
                            !strcmp(value, "readback") || !strcmp(value, "readback-negative") ||
                            !strcmp(value, "readback-mip-layer") ||
                            !strcmp(value, "readback-mip-layer-negative"))
             ? value
             : NULL;
}

static uint32_t
check_rv710_signal_only_submit(VkDevice const device, VkQueue const queue)
{
   VkFence fence = VK_NULL_HANDLE;
   VkFenceCreateInfo const fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   VkResult result = vkCreateFence(device, &fence_info, NULL, &fence);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 signal-only vkCreateFence failed with %d\n", result);
      return 1;
   }
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
   };
   result = vkQueueSubmit(queue, 1, &submit_info, fence);
   if (result == VK_SUCCESS) {
      result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000));
   }
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 signal-only fence submission failed with %d\n", result);
   } else {
      fprintf(stderr, "  RV710 signal-only fence submission completed\n");
   }
   vkDestroyFence(device, fence, NULL);
   return result != VK_SUCCESS;
}

static uint32_t
check_rv710_empty_submit(VkDevice const device, VkQueue const queue)
{
   VkCommandPool command_pool = VK_NULL_HANDLE;
   VkCommandBuffer command_buffer = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   uint32_t failures = 0;

   VkCommandPoolCreateInfo const command_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkResult result = vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 empty-submit vkCreateCommandPool failed with %d\n", result);
      return 1;
   }
   VkCommandBufferAllocateInfo const command_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   result = vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 empty-submit vkAllocateCommandBuffers failed with %d\n", result);
      failures = 1;
      goto cleanup;
   }
   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   result = vkBeginCommandBuffer(command_buffer, &begin_info);
   if (result == VK_SUCCESS) {
      result = vkEndCommandBuffer(command_buffer);
   }
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 empty command-buffer recording failed with %d\n", result);
      failures = 1;
      goto cleanup;
   }
   VkFenceCreateInfo const fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   result = vkCreateFence(device, &fence_info, NULL, &fence);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 empty-submit vkCreateFence failed with %d\n", result);
      failures = 1;
      goto cleanup;
   }
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   result = vkQueueSubmit(queue, 1, &submit_info, fence);
   if (result == VK_SUCCESS) {
      result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000));
   }
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 empty preamble/fence submission failed with %d\n", result);
      failures = 1;
   } else {
      fprintf(stderr, "  RV710 empty preamble/fence submission completed\n");
   }

cleanup:
   vkDestroyFence(device, fence, NULL);
   if (command_buffer != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
   }
   vkDestroyCommandPool(device, command_pool, NULL);
   return failures;
}

/* B3 is intentionally an opt-in RV710-only probe. The destination starts with the inverse pattern,
 * so a skipped CP DMA command cannot pass merely because mapped memory happened to contain the
 * requested data. This does not exercise unaligned copies, images, or cache transitions beyond the
 * transfer write becoming visible to the host.
 */
static uint32_t
check_rv710_cp_dma_buffer_copy(VkPhysicalDevice const physical_device, VkDevice const device,
                               VkQueue const queue, bool const fill, bool const unaligned)
{
   enum { dword_count = 16, byte_count = dword_count * sizeof(uint32_t) };
   VkBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   VkDeviceMemory memories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   uint32_t * mappings[2] = {NULL, NULL};
   VkCommandPool command_pool = VK_NULL_HANDLE;
   VkCommandBuffer command_buffer = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   uint32_t failures = 0;

   VkBufferCreateInfo const buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = byte_count,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   for (uint32_t buffer_index = 0; buffer_index < 2; ++buffer_index) {
      VkResult result = vkCreateBuffer(device, &buffer_info, NULL, &buffers[buffer_index]);
      if (result != VK_SUCCESS) {
         fprintf(stderr, "  RV710 CP-DMA vkCreateBuffer failed with %d\n", result);
         failures = 1;
         goto cleanup;
      }
      VkMemoryRequirements requirements;
      vkGetBufferMemoryRequirements(device, buffers[buffer_index], &requirements);
      VkPhysicalDeviceMemoryProperties memory_properties;
      vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
      uint32_t memory_type = UINT32_MAX;
      for (uint32_t type_index = 0; type_index < memory_properties.memoryTypeCount; ++type_index) {
         if ((requirements.memoryTypeBits & ((uint32_t)1 << type_index)) &&
             (memory_properties.memoryTypes[type_index].propertyFlags &
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            memory_type = type_index;
            break;
         }
      }
      if (memory_type == UINT32_MAX) {
         fprintf(stderr, "  RV710 CP-DMA buffer has no host-visible memory type\n");
         failures = 1;
         goto cleanup;
      }
      VkMemoryAllocateInfo const allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = memory_type,
      };
      result = vkAllocateMemory(device, &allocate_info, NULL, &memories[buffer_index]);
      if (result != VK_SUCCESS ||
          (result = vkBindBufferMemory(device, buffers[buffer_index], memories[buffer_index], 0)) !=
             VK_SUCCESS ||
          (result = vkMapMemory(device, memories[buffer_index], 0, VK_WHOLE_SIZE, 0,
                                (void **)&mappings[buffer_index])) != VK_SUCCESS) {
         fprintf(stderr, "  RV710 CP-DMA buffer allocation/bind/map failed with %d\n", result);
         failures = 1;
         goto cleanup;
      }
   }

   for (uint32_t index = 0; index < dword_count; ++index) {
      mappings[0][index] = UINT32_C(0x1a2b3c40) + index;
      mappings[1][index] = fill ? ~UINT32_C(0x76543210) : ~mappings[0][index];
   }
   VkMappedMemoryRange const source_flush = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = memories[0],
      .size = VK_WHOLE_SIZE,
   };
   if (vkFlushMappedMemoryRanges(device, 1, &source_flush) != VK_SUCCESS) {
      fprintf(stderr, "  RV710 CP-DMA source flush failed\n");
      failures = 1;
      goto cleanup;
   }

   VkCommandPoolCreateInfo const command_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandBufferAllocateInfo command_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandBufferCount = 1,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
   };
   VkCommandBufferBeginInfo const begin_info = {.sType =
                                                   VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VkBufferCopy const copy = unaligned ? (VkBufferCopy){.srcOffset = 4, .size = byte_count - 4}
                                       : (VkBufferCopy){.size = byte_count};
   VkFenceCreateInfo const fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkResult result = vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool);
   if (result == VK_SUCCESS) {
      command_buffer_info.commandPool = command_pool;
      result = vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer);
   }
   if (result == VK_SUCCESS)
      result = vkBeginCommandBuffer(command_buffer, &begin_info);
   if (result == VK_SUCCESS) {
      if (fill)
         vkCmdFillBuffer(command_buffer, buffers[1], 0, byte_count, UINT32_C(0x76543210));
      else
         vkCmdCopyBuffer(command_buffer, buffers[0], buffers[1], 1, &copy);
      result = vkEndCommandBuffer(command_buffer);
   }
   if (result == VK_SUCCESS)
      result = vkCreateFence(device, &fence_info, NULL, &fence);
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   if (result == VK_SUCCESS)
      result = vkQueueSubmit(queue, 1, &submit_info, fence);
   if (result == VK_SUCCESS)
      result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000));
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 CP-DMA copy submission failed with %d\n", result);
      failures = 1;
      goto cleanup;
   }
   VkMappedMemoryRange const destination_invalidate = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = memories[1],
      .size = VK_WHOLE_SIZE,
   };
   if (vkInvalidateMappedMemoryRanges(device, 1, &destination_invalidate) != VK_SUCCESS) {
      fprintf(stderr, "  RV710 CP-DMA destination invalidate failed\n");
      failures = 1;
      goto cleanup;
   }
   for (uint32_t index = 0; index < dword_count; ++index) {
      uint32_t const expected = fill ? UINT32_C(0x76543210)
                                : unaligned && index == dword_count - 1
                                   ? ~mappings[0][index]
                                   : mappings[0][index + unaligned];
      if (mappings[1][index] != expected) {
         fprintf(stderr, "  RV710 CP-DMA readback mismatch at %u: got 0x%08x expected 0x%08x\n",
                 index, mappings[1][index], expected);
         failures = 1;
         break;
      }
   }
   if (!failures)
      fprintf(stderr, "  RV710 CP-DMA %s buffer %s/readback completed\n",
              unaligned ? "unaligned" : "64-byte", fill ? "fill" : "copy");

cleanup:
   vkDestroyFence(device, fence, NULL);
   if (command_buffer != VK_NULL_HANDLE)
      vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
   vkDestroyCommandPool(device, command_pool, NULL);
   for (uint32_t buffer_index = 0; buffer_index < 2; ++buffer_index) {
      if (mappings[buffer_index] != NULL)
         vkUnmapMemory(device, memories[buffer_index]);
      vkDestroyBuffer(device, buffers[buffer_index], NULL);
      vkFreeMemory(device, memories[buffer_index], NULL);
   }
   return failures;
}

/* Linear BC1 is deliberately isolated from the still-unsupported tiled BC1 path.  The 8x8
 * image contains four 8-byte blocks; inverse sentinels make a skipped or partial transfer fail.
 * The negative mode uses a non-zero buffer offset and requires the shifted 32-byte image to match.
 * This checks block addressing and offset handling, but does not prove optimal/tiled
 * BC1 layout or format filtering on other R700 chips. */
static uint32_t
check_rv710_linear_bc1_roundtrip(VkPhysicalDevice const physical_device, VkDevice const device,
                                 VkQueue const queue, bool const negative, bool const to_buffer,
                                 bool const mip_layer)
{
   enum { block_bytes = 8, block_columns = 2, block_rows = 2, image_bytes = 32, buffer_bytes = 40 };
   uint8_t source[buffer_bytes], inverse[image_bytes];
   for (uint32_t i = 0; i < buffer_bytes; ++i) {
      source[i] = (uint8_t)(0x31u + i * 7u);
      if (i < image_bytes)
         inverse[i] = (uint8_t)~source[i];
   }
   VkImage image = VK_NULL_HANDLE;
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory image_memory = VK_NULL_HANDLE, buffer_memory = VK_NULL_HANDLE;
   uint8_t *image_mapping = NULL, *buffer_mapping = NULL;
   VkCommandPool command_pool = VK_NULL_HANDLE;
   VkCommandBuffer command_buffer = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   uint32_t failures = 0;
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   VkImageCreateInfo const image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
      .extent = {mip_layer ? 16u : 8u, mip_layer ? 16u : 8u, 1},
      .mipLevels = mip_layer ? 2 : 1, .arrayLayers = mip_layer ? 2 : 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
   };
   VkResult result = vkCreateImage(device, &image_info, NULL, &image);
   VkMemoryRequirements image_requirements = {0};
   if (result == VK_SUCCESS)
      vkGetImageMemoryRequirements(device, image, &image_requirements);
   uint32_t image_type = UINT32_MAX;
   for (uint32_t i = 0; result == VK_SUCCESS && i < properties.memoryTypeCount; ++i)
      if ((image_requirements.memoryTypeBits & ((uint32_t)1 << i)) &&
          (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
         image_type = i;
         break;
      }
   if (result == VK_SUCCESS && image_type == UINT32_MAX)
      result = VK_ERROR_FEATURE_NOT_PRESENT;
   VkMemoryAllocateInfo const image_alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = result == VK_SUCCESS ? image_requirements.size : 0,
      .memoryTypeIndex = image_type,
   };
   if (result == VK_SUCCESS)
      result = vkAllocateMemory(device, &image_alloc, NULL, &image_memory);
   if (result == VK_SUCCESS)
      result = vkBindImageMemory(device, image, image_memory, 0);
   if (result == VK_SUCCESS)
      result = vkMapMemory(device, image_memory, 0, VK_WHOLE_SIZE, 0, (void **)&image_mapping);
   VkImageSubresource const subresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubresourceLayout image_layout = {0};
   if (result == VK_SUCCESS) {
      VkImageSubresource target_subresource = subresource;
      target_subresource.mipLevel = mip_layer ? 1 : 0;
      target_subresource.arrayLayer = mip_layer ? 1 : 0;
      vkGetImageSubresourceLayout(device, image, &target_subresource, &image_layout);
      if (mip_layer)
         memset(image_mapping, 0xA5, image_requirements.size);
      for (uint32_t row = 0; row < block_rows; ++row)
         memcpy(image_mapping + image_layout.offset + row * image_layout.rowPitch,
                (to_buffer ? source : inverse) + row * block_columns * block_bytes,
                block_columns * block_bytes);
      if (mip_layer) {
         VkSubresourceLayout wrong_layout;
         vkGetImageSubresourceLayout(device, image, &subresource, &wrong_layout);
         for (uint32_t row = 0; row < block_rows; ++row)
            memset(image_mapping + wrong_layout.offset + row * wrong_layout.rowPitch, 0x5A,
                   block_columns * block_bytes);
      }
   }
   VkBufferCreateInfo const buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buffer_bytes,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   if (result == VK_SUCCESS)
      result = vkCreateBuffer(device, &buffer_info, NULL, &buffer);
   VkMemoryRequirements buffer_requirements = {0};
   if (result == VK_SUCCESS)
      vkGetBufferMemoryRequirements(device, buffer, &buffer_requirements);
   uint32_t buffer_type = UINT32_MAX;
   for (uint32_t i = 0; result == VK_SUCCESS && i < properties.memoryTypeCount; ++i)
      if ((buffer_requirements.memoryTypeBits & ((uint32_t)1 << i)) &&
          (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
         buffer_type = i;
         break;
      }
   if (result == VK_SUCCESS && buffer_type == UINT32_MAX)
      result = VK_ERROR_FEATURE_NOT_PRESENT;
   VkMemoryAllocateInfo const buffer_alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = result == VK_SUCCESS ? buffer_requirements.size : 0,
      .memoryTypeIndex = buffer_type,
   };
   if (result == VK_SUCCESS)
      result = vkAllocateMemory(device, &buffer_alloc, NULL, &buffer_memory);
   if (result == VK_SUCCESS)
      result = vkBindBufferMemory(device, buffer, buffer_memory, 0);
   if (result == VK_SUCCESS)
      result = vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&buffer_mapping);
   if (result == VK_SUCCESS) {
      if (to_buffer) {
         for (uint32_t i = 0; i < buffer_bytes; ++i)
            buffer_mapping[i] = inverse[i % image_bytes];
      } else {
         memcpy(buffer_mapping, source, buffer_bytes);
      }
      VkMappedMemoryRange ranges[2] = {
         {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = image_memory, .size = VK_WHOLE_SIZE},
         {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = buffer_memory, .size = VK_WHOLE_SIZE},
      };
      result = vkFlushMappedMemoryRanges(device, 2, ranges);
   }
   VkCommandPoolCreateInfo const pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                               .queueFamilyIndex = 0};
   VkCommandBufferAllocateInfo const alloc_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                    .commandBufferCount = 1};
   VkCommandBufferBeginInfo const begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   if (result == VK_SUCCESS)
      result = vkCreateCommandPool(device, &pool_info, NULL, &command_pool);
   if (result == VK_SUCCESS) {
      VkCommandBufferAllocateInfo info = alloc_info;
      info.commandPool = command_pool;
      result = vkAllocateCommandBuffers(device, &info, &command_buffer);
   }
   if (result == VK_SUCCESS)
      result = vkBeginCommandBuffer(command_buffer, &begin_info);
   VkImageMemoryBarrier const initial = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
      .dstAccessMask = to_buffer ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .baseMipLevel = mip_layer && !negative ? 1 : 0,
                           .levelCount = 1,
                           .baseArrayLayer = mip_layer && !negative ? 1 : 0,
                           .layerCount = 1},
   };
   VkBufferImageCopy const region = {
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = mip_layer && !negative ? 1 : 0,
                           .baseArrayLayer = mip_layer && !negative ? 1 : 0,
                           .layerCount = 1},
      .bufferOffset = negative ? 8 : 0,
      .imageExtent = {8, 8, 1},
   };
   if (result == VK_SUCCESS) {
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, NULL, 0, NULL, 1, &initial);
      if (to_buffer) {
         vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
         VkBufferMemoryBarrier const host = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer, .offset = 0, .size = VK_WHOLE_SIZE,
         };
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &host, 0, NULL);
      } else {
         vkCmdCopyBufferToImage(command_buffer, buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
         VkImageMemoryBarrier const host = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image, .subresourceRange = initial.subresourceRange,
         };
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 0, NULL, 1, &host);
      }
      result = vkEndCommandBuffer(command_buffer);
   }
   VkFenceCreateInfo const fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (result == VK_SUCCESS)
      result = vkCreateFence(device, &fence_info, NULL, &fence);
   VkSubmitInfo const submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                .commandBufferCount = 1, .pCommandBuffers = &command_buffer};
   if (result == VK_SUCCESS)
      result = vkQueueSubmit(queue, 1, &submit, fence);
   if (result == VK_SUCCESS)
      result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000));
   if (result == VK_SUCCESS) {
      VkMappedMemoryRange const invalidate = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                              .memory = image_memory, .size = VK_WHOLE_SIZE};
      result = vkInvalidateMappedMemoryRanges(device, 1, &invalidate);
   }
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 linear BC1 %s submission failed with %d\n",
              to_buffer ? (negative ? "readback-negative" : "readback")
                        : (negative ? "negative" : "roundtrip"), result);
      failures = 1;
   } else {
      uint32_t mismatches = 0;
      if (to_buffer) {
         for (uint32_t i = 0; i < buffer_bytes; ++i) {
            bool const copied = i >= (negative ? 8u : 0u) && i < (negative ? 40u : 32u);
            uint8_t const expected = copied ? source[i - (negative ? 8u : 0u)] : inverse[i % image_bytes];
            if (buffer_mapping[i] != expected)
               ++mismatches;
         }
      } else {
         for (uint32_t row = 0; row < block_rows; ++row)
            for (uint32_t col = 0; col < block_columns; ++col)
               for (uint32_t byte = 0; byte < block_bytes; ++byte) {
                  uint32_t const image_index = (row * block_columns + col) * block_bytes + byte;
                  uint8_t const expected = negative ? source[8 + image_index] : source[image_index];
                  if (image_mapping[image_layout.offset + row * image_layout.rowPitch +
                                    col * block_bytes + byte] != expected)
                     ++mismatches;
               }
      }
      uint32_t const expected_mismatches = negative && mip_layer ? image_bytes : 0;
      if (mismatches != expected_mismatches) {
         fprintf(stderr, "  RV710 linear BC1 %s observed %u mismatches, expected %u\n",
                 to_buffer ? (negative ? "readback negative control" : "readback")
                           : (negative ? "negative control" : "roundtrip"),
                 mismatches, expected_mismatches);
         failures = 1;
      } else {
         fprintf(stderr, "  RV710 linear BC1 %s completed%s\n",
                 to_buffer ? (negative ? "readback negative control" : "readback")
                           : (negative ? "negative control" : "roundtrip"),
                 negative ? " with expected buffer offset" : "");
      }
   }
   if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, NULL);
   if (command_buffer != VK_NULL_HANDLE) vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
   if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, NULL);
   if (image_mapping != NULL) vkUnmapMemory(device, image_memory);
   if (buffer_mapping != NULL) vkUnmapMemory(device, buffer_memory);
   if (image != VK_NULL_HANDLE) vkDestroyImage(device, image, NULL);
   if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, NULL);
   if (image_memory != VK_NULL_HANDLE) vkFreeMemory(device, image_memory, NULL);
   if (buffer_memory != VK_NULL_HANDLE) vkFreeMemory(device, buffer_memory, NULL);
   return failures;
}

/* This is deliberately opt-in: it is the first TeraScale 1 meta-draw/readback probe. The source
 * is a host-written linear image, avoiding the not-yet-portable buffer-to-image meta shader. Four
 * different texels and an inverse-pattern destination are the copy negative control: neither a
 * skipped draw nor a uniform/wrong-coordinate fetch can pass. For clear, every initial texel is
 * different from the expected clear result, so fence completion without a write also fails. It
 * does not validate tiled images, layers, non-RGBA8 formats, or the generic buffer-to-image
 * direction.
 */
enum rv710_linear_image_operation {
   RV710_LINEAR_IMAGE_READBACK,
   RV710_LINEAR_IMAGE_CLEAR,
   RV710_LINEAR_BUFFER_UPLOAD,
   RV710_TILED_IMAGE_ROUNDTRIP,
};

static uint32_t
check_rv710_linear_image_readback(VkPhysicalDevice const physical_device, VkDevice const device,
                                  VkQueue const queue,
                                  enum rv710_linear_image_operation const operation)
{
   char const * const macro_variant = getenv("TERAKAN_DEBUG_TERASCALE_1_MACROTILED_ROUNDTRIP");
   bool const meta_state_only = getenv("TERAKAN_DEBUG_TERASCALE_1_META_STATE_ONLY") != NULL;
   bool const offset_copy = operation == RV710_TILED_IMAGE_ROUNDTRIP && macro_variant &&
                            !strcmp(macro_variant, "offset");
   bool const mip_layer_copy = operation == RV710_TILED_IMAGE_ROUNDTRIP && macro_variant &&
                               !strcmp(macro_variant, "mip-layer");
   bool const mip_layer_negative = operation == RV710_TILED_IMAGE_ROUNDTRIP && macro_variant &&
                                   !strcmp(macro_variant, "mip-layer-negative");
   bool const mip_copy = operation == RV710_TILED_IMAGE_ROUNDTRIP && macro_variant &&
                         (!strcmp(macro_variant, "mip") || mip_layer_copy || mip_layer_negative);
   bool const layer_copy = operation == RV710_TILED_IMAGE_ROUNDTRIP && macro_variant &&
                           (!strcmp(macro_variant, "layer") || mip_layer_copy || mip_layer_negative);
   bool const layer_negative = operation == RV710_TILED_IMAGE_ROUNDTRIP && macro_variant &&
                               !strcmp(macro_variant, "layer-negative");
   bool const macrotiled = operation == RV710_TILED_IMAGE_ROUNDTRIP && macro_variant &&
                           (!strcmp(macro_variant, "1") || !strcmp(macro_variant, "edge") ||
                            offset_copy || mip_copy || layer_copy || layer_negative || mip_layer_negative);
   bool const macro_edge = macrotiled &&
                           (offset_copy || mip_copy || layer_copy || layer_negative || mip_layer_negative ||
                            !strcmp(macro_variant, "edge"));
   uint32_t const width = macro_edge ? 129 : macrotiled ? 128 : 2;
   uint32_t const height = macro_edge ? 65 : macrotiled ? 128 : 2;
   uint32_t const byte_count = width * height * 4;
   uint32_t source_words[129 * 128] = {
      UINT32_C(0x10203040),
      UINT32_C(0x55667788),
      UINT32_C(0x90abcdef),
      UINT32_C(0x13579bdf),
   };
   uint32_t inverse_words[129 * 128] = {
      ~UINT32_C(0x10203040),
      ~UINT32_C(0x55667788),
      ~UINT32_C(0x90abcdef),
      ~UINT32_C(0x13579bdf),
   };
   if (macrotiled) {
      for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
         source_words[pixel] = UINT32_C(0x10203040) + pixel * UINT32_C(0x01030507);
         inverse_words[pixel] = ~source_words[pixel];
      }
   }
   VkImage image = VK_NULL_HANDLE;
   VkImage tiled_image = VK_NULL_HANDLE;
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory image_memory = VK_NULL_HANDLE, buffer_memory = VK_NULL_HANDLE;
   VkDeviceMemory tiled_image_memory = VK_NULL_HANDLE;
   uint8_t * image_mapping = NULL;
   uint32_t * buffer_mapping = NULL;
   VkCommandPool command_pool = VK_NULL_HANDLE;
   VkCommandBuffer command_buffer = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   uint32_t failures = 0;
   VkPhysicalDeviceMemoryProperties memory_properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

   VkImageCreateInfo const image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {width, height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = operation == RV710_TILED_IMAGE_ROUNDTRIP
                          ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_GENERAL,
   };
   VkResult result = vkCreateImage(device, &image_info, NULL, &image);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 linear-readback vkCreateImage failed with %d\n", result);
      return 1;
   }
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);
   uint32_t image_memory_type = UINT32_MAX;
   for (uint32_t type = 0; type < memory_properties.memoryTypeCount; ++type) {
      if ((image_requirements.memoryTypeBits & ((uint32_t)1 << type)) &&
          (memory_properties.memoryTypes[type].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
         image_memory_type = type;
         break;
      }
   }
   if (image_memory_type == UINT32_MAX) {
      fprintf(stderr, "  RV710 linear-readback image has no host-visible memory type\n");
      failures = 1;
      goto cleanup;
   }
   VkMemoryAllocateInfo const image_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = image_memory_type,
   };
   result = vkAllocateMemory(device, &image_allocate_info, NULL, &image_memory);
   if (result == VK_SUCCESS)
      result = vkBindImageMemory(device, image, image_memory, 0);
   if (result == VK_SUCCESS)
      result = vkMapMemory(device, image_memory, 0, VK_WHOLE_SIZE, 0, (void **)&image_mapping);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 linear-readback image allocation/bind/map failed with %d\n", result);
      failures = 1;
      goto cleanup;
   }
   VkImageSubresource const subresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubresourceLayout image_layout;
   vkGetImageSubresourceLayout(device, image, &subresource, &image_layout);
   for (uint32_t y = 0; y < height; ++y)
      memcpy(image_mapping + image_layout.offset + y * image_layout.rowPitch,
             operation == RV710_LINEAR_BUFFER_UPLOAD || operation == RV710_TILED_IMAGE_ROUNDTRIP
                ? &inverse_words[y * width]
                : &source_words[y * width],
             width * sizeof(uint32_t));
   VkMappedMemoryRange const image_flush = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = image_memory,
      .size = VK_WHOLE_SIZE,
   };
   if (vkFlushMappedMemoryRanges(device, 1, &image_flush) != VK_SUCCESS) {
      fprintf(stderr, "  RV710 linear-readback image flush failed\n");
      failures = 1;
      goto cleanup;
   }

   if (operation == RV710_TILED_IMAGE_ROUNDTRIP) {
      VkImageCreateInfo tiled_image_info = image_info;
      tiled_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
      tiled_image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (mip_copy) {
         tiled_image_info.extent.width *= 2;
         tiled_image_info.extent.height *= 2;
         tiled_image_info.mipLevels = 2;
      }
      if (layer_copy || layer_negative)
         tiled_image_info.arrayLayers = 2;
      result = vkCreateImage(device, &tiled_image_info, NULL, &tiled_image);
      VkMemoryRequirements tiled_requirements;
      if (result == VK_SUCCESS)
         vkGetImageMemoryRequirements(device, tiled_image, &tiled_requirements);
      uint32_t tiled_memory_type = UINT32_MAX;
      for (uint32_t type = 0; result == VK_SUCCESS && type < memory_properties.memoryTypeCount;
           ++type) {
         if (tiled_requirements.memoryTypeBits & ((uint32_t)1 << type)) {
            tiled_memory_type = type;
            break;
         }
      }
      if (result == VK_SUCCESS && tiled_memory_type == UINT32_MAX)
         result = VK_ERROR_FEATURE_NOT_PRESENT;
      VkMemoryAllocateInfo const tiled_allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = result == VK_SUCCESS ? tiled_requirements.size : 0,
         .memoryTypeIndex = tiled_memory_type,
      };
      if (result == VK_SUCCESS && tiled_memory_type != UINT32_MAX)
         result = vkAllocateMemory(device, &tiled_allocate_info, NULL, &tiled_image_memory);
      if (result == VK_SUCCESS)
         result = vkBindImageMemory(device, tiled_image, tiled_image_memory, 0);
      if (result != VK_SUCCESS) {
         fprintf(stderr, "  RV710 tiled-roundtrip image allocation/bind failed with %d\n", result);
         failures = 1;
         goto cleanup;
      }
   }

   VkBufferCreateInfo const buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = byte_count,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   result = vkCreateBuffer(device, &buffer_info, NULL, &buffer);
   VkMemoryRequirements buffer_requirements;
   if (result == VK_SUCCESS)
      vkGetBufferMemoryRequirements(device, buffer, &buffer_requirements);
   uint32_t buffer_memory_type = UINT32_MAX;
   for (uint32_t type = 0; type < memory_properties.memoryTypeCount; ++type) {
      if ((buffer_requirements.memoryTypeBits & ((uint32_t)1 << type)) &&
          (memory_properties.memoryTypes[type].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
         buffer_memory_type = type;
         break;
      }
   }
   if (result != VK_SUCCESS || buffer_memory_type == UINT32_MAX) {
      fprintf(stderr, "  RV710 linear-readback buffer creation/type selection failed with %d\n",
              result);
      failures = 1;
      goto cleanup;
   }
   VkMemoryAllocateInfo const buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buffer_requirements.size,
      .memoryTypeIndex = buffer_memory_type,
   };
   result = vkAllocateMemory(device, &buffer_allocate_info, NULL, &buffer_memory);
   if (result == VK_SUCCESS)
      result = vkBindBufferMemory(device, buffer, buffer_memory, 0);
   if (result == VK_SUCCESS)
      result = vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&buffer_mapping);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 linear-readback buffer allocation/bind/map failed with %d\n",
              result);
      failures = 1;
      goto cleanup;
   }
   for (uint32_t texel = 0; texel < width * height; ++texel)
      buffer_mapping[texel] =
         operation == RV710_LINEAR_BUFFER_UPLOAD || operation == RV710_TILED_IMAGE_ROUNDTRIP
            ? source_words[texel]
            : inverse_words[texel];
   VkMappedMemoryRange const buffer_flush = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = buffer_memory,
      .size = VK_WHOLE_SIZE,
   };
   if (vkFlushMappedMemoryRanges(device, 1, &buffer_flush) != VK_SUCCESS) {
      fprintf(stderr, "  RV710 linear-readback buffer flush failed\n");
      failures = 1;
      goto cleanup;
   }

   VkCommandPoolCreateInfo const command_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandBufferAllocateInfo command_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandBufferCount = 1,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
   };
   VkCommandBufferBeginInfo const begin_info = {.sType =
                                                   VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VkBufferImageCopy const region = {
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = mip_copy ? 1 : 0,
                           .baseArrayLayer = (layer_copy || layer_negative || mip_layer_negative) ? 1 : 0,
                           .layerCount = 1},
      .imageExtent = {width, height, 1},
   };
   VkClearColorValue const clear_value = {.float32 = {0.0f, 1.0f, 0.0f, 1.0f}};
   uint32_t const clear_word = UINT32_C(0xff00ff00);
   VkImageSubresourceRange const clear_range = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .levelCount = 1,
      .layerCount = 1,
   };
   VkFenceCreateInfo const fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   result = vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool);
   if (result == VK_SUCCESS) {
      command_buffer_info.commandPool = command_pool;
      result = vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer);
   }
   if (result == VK_SUCCESS)
      result = vkBeginCommandBuffer(command_buffer, &begin_info);
   if (result == VK_SUCCESS) {
      /* The ICD exposes the legacy entrypoint; its generated common wrapper reaches the same
       * CmdCopyImageToBuffer2 implementation. Calling the core-1.3 entrypoint directly here
       * instead invokes a NULL dispatch slot on this driver and tests no meta code at all.
       */
      if (operation == RV710_LINEAR_IMAGE_CLEAR) {
         vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, &clear_value, 1,
                              &clear_range);
         /* Queue/fence completion alone does not make dirty CB cache lines visible to a host
          * mapping. Exercise the driver's ordinary transfer-to-host dependency path so this probe
          * validates both the draw and the TeraScale 1 cache tail rather than merely rereading the
          * pre-draw host cache contents. */
         VkImageMemoryBarrier const host_read_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = clear_range,
         };
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1,
                              &host_read_barrier);
      } else if (operation == RV710_LINEAR_IMAGE_READBACK) {
         vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
      } else if (operation == RV710_LINEAR_BUFFER_UPLOAD) {
         vkCmdCopyBufferToImage(command_buffer, buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
         VkImageMemoryBarrier const host_read_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = clear_range,
         };
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1,
                              &host_read_barrier);
      } else {
         /* The default 2x2 optimal image is 1D microtiled. The optional 128x128 and 129x65 cases
          * cross macrotiles, with the latter also exercising row padding. The mip variant instead
          * addresses level 1. None tests bank rotation between layers. The linear destination
          * keeps inverse host sentinels.
          */
         VkImageMemoryBarrier initial_barriers[2] = {
            {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
             .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
             .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
             .newLayout = VK_IMAGE_LAYOUT_GENERAL,
             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
             .image = tiled_image, .subresourceRange = clear_range},
            {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
             .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
             .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
             .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
             .newLayout = VK_IMAGE_LAYOUT_GENERAL,
             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
             .image = image, .subresourceRange = clear_range},
         };
         initial_barriers[0].subresourceRange.levelCount = mip_copy ? 2 : 1;
         initial_barriers[0].subresourceRange.layerCount = (layer_copy || layer_negative) ? 2 : 1;
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2,
                              initial_barriers);
         if (mip_copy) {
            VkClearColorValue const other_mip_colour = {.float32 = {1.0f, 0.0f, 1.0f, 1.0f}};
            vkCmdClearColorImage(command_buffer, tiled_image, VK_IMAGE_LAYOUT_GENERAL,
                                 &other_mip_colour, 1, &clear_range);
         }
         if (layer_copy || layer_negative) {
            VkClearColorValue const other_layer_colour = {.float32 = {1.0f, 0.0f, 1.0f, 1.0f}};
            VkImageSubresourceRange const other_layer_range = {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseArrayLayer = 0, .layerCount = 1};
            vkCmdClearColorImage(command_buffer, tiled_image, VK_IMAGE_LAYOUT_GENERAL,
                                 &other_layer_colour, 1, &other_layer_range);
         }
         vkCmdCopyBufferToImage(command_buffer, buffer, tiled_image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                &region);
         VkImageMemoryBarrier barriers[2] = {
            {
               .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
               .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
               .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
               .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
               .newLayout = VK_IMAGE_LAYOUT_GENERAL,
               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
               .image = tiled_image,
               .subresourceRange = clear_range,
            },
            {
               .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
               .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
               .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
               .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
               .newLayout = VK_IMAGE_LAYOUT_GENERAL,
               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
               .image = image,
               .subresourceRange = clear_range,
            },
         };
         barriers[0].subresourceRange.levelCount = mip_copy ? 2 : 1;
         barriers[0].subresourceRange.layerCount =
            (layer_copy || layer_negative || mip_layer_negative) ? 2 : 1;
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, barriers);
         VkImageCopy const image_region = {
            .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .mipLevel = mip_layer_negative ? 0 : mip_copy ? 1 : 0,
                               .baseArrayLayer = mip_layer_negative ? 0 : layer_copy ? 1 : 0, .layerCount = 1},
            .srcOffset = {offset_copy ? 1 : 0, offset_copy ? 2 : 0, 0},
            .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
            .dstOffset = {offset_copy ? 3 : 0, offset_copy ? 1 : 0, 0},
            .extent = {offset_copy ? width - 4 : width, offset_copy ? height - 3 : height, 1},
         };
         vkCmdCopyImage(command_buffer, tiled_image, VK_IMAGE_LAYOUT_GENERAL, image,
                        VK_IMAGE_LAYOUT_GENERAL, 1, &image_region);
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1, &barriers[1]);
      }
      result = vkEndCommandBuffer(command_buffer);
   }
   if (result == VK_SUCCESS)
      result = vkCreateFence(device, &fence_info, NULL, &fence);
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   if (result == VK_SUCCESS)
      result = vkQueueSubmit(queue, 1, &submit_info, fence);
   if (result == VK_SUCCESS)
      result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000));
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  RV710 linear image %s submission failed with %d\n",
              operation == RV710_LINEAR_IMAGE_CLEAR     ? "clear"
              : operation == RV710_LINEAR_BUFFER_UPLOAD ? "buffer upload"
              : operation == RV710_TILED_IMAGE_ROUNDTRIP ? "tiled roundtrip"
                                                        : "readback",
              result);
      failures = 1;
      goto cleanup;
   }
   if (operation != RV710_LINEAR_IMAGE_READBACK) {
      VkMappedMemoryRange const image_invalidate = {
         .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
         .memory = image_memory,
         .size = VK_WHOLE_SIZE,
      };
      if (vkInvalidateMappedMemoryRanges(device, 1, &image_invalidate) != VK_SUCCESS) {
         fprintf(stderr, "  RV710 linear image clear invalidate failed\n");
         failures = 1;
         goto cleanup;
      }
      uint32_t mismatch_count = 0;
      for (uint32_t y = 0; y < height; ++y) {
         uint32_t const * const row =
            (uint32_t const *)(image_mapping + image_layout.offset + y * image_layout.rowPitch);
         for (uint32_t x = 0; x < width; ++x) {
            uint32_t expected =
               operation == RV710_LINEAR_IMAGE_CLEAR && !meta_state_only
                  ? clear_word
                  : source_words[y * width + x];
            if (offset_copy) {
               /* Independent oracle for source (1,2), destination (3,1), size (125,62).
                * Check inverse sentinels everywhere outside the destination rectangle too.
                */
               expected = x >= 3 && x < width - 1 && y >= 1 && y < height - 2
                             ? source_words[(y + 1) * width + x - 2]
                             : inverse_words[y * width + x];
            }
            if (row[x] != expected) {
               ++mismatch_count;
               if (!layer_negative && !mip_layer_negative) {
                  fprintf(stderr,
                          "  RV710 linear image %s mismatch at (%u,%u): got 0x%08x expected 0x%08x\n",
                          operation == RV710_LINEAR_IMAGE_CLEAR
                             ? (meta_state_only ? "state-only clear" : "clear")
                          : operation == RV710_TILED_IMAGE_ROUNDTRIP ? "tiled roundtrip"
                                                                     : "buffer upload",
                          x, y,
                          row[x], expected);
               }
               failures = 1;
            }
         }
      }
      if ((layer_negative || mip_layer_negative) && mismatch_count == width * height) {
         /* Negative hardware control: layer 0 was deliberately magenta, while the oracle expects
          * layer 1's distinct pattern. A full mismatch proves baseArrayLayer is observable. */
         fprintf(stderr, "  RV710 %s-negative control observed %u expected mismatches\n",
                 mip_layer_negative ? "mip-layer" : "layer", mismatch_count);
         failures = 0;
      }
      if (!failures)
         fprintf(stderr, "  RV710 linear image %ux%u %s readback completed\n", width, height,
                 operation == RV710_LINEAR_IMAGE_CLEAR
                    ? (meta_state_only ? "state-only clear" : "clear")
                 : operation == RV710_TILED_IMAGE_ROUNDTRIP ? "tiled roundtrip"
                                                            : "buffer upload");
      goto cleanup;
   }
   VkMappedMemoryRange const buffer_invalidate = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = buffer_memory,
      .size = VK_WHOLE_SIZE,
   };
   if (vkInvalidateMappedMemoryRanges(device, 1, &buffer_invalidate) != VK_SUCCESS) {
      fprintf(stderr, "  RV710 linear-readback buffer invalidate failed\n");
      failures = 1;
      goto cleanup;
   }
   for (uint32_t texel = 0; texel < width * height; ++texel) {
      if (buffer_mapping[texel] != source_words[texel]) {
         fprintf(stderr, "  RV710 linear-readback mismatch at %u: got 0x%08x expected 0x%08x\n",
                 texel, buffer_mapping[texel], source_words[texel]);
         failures = 1;
      }
   }
   if (!failures)
      fprintf(stderr, "  RV710 linear image 2x2 readback completed\n");

cleanup:
   vkDestroyFence(device, fence, NULL);
   if (command_buffer != VK_NULL_HANDLE)
      vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
   vkDestroyCommandPool(device, command_pool, NULL);
   if (buffer_mapping != NULL)
      vkUnmapMemory(device, buffer_memory);
   vkDestroyBuffer(device, buffer, NULL);
   vkFreeMemory(device, buffer_memory, NULL);
   if (image_mapping != NULL)
      vkUnmapMemory(device, image_memory);
   vkDestroyImage(device, image, NULL);
   vkFreeMemory(device, image_memory, NULL);
   vkDestroyImage(device, tiled_image, NULL);
   vkFreeMemory(device, tiled_image_memory, NULL);
   return failures;
}

static uint32_t
check_terascale_1_image_layout(VkDevice const device, VkImageCreateInfo const * const image_info,
                               char const * const name, bool const check_linear_layout,
                               VkDeviceSize const minimum_size_exclusive)
{
   uint32_t failures = 0;
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;

   VkResult const create_result = vkCreateImage(device, image_info, NULL, &image);
   if (create_result != VK_SUCCESS) {
      fprintf(stderr, "  %s vkCreateImage failed with %d\n", name, create_result);
      return 1;
   }

   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(device, image, &requirements);
   if (requirements.size == 0 || requirements.alignment < 256 ||
       (requirements.alignment & (requirements.alignment - 1)) != 0) {
      fprintf(stderr, "  %s invalid memory requirements: size=%llu alignment=%llu\n", name,
              (unsigned long long)requirements.size, (unsigned long long)requirements.alignment);
      ++failures;
      goto cleanup;
   }
   if (requirements.size <= minimum_size_exclusive) {
      fprintf(stderr, "  %s is missing required auxiliary storage: size=%llu must exceed %llu\n",
              name, (unsigned long long)requirements.size,
              (unsigned long long)minimum_size_exclusive);
      ++failures;
      goto cleanup;
   }

   if (check_linear_layout) {
      VkImageSubresource const subresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      };
      VkSubresourceLayout layout;
      vkGetImageSubresourceLayout(device, image, &subresource, &layout);
      uint64_t const expected_size = layout.rowPitch * image_info->extent.height;
      if (layout.offset != 0 || layout.rowPitch != requirements.alignment ||
          layout.arrayPitch != layout.size || layout.depthPitch != layout.size ||
          layout.size != expected_size) {
         fprintf(stderr,
                 "  %s unexpected linear layout: offset=%llu row=%llu array=%llu depth=%llu "
                 "size=%llu expected_size=%llu requirement_alignment=%llu\n",
                 name, (unsigned long long)layout.offset, (unsigned long long)layout.rowPitch,
                 (unsigned long long)layout.arrayPitch, (unsigned long long)layout.depthPitch,
                 (unsigned long long)layout.size, (unsigned long long)expected_size,
                 (unsigned long long)requirements.alignment);
         ++failures;
      }
   }

   uint32_t const memory_type = first_memory_type(requirements.memoryTypeBits);
   if (memory_type == UINT32_MAX) {
      fprintf(stderr, "  %s has no compatible memory type\n", name);
      ++failures;
      goto cleanup;
   }
   VkMemoryAllocateInfo const allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   VkResult const allocate_result = vkAllocateMemory(device, &allocate_info, NULL, &memory);
   if (allocate_result != VK_SUCCESS) {
      fprintf(stderr, "  %s vkAllocateMemory failed with %d\n", name, allocate_result);
      ++failures;
      goto cleanup;
   }
   VkResult const bind_result = vkBindImageMemory(device, image, memory, 0);
   if (bind_result != VK_SUCCESS) {
      fprintf(stderr, "  %s vkBindImageMemory failed with %d\n", name, bind_result);
      ++failures;
   } else {
      fprintf(stderr, "  %s create/layout/allocate/bind succeeded (size=%llu alignment=%llu)\n",
              name, (unsigned long long)requirements.size,
              (unsigned long long)requirements.alignment);
   }

cleanup:
   vkDestroyImage(device, image, NULL);
   if (memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, memory, NULL);
   }
   return failures;
}

static uint32_t
check_terascale_1_image_layouts(VkDevice const device)
{
   VkImageCreateInfo const linear = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {37, 19, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImageCreateInfo optimal = linear;
   optimal.extent = (VkExtent3D){300, 50, 1};
   optimal.mipLevels = 3;
   optimal.tiling = VK_IMAGE_TILING_OPTIMAL;
   optimal.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

   VkImageCreateInfo bc1 = optimal;
   bc1.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
   bc1.extent = (VkExtent3D){1023, 511, 1};
   bc1.mipLevels = 4;

   VkImageCreateInfo expand_3x = linear;
   expand_3x.format = VK_FORMAT_R32G32B32_SFLOAT;
   expand_3x.extent = (VkExtent3D){81, 5, 1};
   expand_3x.mipLevels = 2;

   /* 1024x1024 RGBA8 2x has an exactly 8 MiB main surface on the RV610 used for this test. R600's
    * classic r600_texture_get_fmask_info() additionally allocates an over-sized FMASK, and
    * r600_texture_get_cmask_info() a CMASK. Requiring strictly more than the main surface catches
    * the former TeraScale 1 stub, which reported exactly 8 MiB and still advertised MSAA. This is
    * an allocation/layout test only; it does not claim that rendering or resolving is validated
    * while queue submission remains disabled.
    */
   VkImageCreateInfo msaa_color = optimal;
   msaa_color.extent = (VkExtent3D){1024, 1024, 1};
   msaa_color.mipLevels = 1;
   msaa_color.samples = VK_SAMPLE_COUNT_2_BIT;
   msaa_color.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

   /* DB uses the same 2D tiled pitch/slice encoding at every sample count on R6xx/R7xx; sample
    * count is PA_SC state. This only validates allocation/binding of the 4x depth surface -- no
    * DB packet can be submitted until the global TeraScale 1 queue guard is removed. */
   VkImageCreateInfo msaa_depth = msaa_color;
   msaa_depth.format = VK_FORMAT_D32_SFLOAT;
   msaa_depth.samples = VK_SAMPLE_COUNT_4_BIT;
   msaa_depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

   uint32_t failures = 0;
   failures += check_terascale_1_image_layout(device, &linear, "linear RGBA8", true, 0);
   failures +=
      check_terascale_1_image_layout(device, &optimal, "2D-tiled RGBA8 mip chain", false, 0);
   failures += check_terascale_1_image_layout(device, &bc1, "2D-tiled BC1 mip chain", false, 0);
   failures +=
      check_terascale_1_image_layout(device, &expand_3x, "linear R32G32B32 mip chain", false, 0);
   failures += check_terascale_1_image_layout(device, &msaa_color, "2x MSAA RGBA8 metadata", false,
                                              UINT64_C(1024) * 1024 * 4 * 2);
   failures += check_terascale_1_image_layout(device, &msaa_depth, "4x MSAA D32", false, 0);
   return failures;
}

static uint32_t
check_terascale_1_application_graphics_shader_compile(VkDevice const device,
                                                      VkSampleCountFlagBits const samples)
{
   VkShaderModule vertex_module = VK_NULL_HANDLE, fragment_module = VK_NULL_HANDLE;
   VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
   VkRenderPass render_pass = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   uint32_t failures = 0;

   VkShaderModuleCreateInfo const vertex_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(application_vertex_spirv),
      .pCode = application_vertex_spirv,
   };
   VkResult result = vkCreateShaderModule(device, &vertex_module_info, NULL, &vertex_module);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  application vertex vkCreateShaderModule failed with %d\n", result);
      return 1;
   }
   VkShaderModuleCreateInfo const fragment_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(application_fragment_spirv),
      .pCode = application_fragment_spirv,
   };
   result = vkCreateShaderModule(device, &fragment_module_info, NULL, &fragment_module);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  application fragment vkCreateShaderModule failed with %d\n", result);
      failures = 1;
      goto cleanup;
   }

   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   result = vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &pipeline_layout);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  application vkCreatePipelineLayout failed with %d\n", result);
      failures = 1;
      goto cleanup;
   }

   VkAttachmentDescription const attachment = {
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = samples,
      .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   VkAttachmentReference const color_reference = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   VkSubpassDescription const subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_reference,
   };
   VkRenderPassCreateInfo const render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   result = vkCreateRenderPass(device, &render_pass_info, NULL, &render_pass);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  application vkCreateRenderPass failed with %d\n", result);
      failures = 1;
      goto cleanup;
   }

   VkPipelineShaderStageCreateInfo const stages[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex_module,
         .pName = "main",
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment_module,
         .pName = "main",
      },
   };
   VkVertexInputBindingDescription const vertex_binding = {
      .binding = 0,
      .stride = 16,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
   };
   VkVertexInputAttributeDescription const vertex_attribute = {
      .location = 0,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32A32_UINT,
   };
   VkPipelineVertexInputStateCreateInfo const vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &vertex_binding,
      .vertexAttributeDescriptionCount = 1,
      .pVertexAttributeDescriptions = &vertex_attribute,
   };
   VkPipelineInputAssemblyStateCreateInfo const input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport const viewport = {.width = 1.0F, .height = 1.0F, .maxDepth = 1.0F};
   VkRect2D const scissor = {.extent = {1, 1}};
   VkPipelineViewportStateCreateInfo const viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &viewport,
      .scissorCount = 1,
      .pScissors = &scissor,
   };
   VkPipelineRasterizationStateCreateInfo const rasterization = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .lineWidth = 1.0F,
   };
   VkPipelineMultisampleStateCreateInfo const multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = samples,
   };
   VkPipelineColorBlendAttachmentState const blend_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo const blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
   };
   VkGraphicsPipelineCreateInfo const pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterization,
      .pMultisampleState = &multisample,
      .pColorBlendState = &blend,
      .layout = pipeline_layout,
      .renderPass = render_pass,
   };
   result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  TeraScale 1 %ux application VS/FS pipeline compilation failed with %d\n",
              (unsigned)samples, result);
      failures = 1;
   } else {
      fprintf(stderr, "  TeraScale 1 %ux application VS/FS pipeline compilation succeeded\n",
              (unsigned)samples);
   }

cleanup:
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyRenderPass(device, render_pass, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyShaderModule(device, fragment_module, NULL);
   vkDestroyShaderModule(device, vertex_module, NULL);
   return failures;
}

int
main(void)
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-terascale-1-enumeration-test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   VK_CHECK(vkCreateInstance(&instance_info, NULL, &instance));

   uint32_t physical_device_count = 0;
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, NULL));
   VkPhysicalDevice physical_devices[8];
   if (physical_device_count > 8) {
      physical_device_count = 8;
   }
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices));

   uint32_t terascale_1_devices_checked = 0;
   uint32_t failures = 0;
   for (uint32_t device_index = 0; device_index < physical_device_count; ++device_index) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(physical_devices[device_index], &properties);
      if (strstr(properties.deviceName, "TeraScale 1") == NULL) {
         continue;
      }
      ++terascale_1_devices_checked;
      fprintf(stderr, "found %s (vendor=0x%04x device=0x%04x)\n", properties.deviceName,
              properties.vendorID, properties.deviceID);

      if (properties.vendorID != 0x1002) {
         fprintf(stderr, "  vendorID is 0x%04x, expected 0x1002 (ATI/AMD)\n", properties.vendorID);
         ++failures;
      }
      if (properties.apiVersion == 0) {
         fprintf(stderr, "  apiVersion is 0\n");
         ++failures;
      }
      VkPhysicalDeviceSampleLocationsPropertiesEXT sample_location_properties = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT,
      };
      VkPhysicalDeviceProperties2 properties_2 = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
         .pNext = &sample_location_properties,
      };
      vkGetPhysicalDeviceProperties2(physical_devices[device_index], &properties_2);
      if (sample_location_properties.maxSampleLocationGridSize.width != 1 ||
          sample_location_properties.maxSampleLocationGridSize.height != 1) {
         fprintf(stderr, "  TeraScale 1 sample-location grid is %ux%u, expected 1x1\n",
                 sample_location_properties.maxSampleLocationGridSize.width,
                 sample_location_properties.maxSampleLocationGridSize.height);
         ++failures;
      }

      float const priority = 1.0F;
      VkDeviceQueueCreateInfo const queue_info = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = 0,
         .queueCount = 1,
         .pQueuePriorities = &priority,
      };
      VkDeviceCreateInfo const device_info = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos = &queue_info,
      };
      VkDevice device;
      VkResult const create_result =
         vkCreateDevice(physical_devices[device_index], &device_info, NULL, &device);
      if (create_result != VK_SUCCESS) {
         fprintf(stderr, "  vkCreateDevice failed with %d on hardware-validated TeraScale 1\n",
                 create_result);
         ++failures;
      } else {
         fprintf(stderr, "  minimal TeraScale 1 vkCreateDevice succeeded\n");

         failures += check_terascale_1_image_layouts(device);
         failures +=
            check_terascale_1_application_graphics_shader_compile(device, VK_SAMPLE_COUNT_1_BIT);
         failures +=
            check_terascale_1_application_graphics_shader_compile(device, VK_SAMPLE_COUNT_4_BIT);

         VkQueue queue;
         vkGetDeviceQueue(device, 0, 0, &queue);
         if (!terascale_1_submit_opted_in()) {
            VkSubmitInfo const empty_submit = {
               .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            };
            VkResult const submit_result = vkQueueSubmit(queue, 1, &empty_submit, VK_NULL_HANDLE);
            if (submit_result != VK_ERROR_DEVICE_LOST) {
               fprintf(stderr,
                       "  guarded TeraScale 1 queue submission returned %d, expected "
                       "VK_ERROR_DEVICE_LOST\n",
                       submit_result);
               ++failures;
            } else {
               fprintf(stderr, "  TeraScale 1 queue submission remains safely disabled\n");
            }
         } else if (properties.deviceID == 0x954f) {
            char const * const bc1_mode = terascale_1_bc1_roundtrip_mode();
            failures +=
               bc1_mode != NULL
                  ? check_rv710_linear_bc1_roundtrip(physical_devices[device_index], device, queue,
                                                     !strcmp(bc1_mode, "negative") ||
                                                        !strcmp(bc1_mode, "readback-negative") ||
                                                        !strcmp(bc1_mode, "readback-mip-layer-negative"),
                                                     !strncmp(bc1_mode, "readback", 8),
                                                     strstr(bc1_mode, "mip-layer") != NULL)
               : terascale_1_linear_image_readback_opted_in() ||
                     terascale_1_linear_image_clear_opted_in() ||
                     terascale_1_linear_buffer_upload_opted_in() ||
                     terascale_1_tiled_image_roundtrip_opted_in()
                  ? check_rv710_linear_image_readback(
                       physical_devices[device_index], device, queue,
                       terascale_1_linear_image_clear_opted_in()     ? RV710_LINEAR_IMAGE_CLEAR
                       : terascale_1_tiled_image_roundtrip_opted_in()
                          ? RV710_TILED_IMAGE_ROUNDTRIP
                       : terascale_1_linear_buffer_upload_opted_in() ? RV710_LINEAR_BUFFER_UPLOAD
                                                                     : RV710_LINEAR_IMAGE_READBACK)
               : terascale_1_cp_dma_unaligned_copy_opted_in()
                  ? check_rv710_cp_dma_buffer_copy(physical_devices[device_index], device, queue,
                                                   false, true)
               : terascale_1_cp_dma_fill_opted_in()
                  ? check_rv710_cp_dma_buffer_copy(physical_devices[device_index], device, queue,
                                                   true, false)
               : terascale_1_cp_dma_copy_opted_in()
                  ? check_rv710_cp_dma_buffer_copy(physical_devices[device_index], device, queue,
                                                   false, false)
               : terascale_1_signal_only_opted_in() ? check_rv710_signal_only_submit(device, queue)
                                                    : check_rv710_empty_submit(device, queue);
         } else {
            fprintf(stderr,
                    "  TeraScale 1 submit opt-in deliberately skips non-RV710 device 0x%04x\n",
                    properties.deviceID);
         }
      }
      if (create_result == VK_SUCCESS) {
         vkDestroyDevice(device, NULL);
      }
   }

   if (terascale_1_devices_checked == 0) {
      printf("terascale_1_enumeration: no TeraScale 1 device present on this machine, nothing to "
             "check, PASS\n");
   } else {
      printf("terascale_1_enumeration: checked=%u bad=%u %s\n", terascale_1_devices_checked,
             failures, failures == 0 ? "PASS" : "FAIL");
   }

   vkDestroyInstance(instance, NULL);
   return failures == 0 ? 0 : 1;
}
