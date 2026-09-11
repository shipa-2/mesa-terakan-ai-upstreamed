/*
 * Copyright © 2025 Vitaliy Triang3l Kuzmin
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

#include "terakan_query.h"

#include "meta/terakan_meta.h"
#include "terakan_barrier.h"
#include "terakan_bo.h"
#include "terakan_command_buffer.h"
#include "terakan_cp_dma.h"
#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_format.h"
#include "terakan_instance.h"
#include "terakan_physical_device.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "c11/threads.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/bitscan.h"
#include "util/hash_table.h"
#include "util/list.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "util/u_atomic.h"
#include "util/u_endian.h"
#include "vk_enum_to_str.h"
#include "vk_log.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t const terakan_query_sample_event_initiators[TERAKAN_QUERY_SAMPLE_INDEX_COUNT] = {
   [TERAKAN_QUERY_SAMPLE_INDEX_ZPASS] = EVENT_TYPE(EVENT_TYPE_ZPASS_DONE) | EVENT_INDEX(1),

   [TERAKAN_QUERY_SAMPLE_INDEX_PIPELINESTAT] =
      EVENT_TYPE(EVENT_TYPE_SAMPLE_PIPELINESTAT) | EVENT_INDEX(2),

   [TERAKAN_QUERY_SAMPLE_INDEX_STREAMOUTSTATS_0] =
      EVENT_TYPE(EVENT_TYPE_SAMPLE_STREAMOUTSTATS) | EVENT_INDEX(3),
   [TERAKAN_QUERY_SAMPLE_INDEX_STREAMOUTSTATS_1] =
      EVENT_TYPE(EVENT_TYPE_SAMPLE_STREAMOUTSTATS1) | EVENT_INDEX(3),
   [TERAKAN_QUERY_SAMPLE_INDEX_STREAMOUTSTATS_2] =
      EVENT_TYPE(EVENT_TYPE_SAMPLE_STREAMOUTSTATS2) | EVENT_INDEX(3),
   [TERAKAN_QUERY_SAMPLE_INDEX_STREAMOUTSTATS_3] =
      EVENT_TYPE(EVENT_TYPE_SAMPLE_STREAMOUTSTATS3) | EVENT_INDEX(3),
};

static enum terakan_query_sample_index
terakan_query_get_sample_index(VkQueryType const query_type, uint32_t const query_index)
{
   switch (query_type) {
   case VK_QUERY_TYPE_OCCLUSION:
      return TERAKAN_QUERY_SAMPLE_INDEX_ZPASS;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      return TERAKAN_QUERY_SAMPLE_INDEX_PIPELINESTAT;
   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT:
      switch (query_index) {
      case 1:
         return TERAKAN_QUERY_SAMPLE_INDEX_STREAMOUTSTATS_1;
      case 2:
         return TERAKAN_QUERY_SAMPLE_INDEX_STREAMOUTSTATS_2;
      case 3:
         return TERAKAN_QUERY_SAMPLE_INDEX_STREAMOUTSTATS_3;
      default:
         assert(query_index == 0);
         return TERAKAN_QUERY_SAMPLE_INDEX_STREAMOUTSTATS_0;
      }
   default:
      assert(!"Unsupported query type");
      return TERAKAN_QUERY_SAMPLE_INDEX_COUNT;
   }
}

/* Mapping of log2(VkQueryPipelineStatisticFlagBits) to hardware counter indices. Not the identity:
 * SAMPLE_PIPELINESTAT writes the counters in the hardware's own order, which has nothing to do with
 * the order of the bits in VkQueryPipelineStatisticFlags.
 */
enum terakan_query_pipelinestat_hw_counter const
   terakan_query_pipelinestat_vk_hw_counters[] = {
      TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_IA_VERTICES,
      TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_IA_PRIMITIVES,
      TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_VS_INVOCATIONS,
      TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_GS_INVOCATIONS,
      TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_GS_PRIMITIVES,
      TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_CLIPPING_INVOCATIONS,
      TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_CLIPPING_PRIMITIVES,
      TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_FS_INVOCATIONS,
      TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_TCS_PATCHES,
      TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_TES_INVOCATIONS,
      TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_CS_INVOCATIONS,
};

struct terakan_query_streamout_sample {
   uint64_t num_primitives_written;
   uint64_t primitives_storage_needed;
};

#define TERAKAN_QUERY_POOL_MAX_VK_RESULT_COUNTERS                                                  \
   MAX2(TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_COUNT, 2)

unsigned
terakan_query_pool_vk_result_size_counters(struct terakan_query_pool const * const query_pool)
{
   switch (query_pool->vk.query_type) {
   case VK_QUERY_TYPE_OCCLUSION:
   case VK_QUERY_TYPE_TIMESTAMP:
      return 1;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      return util_bitcount(query_pool->vk.pipeline_statistics);
   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT:
      return 2;
   default:
      assert(!"Unsupported query type");
      return 0;
   }
}

/* Query pool memory is written by the ME -- the availability fills of vkCmdResetQueryPool and
 * vkCmdEndQuery go through CP DMA, and the samples themselves through EVENT_WRITE -- while
 * vkCmdCopyQueryPoolResults reads that same memory from a vertex shader in a meta draw. The ME runs
 * ahead of the shader pipeline, so nothing in the command stream keeps the two apart on its own: an
 * application that copies results and then resets or reuses the same queries, which is the ordinary
 * per-frame pattern, would have the reset's fill land in the middle of a copy still fetching from
 * it, and read back a mix of the old and new contents.
 *
 * The copy does not drain the pipeline itself. It raises
 * TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_VS as a pending barrier action instead, so a run
 * of back-to-back copies -- one per query pool, say -- costs one wait rather than one per copy.
 * The wait is emitted here, immediately before the first write that could overtake it. When no copy
 * is outstanding the pending action is clear and terakan_barrier_emit_pending_actions returns
 * without emitting anything.
 */
static void
terakan_query_sync_before_pool_write(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_barrier_emit_pending_actions(command_writer,
                                        TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_VS);
}

/* The command-side query path still emits R8xx/Evergreen-shaped EVENT/CP-DMA and meta-UAV
 * operations. R700 DB query state now has a CPU encoder, but those surrounding operations have
 * not passed an RV710 command-stream/readback test. Fail during recording instead of allowing the
 * opt-in TeraScale 1 submit diagnostic to send an unvalidated query stream. Host-side query-pool
 * reset/results remain available because they do not touch the GPU. */
static bool
terakan_query_reject_terascale_1(struct terakan_command_buffer * const command_buffer)
{
   if (!terakan_gfx_command_writer_physical_device(command_buffer->command_writer.gfx)
           ->chip_info.is_terascale_1) {
      return false;
   }
   vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_FEATURE_NOT_PRESENT);
   return true;
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdResetQueryPool(VkCommandBuffer const commandBuffer, VkQueryPool const queryPool,
                          uint32_t firstQuery, uint32_t queryCount)
{
   struct terakan_command_buffer * const command_buffer =
      terakan_command_buffer_from_handle(commandBuffer);
   if (terakan_query_reject_terascale_1(command_buffer)) {
      return;
   }
   struct terakan_query_pool const * const query_pool = terakan_query_pool_from_handle(queryPool);
   terakan_query_pool_clamp_range(query_pool, &firstQuery, &queryCount);
   if (unlikely(queryCount == 0)) {
      /* If no writes will be emitted, there's nothing to sync to after this reset. */
      return;
   }
   struct terakan_gfx_command_writer * const command_writer = command_buffer->command_writer.gfx;
   terakan_query_sync_before_pool_write(command_writer);
   /* Mark as unavailable. */
   terakan_cp_dma_fill(
      command_writer, 0, query_pool->bo,
      query_pool->bo->va + terakan_query_pool_availability_offset_bytes(query_pool, firstQuery),
      TERAKAN_BO_PRIORITY_QUERY, (VkDeviceSize)sizeof(uint32_t) * queryCount);
   /* Not deferred to the next query access: terakan_cp_dma_sync_cp_me emits nothing, it only sets
    * the sync bit in the CP DMA packet that was just written, so there is nothing to save by
    * putting it off -- and the packet has to be reachable when it happens, which it stops being
    * once the indirect buffer is closed.
    */
   terakan_cp_dma_sync_cp_me(command_writer);
}

static void
terakan_query_pool_reset(struct terakan_query_pool const * const query_pool, uint32_t first_query,
                         uint32_t query_count)
{
   terakan_query_pool_clamp_range(query_pool, &first_query, &query_count);
   /* Mark as unavailable. */
   memset((char *)query_pool->bo->mapping +
             terakan_query_pool_availability_offset_bytes(query_pool, first_query),
          0, sizeof(uint32_t) * query_count);
}

VKAPI_ATTR void VKAPI_CALL
terakan_ResetQueryPool(UNUSED VkDevice const device, VkQueryPool const queryPool,
                       uint32_t const firstQuery, uint32_t const queryCount)
{
   terakan_query_pool_reset(terakan_query_pool_from_handle(queryPool), firstQuery, queryCount);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBeginQueryIndexedEXT(VkCommandBuffer const commandBuffer, VkQueryPool const queryPool,
                                uint32_t const query, VkQueryControlFlags const flags,
                                uint32_t const index)
{
   struct terakan_command_buffer * const command_buffer =
      terakan_command_buffer_from_handle(commandBuffer);
   if (terakan_query_reject_terascale_1(command_buffer)) {
      return;
   }
   struct terakan_query_pool const * const query_pool = terakan_query_pool_from_handle(queryPool);

   /* #MemoryIntegrity */
   if (unlikely(query >= query_pool->vk.query_count)) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer = command_buffer->command_writer.gfx;

   terakan_query_sync_before_pool_write(command_writer);

   /* Sample the initial value, and make sure the final value of the hardware counters for the query
    * type is sampled at the end of the indirect buffer in case this query ends up spanning multiple
    * indirect buffers (reserving space in the indirect buffer for 2 samples for this reason, but
    * may conditionally emit only one).
    */

   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 4 * 2, 1 * 2, 0, 1 * 2);
   if (unlikely(packet == NULL)) {
      return;
   }

   struct terakan_command_buffer_indirect_buffer const * const begin_indirect_buffer =
      command_writer->indirect_buffer;

   VkQueryType const query_type = query_pool->vk.query_type;
   enum terakan_query_sample_index const sample_index =
      terakan_query_get_sample_index(query_type, index);
   uint32_t const event_initiator = terakan_query_sample_event_initiators[sample_index];

   VkDeviceSize const samples_offset_bytes =
      terakan_query_pool_samples_offset_bytes(query_pool, query);

   *packet++ = PKT3(PKT3_EVENT_WRITE, 3 - 1, 0);
   *packet++ = event_initiator;
   uint64_t const query_va = query_pool->bo->va + samples_offset_bytes;
   uint32_t const * const packet_address = packet;
   *packet++ = (uint32_t)query_va;
   *packet++ = (query_va >> 32) & 0xFF;
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_address, packet_address + 1,
      TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_LO, TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                query_pool->bo, false, true,
                                                TERAKAN_BO_PRIORITY_QUERY));

   struct terakan_command_buffer_indirect_buffer_query_sample * const indirect_buffer_end_sample =
      &command_writer->indirect_buffer->query_begin_end_samples[sample_index][1];
   if (indirect_buffer_end_sample->bo == NULL) {
      unsigned const indirect_buffer_end_sample_size_bytes =
         (sizeof(uint64_t) * query_pool->samples_size_counters) >>
         (sample_index == TERAKAN_QUERY_SAMPLE_INDEX_ZPASS ? 0 : 1);
      void * const indirect_buffer_end_sample_mapping = terakan_push_buffer_allocate_kcache(
         command_writer->base.command_buffer, indirect_buffer_end_sample_size_bytes,
         &indirect_buffer_end_sample->bo, &indirect_buffer_end_sample->va_kcache_lines);
      if (likely(indirect_buffer_end_sample_mapping != NULL)) {
         if (sample_index == TERAKAN_QUERY_SAMPLE_INDEX_ZPASS) {
            /* Make sure that for disabled render backends, all subtractions are 0 - 0. */
            memset(indirect_buffer_end_sample_mapping, 0, indirect_buffer_end_sample_size_bytes);
         }
         uint32_t * finalizer_packet =
            terakan_gfx_command_writer_add_finalizer(command_writer, 4, 0, 1);
         *finalizer_packet++ = PKT3(PKT3_EVENT_WRITE, 3 - 1, 0);
         *finalizer_packet++ = event_initiator;
         uint32_t const * const finalizer_packet_address = finalizer_packet;
         /* Z pass samples from different render backends have a stride of 2 counters, interleave
          * the beginning and the end.
          */
         *finalizer_packet++ =
            (indirect_buffer_end_sample->va_kcache_lines << 8) +
            (sample_index == TERAKAN_QUERY_SAMPLE_INDEX_ZPASS ? sizeof(uint64_t) : 0);
         *finalizer_packet++ = indirect_buffer_end_sample->va_kcache_lines >> 24;
         terakan_gfx_command_writer_add_relocation_for_40_bits(
            command_writer, &finalizer_packet, finalizer_packet_address,
            finalizer_packet_address + 1, TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_LO,
            TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_HI,
            terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                      indirect_buffer_end_sample->bo, false, true,
                                                      TERAKAN_BO_PRIORITY_QUERY));
      }
   }

   terakan_gfx_command_writer_emit_done(command_writer, packet);

   /* Remember the info for ending. */

   if (command_writer->active_queries == NULL) {
      struct terakan_command_pool * const command_pool = container_of(
         command_writer->base.command_buffer->vk.pool, struct terakan_command_pool, vk);
      struct terakan_query_active_table * active_queries;
      if (!list_is_empty(&command_pool->active_query_tables_free)) {
         active_queries = list_first_entry(&command_pool->active_query_tables_free,
                                           struct terakan_query_active_table, free_link);
         list_del(&active_queries->free_link);
         assert(_mesa_hash_table_num_entries(&active_queries->begin_indirect_buffer_ht) == 0);
      } else {
         active_queries = ralloc(NULL, struct terakan_query_active_table);
         if (unlikely(active_queries == NULL)) {
            vk_command_buffer_set_error(&command_writer->base.command_buffer->vk,
                                        VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
         }
         if (unlikely(!_mesa_hash_table_init(&active_queries->begin_indirect_buffer_ht,
                                             active_queries, _mesa_hash_pointer,
                                             _mesa_key_pointer_equal))) {
            vk_command_buffer_set_error(&command_writer->base.command_buffer->vk,
                                        VK_ERROR_OUT_OF_HOST_MEMORY);
            ralloc_free(active_queries);
            return;
         }
      }
      command_writer->active_queries = active_queries;
   }

   /* If beginning a query, optionally resetting it, and ending it, this insertion will replace the
    * previous beginning.
    */
   if (unlikely(
          _mesa_hash_table_insert(&command_writer->active_queries->begin_indirect_buffer_ht,
                                  (char const *)query_pool->bo->mapping + samples_offset_bytes,
                                  (void *)begin_indirect_buffer) == NULL)) {
      vk_command_buffer_set_error(&command_writer->base.command_buffer->vk,
                                  VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   ++command_writer->active_query_counts[sample_index];

   /* Set up counting. */
   switch (query_type) {
   case VK_QUERY_TYPE_OCCLUSION:
      terakan_app_config_draw_push_db_count_control_zpass_query_active(
         &command_writer->app_config_draw);
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT:
      ++command_writer->active_pipelinestat_streamoutstats_query_count;
      break;
   default:
      break;
   }

#ifdef TERAKAN_REGRESSION_TEST
   if (container_of(terakan_gfx_command_writer_physical_device(command_writer)->vk.instance,
                    struct terakan_instance const, vk)
          ->regression_test_flags &
       BITFIELD64_BIT(TERAKAN_REGRESSION_TEST_SHIFT_SPLIT_INDIRECT_BUFFER_AT_QUERY_BEGIN_END)) {
      /* Take the beginning sample and do the actions in the query in separate indirect buffers to
       * perform accumulation for testing it.
       */
      terakan_gfx_command_writer_end_indirect_buffer(command_writer);
   }
#endif
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdEndQueryIndexedEXT(VkCommandBuffer const commandBuffer, VkQueryPool const queryPool,
                              uint32_t const query, uint32_t const index)
{
   struct terakan_command_buffer * const command_buffer =
      terakan_command_buffer_from_handle(commandBuffer);
   if (terakan_query_reject_terascale_1(command_buffer)) {
      return;
   }
   struct terakan_query_pool const * const query_pool = terakan_query_pool_from_handle(queryPool);

   /* #MemoryIntegrity */
   if (unlikely(query >= query_pool->vk.query_count)) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer = command_buffer->command_writer.gfx;

   terakan_query_sync_before_pool_write(command_writer);

   VkDeviceSize const samples_offset_bytes =
      terakan_query_pool_samples_offset_bytes(query_pool, query);

   /* Find the beginning within this command buffer and remove it. */
   struct terakan_command_buffer_indirect_buffer const * begin_indirect_buffer;
   {
      assert(command_writer->active_queries != NULL);
      if (unlikely(command_writer->active_queries == NULL)) {
         return;
      }
      struct hash_entry * const active_query_entry =
         _mesa_hash_table_search(&command_writer->active_queries->begin_indirect_buffer_ht,
                                 (char const *)query_pool->bo->mapping + samples_offset_bytes);
      assert(active_query_entry != NULL);
      if (unlikely(active_query_entry == NULL)) {
         return;
      }
      begin_indirect_buffer = active_query_entry->data;
      _mesa_hash_table_remove(&command_writer->active_queries->begin_indirect_buffer_ht,
                              active_query_entry);
   }

   VkQueryType const query_type = query_pool->vk.query_type;

   switch (query_type) {
   case VK_QUERY_TYPE_OCCLUSION:
      terakan_app_config_draw_pop_db_count_control_zpass_query_active(
         &command_writer->app_config_draw);
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT:
      --command_writer->active_pipelinestat_streamoutstats_query_count;
      break;
   default:
      break;
   }

   /* Sample the final value. */

   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);

#ifdef TERAKAN_REGRESSION_TEST
   if (container_of(terakan_device_physical_device(device)->vk.instance,
                    struct terakan_instance const, vk)
          ->regression_test_flags &
       BITFIELD64_BIT(TERAKAN_REGRESSION_TEST_SHIFT_SPLIT_INDIRECT_BUFFER_AT_QUERY_BEGIN_END)) {
      /* Do the actions in the query and take the ending sample in separate indirect buffers to
       * perform accumulation for testing it.
       */
      terakan_gfx_command_writer_end_indirect_buffer(command_writer);
   }
#endif

   enum terakan_query_sample_index const sample_index =
      terakan_query_get_sample_index(query_type, index);

   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 4, 1, 0, 1);
   /* Decrement the active query count only after obtaining the indirect buffer for the final
    * sample, so if the query spans multiple indirect buffers, there is the indirect buffer
    * beginning sample for the same indirect buffer, because the difference between the two samples
    * for the last indirect buffer needs to be included in the accumulation.
    */
   --command_writer->active_query_counts[sample_index];
   if (unlikely(packet == NULL)) {
      return;
   }

   /* Remember the indirect buffer submission that the end sample is done in. During the
    * accumulation of samples from submissions, new indirect buffers may be started to execute the
    * accumulation shader, even multiple times.
    */
   struct terakan_command_buffer_indirect_buffer const * const end_indirect_buffer =
      command_writer->indirect_buffer;

   VkDeviceSize const samples_size_bytes =
      (VkDeviceSize)(sizeof(uint64_t) * query_pool->samples_size_counters);
   /* Z pass samples from different render backends have a stride of 2 counters, interleave the
    * beginning and the end.
    */
   uint64_t const end_sample_va =
      query_pool->bo->va + samples_offset_bytes +
      (query_type == VK_QUERY_TYPE_OCCLUSION ? sizeof(uint64_t) : samples_size_bytes / 2);

   struct terakan_bo const * const accumulator = device->query_accumulator_bo;

   if (unlikely(end_indirect_buffer != begin_indirect_buffer &&
                (end_indirect_buffer->query_begin_end_samples[sample_index][0].bo == NULL ||
                 begin_indirect_buffer->query_begin_end_samples[sample_index][1].bo == NULL))) {
      /* Simplify the logic if allocation of the indirect buffer beginning and end samples has
       * failed (will normally be reported as "out of memory" by vkEndCommandBuffer).
       * This may also occur in case of invalid usage when transform feedback stream indices don't
       * match between the beginning and the end.
       */
      begin_indirect_buffer = end_indirect_buffer;
   }
   bool const accumulate = end_indirect_buffer != begin_indirect_buffer;

   *packet++ = PKT3(PKT3_EVENT_WRITE, 3 - 1, 0);
   *packet++ = terakan_query_sample_event_initiators[sample_index];
   uint64_t const end_sample_event_write_va = accumulate ? accumulator->va : end_sample_va;
   uint32_t const * const packet_address = packet;
   *packet++ = (uint32_t)end_sample_event_write_va;
   *packet++ = (end_sample_event_write_va >> 32) & 0xFF;
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_address, packet_address + 1,
      TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_LO, TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                accumulate ? accumulator : query_pool->bo, false,
                                                true, TERAKAN_BO_PRIORITY_QUERY));

   terakan_gfx_command_writer_emit_done(command_writer, packet);

   if (accumulate) {
      /* Because a single Vulkan command buffer may be executed as multiple kernel driver
       * submissions, any other work (such as command buffers of other processes) done by the GPU
       * between indirect buffers of the command buffer must be excluded from the query result.
       *
       * Accumulate the samples from all indirect buffers the query was active in, making the end
       * values relative to the beginning sample.
       *
       * First, add the counts for entire indirect buffers between (exclusively) the first and the
       * last indirect buffers with the query active, writing intermediate counts to the temporary
       * accumulator.
       *
       * Then, add the tail of the first indirect buffer, and write the end value relative to the
       * beginning value to the query pool.
       *
       * End value =
       *      beginning sample
       *    + (first indirect buffer end - beginning sample)
       *    + sum{middle indirect buffer end - middle indirect buffer beginning}
       *    + (end sample - last indirect buffer beginning)
       *
       * Simplified: end value =
       *      end sample
       *    + sum{middle indirect buffer end - middle indirect buffer beginning}
       *    + (first indirect buffer end - last indirect buffer beginning)
       *
       * The same formula, and thus the same shader, can be used for both parts:
       *    UAV <-   kcache indirect buffer end
       *           - kcache indirect buffer beginning
       *           + kcache accumulator
       */

      unsigned const accumulation_uav_dwords =
         terakan_meta_query_accum_begin(command_writer, query_type);

      /* TODO(Triang3l): Integrate the `SURFACE_SYNC`s here into the command buffer barrier
       * architecture.
       */

      /* Make all indirect buffer beginning and end samples visible to the kcache. */
      {
         packet = terakan_gfx_command_writer_emit(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
         if (unlikely(packet == NULL)) {
            return;
         }
         *packet++ = PKT3(PKT3_SURFACE_SYNC, 4 - 1, 0);
         *packet++ = S_0085F0_SH_ACTION_ENA(true) | TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME;
         *packet++ = UINT32_MAX;
         *packet++ = 0;
         *packet++ = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;
         terakan_gfx_command_writer_emit_done(command_writer, packet);
      }

      for (struct terakan_command_buffer_indirect_buffer const * middle_indirect_buffer =
              list_entry(begin_indirect_buffer->link.next,
                         struct terakan_command_buffer_indirect_buffer const, link);
           middle_indirect_buffer != end_indirect_buffer;
           middle_indirect_buffer =
              list_entry(middle_indirect_buffer->link.next,
                         struct terakan_command_buffer_indirect_buffer const, link)) {
         struct terakan_command_buffer_indirect_buffer_query_sample const * const
            middle_indirect_buffer_samples =
               middle_indirect_buffer->query_begin_end_samples[sample_index];
         if (unlikely(middle_indirect_buffer_samples[0].bo == NULL ||
                      middle_indirect_buffer_samples[1].bo == NULL)) {
            /* Memory allocation failed possibly, or stream index mismatch. */
            continue;
         }
         terakan_meta_query_accum(command_writer, &middle_indirect_buffer_samples[1],
                                  &middle_indirect_buffer_samples[0], accumulator, accumulator->va,
                                  accumulation_uav_dwords);

         /* Make UAV writes to the accumulator available to ME, and make it visible to the kcache in
          * the next `terakan_meta_query_accum`.
          */
         {
            packet = terakan_gfx_command_writer_emit_with_bo(
               command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5, 1, 1, 0);
            if (unlikely(packet == NULL)) {
               return;
            }
            *packet++ = PKT3(PKT3_SURFACE_SYNC, 4 - 1, 0);
            *packet++ = TERAKAN_META_QUERY_ACCUM_UAV_CP_COHER_CNTL | S_0085F0_SH_ACTION_ENA(1) |
                        TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME;
            /* Query samples are never larger than 0x100 bytes. */
            *packet++ = 1;
            uint32_t const * const packet_cp_coher_base = packet;
            *packet++ = (uint32_t)(accumulator->va >> 8);
            *packet++ = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;
            terakan_gfx_command_writer_add_relocation(
               command_writer, &packet, packet_cp_coher_base, *packet_cp_coher_base,
               TERASCALE_WDDM_PATCH_IDS_SURFACE_SYNC_COHER_BASE,
               terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                         accumulator, true, true,
                                                         TERAKAN_BO_PRIORITY_QUERY));
            terakan_gfx_command_writer_emit_done(command_writer, packet);
         }
      }

      terakan_meta_query_accum(command_writer,
                               &begin_indirect_buffer->query_begin_end_samples[sample_index][1],
                               &end_indirect_buffer->query_begin_end_samples[sample_index][0],
                               query_pool->bo, end_sample_va, accumulation_uav_dwords);

      /* Make UAV writes of the final result available to other query operations (in ME). */
      /* TODO(Triang3l): Is it needed after switching to separate availability? */
      {
         packet = terakan_gfx_command_writer_emit_with_bo(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5, 1, 1, 0);
         if (unlikely(packet == NULL)) {
            return;
         }
         *packet++ = PKT3(PKT3_SURFACE_SYNC, 4 - 1, 0);
         *packet++ =
            TERAKAN_META_QUERY_ACCUM_UAV_CP_COHER_CNTL | TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME;
         VkDeviceSize const sample_extent_bytes = query_type == VK_QUERY_TYPE_OCCLUSION
                                                     ? samples_size_bytes - sizeof(uint64_t)
                                                     : samples_size_bytes / 2;
         *packet++ = (uint32_t)((end_sample_va + (sample_extent_bytes - 1)) >> 8) -
                     (uint32_t)(end_sample_va >> 8) + 1;
         uint32_t const * const packet_cp_coher_base = packet;
         *packet++ = (uint32_t)(end_sample_va >> 8);
         *packet++ = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;
         terakan_gfx_command_writer_add_relocation(
            command_writer, &packet, packet_cp_coher_base, *packet_cp_coher_base,
            TERASCALE_WDDM_PATCH_IDS_SURFACE_SYNC_COHER_BASE,
            terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                      accumulator, false, true,
                                                      TERAKAN_BO_PRIORITY_QUERY));
         terakan_gfx_command_writer_emit_done(command_writer, packet);
      }
   }

   /* Mark the query as available. Using CP DMA instead of MEM_WRITE because the latter requires an
    * alignment of 8 rather than 4.
    */
   terakan_cp_dma_fill(
      command_writer, UINT32_MAX, query_pool->bo,
      query_pool->bo->va + terakan_query_pool_availability_offset_bytes(query_pool, query),
      TERAKAN_BO_PRIORITY_QUERY, sizeof(uint32_t));
   /* Make the availability write available to other query operations (in ME), and also to the CPU
    * as soon as possible.
    */
   terakan_cp_dma_sync_cp_me(command_writer);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdWriteTimestamp2(VkCommandBuffer const commandBuffer,
                           UNUSED VkPipelineStageFlags2 const stage, VkQueryPool const queryPool,
                           uint32_t const query)
{
   struct terakan_command_buffer * const command_buffer =
      terakan_command_buffer_from_handle(commandBuffer);
   if (terakan_query_reject_terascale_1(command_buffer)) {
      return;
   }
   struct terakan_query_pool const * const query_pool = terakan_query_pool_from_handle(queryPool);

   /* #MemoryIntegrity
    * Checking the query type is not necessary to prevent going out of bounds, a timestamp query is
    * 1 counter, and all query types have at least 1 counter per sample.
    */
   if (unlikely(query >= query_pool->vk.query_count)) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer = command_buffer->command_writer.gfx;

   terakan_query_sync_before_pool_write(command_writer);

   uint64_t const sample_va =
      query_pool->bo->va + terakan_query_pool_samples_offset_bytes(query_pool, query);

   /* TODO(Triang3l): Use COPY_DATA for TOP_OF_PIPE timestamp writes on kernel drivers supporting
    * it, although some applications misuse TOP_OF_PIPE timestamps:
    * https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/22823
    */

   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 6, 1, 0, 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_EVENT_WRITE_EOP, 5 - 1, 0);
   *packet++ = EVENT_TYPE(EVENT_TYPE_BOTTOM_OF_PIPE_TS) | EVENT_INDEX(5);
   uint32_t const * const packet_address = packet;
   *packet++ = (uint32_t)sample_va;
   /* Waiting for write confirmation. */
   *packet++ = ((sample_va >> 32) & 0xFF) | EOP_INT_SEL(EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM) |
               EOP_DATA_SEL(EOP_DATA_SEL_TIMESTAMP);
   /* DATA_LO/HI is unused. */
   *packet++ = 0;
   *packet++ = 0;
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_address, packet_address + 1,
      TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_EOP_LO, TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_EOP_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                query_pool->bo, false, true,
                                                TERAKAN_BO_PRIORITY_QUERY));
   terakan_gfx_command_writer_emit_done(command_writer, packet);

   /* Mark the query as available. Using CP DMA instead of MEM_WRITE because the latter requires an
    * alignment of 8 rather than 4.
    */
   terakan_cp_dma_fill(
      command_writer, UINT32_MAX, query_pool->bo,
      query_pool->bo->va + terakan_query_pool_availability_offset_bytes(query_pool, query),
      TERAKAN_BO_PRIORITY_QUERY, sizeof(uint32_t));
   /* Make the availability write available to other query operations (in ME), and also to the CPU
    * as soon as possible.
    */
   terakan_cp_dma_sync_cp_me(command_writer);
}

static uint64_t
terakan_query_swap_counter(uint64_t const counter)
{
#if UTIL_ARCH_BIG_ENDIAN
   /* "Swap function used for data write" in INDIRECT_BUFFER packets is ENDIAN_8IN32. */
   counter = (counter >> 32) | (counter << 32);
#endif
   return counter;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_GetQueryPoolResults(VkDevice const deviceHandle, VkQueryPool const queryPool,
                            uint32_t firstQuery, uint32_t queryCount, UNUSED size_t const dataSize,
                            void * const pData, VkDeviceSize const stride,
                            VkQueryResultFlags const flags)
{
   VkResult result = VK_SUCCESS;

   struct terakan_query_pool const * const query_pool = terakan_query_pool_from_handle(queryPool);

   terakan_query_pool_clamp_range(query_pool, &firstQuery, &queryCount);

   uint64_t const * src_samples_ptr =
      (uint64_t const *)((char const *)query_pool->bo->mapping +
                         terakan_query_pool_samples_offset_bytes(query_pool, firstQuery));
   uint32_t const * src_availability_ptr =
      (uint32_t const *)((char const *)query_pool->bo->mapping +
                         terakan_query_pool_availability_offset_bytes(query_pool, firstQuery));

   unsigned const dst_result_size_counters = terakan_query_pool_vk_result_size_counters(query_pool);
   char * dst_ptr = pData;

   for (uint32_t queries_remaining = queryCount; queries_remaining != 0;) {
      bool const query_available = p_atomic_read(src_availability_ptr) != 0;
      if (!query_available) {
         if (flags & VK_QUERY_RESULT_WAIT_BIT) {
            if (unlikely(vk_device_is_lost(vk_device_from_handle(deviceHandle)))) {
               result = VK_ERROR_DEVICE_LOST;
            } else {
               thrd_yield();
               continue;
            }
         }

         /* Don't downgrade VK_ERROR_DEVICE_LOST to VK_NOT_READY. */
         if (result == VK_SUCCESS) {
            result = VK_NOT_READY;
         }

         /* Section "Query Operation" of the Vulkan 1.0.28 specification says:
          *
          *     "If VK_QUERY_RESULT_WAIT_BIT and VK_QUERY_RESULT_PARTIAL_BIT are both not set then
          *     no result values are written to pData for queries that are in the unavailable state
          *     at the time of the call, and vkGetQueryPoolResults returns VK_NOT_READY. However,
          *     availability state is still written to pData for those queries if
          *     VK_QUERY_RESULT_WITH_AVAILABILITY_BIT is set."
          */
         if (flags & VK_QUERY_RESULT_PARTIAL_BIT) {
            memset(dst_ptr, 0,
                   (flags & VK_QUERY_RESULT_64_BIT ? sizeof(uint64_t) : sizeof(uint32_t)) *
                      dst_result_size_counters);
         }
      } else {
         uint64_t query_result[TERAKAN_QUERY_POOL_MAX_VK_RESULT_COUNTERS];

         switch (query_pool->vk.query_type) {
         case VK_QUERY_TYPE_OCCLUSION: {
            query_result[0] = 0;
            for (unsigned sample_qword_index = 0;
                 sample_qword_index < query_pool->samples_size_counters; sample_qword_index += 2) {
               /* For disabled render backends, all samples are zero-initialized and never modified
                * in the driver, so no need to check the upper bit.
                */
               query_result[0] +=
                  terakan_query_swap_counter(src_samples_ptr[sample_qword_index + 1]) -
                  terakan_query_swap_counter(src_samples_ptr[sample_qword_index]);
            }
         } break;

         case VK_QUERY_TYPE_PIPELINE_STATISTICS: {
            unsigned dst_counter_index = 0;
            u_foreach_bit (pipelinestat_vk_index, query_pool->vk.pipeline_statistics) {
               enum terakan_query_pipelinestat_hw_counter const pipelinestat_hw_index =
                  terakan_query_pipelinestat_vk_hw_counters[pipelinestat_vk_index];
               query_result[dst_counter_index] =
                  terakan_query_swap_counter(
                     src_samples_ptr[TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_COUNT +
                                     pipelinestat_hw_index]) -
                  terakan_query_swap_counter(src_samples_ptr[pipelinestat_hw_index]);
               ++dst_counter_index;
            }
            assert(dst_counter_index == dst_result_size_counters);
         } break;

         case VK_QUERY_TYPE_TIMESTAMP: {
            query_result[0] = terakan_query_swap_counter(*src_samples_ptr);
         } break;

         case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT: {
            for (unsigned counter_index = 0;
                 counter_index < sizeof(struct terakan_query_streamout_sample) / sizeof(uint64_t);
                 ++counter_index) {
               query_result[counter_index] =
                  terakan_query_swap_counter(
                     src_samples_ptr[sizeof(struct terakan_query_streamout_sample) /
                                        sizeof(uint64_t) +
                                     counter_index]) -
                  terakan_query_swap_counter(src_samples_ptr[counter_index]);
            }
         } break;

         default:
            assert(!"Unsupported query type");
         }

         if (flags & VK_QUERY_RESULT_64_BIT) {
            uint64_t * const dst_counters = (uint64_t *)dst_ptr;
            for (unsigned counter_index = 0; counter_index < dst_result_size_counters;
                 ++counter_index) {
               dst_counters[counter_index] = query_result[counter_index];
            }
         } else {
            uint32_t * const dst_counters = (uint32_t *)dst_ptr;
            for (unsigned counter_index = 0; counter_index < dst_result_size_counters;
                 ++counter_index) {
               dst_counters[counter_index] = (uint32_t)query_result[counter_index];
            }
         }
      }

      if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
         if (flags & VK_QUERY_RESULT_64_BIT) {
            ((uint64_t *)dst_ptr)[dst_result_size_counters] = query_available ? UINT64_MAX : 0;
         } else {
            ((uint32_t *)dst_ptr)[dst_result_size_counters] = query_available ? UINT32_MAX : 0;
         }
      }

      --queries_remaining;
      src_samples_ptr += query_pool->samples_size_counters;
      ++src_availability_ptr;
      dst_ptr += stride;
   }

   return result;
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyQueryPool(VkDevice const deviceHandle, VkQueryPool const queryPool,
                         VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_query_pool * const query_pool = terakan_query_pool_from_handle(queryPool);

   if (query_pool == NULL) {
      return;
   }

   terakan_bo_free(query_pool->bo, pAllocator);

   vk_query_pool_destroy(vk_device_from_handle(deviceHandle), pAllocator, &query_pool->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateQueryPool(VkDevice const deviceHandle,
                        VkQueryPoolCreateInfo const * const pCreateInfo,
                        VkAllocationCallbacks const * const pAllocator,
                        VkQueryPool * const pQueryPool)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);
   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);

   uint8_t samples_size_counters;
   /* Multiplication by 2 for beginning and ending values.
    *
    * For occlusion queries, beginning and end samples for each render backend are interleaved:
    * RB 0 beginning, RB 0 end, RB 1 beginning, RB 1 end...
    *
    * For pipeline statistics and transform feedback queries, the end statistics structure is after
    * the beginning statistics structure.
    */
   switch (pCreateInfo->queryType) {
   case VK_QUERY_TYPE_OCCLUSION:
      samples_size_counters = 2 << physical_device->chip_info.max_render_backends_log2;
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      /* #MemoryIntegrity: Don't allow invalid pipeline statistics flags to cause inconsistent
       * address calculations, especially in vkCmdCopyQueryPoolResults.
       */
      if (unlikely((uint32_t)pCreateInfo->pipelineStatistics >>
                   ARRAY_SIZE(terakan_query_pipelinestat_vk_hw_counters))) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "Pipeline statistics flags 0x%" PRIX32 " contain unsupported bits",
                          (uint32_t)pCreateInfo->pipelineStatistics);
      }
      /* #MemoryIntegrity: Don't work with zero-size results, especially avoid division by zero.
       * VUID-VkQueryPoolCreateInfo-queryType-09534: "If queryType is
       * VK_QUERY_TYPE_PIPELINE_STATISTICS, pipelineStatistics must not be zero"
       */
      if (unlikely(!pCreateInfo->pipelineStatistics)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "No pipeline statistics counters enabled");
      }
      samples_size_counters = TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_COUNT * 2;
      break;
   case VK_QUERY_TYPE_TIMESTAMP:
      samples_size_counters = 1;
      break;
   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT:
      samples_size_counters = sizeof(struct terakan_query_streamout_sample) / sizeof(uint64_t) * 2;
      break;
   default:
      /* #MemoryIntegrity: Don't work with invalid query types, handling of which is undefined in
       * the query pool functions.
       */
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT, "Query type %s is not supported",
                       vk_QueryType_to_str(pCreateInfo->queryType));
   }

   /* Append an array of 32-bit result availabilities.
    * The availabilities are explicitly stored separately from the samples, rather than using
    * special sample values, to avoid dependencies on the memory access granularities and ordering
    * involved on both the device and the host, so that samples are made available only after the
    * writes of them have been fully confirmed, and also to make sure that any sample values that
    * the device can write are considered valid.
    */
   VkDeviceSize bo_size =
      (VkDeviceSize)(sizeof(uint64_t) * samples_size_counters + sizeof(uint32_t)) *
      pCreateInfo->queryCount;

   VkResult result;

   struct terakan_query_pool * const query_pool =
      vk_query_pool_create(&device->vk, pCreateInfo, pAllocator, sizeof(struct terakan_query_pool));
   if (query_pool == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   query_pool->samples_size_counters = samples_size_counters;

   if (query_pool->vk.query_type == VK_QUERY_TYPE_PIPELINE_STATISTICS) {
      query_pool->copy_dst_result_size_counters =
         util_bitcount((uint32_t)query_pool->vk.pipeline_statistics);
      u_foreach_bit (pipelinestat_vk_index, query_pool->vk.pipeline_statistics) {
         query_pool->pipelinestat_hw_counters |=
            BITFIELD_BIT(terakan_query_pipelinestat_vk_hw_counters[pipelinestat_vk_index]);
      }
      terakan_meta_query_copy_init_offsets(query_pool->vk.pipeline_statistics,
                                           query_pool->pipelinestat_copy_offsets_32_bit,
                                           query_pool->pipelinestat_copy_offsets_64_bit);
   } else if (query_pool->vk.query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT) {
      query_pool->copy_dst_result_size_counters = 2;
   } else {
      query_pool->copy_dst_result_size_counters = 1;
   }

   /* UAVs with 4 bytes per element will be used for accumulation of samples from multiple indirect
    * buffers.
    */
   unsigned const uav_bytes_per_element = sizeof(uint32_t);
   if (physical_device->submission_info_gfx.buffer_uav_validated_as_image) {
      uint32_t const uav_size_granularity =
         uav_bytes_per_element *
         terakan_format_pitch_alignment_linear_surfels(
            uav_bytes_per_element, physical_device->tiling_info.pipe_interleave_bytes_log2);
      bo_size = ALIGN_POT(bo_size, (VkDeviceSize)uav_size_granularity);
   }
   result = device->winsys_fn->bo->allocate_device_memory(
      device, bo_size,
      1u << terakan_color_descriptor_buffer_uav_base_granularity_log2(uav_bytes_per_element,
                                                                      physical_device),
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
         VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
      0, pAllocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &query_pool->bo);
   if (result != VK_SUCCESS) {
      result = vk_error(device, result);
      goto fail_query_pool;
   }

   if (terakan_bo_map(query_pool->bo) == NULL) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_bo;
   }

   if (pCreateInfo->queryType == VK_QUERY_TYPE_OCCLUSION) {
      /* Make sure that for disabled render backends, all subtractions are 0 - 0. */
      memset(
         (char *)query_pool->bo->mapping + terakan_query_pool_samples_offset_bytes(query_pool, 0),
         0, sizeof(uint64_t) * samples_size_counters * pCreateInfo->queryCount);
   }

   if (pCreateInfo->flags & VK_QUERY_POOL_CREATE_RESET_BIT_KHR) {
      terakan_query_pool_reset(query_pool, 0, query_pool->vk.query_count);
   }

   *pQueryPool = terakan_query_pool_to_handle(query_pool);
   return VK_SUCCESS;

fail_bo:
   terakan_bo_free(query_pool->bo, pAllocator);
fail_query_pool:
   vk_query_pool_destroy(&device->vk, pAllocator, &query_pool->vk);
   return result;
}

void
terakan_query_sample_in_new_indirect_buffer(struct terakan_gfx_command_writer * const command_writer)
{
   for (unsigned sample_index = 0; sample_index < TERAKAN_QUERY_SAMPLE_INDEX_COUNT;
        ++sample_index) {
      if (command_writer->active_query_counts[sample_index] == 0) {
         continue;
      }
      uint32_t const event_initiator = terakan_query_sample_event_initiators[sample_index];
      /* Take both beginning (in the preamble) and, similarly to when beginning a query (as some
       * queries may stay active after the end of this new indirect buffer), the end (in the
       * finalizer) samples.
       */
      for (unsigned is_end_sample = 0; is_end_sample <= 1; ++is_end_sample) {
         struct terakan_command_buffer_indirect_buffer_query_sample * const sample =
            &command_writer->indirect_buffer->query_begin_end_samples[sample_index][is_end_sample];
         if (sample_index == TERAKAN_QUERY_SAMPLE_INDEX_ZPASS && is_end_sample) {
            /* Z pass samples from different render backends have a stride of 2 counters, interleave
             * the beginning and the end.
             */
            *sample = command_writer->indirect_buffer->query_begin_end_samples[sample_index][0];
         } else {
            /* No query type has samples larger than a kcache line. */
            void * const sample_mapping = terakan_push_buffer_allocate_kcache(
               command_writer->base.command_buffer, TERAKAN_KCACHE_HW_LINE_BYTES, &sample->bo,
               &sample->va_kcache_lines);
            if (unlikely(sample_mapping == NULL)) {
               return;
            }
            if (sample_index == TERAKAN_QUERY_SAMPLE_INDEX_ZPASS) {
               /* Make sure that for disabled render backends, all subtractions are 0 - 0. */
               memset(sample_mapping, 0,
                      (sizeof(uint64_t) * 2)
                         << terakan_gfx_command_writer_physical_device(command_writer)
                               ->chip_info.max_render_backends_log2);
            }
         }
         uint32_t * const packet_start = terakan_gfx_command_writer_emit_with_bo(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 4, 1, 0, 1);
         if (unlikely(packet_start == NULL)) {
            return;
         }
         uint32_t * packet = packet_start;
         if (is_end_sample) {
            /* Write the end sample to the finalizer instead. */
            packet = terakan_gfx_command_writer_add_finalizer(command_writer, 4, 0, 1);
         }
         *packet++ = PKT3(PKT3_EVENT_WRITE, 3 - 1, 0);
         *packet++ = event_initiator;
         uint32_t const * const packet_address = packet;
         *packet++ =
            (sample->va_kcache_lines << 8) +
            (sample_index == TERAKAN_QUERY_SAMPLE_INDEX_ZPASS && is_end_sample ? sizeof(uint64_t)
                                                                               : 0);
         *packet++ = sample->va_kcache_lines >> 24;
         terakan_gfx_command_writer_add_relocation_for_40_bits(
            command_writer, &packet, packet_address, packet_address + 1,
            TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_LO, TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_HI,
            terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                      sample->bo, false, true,
                                                      TERAKAN_BO_PRIORITY_QUERY));
         /* Pass the actual append pointer for the bottom-up part (not the finalizer). */
         terakan_gfx_command_writer_emit_done(command_writer,
                                              is_end_sample ? packet_start : packet);
      }
   }
}
