/*
 * Copyright © 2026 Terakan contributors
 */

#include "terakan_app_config_compute.h"
#include "terakan_barrier.h"
#include "terakan_bo.h"
#include "terakan_buffer.h"
#include "terakan_command_buffer.h"
#include "terakan_entrypoints.h"
#include "terakan_hw_config_shared.h"
#include "terakan_push_constants.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "vk_command_buffer.h"
#include "vk_pipeline.h"

#include <assert.h>
#include <stdint.h>

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBindPipeline(VkCommandBuffer const commandBuffer,
                        VkPipelineBindPoint const pipelineBindPoint, VkPipeline const pipeline)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(vk_pipeline, pipe, pipeline);

   assert(pipe->bind_point == pipelineBindPoint);
   pipe->ops->cmd_bind(cmd_buffer, pipe);
}

static void
terakan_vk_emit_dispatch_direct(struct terakan_gfx_command_writer * const command_writer,
                                uint32_t const group_count_x, uint32_t const group_count_y,
                                uint32_t const group_count_z)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 5);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_DISPATCH_DIRECT, 3, 0) | TERAKAN_PACKET3_COMPUTE;
   *packet++ = group_count_x;
   *packet++ = group_count_y;
   *packet++ = group_count_z;
   *packet++ = 1; /* VGT_DISPATCH_INITIATOR = COMPUTE_SHADER_EN */
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_vk_cmd_dispatch(VkCommandBuffer const commandBuffer, uint32_t const groupCountX,
                        uint32_t const groupCountY, uint32_t const groupCountZ,
                        uint32_t const baseGroupX, uint32_t const baseGroupY,
                        uint32_t const baseGroupZ)
{
   if (unlikely(groupCountX == 0 || groupCountY == 0 || groupCountZ == 0)) {
      return;
   }
   if (unlikely(baseGroupX != 0 || baseGroupY != 0 || baseGroupZ != 0)) {
      /* TODO(Triang3l): VGT_COMPUTE_START_* for non-zero base groups. */
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   /* Flush CB UAV data and CS state before dispatch so compute shader can read previous writes. */
   terakan_barrier_emit_actions_unconditionally(
      command_writer,
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV | TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS |
         TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CS);

   terakan_gfx_command_writer_before_app_dispatch(command_writer);
   terakan_vk_emit_dispatch_direct(command_writer, groupCountX, groupCountY, groupCountZ);

   /* Invalidate TC, VC, and SH after dispatch so subsequent reads get fresh data. */
   command_writer->pending_barrier_actions |=
      TERAKAN_BARRIER_ACTION_INV_TC | TERAKAN_BARRIER_ACTION_INV_VC |
      TERAKAN_BARRIER_ACTION_INV_SH;
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDispatch(VkCommandBuffer const commandBuffer, uint32_t const groupCountX,
                    uint32_t const groupCountY, uint32_t const groupCountZ)
{
   terakan_vk_cmd_dispatch(commandBuffer, groupCountX, groupCountY, groupCountZ, 0, 0, 0);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDispatchBase(VkCommandBuffer const commandBuffer, uint32_t const baseGroupX,
                        uint32_t const baseGroupY, uint32_t const baseGroupZ,
                        uint32_t const groupCountX, uint32_t const groupCountY,
                        uint32_t const groupCountZ)
{
   terakan_vk_cmd_dispatch(commandBuffer, groupCountX, groupCountY, groupCountZ, baseGroupX,
                           baseGroupY, baseGroupZ);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDispatchBaseKHR(VkCommandBuffer const commandBuffer, uint32_t const baseGroupX,
                           uint32_t const baseGroupY, uint32_t const baseGroupZ,
                           uint32_t const groupCountX, uint32_t const groupCountY,
                           uint32_t const groupCountZ)
{
   terakan_CmdDispatchBase(commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX,
                           groupCountY, groupCountZ);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDispatchIndirect(VkCommandBuffer const commandBuffer, VkBuffer const bufferHandle,
                            VkDeviceSize const offset)
{
   struct terakan_buffer const * const buffer = terakan_buffer_from_handle(bufferHandle);
   if (unlikely(buffer == NULL)) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   if (offset + 12 > buffer->vk.size) {
      return;
   }

   uint32_t * mapping = terakan_bo_map((struct terakan_bo *)buffer->bo);
   if (unlikely(mapping == NULL)) {
      return;
   }
   uint32_t const * const params =
      (uint32_t const *)((char *)mapping + (buffer->va - buffer->bo->va + offset));
   uint32_t const grid_x = params[0];
   uint32_t const grid_y = params[1];
   uint32_t const grid_z = params[2];
   terakan_bo_unmap((struct terakan_bo *)buffer->bo);

   if (grid_x == 0 || grid_y == 0 || grid_z == 0) {
      return;
   }

   /* Flush CB UAV data and CS state before dispatch so compute shader can read previous writes. */
   terakan_barrier_emit_actions_unconditionally(
      command_writer,
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV | TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS |
         TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CS);

   terakan_gfx_command_writer_before_app_dispatch(command_writer);
   terakan_vk_emit_dispatch_direct(command_writer, grid_x, grid_y, grid_z);

   /* Invalidate TC, VC, and SH after dispatch so subsequent reads get fresh data. */
   command_writer->pending_barrier_actions |=
      TERAKAN_BARRIER_ACTION_INV_TC | TERAKAN_BARRIER_ACTION_INV_VC |
      TERAKAN_BARRIER_ACTION_INV_SH;
}
