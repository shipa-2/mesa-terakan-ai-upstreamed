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

#include "terakan_command_buffer.h"

#include "terakan_barrier.h"
#include "terakan_cp_dma.h"
#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_instance.h"
#include "terakan_limits.h"
#include "terakan_physical_device.h"
#include "terakan_queue.h"
#include "terakan_shader.h"
#include "terakan_vertex_input.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/bitscan.h"
#include "util/hash_table.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void
terakan_bo_reference_writer_reset(struct terakan_bo_reference_writer * const writer,
                                  void * const bo_references, uint32_t const max_bo_reference_count)
{
   writer->references = bo_references;
   assert(max_bo_reference_count <= TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT);
   writer->max_reference_count = max_bo_reference_count;

   writer->reference_count = 0;

   BITSET_ZERO(writer->map_entries_used);
}

uint32_t
terakan_bo_reference_writer_add_reference(struct terakan_bo_reference_writer * const writer,
                                          struct terakan_bo const * const bo, bool const is_reading,
                                          bool const is_writing,
                                          enum terakan_bo_priority const priority)
{
   /* Provide two slots per hash value for quick handling of collisions by effectively doing
    * separate chaining if there are only 2 BOs per hash in this open addressing scheme (though this
    * separate-chaining-like behavior is not guaranteed if BOs with another hash value have stomped
    * on the two entries for this hash value if there were many collisions for other hash values).
    */
   uint32_t const hash = (bo->creation_number << 1) & TERAKAN_BO_REFERENCE_HASH_MASK;

   /* Search for the hash map entry.
    * Expect that more recently created BOs are more likely to be used than older ones (in a
    * scenario of an application preferring dedicated allocations - in applications with
    * suballocation from large allocations, collisions are less likely overall as the total number
    * of allocations ever created is likely to be smaller). Thus, because the hash is the lower bits
    * of the BO creation number, by going forward from the most recently created BO, collision
    * resolution will immediately end up at a hash value for a very old BO.
    */
   uint32_t reference_index = UINT32_MAX;
   uint32_t collisions;
   for (collisions = 0; collisions <= TERAKAN_BO_REFERENCE_HASH_MASK; ++collisions) {
      uint32_t const check_hash = (hash + collisions) & TERAKAN_BO_REFERENCE_HASH_MASK;
      if (!BITSET_TEST(writer->map_entries_used, check_hash)) {
         /* Didn't find an existing BO. */
         break;
      }
      uint32_t const check_reference_index = writer->map[check_hash];
      if (writer->reference_bos[check_reference_index] == bo) {
         /* Found an existing BO. */
         reference_index = check_reference_index;
         break;
      }
   }
   if (unlikely(collisions > TERAKAN_BO_REFERENCE_HASH_MASK)) {
      /* No free space in the hash map.
       * External code may assume that this will never happen if there's space for BO references
       * themselves, so normally this should happen only if the capacity of the hash map matches the
       * total maximum number of BO references, and there's already no free space in the BO
       * reference array.
       * A hash map smaller than the maximum BO reference count is pointless anyway because it would
       * effectively clamp the maximum BO reference count.
       */
      assert(writer->reference_count >= writer->max_reference_count);
      return UINT32_MAX;
   }

   /* Create the new or update the existing reference. */
   size_t const reference_size = bo->device->bo_reference_size;
   if (reference_index != UINT32_MAX) {
      bo->device->winsys_fn->queue->update_bo_reference(
         (char *)writer->references + reference_size * reference_index, bo, is_reading, is_writing,
         priority);
   } else {
      if (unlikely(writer->reference_count >= writer->max_reference_count)) {
         return UINT32_MAX;
      }
      reference_index = writer->reference_count++;
      writer->reference_bos[reference_index] = bo;
      bo->device->winsys_fn->queue->create_bo_reference(
         (char *)writer->references + reference_size * reference_index, bo, is_reading, is_writing,
         priority);
   }

   /* Insert the BO reference in the beginning of the collision chain for this hash, moving the
    * collisions forward by one (so they're ordered from the most recently used to the least
    * recently used), so when this BO is referenced again in the near future, it will be found
    * quickly.
    */
   for (uint32_t collision_index = 0; collision_index < collisions; ++collision_index) {
      writer->map[(hash + (collisions - collision_index)) & TERAKAN_BO_REFERENCE_HASH_MASK] =
         writer->map[(hash + (collisions - collision_index - 1)) & TERAKAN_BO_REFERENCE_HASH_MASK];
   }
   BITSET_SET(writer->map_entries_used, (hash + collisions) & TERAKAN_BO_REFERENCE_HASH_MASK);
   writer->map[hash] = reference_index;

   return reference_index;
}

struct terakan_queue_submission_size
terakan_command_buffer_optimal_submission_size_gfx(
   struct terakan_physical_device_submission_info const * const submission_info_gfx)
{
   /* Subtract the reserved amount from the optimal size of the application's indirect buffers
    * because it's preferable to keep the amount round, as the winsys may internally allocate its
    * submission memory with a large alignment (for instance, according to the addresses, it seems
    * that in the WDDM Radeon Software driver, memory for submissions is allocated with VirtualAlloc
    * granularity), and adding a few BO references or relocations beyond some power-of-two amount
    * may result in a lot of padding.
    */
   return terakan_queue_submission_size_subtract_saturating(
      (struct terakan_queue_submission_size){
         .bo_references = TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT,
         .indirect_buffer_dwords = TERAKAN_GFX_OPTIMAL_INDIRECT_BUFFER_SIZE_DWORDS,
         .relocations =
            submission_info_gfx->relocation_type == TERAKAN_QUEUE_RELOCATION_TYPE_WDDM_PATCH
               ? TERAKAN_GFX_OPTIMAL_WDDM_RELOCATION_COUNT
               : 0,
      },
      submission_info_gfx->submission_outer_reserved_amount);
}

#define TERAKAN_PUSH_BUFFER_SIZE_BYTES                                                             \
   ALIGN_POT(MAX2((uint32_t)1 << 16, sizeof(uint32_t) * 2 * TERAKAN_VERTEX_INPUT_FS_MAX_QWORDS),   \
             TERAKAN_KCACHE_HW_LINE_BYTES)
#define TERAKAN_PUSH_BUFFER_ALIGNMENT ((uint32_t)1 << 8)
static_assert(TERAKAN_PUSH_BUFFER_SIZE_BYTES >= TERAKAN_KCACHE_HW_MAX_BUFFER_SIZE_BYTES &&
                 TERAKAN_PUSH_BUFFER_ALIGNMENT >= TERAKAN_KCACHE_HW_LINE_BYTES,
              "Push buffers must be usable for kcache buffers.");
static_assert(TERAKAN_PUSH_BUFFER_SIZE_BYTES >= ((uint32_t)1 << 16) &&
                 TERAKAN_PUSH_BUFFER_ALIGNMENT >= TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT,
              "Push buffers must be usable for vkCmdUpdateBuffer done via CP DMA.");
static_assert(TERAKAN_PUSH_BUFFER_SIZE_BYTES >=
                    sizeof(uint32_t) * 2 * TERAKAN_VERTEX_INPUT_FS_MAX_QWORDS &&
                 TERAKAN_PUSH_BUFFER_ALIGNMENT >= TERAKAN_SHADER_PROGRAM_ALIGNMENT,
              "Push buffers must be usable for dynamic vertex fetch shaders.");
static_assert(TERAKAN_PUSH_BUFFER_SIZE_BYTES >= sizeof(uint32_t) * 3 &&
                 TERAKAN_PUSH_BUFFER_ALIGNMENT >= sizeof(uint32_t),
              "Push buffers must be usable for direct draw and dispatch parameters.");
static_assert(
   TERAKAN_PUSH_BUFFER_SIZE_BYTES >= sizeof(uint32_t) * 4 &&
      TERAKAN_PUSH_BUFFER_ALIGNMENT >= sizeof(uint32_t),
   "Push buffers must be usable for at least one rectangle with 16.16 coordinates packed in vertex "
   "indices.");

void *
terakan_push_buffer_allocate(struct terakan_command_buffer * const command_buffer,
                             uint32_t const size_bytes, uint32_t const alignment_bytes,
                             struct terakan_bo const ** const bo_out, uint64_t * const va_out)
{
   assert(util_is_power_of_two_nonzero(alignment_bytes));
   assert(alignment_bytes <= TERAKAN_PUSH_BUFFER_ALIGNMENT);

   assert(size_bytes <= TERAKAN_PUSH_BUFFER_SIZE_BYTES);
   if (unlikely(size_bytes > TERAKAN_PUSH_BUFFER_SIZE_BYTES)) {
      vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_UNKNOWN);
      return NULL;
   }

   if (!list_is_empty(&command_buffer->push_buffers)) {
      uint32_t const existing_buffer_offset_bytes =
         ALIGN_POT(command_buffer->current_push_buffer_used_bytes, alignment_bytes);
      uint32_t const existing_buffer_new_used_bytes = existing_buffer_offset_bytes + size_bytes;
      if (existing_buffer_new_used_bytes <= TERAKAN_PUSH_BUFFER_SIZE_BYTES) {
         command_buffer->current_push_buffer_used_bytes = existing_buffer_new_used_bytes;
         struct terakan_push_buffer * const existing_buffer =
            list_first_entry(&command_buffer->push_buffers, struct terakan_push_buffer, link);
         *bo_out = existing_buffer->bo;
         *va_out = existing_buffer->bo->va + existing_buffer_offset_bytes;
         return (char *)existing_buffer->bo->mapping + existing_buffer_offset_bytes;
      }
   }

   struct terakan_push_buffer * new_buffer;

   struct terakan_command_pool * const command_pool =
      container_of(command_buffer->vk.pool, struct terakan_command_pool, vk);
   if (!list_is_empty(&command_pool->push_buffers_free)) {
      new_buffer =
         list_first_entry(&command_pool->push_buffers_free, struct terakan_push_buffer, link);
      list_del(&new_buffer->link);
   } else {
      new_buffer = vk_alloc(&command_pool->vk.alloc, sizeof(struct terakan_push_buffer),
                            alignof(struct terakan_push_buffer), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (new_buffer == NULL) {
         vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }

      struct terakan_device * const device = terakan_command_buffer_device(command_buffer);

      VkResult const bo_allocate_result = device->winsys_fn->bo->allocate_device_memory(
         device, TERAKAN_PUSH_BUFFER_SIZE_BYTES, TERAKAN_PUSH_BUFFER_ALIGNMENT,
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
         0, &command_pool->vk.alloc, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &new_buffer->bo);
      if (bo_allocate_result != VK_SUCCESS) {
         vk_command_buffer_set_error(&command_buffer->vk, bo_allocate_result);
         vk_free(&command_pool->vk.alloc, new_buffer);
         return NULL;
      }

      if (terakan_bo_map(new_buffer->bo) == NULL) {
         vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         terakan_bo_free(new_buffer->bo, &command_pool->vk.alloc);
         vk_free(&command_pool->vk.alloc, new_buffer);
         return NULL;
      }
   }

   list_add(&new_buffer->link, &command_buffer->push_buffers);
   command_buffer->current_push_buffer_used_bytes = size_bytes;
   *bo_out = new_buffer->bo;
   *va_out = new_buffer->bo->va;
   return new_buffer->bo->mapping;
}

static struct terakan_command_buffer_indirect_buffer *
terakan_command_buffer_new_indirect_buffer(struct terakan_command_buffer * const command_buffer)
{
   if (vk_command_buffer_has_error(&command_buffer->vk)) {
      return NULL;
   }

   struct terakan_command_pool * const command_pool =
      container_of(command_buffer->vk.pool, struct terakan_command_pool, vk);

   struct terakan_command_buffer_indirect_buffer * indirect_buffer;
   if (!list_is_empty(&command_pool->indirect_buffers_free)) {
      indirect_buffer = list_first_entry(&command_pool->indirect_buffers_free,
                                         struct terakan_command_buffer_indirect_buffer, link);
      list_del(&indirect_buffer->link);
   } else {
      indirect_buffer = vk_alloc(
         &command_pool->vk.alloc, sizeof(struct terakan_command_buffer_indirect_buffer),
         alignof(struct terakan_command_buffer_indirect_buffer), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (indirect_buffer == NULL) {
         vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }

      struct terakan_device const * const device = terakan_command_buffer_device(command_buffer);

      indirect_buffer->bo_references = vk_alloc(
         &command_pool->vk.alloc,
         device->bo_reference_size * device->command_buffer_submission_size_gfx.bo_references,
         device->bo_reference_alignment, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (indirect_buffer->bo_references == NULL) {
         vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         vk_free(&command_pool->vk.alloc, indirect_buffer);
         return NULL;
      }

      indirect_buffer->indirect_buffer = vk_alloc(
         &command_pool->vk.alloc,
         sizeof(uint32_t) * device->command_buffer_submission_size_gfx.indirect_buffer_dwords,
         alignof(uint32_t), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (indirect_buffer->indirect_buffer == NULL) {
         vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         vk_free(&command_pool->vk.alloc, indirect_buffer->bo_references);
         vk_free(&command_pool->vk.alloc, indirect_buffer);
         return NULL;
      }

      indirect_buffer->relocations = NULL;
      if (device->command_buffer_submission_size_gfx.relocations != 0) {
         indirect_buffer->relocations = vk_alloc(
            &command_pool->vk.alloc,
            sizeof(struct terakan_queue_relocation_wddm_patch) *
               device->command_buffer_submission_size_gfx.relocations,
            alignof(struct terakan_queue_relocation_wddm_patch), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
         if (indirect_buffer->relocations == NULL) {
            vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
            vk_free(&command_pool->vk.alloc, indirect_buffer->indirect_buffer);
            vk_free(&command_pool->vk.alloc, indirect_buffer->bo_references);
            vk_free(&command_pool->vk.alloc, indirect_buffer);
            return NULL;
         }
      }
   }

   indirect_buffer->bo_reference_count = 0;
   indirect_buffer->indirect_buffer_size_dwords = 0;
   indirect_buffer->relocation_count = 0;

   list_addtail(&indirect_buffer->link, &command_buffer->indirect_buffers);

   return indirect_buffer;
}

static void
terakan_command_buffer_release_command_writer(struct terakan_command_buffer * const command_buffer)
{
   struct terakan_gfx_command_writer * const gfx_command_writer =
      command_buffer->command_writer.gfx;
   if (gfx_command_writer == NULL) {
      return;
   }

   struct terakan_command_pool * const command_pool =
      container_of(command_buffer->vk.pool, struct terakan_command_pool, vk);

   if (gfx_command_writer->active_queries != NULL) {
      _mesa_hash_table_clear(&gfx_command_writer->active_queries->begin_indirect_buffer_ht, NULL);
      list_add(&gfx_command_writer->active_queries->free_link,
               &command_pool->active_query_tables_free);
      gfx_command_writer->active_queries = NULL;
   }

   list_add(&gfx_command_writer->base.free_link, &command_pool->command_writers_free);
   command_buffer->command_writer.gfx = NULL;
}

static void
terakan_command_buffer_release_resources(struct terakan_command_buffer * const command_buffer)
{
   struct terakan_command_pool * const command_pool =
      container_of(command_buffer->vk.pool, struct terakan_command_pool, vk);

   terakan_command_buffer_release_command_writer(command_buffer);

   list_for_each_entry_safe (struct terakan_command_buffer_indirect_buffer, indirect_buffer,
                             &command_buffer->indirect_buffers, link) {
      list_move_to(&indirect_buffer->link, &command_pool->indirect_buffers_free);
   }

   list_splice(&command_buffer->push_buffers, &command_pool->push_buffers_free);
   list_inithead(&command_buffer->push_buffers);
}

static void
terakan_command_buffer_reset(struct vk_command_buffer * const command_buffer_base,
                             UNUSED VkCommandBufferResetFlags const flags)
{
   struct terakan_command_buffer * command_buffer =
      container_of(command_buffer_base, struct terakan_command_buffer, vk);

   vk_command_buffer_reset(&command_buffer->vk);

   terakan_command_buffer_release_resources(command_buffer);
}

static void
terakan_command_buffer_destroy(struct vk_command_buffer * const command_buffer_base)
{
   struct terakan_command_buffer * command_buffer =
      container_of(command_buffer_base, struct terakan_command_buffer, vk);

   terakan_command_buffer_release_resources(command_buffer);

   vk_command_buffer_finish(&command_buffer->vk);

   vk_free(&command_buffer->vk.pool->alloc, command_buffer);
}

static VkResult
terakan_command_buffer_create(struct vk_command_pool * const command_pool,
                              UNUSED VkCommandBufferLevel level,
                              struct vk_command_buffer ** const command_buffer_out)
{
   VkResult result;

   struct terakan_command_buffer * command_buffer =
      vk_alloc(&command_pool->alloc, sizeof(struct terakan_command_buffer),
               alignof(struct terakan_command_buffer), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (command_buffer == NULL) {
      return vk_error(command_pool->base.device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   result =
      vk_command_buffer_init(command_pool, &command_buffer->vk, &terakan_command_buffer_ops, 0);
   if (result != VK_SUCCESS) {
      vk_free(&command_pool->alloc, command_buffer);
      return result;
   }

   command_buffer->vk.dynamic_graphics_state.vi = &command_buffer->dynamic_vertex_input_;
   command_buffer->vk.dynamic_graphics_state.ms.sample_locations =
      &command_buffer->dynamic_sample_locations_;

   list_inithead(&command_buffer->push_buffers);
   command_buffer->current_push_buffer_used_bytes = TERAKAN_PUSH_BUFFER_SIZE_BYTES;

   for (size_t shader_ring_index = 0; shader_ring_index < TERAKAN_SHADER_RING_INDEX_COUNT;
        ++shader_ring_index) {
      command_buffer->shader_ring_bytes_needed_for_se_shr8[shader_ring_index] = 0;
   }

   list_inithead(&command_buffer->indirect_buffers);

   command_buffer->command_writer.gfx = NULL;

   *command_buffer_out = &command_buffer->vk;
   return VK_SUCCESS;
}

struct vk_command_buffer_ops const terakan_command_buffer_ops = {
   .create = terakan_command_buffer_create,
   .reset = terakan_command_buffer_reset,
   .destroy = terakan_command_buffer_destroy,
};

#ifdef TERAKAN_REGRESSION_TEST
static void
terakan_gfx_command_writer_reset_regression_test_indirect_buffer_splitting(
   struct terakan_gfx_command_writer * const command_writer)
{
   command_writer->actions_before_next_regression_test_indirect_buffer_split =
      container_of(terakan_gfx_command_writer_physical_device(command_writer)->vk.instance,
                   struct terakan_instance const, vk)
         ->regression_test_split_indirect_buffer_after_actions;
   if (command_writer->actions_before_next_regression_test_indirect_buffer_split <= 0) {
      /* Don't enable indirect buffer splitting testing. */
      command_writer->actions_before_next_regression_test_indirect_buffer_split = -1;
   }
}
#endif

void
terakan_gfx_command_writer_end_indirect_buffer(
   struct terakan_gfx_command_writer * const command_writer)
{
#ifndef NDEBUG
   assert(command_writer->current_emission_packet_dwords == UINT32_MAX &&
          "terakan_gfx_command_writer_emit_done must be called with the final append pointer after "
          "every command emission");
#endif

   if (command_writer->indirect_buffer == NULL) {
      return;
   }

   /* Make sure that there are no pending CP DMA commands after the indirect buffer submission is
    * executed, so memory accessed by CP DMA in it can be released to the kernel after it.
    */
   terakan_cp_dma_sync_cp_me(command_writer);

   command_writer->indirect_buffer->bo_reference_count =
      command_writer->base.bo_reference_writer.reference_count;

   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);

   /* Move finalizers to the end of the rest of the packets. */
   uint32_t const finalizer_size_dwords =
      device->command_buffer_submission_size_gfx.indirect_buffer_dwords -
      (uint32_t)(command_writer->indirect_buffer_finalizer_prepend_ptr -
                 command_writer->indirect_buffer->indirect_buffer);
   if (likely(command_writer->indirect_buffer_finalizer_prepend_ptr !=
              command_writer->indirect_buffer_append_ptr)) {
      memmove(command_writer->indirect_buffer_append_ptr,
              command_writer->indirect_buffer_finalizer_prepend_ptr,
              sizeof(uint32_t) * finalizer_size_dwords);
   }
   if (terakan_device_physical_device(device)->submission_info_gfx.base.relocation_type ==
       TERAKAN_QUEUE_RELOCATION_TYPE_WDDM_PATCH) {
      /* Adjust finalizer relocation patch offsets, and clear the split offset field that's used by
       * Terakan to store the linked list of finalizer relocations.
       */
      struct terakan_queue_relocation_wddm_patch * const patches =
         (struct terakan_queue_relocation_wddm_patch *)command_writer->indirect_buffer->relocations;
      uint32_t const finalizer_distance_bytes =
         (uint32_t)(sizeof(uint32_t) * (command_writer->indirect_buffer_finalizer_prepend_ptr -
                                        command_writer->indirect_buffer_append_ptr));
      while (command_writer->indirect_buffer_finalizer_relocation_list != UINT32_MAX) {
         struct terakan_queue_relocation_wddm_patch * const patch =
            &patches[command_writer->indirect_buffer_finalizer_relocation_list];
         patch->patch_offset -= finalizer_distance_bytes;
         command_writer->indirect_buffer_finalizer_relocation_list = patch->split_offset;
         patch->split_offset = 0;
      }
   }

   uint32_t indirect_buffer_size_dwords =
      (uint32_t)(command_writer->indirect_buffer_append_ptr -
                 command_writer->indirect_buffer->indirect_buffer) +
      finalizer_size_dwords;

   while (indirect_buffer_size_dwords &
          (TERAKAN_QUEUE_INDIRECT_BUFFER_SIZE_ALIGNMENT_DWORDS_GFX - 1)) {
      command_writer->indirect_buffer->indirect_buffer[indirect_buffer_size_dwords++] =
         PKT_TYPE_S(2);
   }

   command_writer->indirect_buffer->indirect_buffer_size_dwords = indirect_buffer_size_dwords;

   command_writer->indirect_buffer = NULL;

#ifdef TERAKAN_REGRESSION_TEST
   terakan_gfx_command_writer_reset_regression_test_indirect_buffer_splitting(command_writer);
#endif
}

static void
terakan_gfx_command_writer_emit_preamble(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_physical_device const * const physical_device =
      terakan_gfx_command_writer_physical_device(command_writer);
   struct terakan_physical_device_submission_info_gfx const * const submission_info_gfx =
      &physical_device->submission_info_gfx;

   uint32_t * packet;

   /* Disable register shadowing before setting any registers. */
   packet = terakan_gfx_command_writer_emit(command_writer,
                                            TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 3);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_CONTEXT_CONTROL, 1, 0);
   *packet++ = (uint32_t)1 << 31; /* CC0_UPDATE_LOAD_ENABLES(1) */
   *packet++ = (uint32_t)1 << 31; /* CC1_UPDATE_SHADOW_ENABLES(1) */
   terakan_gfx_command_writer_emit_done(command_writer, packet);

   if (submission_info_gfx->need_sq_alu_const_mode_control) {
      /* Switch to the Direct3D 10 mode for ALU constants (provided via kcache buffers).
       * DRM Radeon 2.50.0 does this MODE_CONTROL before every indirect buffer execution and doesn't
       * allow it in submissions without virtual memory, but WDDM Radeon Software as of
       * 15.301.1901 does it in submissions after CONTEXT_CONTROL.
       */
      packet = terakan_gfx_command_writer_emit(command_writer,
                                               TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_MODE_CONTROL, 0, 0);
      *packet++ = 1;
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }
}

static void
terakan_gfx_command_writer_emit_hw_config(
   struct terakan_gfx_command_writer * const command_writer,
   enum terakan_gfx_command_writer_emit_contents const contents)
{
   if (contents == TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW) {
      if (command_writer->app_config_compute.shader != NULL) {
         terakan_hw_config_shared_compute_emit_modified(command_writer);

         if (!command_writer->hw_config_compute_initialized_in_indirect_buffer) {
            command_writer->hw_config_compute_initialized_in_indirect_buffer = true;
            terakan_hw_config_compute_set_all_modified(&command_writer->hw_config_compute);
         }
         terakan_hw_config_compute_emit_modified(command_writer);

         if (!command_writer->hw_config_sqk_initialized_in_indirect_buffer) {
            command_writer->hw_config_sqk_initialized_in_indirect_buffer = true;
            terakan_hw_config_sqk_begin_emitting_first_draw_dispatch_in_indirect_buffer(
               command_writer);
         }
         terakan_hw_config_sqk_emit_modified_for_compute(command_writer);

         terakan_hw_config_draw_emit_modified(command_writer);
         return;
      }

      terakan_hw_config_shared_draw_emit_modified(command_writer);

      if (!command_writer->hw_config_draw_initialized_in_indirect_buffer) {
         command_writer->hw_config_draw_initialized_in_indirect_buffer = true;
         terakan_hw_config_draw_set_all_modified(&command_writer->hw_config_draw);
         terakan_hw_config_draw_emit_constant(command_writer);
      }
      terakan_hw_config_draw_emit_modified(command_writer);

      if (!command_writer->hw_config_sqk_initialized_in_indirect_buffer) {
         command_writer->hw_config_sqk_initialized_in_indirect_buffer = true;
         terakan_hw_config_sqk_begin_emitting_first_draw_dispatch_in_indirect_buffer(
            command_writer);
      }
      terakan_hw_config_sqk_emit_modified_for_draw(command_writer);
   }
}

uint32_t *
terakan_gfx_command_writer_emit_with_bo(struct terakan_gfx_command_writer * const command_writer,
                                        enum terakan_gfx_command_writer_emit_contents const contents,
                                        uint32_t const packet_dwords, uint32_t const bo_count,
                                        uint32_t const relocation_for_32_bits_count,
                                        uint32_t const relocation_for_40_bits_count)
{
#ifndef NDEBUG
   assert(command_writer->current_emission_packet_dwords == UINT32_MAX &&
          "terakan_gfx_command_writer_emit_done must be called with the final append pointer after "
          "every command emission");
#endif

   if (unlikely(vk_command_buffer_has_error(&command_writer->base.command_buffer->vk))) {
      return NULL;
   }

   assert(contents != TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_INVALID);
   bool const is_inner = command_writer->is_in_outer_emit_call;
   if (is_inner) {
      assert((contents == TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG ||
              contents == TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER) &&
             "Draws and dispatches must be outer emissions, they must not be emitted by indirect "
             "buffer setup or by hardware state applying");
   } else {
      assert((contents == TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW ||
              contents == TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER) &&
             "`hw_config` emissions must be done only from within `hw_config` applying functions "
             "invoked from outer emissions");
      command_writer->is_in_outer_emit_call = true;
   }

   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);

   enum terakan_queue_relocation_type const relocation_type =
      terakan_device_physical_device(device)->submission_info_gfx.base.relocation_type;
   uint32_t relocation_count;
   bool relocation_array_used = false;
   uint32_t total_packet_dwords = packet_dwords;
   switch (relocation_type) {
   case TERAKAN_QUEUE_RELOCATION_TYPE_DRM_NOP:
      /* One relocation for the whole address. */
      relocation_count = relocation_for_32_bits_count + relocation_for_40_bits_count;
      total_packet_dwords += 2 * relocation_count;
      break;
   case TERAKAN_QUEUE_RELOCATION_TYPE_WDDM_PATCH:
      /* One relocation per address dword (two for 40-bit addresses). */
      relocation_count = relocation_for_32_bits_count + 2 * relocation_for_40_bits_count;
      relocation_array_used = true;
      break;
   default:
      assert(relocation_type == TERAKAN_QUEUE_RELOCATION_TYPE_NONE);
      relocation_count = 0;
   }

#ifdef TERAKAN_REGRESSION_TEST
   if (contents == TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW &&
       command_writer->actions_before_next_regression_test_indirect_buffer_split >= 0) {
      if (command_writer->actions_before_next_regression_test_indirect_buffer_split == 0) {
         terakan_gfx_command_writer_end_indirect_buffer(command_writer);
      }
      --command_writer->actions_before_next_regression_test_indirect_buffer_split;
   }
#endif

   if (command_writer->indirect_buffer != NULL) {
      if (!is_inner) {
         /* Apply the modified tracked configuration in the existing indirect buffer. */
         terakan_gfx_command_writer_emit_hw_config(command_writer, contents);
      }

      if (command_writer->indirect_buffer != NULL &&
          ((command_writer->indirect_buffer_finalizer_prepend_ptr -
            command_writer->indirect_buffer_append_ptr) < total_packet_dwords ||
           (device->command_buffer_submission_size_gfx.bo_references -
            command_writer->base.bo_reference_writer.reference_count) < bo_count ||
           (relocation_array_used &&
            (device->command_buffer_submission_size_gfx.relocations -
             command_writer->indirect_buffer->relocation_count) < relocation_count))) {
         /* Space exhausted in the current indirect buffer, either by inner emissions, or by this
          * emission.
          */
         terakan_gfx_command_writer_end_indirect_buffer(command_writer);
         assert(command_writer->indirect_buffer == NULL);
      }
   }

   if (command_writer->indirect_buffer == NULL) {
      if (is_inner) {
         /* The outer emission that invoked this inner emission will handle the overflow, to avoid
          * making the configuration emission functions reentrant, simplifying clearing modified
          * configuration flags in them.
          */
         return NULL;
      }

      /* Start a new indirect buffer. */

      command_writer->indirect_buffer =
         terakan_command_buffer_new_indirect_buffer(command_writer->base.command_buffer);
      if (command_writer->indirect_buffer == NULL) {
         if (!is_inner) {
            command_writer->is_in_outer_emit_call = false;
         }
         return NULL;
      }

      command_writer->indirect_buffer_append_ptr = command_writer->indirect_buffer->indirect_buffer;
      command_writer->indirect_buffer_finalizer_prepend_ptr =
         command_writer->indirect_buffer->indirect_buffer +
         device->command_buffer_submission_size_gfx.indirect_buffer_dwords;

      command_writer->indirect_buffer_finalizer_relocation_list = UINT32_MAX;

      /* Closing the previous indirect buffer should have synced it. */
      assert(command_writer->last_unsynced_cp_dma_sync_dword == NULL);

      terakan_bo_reference_writer_reset(&command_writer->base.bo_reference_writer,
                                        command_writer->indirect_buffer->bo_references,
                                        terakan_gfx_command_writer_device(command_writer)
                                           ->command_buffer_submission_size_gfx.bo_references);

      command_writer->indirect_buffer->shader_rings_bo_placeholder_reference = UINT32_MAX;
      for (size_t shader_ring_index = 0; shader_ring_index < TERAKAN_SHADER_RING_INDEX_COUNT;
           ++shader_ring_index) {
         command_writer->indirect_buffer->shader_rings[shader_ring_index]
            .set_base_argument_offsets_dwords[0] = UINT32_MAX;
      }

      memset(&command_writer->indirect_buffer->query_begin_end_samples, 0,
             sizeof(command_writer->indirect_buffer->query_begin_end_samples));

      terakan_gfx_command_writer_emit_preamble(command_writer);

      command_writer->hw_config_draw_initialized_in_indirect_buffer = false;
      command_writer->hw_config_sqk_initialized_in_indirect_buffer = false;

      terakan_hw_config_shared_indirect_buffer_begun(command_writer);

      terakan_query_sample_in_new_indirect_buffer(command_writer);

      /* Apply the tracked configuration in the new (either the first, or after an overflow)
       * indirect buffer.
       */
      terakan_gfx_command_writer_emit_hw_config(command_writer, contents);

      if (unlikely((command_writer->indirect_buffer_finalizer_prepend_ptr -
                    command_writer->indirect_buffer_append_ptr) < total_packet_dwords ||
                   (device->command_buffer_submission_size_gfx.bo_references -
                    command_writer->base.bo_reference_writer.reference_count) < bo_count ||
                   (relocation_array_used &&
                    (device->command_buffer_submission_size_gfx.relocations -
                     command_writer->indirect_buffer->relocation_count) < relocation_count))) {
         assert(
            !"The command emission and all the needed state setting can't fit in a single indirect "
             "buffer, this is likely a Terakan bug because an indirect buffer must be large enough "
             "to support the worst case");
         vk_command_buffer_set_error(&command_writer->base.command_buffer->vk,
                                     VK_ERROR_OUT_OF_HOST_MEMORY);
         if (!is_inner) {
            command_writer->is_in_outer_emit_call = false;
         }
         return NULL;
      }
   }

   if (!is_inner) {
      command_writer->is_in_outer_emit_call = false;
   }

#ifndef NDEBUG
   command_writer->current_emission_packet_dwords = total_packet_dwords;
   command_writer->current_emission_relocations_remaining = relocation_count;
#endif

   return command_writer->indirect_buffer_append_ptr;
}

uint32_t *
terakan_gfx_command_writer_add_finalizer(struct terakan_gfx_command_writer * const command_writer,
                                         uint32_t const packet_dwords,
                                         uint32_t const relocation_for_32_bits_count,
                                         uint32_t const relocation_for_40_bits_count)
{
#ifndef NDEBUG
   assert(command_writer->current_emission_packet_dwords != UINT32_MAX &&
          "Command emission must be started");
#endif

   uint32_t total_packet_dwords = packet_dwords;
   switch (terakan_gfx_command_writer_physical_device(command_writer)
              ->submission_info_gfx.base.relocation_type) {
   case TERAKAN_QUEUE_RELOCATION_TYPE_DRM_NOP:
      total_packet_dwords += 2 * (relocation_for_32_bits_count + relocation_for_40_bits_count);
      break;
   default:
      break;
   }

#ifndef NDEBUG
   assert(command_writer->current_emission_packet_dwords >= total_packet_dwords);
   command_writer->current_emission_packet_dwords -= total_packet_dwords;
#endif

   command_writer->indirect_buffer_finalizer_prepend_ptr -= total_packet_dwords;
   return command_writer->indirect_buffer_finalizer_prepend_ptr;
}

uint32_t
terakan_gfx_command_writer_add_relocation(struct terakan_gfx_command_writer * const command_writer,
                                          uint32_t ** const indirect_buffer_append_ptr,
                                          uint32_t const * const address_in_packet,
                                          uint32_t const wddm_allocation_offset,
                                          uint64_t const wddm_patch_ids,
                                          uint32_t const bo_reference)
{
   enum terakan_queue_relocation_type const relocation_type =
      terakan_gfx_command_writer_physical_device(command_writer)
         ->submission_info_gfx.base.relocation_type;

   if (relocation_type == TERAKAN_QUEUE_RELOCATION_TYPE_NONE) {
      return 0;
   }

#ifndef NDEBUG
   assert(command_writer->current_emission_packet_dwords != UINT32_MAX &&
          "Command emission must be started");
   assert(command_writer->current_emission_relocations_remaining != 0);
   --command_writer->current_emission_relocations_remaining;
#endif

   switch (relocation_type) {
   case TERAKAN_QUEUE_RELOCATION_TYPE_DRM_NOP: {
      *((*indirect_buffer_append_ptr)++) = PKT3(PKT3_NOP, 0, 0);
      uint32_t const relocation_handle =
         (uint32_t)(*indirect_buffer_append_ptr - command_writer->indirect_buffer->indirect_buffer);
      /* DRM Radeon accepts BO reference offsets in dwords, so multiplying by
       * `sizeof(struct drm_radeon_cs_reloc) / sizeof(__u32)`.
       */
      *((*indirect_buffer_append_ptr)++) = 4 * bo_reference;
      return relocation_handle;
   } break;

   case TERAKAN_QUEUE_RELOCATION_TYPE_WDDM_PATCH: {
      uint32_t const relocation_handle = command_writer->indirect_buffer->relocation_count++;
      struct terakan_queue_relocation_wddm_patch * const patch =
         &((struct terakan_queue_relocation_wddm_patch *)
              command_writer->indirect_buffer->relocations)[relocation_handle];
      /* + 1 because a hAllocation = 0 allocation list entry is prepended when submitting. */
      patch->allocation_index = 1 + bo_reference;
      patch->slot_id = (uint32_t)wddm_patch_ids;
      patch->driver_id = (uint32_t)(wddm_patch_ids >> 32);
      patch->allocation_offset = wddm_allocation_offset;
      patch->patch_offset =
         sizeof(uint32_t) *
         (uint32_t)(address_in_packet - command_writer->indirect_buffer->indirect_buffer);
      if (unlikely(address_in_packet >= command_writer->indirect_buffer_finalizer_prepend_ptr)) {
         /* Will need to adjust the patch offset in this relocation when the finalizers are moved
          * to the rest of the packets.
          */
         patch->split_offset = command_writer->indirect_buffer_finalizer_relocation_list;
         command_writer->indirect_buffer_finalizer_relocation_list = relocation_handle;
      } else {
         patch->split_offset = 0;
      }
      return relocation_handle;
   } break;

   default:
      assert(!"Unsupported relocation type");
   }

   return 0;
}

void
terakan_gfx_command_writer_emit_event_write_eop_discarding_data(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const event)
{
   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 6, 1, 0, 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   struct terakan_bo const * const gfx_discard_bo =
      terakan_gfx_command_writer_device(command_writer)->gfx_discard_bo;
   *packet++ = PKT3(PKT3_EVENT_WRITE_EOP, 5 - 1, 0);
   *packet++ = event;
   uint32_t const * const packet_address = packet;
   *packet++ = (uint32_t)gfx_discard_bo->va;      /* ADDRESS_LO */
   *packet++ = (gfx_discard_bo->va >> 32) & 0xFF; /* ADDRESS_HI, INT_SEL, DATA_SEL */
   *packet++ = 0;                                 /* DATA_LO */
   *packet++ = 0;                                 /* DATA_HI */
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_address, packet_address + 1,
      TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_EOP_LO, TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_EOP_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                gfx_discard_bo, false, true,
                                                TERAKAN_BO_PRIORITY_SYNC));
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_EndCommandBuffer(VkCommandBuffer const commandBuffer)
{
   struct terakan_command_buffer * const command_buffer =
      terakan_command_buffer_from_handle(commandBuffer);

   struct terakan_gfx_command_writer * const gfx_command_writer =
      command_buffer->command_writer.gfx;

   /* Insert a barrier for outstanding transfer writes because command buffers track the needed
    * barriers for this purpose locally, and subsequently submitted command buffers won't be aware
    * of how transfers were actually done in the current command buffer.
    */
   gfx_command_writer->pending_barrier_actions |=
      gfx_command_writer->post_buffer_copy_write_barrier_actions |
      gfx_command_writer->post_color_image_copy_write_barrier_actions |
      gfx_command_writer->post_depth_stencil_image_copy_write_barrier_actions;

   /* As barriers are deferred rather than emitted immediately in vkCmdPipelineBarrier, flush them.
    */
   terakan_barrier_emit_pending_actions(gfx_command_writer, TERAKAN_BARRIER_ACTIONS_ALL);

   terakan_gfx_command_writer_end_indirect_buffer(gfx_command_writer);

   terakan_command_buffer_release_command_writer(command_buffer);

   return vk_command_buffer_end(&command_buffer->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_BeginCommandBuffer(VkCommandBuffer const commandBuffer,
                           VkCommandBufferBeginInfo const * const pBeginInfo)
{
   struct terakan_command_buffer * const command_buffer =
      terakan_command_buffer_from_handle(commandBuffer);

   vk_command_buffer_begin(&command_buffer->vk, pBeginInfo);

   struct terakan_command_pool * const command_pool =
      container_of(command_buffer->vk.pool, struct terakan_command_pool, vk);

   assert(command_buffer->command_writer.gfx == NULL);
   struct terakan_gfx_command_writer * gfx_command_writer;
   if (!list_is_empty(&command_pool->command_writers_free)) {
      gfx_command_writer = list_first_entry(&command_pool->command_writers_free,
                                            struct terakan_gfx_command_writer, base.free_link);
      list_del(&gfx_command_writer->base.free_link);
   } else {
      gfx_command_writer =
         vk_alloc(&command_pool->vk.alloc, sizeof(struct terakan_gfx_command_writer),
                  alignof(struct terakan_gfx_command_writer), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (gfx_command_writer == NULL) {
         return vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      gfx_command_writer->active_queries = NULL;
   }
   command_buffer->command_writer.gfx = gfx_command_writer;

   gfx_command_writer->base.command_buffer = command_buffer;

   /* The first emission will request the first indirect buffer. */
   gfx_command_writer->indirect_buffer = NULL;

   gfx_command_writer->indirect_buffer_append_ptr = NULL;
   gfx_command_writer->indirect_buffer_finalizer_prepend_ptr = NULL;

   gfx_command_writer->indirect_buffer_finalizer_relocation_list = UINT32_MAX;

#ifdef TERAKAN_REGRESSION_TEST
   terakan_gfx_command_writer_reset_regression_test_indirect_buffer_splitting(gfx_command_writer);
#endif

   gfx_command_writer->is_in_outer_emit_call = false;

#ifndef NDEBUG
   gfx_command_writer->current_emission_packet_dwords = UINT32_MAX;
   gfx_command_writer->current_emission_relocations_remaining = 0;
#endif

   gfx_command_writer->last_unsynced_cp_dma_sync_dword = NULL;

   gfx_command_writer->pending_barrier_actions = 0;

   gfx_command_writer->post_buffer_copy_write_barrier_actions = 0;
   gfx_command_writer->post_color_image_copy_write_barrier_actions = 0;
   gfx_command_writer->post_depth_stencil_image_copy_write_barrier_actions = 0;

   memset(&gfx_command_writer->active_query_counts, 0,
          sizeof(gfx_command_writer->active_query_counts));
   gfx_command_writer->active_pipelinestat_streamoutstats_query_count = 0;

   gfx_command_writer->hw_config_draw_initialized_in_indirect_buffer = false;
   gfx_command_writer->hw_config_compute_initialized_in_indirect_buffer = false;
   gfx_command_writer->hw_config_sqk_initialized_in_indirect_buffer = false;
   gfx_command_writer->meta_blit_draw_session_active = false;

   struct terakan_physical_device const * const physical_device =
      terakan_command_buffer_physical_device(command_buffer);
   terakan_hw_config_shared_reset(&gfx_command_writer->hw_config_shared,
                                  &physical_device->chip_info);
   terakan_hw_config_draw_reset(&gfx_command_writer->hw_config_draw);
   terakan_hw_config_compute_reset(&gfx_command_writer->hw_config_compute);
   /* TODO(Triang3l): R9xx `USE_LS_CONSTS` (perform thorough testing, and provide a regression
    * testing flag for disabling).
    */
   terakan_hw_config_sqk_reset(&gfx_command_writer->hw_config_sqk, false);

   terakan_push_constants_state_reset(&gfx_command_writer->push_constants_state);

   struct terakan_device const * const device = terakan_command_buffer_device(command_buffer);

   terakan_app_config_draw_reset(&gfx_command_writer->app_config_draw);
   terakan_app_config_compute_reset(&gfx_command_writer->app_config_compute);
   terakan_app_config_draw_set_pa_vport_z_range_unrestricted(
      &gfx_command_writer->app_config_draw,
      device->vk.enabled_extensions.EXT_depth_range_unrestricted);
   terakan_app_config_draw_set_db_alpha_to_mask(
      &gfx_command_writer->app_config_draw,
      TERAKAN_HW_CONFIG_DRAW_DB_ALPHA_TO_MASK_OFFSETS_CLEAR_MASK,
      container_of(physical_device->vk.base.instance, struct terakan_instance const, vk)->test_flags &
            BITFIELD64_BIT(TERAKAN_TEST_SHIFT_NO_ALPHA_TO_COVERAGE_DITHERING)
         ? TERAKAN_HW_CONFIG_DRAW_DB_ALPHA_TO_MASK_OFFSETS_REGULAR
         : TERAKAN_HW_CONFIG_DRAW_DB_ALPHA_TO_MASK_OFFSETS_DITHERED);

   /* Make the graphics state fully dynamic initially, as expected when drawing with shader objects,
    * because a pipeline object hasn't been bound yet.
    */
   BITSET_ONES(command_buffer->graphics_state_is_dynamic);

   /* Section Appendix B: Memory Model "Availability, Visibility, and Domain Operations" of the
    * Vulkan 1.3.277 specification says:
    *
    *     "vkQueueSubmit performs a memory domain operation from host to device, and a visibility
    *     operation with source scope of the device domain and destination scope of all agents and
    *     references on the device."
    *
    * Make device memory visible to all agents on the device by invalidating all caches.
    * That's done via the command writer's emission logic, after setting up the initial state
    * registers, so the setup is not blocked by the waits involved.
    * This is only necessary for the first command buffer in a submission, but doing that here for
    * simplicity of submitting.
    */
   uint32_t const invalidate_caches_packets[] = {
      PKT3(PKT3_SET_CONTEXT_REG, 1, 0),
      TERAKAN_CONTEXT_REG_OFFSET(R_028354_SX_SURFACE_SYNC),
      /* Scratch buffers, cacheless UAV accesses. */
      /* TODO(Triang3l): Everything else once implemented: stream output, geometry ring buffers.
       * Research how exactly SX surface sync works overall, and whether this register applies to
       * the context being flushed (so that it needs to be set before draws and dispatches) or to
       * the flush itself (so that it must be set before the SURFACE_SYNC packet).
       */
      S_028354_SURFACE_SYNC_MASK(0b1000010000),

      PKT3(PKT3_EVENT_WRITE, 1 - 1, 0),
      EVENT_TYPE(EVENT_TYPE_CACHE_FLUSH_AND_INV_EVENT) | EVENT_INDEX(0),

      PKT3(PKT3_SURFACE_SYNC, 4 - 1, 0),
      S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_CB1_DEST_BASE_ENA(1) |
         S_0085F0_CB2_DEST_BASE_ENA(1) | S_0085F0_CB3_DEST_BASE_ENA(1) |
         S_0085F0_CB4_DEST_BASE_ENA(1) | S_0085F0_CB5_DEST_BASE_ENA(1) |
         S_0085F0_CB6_DEST_BASE_ENA(1) | S_0085F0_CB7_DEST_BASE_ENA(1) |
         S_0085F0_CB8_DEST_BASE_ENA(1) | S_0085F0_CB9_DEST_BASE_ENA(1) |
         S_0085F0_CB10_DEST_BASE_ENA(1) | S_0085F0_CB11_DEST_BASE_ENA(1) |
         S_0085F0_DB_DEST_BASE_ENA(1) | S_0085F0_TC_ACTION_ENA(1) |
         S_0085F0_VC_ACTION_ENA(physical_device->chip_info.has_vertex_cache) |
         S_0085F0_CB_ACTION_ENA(1) | S_0085F0_DB_ACTION_ENA(1) | S_0085F0_SH_ACTION_ENA(1) |
         S_0085F0_SMX_ACTION_ENA(1) | TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME,
      UINT32_MAX,
      0,
      TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL,

      /* Make all prior writes made available by various packets in ME visible to PFP (indirect
       * arguments, index buffers).
       */
      PKT3(PKT3_PFP_SYNC_ME, 0, 0),
      0,
   };
   {
      uint32_t * invalidate_cache_packets_ptr = terakan_gfx_command_writer_emit(
         gfx_command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER,
         ARRAY_SIZE(invalidate_caches_packets));
      if (likely(invalidate_cache_packets_ptr != NULL)) {
         memcpy(invalidate_cache_packets_ptr, invalidate_caches_packets,
                sizeof(invalidate_caches_packets));
         invalidate_cache_packets_ptr += ARRAY_SIZE(invalidate_caches_packets);
         terakan_gfx_command_writer_emit_done(gfx_command_writer, invalidate_cache_packets_ptr);
      }
   }

   return vk_command_buffer_get_record_result(&command_buffer->vk);
}

static void
terakan_command_pool_trim_resources(struct terakan_command_pool * const command_pool)
{
   list_for_each_entry_safe (struct terakan_gfx_command_writer, command_writer,
                             &command_pool->command_writers_free, base.free_link) {
      vk_free(&command_pool->vk.alloc, command_writer);
   }
   list_inithead(&command_pool->command_writers_free);

   list_for_each_entry_safe (struct terakan_query_active_table, active_query_table,
                             &command_pool->active_query_tables_free, free_link) {
      ralloc_free(active_query_table);
   }
   list_inithead(&command_pool->active_query_tables_free);

   list_for_each_entry_safe (struct terakan_command_buffer_indirect_buffer, indirect_buffer,
                             &command_pool->indirect_buffers_free, link) {
      vk_free(&command_pool->vk.alloc, indirect_buffer->relocations);
      vk_free(&command_pool->vk.alloc, indirect_buffer->indirect_buffer);
      vk_free(&command_pool->vk.alloc, indirect_buffer->bo_references);
      vk_free(&command_pool->vk.alloc, indirect_buffer);
   }
   list_inithead(&command_pool->indirect_buffers_free);

   list_for_each_entry_safe (struct terakan_push_buffer, push_buffer,
                             &command_pool->push_buffers_free, link) {
      terakan_bo_free(push_buffer->bo, &command_pool->vk.alloc);
      vk_free(&command_pool->vk.alloc, push_buffer);
   }
   list_inithead(&command_pool->push_buffers_free);
}

VKAPI_ATTR void VKAPI_CALL
terakan_TrimCommandPool(VkDevice const deviceHandle, VkCommandPool const commandPool,
                        UNUSED VkCommandPoolTrimFlags const flags)
{
   struct terakan_command_pool * const command_pool = terakan_command_pool_from_handle(commandPool);

   vk_command_pool_trim(&command_pool->vk, flags);

   terakan_command_pool_trim_resources(command_pool);
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyCommandPool(VkDevice const deviceHandle, VkCommandPool const commandPool,
                           VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_command_pool * const command_pool = terakan_command_pool_from_handle(commandPool);

   if (command_pool == NULL) {
      return;
   }

   /* Finish the command pool base before destroying their dependencies, as finishing the base
    * destroys all allocated command buffers, and resetting command buffers sends their dependencies
    * back to the free lists.
    */
   vk_command_pool_finish(&command_pool->vk);

   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);

   terakan_command_pool_trim_resources(command_pool);

   vk_free2(&device->vk.alloc, pAllocator, command_pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateCommandPool(VkDevice const deviceHandle,
                          VkCommandPoolCreateInfo const * const pCreateInfo,
                          VkAllocationCallbacks const * const pAllocator,
                          VkCommandPool * const pCommandPool)
{
   VkResult result;

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_command_pool * const command_pool =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(struct terakan_command_pool),
                alignof(struct terakan_command_pool), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (command_pool == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   list_inithead(&command_pool->push_buffers_free);

   list_inithead(&command_pool->indirect_buffers_free);

   list_inithead(&command_pool->active_query_tables_free);

   list_inithead(&command_pool->command_writers_free);

   result = vk_command_pool_init(&device->vk, &command_pool->vk, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, command_pool);
      return result;
   }

   *pCommandPool = terakan_command_pool_to_handle(command_pool);
   return VK_SUCCESS;
}
