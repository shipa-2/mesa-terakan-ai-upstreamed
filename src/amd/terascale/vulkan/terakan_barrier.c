/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
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

#include "terakan_barrier.h"
#include "terakan_command_buffer.h"
#include "terakan_cp_dma.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_physical_device.h"

#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "vk_synchronization.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Apply aliases and filter out access not performed by the involved stages. */
static void
terakan_barrier_filter_access(VkAccessFlags2 * const access, VkPipelineStageFlags2 const stages)
{
   VkPipelineStageFlags2 const all_read_access = vk_read_access2_for_pipeline_stage_flags2(stages);
   VkPipelineStageFlags2 const all_write_access =
      vk_write_access2_for_pipeline_stage_flags2(stages);
   if (*access & VK_ACCESS_2_MEMORY_READ_BIT) {
      *access |= all_read_access;
   }
   if (*access & VK_ACCESS_2_MEMORY_WRITE_BIT) {
      *access |= all_write_access;
   }

   if (*access & VK_ACCESS_2_SHADER_READ_BIT) {
      *access |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
   }
   if (*access & VK_ACCESS_2_SHADER_WRITE_BIT) {
      *access |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
   }

   *access &= all_read_access | all_write_access;
}

/* Note that barriers must be transitive, and optimizations breaking that must not be performed.
 * For instance, in a chain like:
 * SHADER_STORAGE_WRITE > SHADER_SAMPLED_READ > VERTEX_ATTRIBUTE_READ
 * the second barrier must not be ignored even though it's read > read, because otherwise the writes
 * made available by CB would've only been made visible to TC, but not to VC.
 */

static enum terakan_barrier_action_flags
terakan_barrier_get_src_actions(struct terakan_gfx_command_writer * const command_writer,
                                VkPipelineStageFlags2 src_stages, VkAccessFlags2 src_access,
                                bool const for_buffer, VkImageAspectFlags const image_aspects)
{
   enum terakan_barrier_action_flags actions = 0;

   src_stages = vk_expand_src_stage_flags2(src_stages);
   terakan_barrier_filter_access(&src_access, src_stages);

   if (src_access & (VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
                     VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT)) {
      actions |= TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_VS;
   }

   if (src_access & (VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) {
      if (src_stages & (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT)) {
         actions |= TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_VS;
      }
      if (src_stages & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) {
         actions |= TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;
      }
      if (src_stages & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) {
         actions |= TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CS;
      }
   }

   if (src_access & VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT) {
      actions |= TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;
   }

   if (src_access &
       (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT)) {
      actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
                 TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_META;
   }

   if (src_access & (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)) {
      actions |=
         TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA | TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META;
   }

   if (src_access & VK_ACCESS_2_TRANSFER_READ_BIT) {
      actions |= TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;
      if (src_stages & (VK_PIPELINE_STAGE_2_COPY_BIT |
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT)) {
         actions |= TERAKAN_BARRIER_ACTION_SYNC_ME_TO_CP_DMA;
      }
   }

   if (src_access & VK_ACCESS_2_TRANSFER_WRITE_BIT) {
      bool const for_color_image =
         (image_aspects & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_PLANE_0_BIT |
                           VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) != 0;
      bool const for_depth_stencil_image =
         (image_aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
      /* Legacy vkCmdPipelineBarrier exposes all copies as TRANSFER / ALL_TRANSFER rather than the
       * synchronization2 COPY stage. Both designate the copy commands tracked by post_*_actions.
       */
      if (src_stages & (VK_PIPELINE_STAGE_2_COPY_BIT |
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT)) {
         if (for_buffer) {
            actions |= command_writer->post_buffer_copy_write_barrier_actions;
            command_writer->post_buffer_copy_write_barrier_actions = 0;
         }
         if (for_color_image) {
            actions |= command_writer->post_color_image_copy_write_barrier_actions;
            command_writer->post_color_image_copy_write_barrier_actions = 0;
         }
         if (for_depth_stencil_image) {
            actions |= command_writer->post_depth_stencil_image_copy_write_barrier_actions;
            command_writer->post_depth_stencil_image_copy_write_barrier_actions = 0;
         }
      }
      if (src_stages & VK_PIPELINE_STAGE_2_RESOLVE_BIT) {
         if (for_color_image) {
            actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
                       TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;
         }
         if (for_depth_stencil_image) {
            actions |= command_writer->post_depth_stencil_image_copy_write_barrier_actions;
         }
      }
      if (src_stages & VK_PIPELINE_STAGE_2_BLIT_BIT) {
         if (for_color_image) {
            actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
                       TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_META;
         }
         if (for_depth_stencil_image) {
            actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA |
                       TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META;
         }
      }
      if (src_stages & VK_PIPELINE_STAGE_2_CLEAR_BIT) {
         if (for_color_image) {
            actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
                       TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_META;
         }
         if (for_depth_stencil_image) {
            actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA |
                       TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META;
         }
      }
   }

   if (src_access & (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) {
      struct terakan_device const * const device =
         terakan_gfx_command_writer_device(command_writer);
      VkPipelineStageFlags2 const src_uav_stages =
         src_stages & ((device->vk.enabled_features.fragmentStoresAndAtomics
                           ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                           : 0) |
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
      if (src_uav_stages) {
         actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV |
                    (src_uav_stages & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                        ? TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS
                        : 0) |
                    (src_uav_stages & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                        ? TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CS
                        : 0);
      }
   }

   return actions;
}

static enum terakan_barrier_action_flags
terakan_barrier_get_dst_actions(struct terakan_gfx_command_writer const * const command_writer,
                                VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_access,
                                bool const for_buffer, VkImageAspectFlags const image_aspects)
{
   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);

   enum terakan_barrier_action_flags actions = 0;

   dst_stages = vk_expand_dst_stage_flags2(dst_stages);
   terakan_barrier_filter_access(&dst_access, dst_stages);

   if (dst_access & (VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT)) {
      actions |= TERAKAN_BARRIER_ACTION_SYNC_PFP_TO_ME;
   }

   /* Invalidate the texture cache.
    * INDIRECT_COMMAND_READ: Draw parameters and NumWorkgroups.
    * UNIFORM_READ: Texture cache for cases not supported by the ALU like dynamic data addressing.
    */
   if (dst_access & (VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT |
                     VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) {
      actions |= TERAKAN_BARRIER_ACTION_INV_TC;
   }

   if (dst_access & VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT) {
      actions |= terakan_device_physical_device(device)->chip_info.has_vertex_cache
                    ? TERAKAN_BARRIER_ACTION_INV_VC
                    : TERAKAN_BARRIER_ACTION_INV_TC;
   }

   if (dst_access & VK_ACCESS_2_UNIFORM_READ_BIT) {
      actions |= TERAKAN_BARRIER_ACTION_INV_SH;
   }

   if (dst_access &
       (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT)) {
      actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
                 TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_META;
   }

   if (dst_access & (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)) {
      actions |=
         TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA | TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META;
   }

   if (dst_access & VK_ACCESS_2_TRANSFER_WRITE_BIT) {
      bool const for_color_image =
         (image_aspects & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_PLANE_0_BIT |
                           VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) != 0;
      bool const for_depth_stencil_image =
         (image_aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
      if (for_buffer) {
         actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV;
      }
      if (for_color_image) {
         actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
                    TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_META;
         if (dst_stages & (VK_PIPELINE_STAGE_2_COPY_BIT |
                           VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
            /* Formats with non-power-of-two bytes per block don't support RTV usage and may use a
             * buffer UAV instead.
             */
            actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV;
         }
      }
      if (for_depth_stencil_image) {
         actions |=
            TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA | TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META;
      }
   }

   if ((dst_access &
        (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) &&
       (dst_stages & ((device->vk.enabled_features.fragmentStoresAndAtomics
                          ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                          : 0) |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT))) {
      actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV;
   }

   return actions;
}

static enum terakan_barrier_action_flags
terakan_barrier_get_image_layout_transition_actions(VkImageLayout const old_layout,
                                                    VkImageLayout const new_layout,
                                                    VkImageAspectFlags const image_aspects)
{
   if (old_layout == new_layout) {
      return 0;
   }

   enum terakan_barrier_action_flags actions = TERAKAN_BARRIER_ACTION_INV_TC;
   if (image_aspects &
       (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_PLANE_0_BIT |
        VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) {
      actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
                 TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_META;
   }
   if (image_aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      actions |=
         TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA | TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META;
   }
   return actions;
}

static void
terakan_barrier_emit_event_write(struct terakan_gfx_command_writer * const command_writer,
                                 uint32_t event)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 2);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_EVENT_WRITE, 0, 0) |
               ((command_writer->barrier_compute_mode_override ||
                 command_writer->hw_config_shared.is_compute_active_)
                   ? TERAKAN_PACKET3_COMPUTE
                   : 0);
   *packet++ = event;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

void
terakan_barrier_emit_pending_actions(struct terakan_gfx_command_writer * const command_writer,
                                     uint32_t const allowed_actions)
{
   enum terakan_barrier_action_flags actions =
      command_writer->pending_barrier_actions & allowed_actions;
   /* Nonzero mostly outside render passes, skip lots of checks if there's nothing to do. */
   if (actions == 0) {
      return;
   }
   if (actions & TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS) {
      /* Clear VS_PARTIAL_FLUSH in post-transfer barrier actions too as it's included in
       * PS_PARTIAL_FLUSH.
       */
      actions |= TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_VS;
   }
   command_writer->pending_barrier_actions &= ~actions;

   uint32_t cp_coher_cntl_cb_db_dest_base_ena = 0;
   uint32_t cp_coher_cntl = 0;

   /* Flushes are performed in bottom-to-top-of-pipe order, so preceding flushes are likely to be
    * able to make subsequent ones not have to wait.
    */

   /* A CB UAV cache flush does not by itself wait for compute shader invocations to finish.
    * Wait for CS first, otherwise a short dispatch may continue issuing RAT writes after the
    * cache has already been flushed. This ordering matches r600_flush_emit and is required before
    * making compute storage writes available to later commands or the host.
    */
   if (actions & TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CS) {
      terakan_barrier_emit_event_write(command_writer,
                                       EVENT_TYPE(EVENT_TYPE_CS_PARTIAL_FLUSH) | EVENT_INDEX(4));
   }

   /* Wait packets must precede cache flush events. Otherwise a short meta draw can still be
    * writing after FLUSH_AND_INV_CB_* has sampled the cache state. This is the ordering used by
    * r600_flush_emit as well.
    */
   if (actions & TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS) {
      terakan_barrier_emit_event_write(command_writer,
                                       EVENT_TYPE(EVENT_TYPE_PS_PARTIAL_FLUSH) | EVENT_INDEX(4));
   } else if (actions & TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_VS) {
      terakan_barrier_emit_event_write(command_writer,
                                       EVENT_TYPE(EVENT_TYPE_VS_PARTIAL_FLUSH) | EVENT_INDEX(4));
   }

   /* Wait for graphics writes to be flushed to prevent read/write-after-write hazards. */

   if (actions &
       (TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV | TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
        TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_META)) {
      cp_coher_cntl_cb_db_dest_base_ena |=
         S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_CB1_DEST_BASE_ENA(1) |
         S_0085F0_CB2_DEST_BASE_ENA(1) | S_0085F0_CB3_DEST_BASE_ENA(1) |
         S_0085F0_CB4_DEST_BASE_ENA(1) | S_0085F0_CB5_DEST_BASE_ENA(1) |
         S_0085F0_CB6_DEST_BASE_ENA(1) | S_0085F0_CB7_DEST_BASE_ENA(1);
      cp_coher_cntl |= S_0085F0_CB_ACTION_ENA(1);
      if (actions & TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV) {
         cp_coher_cntl_cb_db_dest_base_ena |=
            S_0085F0_CB8_DEST_BASE_ENA(1) | S_0085F0_CB9_DEST_BASE_ENA(1) |
            S_0085F0_CB10_DEST_BASE_ENA(1) | S_0085F0_CB11_DEST_BASE_ENA(1);
         /* Flushes and invalidates RTV and UAV data, but not meta. */
         terakan_gfx_command_writer_emit_event_write_eop_discarding_data(
            command_writer, EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_CB_DATA_TS) | EVENT_INDEX(5));
      } else if (actions & TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA) {
         /* Flushes and invalidates RTV data, but not meta or UAV data. */
         terakan_barrier_emit_event_write(
            command_writer, EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_CB_PIXEL_DATA) | EVENT_INDEX(0));
      }
      if (actions & TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_META) {
         terakan_barrier_emit_event_write(
            command_writer, EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_CB_META) | EVENT_INDEX(0));
      }
   }

   if (actions &
       (TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA | TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META)) {
      cp_coher_cntl_cb_db_dest_base_ena |= S_0085F0_DB_DEST_BASE_ENA(1);
      cp_coher_cntl |= S_0085F0_DB_ACTION_ENA(1);
      if (!(~actions & (TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA |
                        TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META))) {
         terakan_barrier_emit_event_write(
            command_writer, EVENT_TYPE(EVENT_TYPE_DB_CACHE_FLUSH_AND_INV) | EVENT_INDEX(0));
      } else {
         if (actions & TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA) {
            terakan_gfx_command_writer_emit_event_write_eop_discarding_data(
               command_writer, EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_DB_DATA_TS) | EVENT_INDEX(5));
         }
         if (actions & TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META) {
            terakan_barrier_emit_event_write(
               command_writer, EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_DB_META) | EVENT_INDEX(0));
         }
      }
   }

   /* Wait for CP DMA writes and reads to complete in ME. */

   if (actions & TERAKAN_BARRIER_ACTION_SYNC_ME_TO_CP_DMA) {
      terakan_cp_dma_sync_cp_me(command_writer);
   }

   /* Specify which read caches to invalidate. */

   if (actions & TERAKAN_BARRIER_ACTION_INV_TC) {
      cp_coher_cntl |= S_0085F0_TC_ACTION_ENA(1);
   }

   if (actions & TERAKAN_BARRIER_ACTION_INV_VC) {
      cp_coher_cntl |= S_0085F0_VC_ACTION_ENA(1);
   }

   if (actions & TERAKAN_BARRIER_ACTION_INV_SH) {
      cp_coher_cntl |= S_0085F0_SH_ACTION_ENA(1);
   }

   /* Perform implicit stage waits and cache flushes and invalidations in ME. */

   /* r600 enables SMX coherency for every CB/DB flush on R700 and newer. Without it, texture
    * reads may observe stale color or depth blocks after an attachment is made shader-readable.
    */
   if (cp_coher_cntl_cb_db_dest_base_ena) {
      cp_coher_cntl |= S_0085F0_SMX_ACTION_ENA(1);
   }

   cp_coher_cntl |= cp_coher_cntl_cb_db_dest_base_ena;
   if (cp_coher_cntl) {
      uint32_t * surface_sync_packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
      if (unlikely(surface_sync_packet == NULL)) {
         return;
      }
      *surface_sync_packet++ =
         PKT3(PKT3_SURFACE_SYNC, 4 - 1, 0) |
         ((command_writer->barrier_compute_mode_override ||
           command_writer->hw_config_shared.is_compute_active_)
             ? TERAKAN_PACKET3_COMPUTE
             : 0);
      *surface_sync_packet++ = cp_coher_cntl | TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME;
      *surface_sync_packet++ = UINT32_MAX;
      *surface_sync_packet++ = 0;
      *surface_sync_packet++ = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;
      terakan_gfx_command_writer_emit_done(command_writer, surface_sync_packet);
   }

   /* Wait until all synchronization in ME is done before starting new PFP reads if PFP is in the
    * second synchronization scope.
    */

   if (actions & TERAKAN_BARRIER_ACTION_SYNC_PFP_TO_ME) {
      uint32_t * pfp_sync_me_packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 2);
      if (unlikely(pfp_sync_me_packet == NULL)) {
         return;
      }
      *pfp_sync_me_packet++ = PKT3(PKT3_PFP_SYNC_ME, 0, 0);
      *pfp_sync_me_packet++ = 0;
      terakan_gfx_command_writer_emit_done(command_writer, pfp_sync_me_packet);
   }
}

void
terakan_barrier_emit_actions_unconditionally(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const actions)
{
   assert(!(actions & ~(uint32_t)TERAKAN_BARRIER_ACTIONS_ALL));
   command_writer->pending_barrier_actions |= actions;
   terakan_barrier_emit_pending_actions(command_writer, actions);
}

/* Evergreen TXF_MS uses FMASK to map sample indices. Unlike r600's combined CMASK/FMASK setup,
 * Terakan exposes FMASK directly, so initialize it to the identity mapping before the first color
 * use. This must be recorded on a command buffer rather than done at image creation because the
 * image memory may not be bound yet.
 *
 * FMASK stores one fragment index per sample, and the field width follows the surface's element
 * size rather than the sample count: 2x and 4x allocate one byte per pixel and use **2 bits per
 * sample**, while 8x allocates four bytes per pixel and uses **4 bits per sample**. The identity
 * value for each is therefore the fragment indices 0..n-1 packed into those fields and repeated
 * across the dword so every pixel sharing it decodes correctly:
 *
 *   2x: 0b0100                            -> 0x04, repeated as 0x04040404
 *   4x: 0b11100100                        -> 0xE4, repeated as 0xE4E4E4E4
 *   8x: nibbles 0,1,2,3,4,5,6,7           -> 0x76543210 (one whole dword, no repetition)
 *
 * All three were determined empirically on real CAICOS hardware with
 * terakan_color_msaa_fetch_test giving every sample its own distinct colour, which is the only way
 * to tell a fetch that lands on the wrong plane from one that lands on the right one -- an earlier
 * version of that test cleared every sample to the same colour and could not distinguish them.
 * The 4x and 8x values match what Gallium r600 uses; the 2x one does not. r600's 0x02020202 is a
 * 1-bit-per-sample identity, which decodes here as fragment 2 for sample 0 -- a plane a 2x surface
 * does not have -- and was measured returning garbage for every sample 0 until it was corrected to
 * the 2-bit-per-sample 0x04040404.
 *
 * Both entry points that can be an image's first color use have to call this: an explicit
 * `VK_IMAGE_LAYOUT_UNDEFINED` image barrier, and a render pass whose attachment declares
 * `initialLayout = VK_IMAGE_LAYOUT_UNDEFINED`. The common runtime does not lower the latter into a
 * barrier -- it forwards it as a `VkRenderingAttachmentInitialLayoutInfoMESA` chained onto the
 * attachment (see vk_render_pass.c) -- so a driver that only looks at barriers silently leaves
 * FMASK uninitialized for the very common case of an application that never transitions the image
 * itself.
 */
void
terakan_barrier_initialize_color_metadata(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_image const * const image, uint32_t const base_array_layer,
   uint32_t const layer_count)
{
   if (image == NULL || !terakan_image_surface_has_color_metadata(&image->surface) ||
       image->bo == NULL) {
      return;
   }

   /* The classic R600/R700 driver does not provide a CP-DMA fill path for initializing FMASK and
    * CMASK. terakan_cp_dma_fill() uses the Evergreen packet encoding, which the RV710 parser
    * rejected during an MSAA clear as "CP DMA src buffer too small" (see the RV710 probe recorded
    * in docs/terakan/TODO.md). Do not emit that packet on TeraScale 1: an application-side
    * command-buffer error is safer than sending a malformed stream to the kernel. This leaves
    * MSAA color initialization/resolve unsupported until a R700-specific metadata initialization
    * mechanism is implemented and read back on hardware.
    */
   if (terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_terascale_1) {
      vk_command_buffer_set_error(&command_writer->base.command_buffer->vk, VK_ERROR_UNKNOWN);
      return;
   }

   uint32_t const sample_count_log2 =
      (uint32_t)terakan_image_vk_sample_count_to_hw_log2(image->vk.samples, false);
   static uint32_t const identity_fmask[4] = {0x00000000, 0x04040404, 0xE4E4E4E4, 0x76543210};

   uint32_t base_slice = base_array_layer;
   uint32_t slice_count = layer_count;
   if (image->vk.image_type == VK_IMAGE_TYPE_3D) {
      base_slice = 0;
      slice_count =
         image->surface.fmask.size_bytes_shr8 / image->surface.fmask.slice_size_bytes_shr8;
   } else {
      slice_count =
         MIN2(slice_count, image->vk.array_layers - MIN2(base_slice, image->vk.array_layers));
   }
   if (slice_count == 0 || base_slice >= image->vk.array_layers) {
      return;
   }

   VkDeviceSize const slice_size = (VkDeviceSize)image->surface.fmask.slice_size_bytes_shr8 << 8;
   command_writer->post_buffer_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_SYNC_ME_TO_CP_DMA;
   terakan_cp_dma_fill(
      command_writer, identity_fmask[sample_count_log2], image->bo,
      image->va + ((VkDeviceSize)image->surface.fmask.offset_in_memory_bytes_shr8 << 8) +
         slice_size * base_slice,
      TERAKAN_BO_PRIORITY_SEPARATE_META, slice_size * slice_count);

   VkDeviceSize const cmask_slice_size =
      (VkDeviceSize)image->surface.cmask.slice_size_bytes_shr8 << 8;
   terakan_cp_dma_fill(
      command_writer, 0xCCCCCCCC, image->bo,
      image->va + ((VkDeviceSize)image->surface.cmask.offset_in_memory_bytes_shr8 << 8) +
         cmask_slice_size * base_slice,
      TERAKAN_BO_PRIORITY_SEPARATE_META, cmask_slice_size * slice_count);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdPipelineBarrier2(VkCommandBuffer const commandBuffer,
                            VkDependencyInfo const * const pDependencyInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   VkImageAspectFlags const global_barrier_image_aspects =
      VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT |
      VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT;
   for (uint32_t barrier_index = 0; barrier_index < pDependencyInfo->memoryBarrierCount;
        ++barrier_index) {
      VkMemoryBarrier2 const * const barrier = &pDependencyInfo->pMemoryBarriers[barrier_index];
      command_writer->pending_barrier_actions |=
         terakan_barrier_get_src_actions(command_writer, barrier->srcStageMask,
                                         barrier->srcAccessMask, true,
                                         global_barrier_image_aspects) |
         terakan_barrier_get_dst_actions(command_writer, barrier->dstStageMask,
                                         barrier->dstAccessMask, true,
                                         global_barrier_image_aspects);
   }

   for (uint32_t barrier_index = 0; barrier_index < pDependencyInfo->bufferMemoryBarrierCount;
        ++barrier_index) {
      VkBufferMemoryBarrier2 const * const barrier =
         &pDependencyInfo->pBufferMemoryBarriers[barrier_index];
      command_writer->pending_barrier_actions |=
         terakan_barrier_get_src_actions(command_writer, barrier->srcStageMask,
                                         barrier->srcAccessMask, true, VK_IMAGE_ASPECT_NONE) |
         terakan_barrier_get_dst_actions(command_writer, barrier->dstStageMask,
                                         barrier->dstAccessMask, true, VK_IMAGE_ASPECT_NONE);
   }

   for (uint32_t barrier_index = 0; barrier_index < pDependencyInfo->imageMemoryBarrierCount;
        ++barrier_index) {
      VkImageMemoryBarrier2 const * const barrier =
         &pDependencyInfo->pImageMemoryBarriers[barrier_index];
      command_writer->pending_barrier_actions |=
         terakan_barrier_get_src_actions(command_writer, barrier->srcStageMask,
                                         barrier->srcAccessMask, false,
                                         barrier->subresourceRange.aspectMask) |
         terakan_barrier_get_dst_actions(command_writer, barrier->dstStageMask,
                                         barrier->dstAccessMask, false,
                                         barrier->subresourceRange.aspectMask) |
         terakan_barrier_get_image_layout_transition_actions(
            barrier->oldLayout, barrier->newLayout, barrier->subresourceRange.aspectMask);

      /* Evergreen TXF_MS uses FMASK to map sample indices, so it has to hold the identity
       * mapping before the first color use. See terakan_barrier_initialize_color_metadata().
       */
      if (barrier->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
          (barrier->subresourceRange.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
         terakan_barrier_initialize_color_metadata(
            command_writer, terakan_image_from_handle(barrier->image),
            barrier->subresourceRange.baseArrayLayer, barrier->subresourceRange.layerCount);
      }
   }
}
