/*
 * Copyright © 2026 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terakan_buffer.h"
#include "terakan_command_buffer.h"
#include "terakan_entrypoints.h"
#include "terakan_vk_state.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "util/u_endian.h"
#include "util/u_math.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBindIndexBuffer2(VkCommandBuffer const commandBuffer, VkBuffer const bufferHandle,
                            VkDeviceSize const offset, VkDeviceSize const size,
                            VkIndexType const indexType)
{
   bool const index_type_32_bit = indexType == VK_INDEX_TYPE_UINT32;
   uint32_t const index_type = index_type_32_bit
                                  ? TERAKAN_HW_CONFIG_DRAW_VGT_DMA_INDEX_TYPE_32_HOST_ENDIAN
                                  : TERAKAN_HW_CONFIG_DRAW_VGT_DMA_INDEX_TYPE_16_HOST_ENDIAN;

   struct terakan_hw_config_draw_vgt_dma_index_buffer index_buffer = {};
   struct terakan_buffer const * const buffer = terakan_buffer_from_handle(bufferHandle);
   /* #MemoryIntegrity is mostly handled deeper, just set up the setter's arguments safely. */
   if (buffer != NULL && offset <= buffer->vk.size) {
      index_buffer.bo = buffer->bo;
      index_buffer.va = buffer->va + offset;
      uint64_t const size_indices_u64 =
         vk_buffer_range(&buffer->vk, offset, size) >> (index_type_32_bit ? 2 : 1);
      index_buffer.size_indices = (uint32_t)MIN2(size_indices_u64, UINT32_MAX);
   }

   struct terakan_app_config_draw * const config =
      &terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx->app_config_draw;

   terakan_app_config_draw_set_vgt_dma_index_buffer(config, index_buffer, index_type);

   /* The `vkCmdSetPrimitiveRestartIndexEXT` reference in the Vulkan 1.4.352 specification says:
    *
    *     "Binding an index buffer invalidates the custom index value."
    */
   terakan_app_config_draw_set_vgt_dma_index_buffer_multi_prim_reset_index(
      config, index_type_32_bit ? 0xFFFFFFFFu : 0xFFFFu);
}

/* TODO(Triang3l): `vkCmdSetPrimitiveRestartIndexEXT`. */

static void
terakan_vk_draw_set_vertex_instance_offsets(struct terakan_gfx_command_writer * const command_writer,
                                            uint32_t const vertex_offset,
                                            uint32_t const instance_offset)
{
   terakan_app_config_draw_set_vgt_index_offset(&command_writer->app_config_draw, vertex_offset);

   /* The instance offset is not needed by internal draws, modify `hw_config` directly. */
   terakan_hw_config_shared_sq_vtx_start_inst_loc(&command_writer->hw_config_shared,
                                                  instance_offset);

   /* Set driver constants for SFN's gl_BaseVertex/gl_BaseInstance/gl_DrawID.
    * The SFN backend reads these from the push constants kcache buffer (index 15).
    */
   struct terakan_push_constants_state * const pc = &command_writer->push_constants_state;
   pc->driver_constants.base_vertex = vertex_offset;
   pc->driver_constants.base_instance = instance_offset;
   pc->driver_constants_modified |=
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_BASE_VERTEX) |
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_BASE_INSTANCE);
}

static void
terakan_vk_before_draw(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_vk_state_dynamic_apply(command_writer);

   terakan_gfx_command_writer_before_app_draw(command_writer);
}

static uint32_t
terakan_vk_buffer_bo_relative_offset(struct terakan_buffer const * const buffer,
                                     VkDeviceSize const offset)
{
   /* DRM Radeon relocates SET_BASE to the BO start without adding the packet offset. */
   return (uint32_t)(buffer->va - buffer->bo->va + offset);
}

static void
terakan_vk_draw_emit_set_base(struct terakan_gfx_command_writer * const command_writer,
                              struct terakan_bo const * const bo)
{
   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 4, 1, 0, 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(EG_PKT3_SET_BASE, 2, 0);
   *packet++ = EG_DRAW_INDEX_INDIRECT_PATCH_TABLE_BASE;
   uint32_t const * const packet_va = packet;
   *packet++ = (uint32_t)bo->va;
   *packet++ = (bo->va >> 32) & 0xFF;
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_va, packet_va + 1, TERASCALE_WDDM_PATCH_IDS_SET_BASE_LO,
      TERASCALE_WDDM_PATCH_IDS_SET_BASE_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer, bo, true,
                                                false, TERAKAN_BO_PRIORITY_DRAW_INDIRECT));
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_vk_draw_emit_draw_indirect(struct terakan_gfx_command_writer * const command_writer,
                                   uint32_t const bo_relative_offset, bool const indexed)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 3);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(indexed ? EG_PKT3_DRAW_INDEX_INDIRECT : EG_PKT3_DRAW_INDIRECT, 1, 0);
   *packet++ = bo_relative_offset;
   *packet++ = S_0287F0_SOURCE_SELECT(indexed ? V_0287F0_DI_SRC_SEL_DMA
                                             : V_0287F0_DI_SRC_SEL_AUTO_INDEX);
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_vk_cmd_draw_indirect(VkCommandBuffer const commandBuffer, VkBuffer const bufferHandle,
                             VkDeviceSize const offset, uint32_t const drawCount,
                             uint32_t stride, bool const indexed)
{
   if (unlikely(drawCount == 0)) {
      return;
   }

   struct terakan_buffer const * const buffer = terakan_buffer_from_handle(bufferHandle);
   if (unlikely(buffer == NULL || buffer->bo == NULL)) {
      return;
   }

   uint32_t const min_stride =
      indexed ? (uint32_t)sizeof(VkDrawIndexedIndirectCommand) : (uint32_t)sizeof(VkDrawIndirectCommand);
   if (stride == 0) {
      stride = min_stride;
   } else if (unlikely(stride < min_stride)) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   terakan_app_config_draw_set_vgt_dma_index_buffer_draw_indexed(&command_writer->app_config_draw,
                                                                 indexed);

   terakan_vk_before_draw(command_writer);

   terakan_vk_draw_emit_set_base(command_writer, buffer->bo);

   /* For indirect draws, the CP reads firstInstance from the indirect buffer, but the SFN
    * backend reads load_base_instance from push constants kcache buffer. We must map the
    * indirect buffer and set base_instance per draw so gl_InstanceIndex is correct.
    * This is critical for apps using SSBO indexed by gl_InstanceIndex (e.g., STK).
    */
   void * const indirect_map = buffer->bo->mapping;
   uint32_t const indirect_va_offset =
      (uint32_t)(buffer->va - buffer->bo->va + offset);
   uint32_t bo_relative_offset = terakan_vk_buffer_bo_relative_offset(buffer, offset);
   for (uint32_t draw_index = 0; draw_index < drawCount; ++draw_index) {
      if (likely(indirect_map != NULL)) {
         uint32_t const * const cmd =
            (uint32_t const *)((char *)indirect_map + indirect_va_offset + draw_index * stride);
         uint32_t const first_instance = indexed ? cmd[4] : cmd[3];
         uint32_t const first_vertex = indexed ? 0 : cmd[0];
         terakan_vk_draw_set_vertex_instance_offsets(command_writer, first_vertex, first_instance);
         terakan_gfx_command_writer_before_app_draw(command_writer);
      }
      terakan_vk_draw_emit_draw_indirect(command_writer, bo_relative_offset, indexed);
      bo_relative_offset += stride;
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDraw(VkCommandBuffer const commandBuffer, uint32_t const vertexCount,
                uint32_t const instanceCount, uint32_t const firstVertex,
                uint32_t const firstInstance)
{
   if (unlikely(instanceCount == 0)) {
      /* `VGT_NUM_INSTANCES` 0 is interpreted as 1. */
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   terakan_hw_config_shared_draw_set_vgt_num_instances(&command_writer->hw_config_shared,
                                                       instanceCount);
   terakan_app_config_draw_set_vgt_dma_index_buffer_draw_indexed(&command_writer->app_config_draw,
                                                                 false);
   terakan_vk_draw_set_vertex_instance_offsets(command_writer, firstVertex, firstInstance);

   terakan_vk_before_draw(command_writer);

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 3);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_DRAW_INDEX_AUTO, 3 - 2, 0);
   *packet++ = vertexCount;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX);
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDrawIndexed(VkCommandBuffer const commandBuffer, uint32_t const indexCount,
                       uint32_t const instanceCount, uint32_t const firstIndex,
                       int32_t const vertexOffset, uint32_t const firstInstance)
{
   if (unlikely(instanceCount == 0)) {
      /* `VGT_NUM_INSTANCES` 0 is interpreted as 1. */
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   terakan_hw_config_shared_draw_set_vgt_num_instances(&command_writer->hw_config_shared,
                                                       instanceCount);
   terakan_app_config_draw_set_vgt_dma_index_buffer_draw_indexed(&command_writer->app_config_draw,
                                                                 true);
   terakan_vk_draw_set_vertex_instance_offsets(command_writer, (uint32_t)vertexOffset,
                                               firstInstance);

   terakan_vk_before_draw(command_writer);

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 4);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(EG_PKT3_DRAW_INDEX_OFFSET, 4 - 2, 0);
   *packet++ = firstIndex;
   *packet++ = indexCount;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_DMA);
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDrawIndirect(VkCommandBuffer const commandBuffer, VkBuffer const buffer,
                        VkDeviceSize const offset, uint32_t const drawCount, uint32_t const stride)
{
   terakan_vk_cmd_draw_indirect(commandBuffer, buffer, offset, drawCount, stride, false);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDrawIndexedIndirect(VkCommandBuffer const commandBuffer, VkBuffer const buffer,
                               VkDeviceSize const offset, uint32_t const drawCount,
                               uint32_t const stride)
{
   terakan_vk_cmd_draw_indirect(commandBuffer, buffer, offset, drawCount, stride, true);
}
