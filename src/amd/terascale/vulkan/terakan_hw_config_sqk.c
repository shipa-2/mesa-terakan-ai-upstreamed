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

#include "terakan_hw_config_sqk.h"

#include "terakan_command_buffer.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/bitscan.h"
#include "util/macros.h"

#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void
terakan_hw_config_sqk_stage_emission_flags_set_all(
   struct terakan_hw_config_sqk_stage_emission_flags * const emission_flags)
{
   emission_flags->kcache = BITFIELD_MASK(TERAKAN_KCACHE_HW_BUFFERS_PER_STAGE);

   emission_flags->samplers = BITFIELD_MASK(TERAKAN_SAMPLER_HW_COUNT_PER_STAGE);

   BITSET_ONES(emission_flags->resources);
}

/* Returns whether the binding was changed. */
static bool
terakan_hw_config_sqk_set_kcache(struct terakan_hw_config_sqk_stage * const stage,
                                 unsigned const index, uint32_t const size_lines,
                                 struct terakan_bo const * const bo, uint32_t const va_lines)
{
   assert(index < TERAKAN_KCACHE_HW_BUFFERS_PER_STAGE);
   struct terakan_hw_config_sqk_kcache_buffer * const stage_buffer = &stage->kcache[index];
   if (size_lines == 0 || bo == NULL) {
      if (!terakan_hw_config_sqk_kcache_buffer_is_bound(stage_buffer)) {
         return false;
      }
      stage_buffer->bo = NULL;
      stage_buffer->size_lines = 0;
   } else {
      assert(size_lines <= TERAKAN_KCACHE_HW_MAX_LINES_IN_BUFFER);
      if (stage_buffer->bo == bo && stage_buffer->va_lines == va_lines &&
          stage_buffer->size_lines == size_lines) {
         return false;
      }
      stage_buffer->bo = bo;
      stage_buffer->va_lines = va_lines;
      stage_buffer->size_lines = size_lines;
   }
   stage->modified.kcache |= BITFIELD_BIT(index);
   return true;
}

void
terakan_hw_config_sqk_set_kcache_vs(struct terakan_hw_config_sqk * const config,
                                    unsigned const index, uint32_t const size_lines,
                                    struct terakan_bo const * const bo, uint32_t const va_lines)
{
   if (terakan_hw_config_sqk_set_kcache(&config->stages_[MESA_SHADER_VERTEX], index, size_lines, bo,
                                        va_lines)) {
      config->modified_in_sw_vs_as_other_hw_stage_.kcache |= BITFIELD_BIT(index);
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_VERTEX);
   }
}

void
terakan_hw_config_sqk_set_kcache_tcs(struct terakan_hw_config_sqk * const config,
                                     unsigned const index, uint32_t const size_lines,
                                     struct terakan_bo const * const bo, uint32_t const va_lines)
{
   if (terakan_hw_config_sqk_set_kcache(&config->stages_[MESA_SHADER_TESS_CTRL], index, size_lines,
                                        bo, va_lines)) {
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_TESS_CTRL);
   }
}

void
terakan_hw_config_sqk_set_kcache_tes(struct terakan_hw_config_sqk * const config,
                                     unsigned const index, uint32_t const size_lines,
                                     struct terakan_bo const * const bo, uint32_t const va_lines)
{
   if (terakan_hw_config_sqk_set_kcache(&config->stages_[MESA_SHADER_TESS_EVAL], index, size_lines,
                                        bo, va_lines)) {
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_TESS_EVAL);
   }
}

void
terakan_hw_config_sqk_set_kcache_gs(struct terakan_hw_config_sqk * const config,
                                    unsigned const index, uint32_t const size_lines,
                                    struct terakan_bo const * const bo, uint32_t const va_lines)
{
   if (terakan_hw_config_sqk_set_kcache(&config->stages_[MESA_SHADER_GEOMETRY], index, size_lines,
                                        bo, va_lines)) {
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_GEOMETRY);
   }
}

void
terakan_hw_config_sqk_set_kcache_fs(struct terakan_hw_config_sqk * const config,
                                    unsigned const index, uint32_t const size_lines,
                                    struct terakan_bo const * const bo, uint32_t const va_lines)
{
   if (terakan_hw_config_sqk_set_kcache(&config->stages_[MESA_SHADER_FRAGMENT], index, size_lines,
                                        bo, va_lines)) {
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_FRAGMENT);
   }
}

void
terakan_hw_config_sqk_set_kcache_cs(struct terakan_hw_config_sqk * const config,
                                    unsigned const index, uint32_t const size_lines,
                                    struct terakan_bo const * const bo, uint32_t const va_lines)
{
   terakan_hw_config_sqk_set_kcache(&config->stages_[MESA_SHADER_COMPUTE], index, size_lines, bo,
                                    va_lines);
}

/* Returns whether the binding was changed. */
static bool
terakan_hw_config_sqk_set_sampler(struct terakan_hw_config_sqk_stage * const stage,
                                  unsigned const index,
                                  struct terakan_sampler_descriptor const * const descriptor)
{
   assert(index < TERAKAN_SAMPLER_HW_COUNT_PER_STAGE);
   /* For simplicity, not storing unbound or invalid samplers in `terakan_hw_config_sqk`. */
   if (unlikely(descriptor == NULL || G_03C008_TYPE(descriptor->sampler[2]))) {
      return false;
   }
   struct terakan_sampler_descriptor * const stage_sampler = &stage->samplers[index];
   if (terakan_hw_config_sqk_bound_sampler_equal(stage_sampler, descriptor)) {
      return false;
   }
   *stage_sampler = *descriptor;
   stage->modified.samplers |= BITFIELD_BIT(index);
   return true;
}

void
terakan_hw_config_sqk_set_sampler_vs(struct terakan_hw_config_sqk * const config,
                                     unsigned const index,
                                     struct terakan_sampler_descriptor const * const descriptor)
{
   if (terakan_hw_config_sqk_set_sampler(&config->stages_[MESA_SHADER_VERTEX], index, descriptor)) {
      config->modified_in_sw_vs_as_other_hw_stage_.samplers |= BITFIELD_BIT(index);
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_VERTEX);
   }
}

void
terakan_hw_config_sqk_set_sampler_tcs(struct terakan_hw_config_sqk * const config,
                                      unsigned const index,
                                      struct terakan_sampler_descriptor const * const descriptor)
{
   if (terakan_hw_config_sqk_set_sampler(&config->stages_[MESA_SHADER_TESS_CTRL], index,
                                         descriptor)) {
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_TESS_CTRL);
   }
}

void
terakan_hw_config_sqk_set_sampler_tes(struct terakan_hw_config_sqk * const config,
                                      unsigned const index,
                                      struct terakan_sampler_descriptor const * const descriptor)
{
   if (terakan_hw_config_sqk_set_sampler(&config->stages_[MESA_SHADER_TESS_EVAL], index,
                                         descriptor)) {
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_TESS_EVAL);
   }
}

void
terakan_hw_config_sqk_set_sampler_gs(struct terakan_hw_config_sqk * const config,
                                     unsigned const index,
                                     struct terakan_sampler_descriptor const * const descriptor)
{
   if (terakan_hw_config_sqk_set_sampler(&config->stages_[MESA_SHADER_GEOMETRY], index,
                                         descriptor)) {
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_GEOMETRY);
   }
}

void
terakan_hw_config_sqk_set_sampler_fs(struct terakan_hw_config_sqk * const config,
                                     unsigned const index,
                                     struct terakan_sampler_descriptor const * const descriptor)
{
   if (terakan_hw_config_sqk_set_sampler(&config->stages_[MESA_SHADER_FRAGMENT], index,
                                         descriptor)) {
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_FRAGMENT);
   }
}

void
terakan_hw_config_sqk_set_sampler_cs(struct terakan_hw_config_sqk * const config,
                                     unsigned const index,
                                     struct terakan_sampler_descriptor const * const descriptor)
{
   terakan_hw_config_sqk_set_sampler(&config->stages_[MESA_SHADER_COMPUTE], index, descriptor);
}

void
terakan_hw_config_sqk_set_resource_vi(struct terakan_hw_config_sqk * const config,
                                      unsigned const index, struct terakan_bo const * const bo,
                                      struct terakan_resource_descriptor const * const descriptor)
{
   assert(index < TERAKAN_RESOURCE_HW_COUNT_FETCH);
   uint32_t const resource_bit = BITFIELD_BIT(index);
   if (bo == NULL || descriptor == NULL) {
      if (!(config->vi_.resources_bound & resource_bit)) {
         return;
      }
      config->vi_.resources_bound &= ~resource_bit;
   } else {
      struct terakan_hw_config_sqk_resource * const stage_resource = &config->resources_vi_[index];
      if ((config->vi_.resources_bound & resource_bit) && stage_resource->bo == bo &&
          memcmp(&stage_resource->descriptor, descriptor, sizeof(*descriptor)) == 0) {
         return;
      }
      stage_resource->bo = bo;
      stage_resource->descriptor = *descriptor;
      config->vi_.resources_bound |= resource_bit;
   }
   config->vi_.resources_modified |= resource_bit;
}

/* Returns whether the binding was changed. */
static bool
terakan_hw_config_sqk_set_resource(struct terakan_hw_config_sqk_stage * const stage,
                                   unsigned const index, struct terakan_bo const * const bo,
                                   struct terakan_resource_descriptor const * const descriptor,
                                   struct terakan_hw_config_sqk_resource * const resources)
{
   if (bo == NULL || descriptor == NULL) {
      if (!BITSET_TEST(stage->resources_bound, index)) {
         return false;
      }
      BITSET_CLEAR(stage->resources_bound, index);
   } else {
      struct terakan_hw_config_sqk_resource * const stage_resource = &resources[index];
      if (BITSET_TEST(stage->resources_bound, index) && stage_resource->bo == bo &&
          memcmp(&stage_resource->descriptor, descriptor, sizeof(*descriptor)) == 0) {
         return false;
      }
      stage_resource->bo = bo;
      stage_resource->descriptor = *descriptor;
      BITSET_SET(stage->resources_bound, index);
   }
   BITSET_SET(stage->modified.resources, index);
   return true;
}

void
terakan_hw_config_sqk_set_resource_vs(struct terakan_hw_config_sqk * const config,
                                      unsigned const index, struct terakan_bo const * const bo,
                                      struct terakan_resource_descriptor const * const descriptor)
{
   assert(index < TERAKAN_RESOURCE_HW_COUNT_VERTEX);
   if (terakan_hw_config_sqk_set_resource(&config->stages_[MESA_SHADER_VERTEX], index, bo,
                                          descriptor, config->resources_vs_)) {
      BITSET_SET(config->modified_in_sw_vs_as_other_hw_stage_.resources, index);
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_VERTEX);
   }
}

void
terakan_hw_config_sqk_set_resource_tcs(struct terakan_hw_config_sqk * const config,
                                       unsigned const index, struct terakan_bo const * const bo,
                                       struct terakan_resource_descriptor const * const descriptor)
{
   assert(index < TERAKAN_RESOURCE_HW_COUNT_VERTEX);
   if (terakan_hw_config_sqk_set_resource(&config->stages_[MESA_SHADER_TESS_CTRL], index, bo,
                                          descriptor, config->resources_tcs_)) {
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_TESS_CTRL);
   }
}

void
terakan_hw_config_sqk_set_resource_tes(struct terakan_hw_config_sqk * const config,
                                       unsigned const index, struct terakan_bo const * const bo,
                                       struct terakan_resource_descriptor const * const descriptor)
{
   assert(index < TERAKAN_RESOURCE_HW_COUNT_VERTEX);
   if (terakan_hw_config_sqk_set_resource(&config->stages_[MESA_SHADER_TESS_EVAL], index, bo,
                                          descriptor, config->resources_tes_)) {
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_TESS_EVAL);
   }
}

void
terakan_hw_config_sqk_set_resource_gs(struct terakan_hw_config_sqk * const config,
                                      unsigned const index, struct terakan_bo const * const bo,
                                      struct terakan_resource_descriptor const * const descriptor)
{
   assert(index < TERAKAN_RESOURCE_HW_COUNT_VERTEX);
   if (terakan_hw_config_sqk_set_resource(&config->stages_[MESA_SHADER_GEOMETRY], index, bo,
                                          descriptor, config->resources_gs_)) {
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_GEOMETRY);
   }
}

void
terakan_hw_config_sqk_set_resource_fs(struct terakan_hw_config_sqk * const config,
                                      unsigned const index, struct terakan_bo const * const bo,
                                      struct terakan_resource_descriptor const * const descriptor)
{
   assert(index < TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
   if (terakan_hw_config_sqk_set_resource(&config->stages_[MESA_SHADER_FRAGMENT], index, bo,
                                          descriptor, config->resources_fs_)) {
      config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_FRAGMENT);
   }
}

void
terakan_hw_config_sqk_set_resource_cs(struct terakan_hw_config_sqk * const config,
                                      unsigned const index, struct terakan_bo const * const bo,
                                      struct terakan_resource_descriptor const * const descriptor)
{
   assert(index < TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
   terakan_hw_config_sqk_set_resource(&config->stages_[MESA_SHADER_COMPUTE], index, bo, descriptor,
                                      config->resources_cs_);
}

struct terakan_hw_config_sqk_set_functions const
   terakan_hw_config_sqk_stage_set_functions[MESA_SHADER_COMPUTE + 1] = {
      [MESA_SHADER_VERTEX] =
         {
            .kcache = terakan_hw_config_sqk_set_kcache_vs,
            .sampler = terakan_hw_config_sqk_set_sampler_vs,
            .resource = terakan_hw_config_sqk_set_resource_vs,
         },
      [MESA_SHADER_TESS_CTRL] =
         {
            .kcache = terakan_hw_config_sqk_set_kcache_tcs,
            .sampler = terakan_hw_config_sqk_set_sampler_tcs,
            .resource = terakan_hw_config_sqk_set_resource_tcs,
         },
      [MESA_SHADER_TESS_EVAL] =
         {
            .kcache = terakan_hw_config_sqk_set_kcache_tes,
            .sampler = terakan_hw_config_sqk_set_sampler_tes,
            .resource = terakan_hw_config_sqk_set_resource_tes,
         },
      [MESA_SHADER_GEOMETRY] =
         {
            .kcache = terakan_hw_config_sqk_set_kcache_gs,
            .sampler = terakan_hw_config_sqk_set_sampler_gs,
            .resource = terakan_hw_config_sqk_set_resource_gs,
         },
      [MESA_SHADER_FRAGMENT] =
         {
            .kcache = terakan_hw_config_sqk_set_kcache_fs,
            .sampler = terakan_hw_config_sqk_set_sampler_fs,
            .resource = terakan_hw_config_sqk_set_resource_fs,
         },
      [MESA_SHADER_COMPUTE] =
         {
            .kcache = terakan_hw_config_sqk_set_kcache_cs,
            .sampler = terakan_hw_config_sqk_set_sampler_cs,
            .resource = terakan_hw_config_sqk_set_resource_cs,
         },
};

static bool
terakan_hw_config_sqk_hw_ls_vses_used_for_vs_tes(struct terakan_hw_config_sqk const * const config)
{
   /* It's important, and expected by other functions, that this is determined by the presence of
    * TES, not TCS, because arbitration happens when setting VS and TES usage. On the other hand,
    * the software TCS constants are always emitted to hardware HS registers directly.
    */
   return config->sw_vs_always_uses_hw_ls_sqk_ ||
          config->stages_[MESA_SHADER_TESS_EVAL].usage != NULL;
}

void
terakan_hw_config_sqk_update_uncached_resources_(
   struct terakan_hw_config_sqk_stage * const stage,
   BITSET_WORD * const resources_last_used_as_uncached,
   struct terakan_hw_config_sqk_resource const * const resources)
{
   if (stage->usage == NULL) {
      /* Nothing to do for this stage at all. Likely called while unbinding the fragment shader. */
      return;
   }

   /* Get the resources, skipping the unused ones to avoid spuriously marking them as modified, for
    * which uncached enablement needs to be changed, and update the uncached enablement.
    */
   BITSET_DECLARE(uncached_toggled_for_bound, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
   for (unsigned word_index = 0; word_index < BITSET_WORDS(TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
        ++word_index) {
      BITSET_WORD const uncached_toggled_word = (resources_last_used_as_uncached[word_index] ^
                                                 stage->usage->resources_uncached[word_index]) &
                                                stage->usage->resources[word_index];
      /* Update the uncached enablement for the resource index, but only actually check and re-emit
       * the descriptor itself if it's bound.
       */
      uncached_toggled_for_bound[word_index] =
         uncached_toggled_word & stage->resources_bound[word_index];
      resources_last_used_as_uncached[word_index] ^= uncached_toggled_word;
   }

   /* Mark buffers for which uncached enablement needs to be toggled as modified to re-emit them
    * with the new uncached flag value.
    */
   unsigned resource_index;
   BITSET_FOREACH_SET (resource_index, uncached_toggled_for_bound,
                       TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE) {
      /* Unbound resources, for which the elements of the `resources` array are undefined, should
       * have already been excluded from the check earlier.
       */
      assert(BITSET_TEST(stage->resources_bound, resource_index));
      struct terakan_resource_descriptor const * const resource =
         &resources[resource_index].descriptor;
      /* Also don't mark as modified if uncached access is forced for it via the descriptor itself
       * anyway.
       */
      if (G_03001C_TYPE(resource->resource[7]) == V_03001C_SQ_TEX_VTX_VALID_BUFFER &&
          !G_03000C_UNCACHED(resource->resource[3])) {
         BITSET_SET(stage->modified.resources, resource_index);
      }
   }
}

void
terakan_hw_config_sqk_set_usage_vs_tes(struct terakan_hw_config_sqk * const config,
                                       struct terakan_shader_sqk_usage const * const usage_vs,
                                       struct terakan_shader_sqk_usage const * const usage_tes)
{
   bool const hw_ls_vses_was_used_for_vs_tes =
      terakan_hw_config_sqk_hw_ls_vses_used_for_vs_tes(config);

   bool const usage_changed_vs =
      terakan_hw_config_sqk_set_draw_usage_(config, MESA_SHADER_VERTEX, usage_vs);
   bool const usage_changed_tes =
      terakan_hw_config_sqk_set_draw_usage_(config, MESA_SHADER_TESS_EVAL, usage_tes);
   if (!usage_changed_vs && !usage_changed_tes) {
      return;
   }

   struct terakan_hw_config_sqk_stage * const stage_vs = &config->stages_[MESA_SHADER_VERTEX];
   struct terakan_hw_config_sqk_stage * const stage_tes = &config->stages_[MESA_SHADER_TESS_EVAL];

   bool const hw_ls_vses_is_used_for_vs_tes =
      terakan_hw_config_sqk_hw_ls_vses_used_for_vs_tes(config);
   if (hw_ls_vses_is_used_for_vs_tes != hw_ls_vses_was_used_for_vs_tes) {
      /* Using the hardware VS/ES registers for both VS and TES constants from now on in this
       * command buffer.
       */
      config->hw_vses_arbitration_needed_ = true;

      /* Change the current hardware register space for the software VS between LS and VS/ES. */
      struct terakan_hw_config_sqk_stage_emission_flags const vs_modified_in_old_stage =
         stage_vs->modified;
      stage_vs->modified = config->modified_in_sw_vs_as_other_hw_stage_;
      config->modified_in_sw_vs_as_other_hw_stage_ = vs_modified_in_old_stage;
   }

   if (config->hw_vses_arbitration_needed_) {
      /* Update the modification flags for the software stage that now uses the VS/ES registers.
       *
       * If it's known that the constants are the same for the old and the new software stages, and
       * they've already been emitted for the old stage, mark the registers whose ownership is being
       * transferred as unmodified. In other cases, specify that they need to be emitted.
       *
       * Only transferring ownership of used registers to avoid spuriously marking unused registers
       * as modified.
       */
      struct terakan_hw_config_sqk_stage * const new_vses_stage =
         hw_ls_vses_is_used_for_vs_tes ? stage_tes : stage_vs;
      if (new_vses_stage->usage != NULL) {
         struct terakan_hw_config_sqk_stage const * old_vses_stage;
         struct terakan_hw_config_sqk_resource const * new_vses_resources;
         struct terakan_hw_config_sqk_resource const * old_vses_resources;
         struct terakan_hw_config_sqk_stage_emission_flags vses_usage_toggled;
         if (hw_ls_vses_is_used_for_vs_tes) {
            config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_TESS_EVAL);

            old_vses_stage = stage_vs;
            new_vses_resources = config->resources_tes_;
            old_vses_resources = config->resources_vs_;

            /* Need to update the registers for which the flag was 0. */
            vses_usage_toggled.kcache = ~config->vses_last_used_by_tes_.kcache;
            vses_usage_toggled.samplers = ~config->vses_last_used_by_tes_.samplers;
            /* Using `ARRAY_SIZE` instead `BITSET_WORDS(TERAKAN_RESOURCE_HW_COUNT_VERTEX)` to
             * initialize the [160, 176) word to avoid undefined behavior.
             */
            for (unsigned word_index = 0; word_index < ARRAY_SIZE(vses_usage_toggled.resources);
                 ++word_index) {
               vses_usage_toggled.resources[word_index] =
                  ~config->vses_last_used_by_tes_.resources[word_index];
            }
         } else {
            config->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_VERTEX);

            old_vses_stage = stage_tes;
            new_vses_resources = config->resources_vs_;
            old_vses_resources = config->resources_tes_;

            /* Need to update the registers for which the flag was 1. */
            vses_usage_toggled = config->vses_last_used_by_tes_;
         }

         vses_usage_toggled.kcache &= new_vses_stage->usage->kcache;
         vses_usage_toggled.samplers &= new_vses_stage->usage->samplers;
         BITSET_AND(vses_usage_toggled.resources, vses_usage_toggled.resources,
                    new_vses_stage->usage->resources);

         config->vses_last_used_by_tes_.kcache ^= vses_usage_toggled.kcache;
         config->vses_last_used_by_tes_.samplers ^= vses_usage_toggled.samplers;
         BITSET_XOR(config->vses_last_used_by_tes_.resources,
                    config->vses_last_used_by_tes_.resources, vses_usage_toggled.resources);

         /* Set that registers marked as modified in the old hardware stage are also modified in the
          * new one, as the up to date constants for them haven't been emitted yet, and the contents
          * of those registers in the current hardware state are unknown.
          *
          * For the constants that are known to have already been emitted to the VS/ES registers for
          * the old software stage, set the modification flag depending on whether a different
          * constant must be bound for the new software stage.
          *
          * For simplicity, update the modification flags for both purposes at once by copying them
          * from the old stage, and then perform the comparison only for hardware registers known to
          * have been emitted for the old stage.
          */

         new_vses_stage->modified.kcache =
            (new_vses_stage->modified.kcache & ~vses_usage_toggled.kcache) |
            (old_vses_stage->modified.kcache & vses_usage_toggled.kcache);
         vses_usage_toggled.kcache &= ~old_vses_stage->modified.kcache;

         new_vses_stage->modified.samplers =
            (new_vses_stage->modified.samplers & ~vses_usage_toggled.samplers) |
            (old_vses_stage->modified.samplers & vses_usage_toggled.samplers);
         vses_usage_toggled.samplers &= ~old_vses_stage->modified.samplers;

         for (unsigned word_index = 0; word_index < BITSET_WORDS(TERAKAN_RESOURCE_HW_COUNT_VERTEX);
              ++word_index) {
            new_vses_stage->modified.resources[word_index] =
               (new_vses_stage->modified.resources[word_index] &
                ~vses_usage_toggled.resources[word_index]) |
               (old_vses_stage->modified.resources[word_index] &
                vses_usage_toggled.resources[word_index]);
            vses_usage_toggled.resources[word_index] &=
               ~old_vses_stage->modified.resources[word_index];
         }

         u_foreach_bit (kcache_buffer_index, vses_usage_toggled.kcache) {
            if (terakan_hw_config_sqk_kcache_buffer_equal(
                   &new_vses_stage->kcache[kcache_buffer_index],
                   &old_vses_stage->kcache[kcache_buffer_index])) {
               continue;
            }
            new_vses_stage->modified.kcache |= BITFIELD_BIT(kcache_buffer_index);
         }

         u_foreach_bit (sampler_index, vses_usage_toggled.samplers) {
            /* Not storing unbound samplers internally in `terakan_hw_config_sqk`, so comparing
             * regardless of the `TYPE`.
             */
            if (terakan_hw_config_sqk_bound_sampler_equal(
                   &new_vses_stage->samplers[sampler_index],
                   &old_vses_stage->samplers[sampler_index])) {
               continue;
            }
            new_vses_stage->modified.samplers |= BITFIELD_BIT(sampler_index);
         }

         unsigned resource_index;
         BITSET_FOREACH_SET (resource_index, vses_usage_toggled.resources,
                             TERAKAN_RESOURCE_HW_COUNT_VERTEX) {
            struct terakan_hw_config_sqk_resource const * const new_resource =
               &new_vses_resources[resource_index];
            struct terakan_hw_config_sqk_resource const * const old_resource =
               &old_vses_resources[resource_index];
            if (BITSET_TEST(new_vses_stage->resources_bound, resource_index)) {
               if (BITSET_TEST(old_vses_stage->resources_bound, resource_index) &&
                   new_resource->bo == old_resource->bo &&
                   memcmp(&new_resource->descriptor, &old_resource->descriptor,
                          sizeof(new_resource->descriptor)) == 0) {
                  continue;
               }
            } else {
               if (!BITSET_TEST(old_vses_stage->resources_bound, resource_index)) {
                  continue;
               }
            }
            BITSET_SET(new_vses_stage->modified.resources, resource_index);
         }
      }
   }
}

void
terakan_hw_config_sqk_begin_emitting_first_draw_dispatch_in_indirect_buffer(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_sqk * const config = &command_writer->hw_config_sqk;

   config->draw_stages_pending_ = BITFIELD_MASK(MESA_SHADER_COMPUTE);

   /* Clearing all resources in a new indirect buffer to avoid emitting every unbound resource
    * individually later.
    * According to Radeon Evergreen / Northern Islands Acceleration, `SET_CTL_CONST` packets always
    * have the shader type set to graphics.
    */
   /* TODO(Triang3l): Does `SQ_TEX_RESOURCE_CLEAR` for ~0 actually clear compute shader resources
    * when the shader type in the header is set to graphics? However, `CLEAR_STATE` for compute will
    * be done when acquiring a compute context anyway.
    */

   config->vi_.resources_modified = config->vi_.resources_bound;

   for (size_t stage_index = 0; stage_index < ARRAY_SIZE(config->stages_); ++stage_index) {
      struct terakan_hw_config_sqk_stage * const stage = &config->stages_[stage_index];
      stage->modified.kcache = BITFIELD_MASK(TERAKAN_KCACHE_HW_BUFFERS_PER_STAGE);
      stage->modified.samplers = BITFIELD_MASK(TERAKAN_SAMPLER_HW_COUNT_PER_STAGE);
      BITSET_COPY(stage->modified.resources, stage->resources_bound);
   }

   config->modified_in_sw_vs_as_other_hw_stage_ = config->stages_[MESA_SHADER_VERTEX].modified;

   {
      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1);
      if (likely(packet != NULL)) {
         *packet++ = PKT3(PKT3_SET_CTL_CONST, 1, 0);
         *packet++ = TERAKAN_CTL_CONST_OFFSET(R_03FF04_SQ_TEX_RESOURCE_CLEAR);
         *packet++ = UINT32_MAX;
         terakan_gfx_command_writer_emit_done(command_writer, packet);
      }
   }
}

static void
terakan_hw_config_sqk_emit_modified_kcache(struct terakan_gfx_command_writer * const command_writer,
                                           struct terakan_hw_config_sqk_stage * const stage,
                                           uint32_t const size_register_offset,
                                           uint32_t const base_register_offset,
                                           uint64_t const base_wddm_patch_ids,
                                           uint32_t const shader_type_flag)
{
   /* According to the Evergreen and Cayman 3D Register Reference Guides, unless the size is 0, both
    * `CONST_BUFFER_SIZE` and `CONST_CACHE` must be written for a buffer (probably if either is
    * written for the buffer in the new hardware state context).
    */

   assert(stage->usage != NULL);
   unsigned buffers_remaining = stage->usage->kcache & stage->modified.kcache;

   while (buffers_remaining) {
      int buffer_range_start, buffer_range_length;
      u_bit_scan_consecutive_range(&buffers_remaining, &buffer_range_start, &buffer_range_length);

      /* Gather which buffers are bound, and thus need not only the size, but also the base to be
       * emitted.
       */
      unsigned bound_buffers_remaining = 0b0;
      for (int range_buffer_index = 0; range_buffer_index < buffer_range_length;
           ++range_buffer_index) {
         int const buffer_index = buffer_range_start + range_buffer_index;
         struct terakan_hw_config_sqk_kcache_buffer const * const buffer =
            &stage->kcache[buffer_index];
         if (terakan_hw_config_sqk_kcache_buffer_is_bound(buffer)) {
            bound_buffers_remaining |= BITFIELD_BIT(buffer_index);
         }
      }

      /* Emit sizes and bases for the current range (in a single emission, for consistency between
       * each other, to make sure they both are set in the indirect buffer even if it's ending).
       */

      unsigned const bound_buffer_count = util_bitcount(bound_buffers_remaining);
      /* Make sure a new base setting packet is emitted for every range of consecutive bound
       * buffers - count the terminations of bound buffer ranges (first promoting to a type wider
       * than 16 bits to include the termination of the range containing the buffer 15).
       */
      unsigned const bound_buffer_range_count = util_bitcount(
         ~(uint32_t)bound_buffers_remaining & ((uint32_t)bound_buffers_remaining << 1));
      uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
         2 + buffer_range_length + (2 * bound_buffer_range_count) + bound_buffer_count,
         bound_buffer_count, bound_buffer_count, 0);
      if (unlikely(packet == NULL)) {
         return;
      }

      /* Sizes. */
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, buffer_range_length, 0) | shader_type_flag;
      *packet++ = size_register_offset + buffer_range_start;
      for (unsigned range_buffer_index = 0; range_buffer_index < (unsigned)buffer_range_length;
           ++range_buffer_index) {
         unsigned const buffer_index = (unsigned)buffer_range_start + range_buffer_index;
         *packet++ = (bound_buffers_remaining & BITFIELD_BIT(buffer_index))
                        ? stage->kcache[buffer_index].size_lines
                        : 0;
      }

      /* Bases. */
      while (bound_buffers_remaining) {
         int bound_buffer_range_start, bound_buffer_range_length;
         u_bit_scan_consecutive_range(&bound_buffers_remaining, &bound_buffer_range_start,
                                      &bound_buffer_range_length);
         *packet++ = PKT3(PKT3_SET_CONTEXT_REG, bound_buffer_range_length, 0) | shader_type_flag;
         *packet++ = base_register_offset + bound_buffer_range_start;
         uint32_t * const packet_bases = packet;
         for (unsigned range_bound_buffer_index = 0;
              range_bound_buffer_index < (unsigned)bound_buffer_range_length;
              ++range_bound_buffer_index) {
            *packet++ = stage->kcache[bound_buffer_range_start + range_bound_buffer_index].va_lines;
         }
         for (unsigned range_bound_buffer_index = 0;
              range_bound_buffer_index < (unsigned)bound_buffer_range_length;
              ++range_bound_buffer_index) {
            unsigned const buffer_index =
               (unsigned)bound_buffer_range_start + range_bound_buffer_index;
            terakan_gfx_command_writer_add_relocation(
               command_writer, &packet, &packet_bases[range_bound_buffer_index],
               packet_bases[range_bound_buffer_index], base_wddm_patch_ids | buffer_index,
               terakan_bo_reference_writer_add_reference(
                  &command_writer->base.bo_reference_writer, stage->kcache[buffer_index].bo, true,
                  false, TERAKAN_BO_PRIORITY_UNIFORM_BUFFER));
         }
      }

      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }

   stage->modified.kcache &= ~stage->usage->kcache;
}

static void
terakan_hw_config_sqk_emit_modified_samplers(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_hw_config_sqk_stage * const stage,
   enum terakan_descriptor_hw_stage const hw_stage_index)
{
   assert(stage->usage != NULL);
   unsigned samplers_remaining = stage->usage->samplers & stage->modified.samplers;
   uint32_t border_colors_to_emit = 0b0;

   while (samplers_remaining) {
      int range_start, range_length;
      u_bit_scan_consecutive_range(&samplers_remaining, &range_start, &range_length);
      unsigned const range_sampler_dword_count = 3 * (unsigned)range_length;
      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
         2 + range_sampler_dword_count);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_SET_SAMPLER, range_sampler_dword_count, 0) |
                  (hw_stage_index == TERAKAN_DESCRIPTOR_HW_STAGE_CS ? TERAKAN_PACKET3_COMPUTE : 0);
      *packet++ = 3 * (TERAKAN_SAMPLER_HW_COUNT_PER_STAGE * (unsigned)hw_stage_index +
                       (unsigned)range_start);
      for (unsigned range_sampler_index = 0; range_sampler_index < (unsigned)range_length;
           ++range_sampler_index) {
         unsigned const sampler_index = (unsigned)range_start + range_sampler_index;
         struct terakan_sampler_descriptor const * const sampler = &stage->samplers[sampler_index];
         memcpy(packet, sampler->sampler, sizeof(uint32_t) * 3);
         packet += 3;
         if (G_03C000_BORDER_COLOR_TYPE(sampler->sampler[0]) ==
             V_03C000_SQ_TEX_BORDER_COLOR_REGISTER) {
            border_colors_to_emit |= BITFIELD_BIT(sampler_index);
         }
      }
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }

   u_foreach_bit (sampler_index, border_colors_to_emit) {
      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1 + 4);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_SET_CTL_CONST, 1 + 4, 0);
      *packet++ = TERAKAN_CTL_CONST_OFFSET(R_00A400_TD_PS_SAMPLER0_BORDER_INDEX) +
                  5 * (unsigned)hw_stage_index;
      *packet++ = sampler_index;
      memcpy(packet, stage->samplers[sampler_index].register_border_color, sizeof(uint32_t) * 4);
      packet += 4;
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }

   stage->modified.samplers &= ~stage->usage->samplers;
}

static void
terakan_hw_config_sqk_emit_unbound_resource(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const resource_hw_index)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CTL_CONST, 1, 0);
   *packet++ = TERAKAN_CTL_CONST_OFFSET(R_03FF04_SQ_TEX_RESOURCE_CLEAR);
   *packet++ = resource_hw_index;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

/* Returns the pointer to the descriptor in the indirect buffer, or NULL if couldn't emit. */
static uint32_t *
terakan_hw_config_sqk_emit_bound_resource(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const resource_hw_index,
   struct terakan_hw_config_sqk_resource const * const resource, uint32_t const shader_type_flag)
{
   uint32_t const * const descriptor = resource->descriptor.resource;

   bool const is_texture = G_03001C_TYPE(descriptor[7]) == V_03001C_SQ_TEX_VTX_VALID_TEXTURE;
   bool const is_multisampled =
      is_texture && (G_030000_DIM(descriptor[0]) == V_030000_SQ_TEX_DIM_2D_MSAA ||
                     G_030000_DIM(descriptor[0]) == V_030000_SQ_TEX_DIM_2D_ARRAY_MSAA);
   /* Based on how DRM Radeon checks whether the mip BO is provided.
    * Images in Terakan always have base level subresources, so the relative mip address is never 0.
    */
   bool const relocate_mips_or_fmask =
      is_texture && (!is_multisampled || G_03000C_MIP_ADDRESS(descriptor[3]) != 0);

   unsigned const descriptor_dword_count =
      sizeof(struct terakan_resource_descriptor) / sizeof(uint32_t);

   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + descriptor_dword_count,
      1, is_texture ? 1 + (uint32_t)relocate_mips_or_fmask : 0, is_texture ? 0 : 1);
   if (unlikely(packet == NULL)) {
      return NULL;
   }

   *packet++ = PKT3(PKT3_SET_RESOURCE, descriptor_dword_count, 0) | shader_type_flag;
   *packet++ = descriptor_dword_count * resource_hw_index;
   uint32_t * const packet_descriptor = packet;
   memcpy(packet_descriptor, descriptor, sizeof(uint32_t) * descriptor_dword_count);
   if (!is_texture) {
      packet_descriptor[TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD] = 0;
   }
   packet += descriptor_dword_count;

   uint32_t const bo_reference = terakan_bo_reference_writer_add_reference(
      &command_writer->base.bo_reference_writer, resource->bo, true, false,
      is_texture ? (is_multisampled ? TERAKAN_BO_PRIORITY_SHADER_READ_IMAGE_MS
                                    : TERAKAN_BO_PRIORITY_SHADER_READ_IMAGE)
                 : descriptor[TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD]);
   if (is_texture) {
      terakan_gfx_command_writer_add_relocation(
         command_writer, &packet, &packet_descriptor[2], packet_descriptor[2],
         TERASCALE_WDDM_PATCH_IDS_SQ_TEX_RESOURCE_BASE, bo_reference);
      if (relocate_mips_or_fmask) {
         terakan_gfx_command_writer_add_relocation(
            command_writer, &packet, &packet_descriptor[3], packet_descriptor[3],
            TERASCALE_WDDM_PATCH_IDS_SQ_TEX_RESOURCE_MIP, bo_reference);
      }
   } else {
      terakan_gfx_command_writer_add_relocation_for_40_bits(
         command_writer, &packet, &packet_descriptor[0], &packet_descriptor[2],
         TERASCALE_WDDM_PATCH_IDS_SQ_VTX_CONSTANT_BASE_LO,
         TERASCALE_WDDM_PATCH_IDS_SQ_VTX_CONSTANT_BASE_HI, bo_reference);
   }

   terakan_gfx_command_writer_emit_done(command_writer, packet);

   return packet_descriptor;
}

static void
terakan_hw_config_sqk_emit_modified_resources_vertex_input(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_sqk * const config = &command_writer->hw_config_sqk;
   u_foreach_bit (resource_index, config->vi_.resources_used & config->vi_.resources_modified) {
      unsigned const resource_hw_index = TERAKAN_RESOURCE_HW_OFFSET_FS + resource_index;
      if (!(config->vi_.resources_bound & BITFIELD_BIT(resource_index))) {
         terakan_hw_config_sqk_emit_unbound_resource(command_writer, resource_hw_index);
         continue;
      }
      terakan_hw_config_sqk_emit_bound_resource(command_writer, resource_hw_index,
                                                &config->resources_vi_[resource_index], 0);
   }
   config->vi_.resources_modified &= ~config->vi_.resources_used;
}

static void
terakan_hw_config_sqk_emit_modified_resources_vertex(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_hw_config_sqk_stage * const stage,
   struct terakan_hw_config_sqk_resource const * const resources, unsigned const resource_hw_offset)
{
   BITSET_DECLARE(resources_to_emit, TERAKAN_RESOURCE_HW_COUNT_VERTEX);
   assert(stage->usage != NULL);
   __bitset_and(resources_to_emit, stage->usage->resources, stage->modified.resources,
                ARRAY_SIZE(resources_to_emit));

   unsigned resource_index;
   BITSET_FOREACH_SET (resource_index, resources_to_emit, TERAKAN_RESOURCE_HW_COUNT_VERTEX) {
      unsigned const resource_hw_index = resource_hw_offset + resource_index;
      if (!BITSET_TEST(stage->resources_bound, resource_index)) {
         terakan_hw_config_sqk_emit_unbound_resource(command_writer, resource_hw_index);
         continue;
      }
      terakan_hw_config_sqk_emit_bound_resource(command_writer, resource_hw_index,
                                                &resources[resource_index], 0);
   }

   __bitset_andnot(stage->modified.resources, stage->modified.resources, stage->usage->resources,
                   TERAKAN_RESOURCE_HW_COUNT_VERTEX);
}

static void
terakan_hw_config_sqk_emit_modified_resources_fragment(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_sqk * const config = &command_writer->hw_config_sqk;
   struct terakan_hw_config_sqk_stage * const stage = &config->stages_[MESA_SHADER_FRAGMENT];

   BITSET_DECLARE(resources_to_emit, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
   assert(stage->usage != NULL);
   BITSET_AND(resources_to_emit, stage->usage->resources, stage->modified.resources);

   unsigned resource_index;
   BITSET_FOREACH_SET (resource_index, resources_to_emit, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE) {
      unsigned const resource_hw_index = TERAKAN_RESOURCE_HW_OFFSET_PS + resource_index;
      if (!BITSET_TEST(stage->resources_bound, resource_index)) {
         terakan_hw_config_sqk_emit_unbound_resource(command_writer, resource_hw_index);
         continue;
      }
      uint32_t * const packet_descriptor = terakan_hw_config_sqk_emit_bound_resource(
         command_writer, resource_hw_index, &config->resources_fs_[resource_index], 0);
      if (BITSET_TEST(config->resources_last_used_as_uncached_fs_, resource_index) &&
          likely(packet_descriptor != NULL) &&
          G_03001C_TYPE(packet_descriptor[7]) == V_03001C_SQ_TEX_VTX_VALID_BUFFER) {
         packet_descriptor[3] |= S_03000C_UNCACHED(true);
      }
   }

   BITSET_ANDNOT(stage->modified.resources, stage->modified.resources, stage->usage->resources);
}

static void
terakan_hw_config_sqk_emit_modified_resources_compute(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_sqk * const config = &command_writer->hw_config_sqk;
   struct terakan_hw_config_sqk_stage * const stage = &config->stages_[MESA_SHADER_COMPUTE];

   BITSET_DECLARE(resources_to_emit, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
   assert(stage->usage != NULL);
   BITSET_AND(resources_to_emit, stage->usage->resources, stage->modified.resources);

   unsigned resource_index;
   BITSET_FOREACH_SET (resource_index, resources_to_emit, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE) {
      unsigned const resource_hw_index = TERAKAN_RESOURCE_HW_OFFSET_CS + resource_index;
      if (!BITSET_TEST(stage->resources_bound, resource_index)) {
         terakan_hw_config_sqk_emit_unbound_resource(command_writer, resource_hw_index);
         continue;
      }
      uint32_t * const packet_descriptor = terakan_hw_config_sqk_emit_bound_resource(
         command_writer, resource_hw_index, &config->resources_cs_[resource_index],
         TERAKAN_PACKET3_COMPUTE);
      if (BITSET_TEST(config->resources_last_used_as_uncached_cs_, resource_index) &&
          likely(packet_descriptor != NULL) &&
          G_03001C_TYPE(packet_descriptor[7]) == V_03001C_SQ_TEX_VTX_VALID_BUFFER) {
         packet_descriptor[3] |= S_03000C_UNCACHED(true);
      }
   }

   BITSET_ANDNOT(stage->modified.resources, stage->modified.resources, stage->usage->resources);
}

typedef void (*terakan_hw_config_sqk_emit_modified_function)(
   struct terakan_gfx_command_writer * command_writer);

static void
terakan_hw_config_sqk_emit_modified_vs(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_sqk * const config = &command_writer->hw_config_sqk;
   struct terakan_hw_config_sqk_stage * const stage = &config->stages_[MESA_SHADER_VERTEX];
   if (terakan_hw_config_sqk_hw_ls_vses_used_for_vs_tes(config)) {
      terakan_hw_config_sqk_emit_modified_kcache(
         command_writer, stage, TERAKAN_CONTEXT_REG_OFFSET(R_028FC0_ALU_CONST_BUFFER_SIZE_LS_0),
         TERAKAN_CONTEXT_REG_OFFSET(R_028F40_ALU_CONST_CACHE_LS_0),
         TERASCALE_WDDM_PATCH_IDS_SQ_ALU_CONST_CACHE_LS_VS, 0);
      terakan_hw_config_sqk_emit_modified_samplers(command_writer, stage,
                                                   TERAKAN_DESCRIPTOR_HW_STAGE_LS);
      terakan_hw_config_sqk_emit_modified_resources_vertex(
         command_writer, stage, config->resources_vs_, TERAKAN_RESOURCE_HW_OFFSET_LS);
   } else {
      terakan_hw_config_sqk_emit_modified_kcache(
         command_writer, stage, TERAKAN_CONTEXT_REG_OFFSET(R_028180_ALU_CONST_BUFFER_SIZE_VS_0),
         TERAKAN_CONTEXT_REG_OFFSET(R_028980_ALU_CONST_CACHE_VS_0),
         TERASCALE_WDDM_PATCH_IDS_SQ_ALU_CONST_CACHE_LS_VS, 0);
      terakan_hw_config_sqk_emit_modified_samplers(command_writer, stage,
                                                   TERAKAN_DESCRIPTOR_HW_STAGE_VSES);
      terakan_hw_config_sqk_emit_modified_resources_vertex(
         command_writer, stage, config->resources_vs_, TERAKAN_RESOURCE_HW_OFFSET_VSES);
   }
}

static void
terakan_hw_config_sqk_emit_modified_tcs(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_sqk_stage * const stage =
      &command_writer->hw_config_sqk.stages_[MESA_SHADER_TESS_CTRL];
   terakan_hw_config_sqk_emit_modified_kcache(
      command_writer, stage, TERAKAN_CONTEXT_REG_OFFSET(R_028F80_ALU_CONST_BUFFER_SIZE_HS_0),
      TERAKAN_CONTEXT_REG_OFFSET(R_028F00_ALU_CONST_CACHE_HS_0),
      TERASCALE_WDDM_PATCH_IDS_SQ_ALU_CONST_CACHE_HS, 0);
   terakan_hw_config_sqk_emit_modified_samplers(command_writer, stage,
                                                TERAKAN_DESCRIPTOR_HW_STAGE_HS);
   terakan_hw_config_sqk_emit_modified_resources_vertex(
      command_writer, stage, command_writer->hw_config_sqk.resources_tcs_,
      TERAKAN_RESOURCE_HW_OFFSET_HS);
}

static void
terakan_hw_config_sqk_emit_modified_tes(struct terakan_gfx_command_writer * const command_writer)
{
   assert(terakan_hw_config_sqk_hw_ls_vses_used_for_vs_tes(&command_writer->hw_config_sqk));
   struct terakan_hw_config_sqk_stage * const stage =
      &command_writer->hw_config_sqk.stages_[MESA_SHADER_TESS_EVAL];
   terakan_hw_config_sqk_emit_modified_kcache(
      command_writer, stage, TERAKAN_CONTEXT_REG_OFFSET(R_028180_ALU_CONST_BUFFER_SIZE_VS_0),
      TERAKAN_CONTEXT_REG_OFFSET(R_028980_ALU_CONST_CACHE_VS_0),
      TERASCALE_WDDM_PATCH_IDS_SQ_ALU_CONST_CACHE_LS_VS, 0);
   terakan_hw_config_sqk_emit_modified_samplers(command_writer, stage,
                                                TERAKAN_DESCRIPTOR_HW_STAGE_VSES);
   terakan_hw_config_sqk_emit_modified_resources_vertex(
      command_writer, stage, command_writer->hw_config_sqk.resources_tes_,
      TERAKAN_RESOURCE_HW_OFFSET_VSES);
}

static void
terakan_hw_config_sqk_emit_modified_gs(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_sqk_stage * const stage =
      &command_writer->hw_config_sqk.stages_[MESA_SHADER_GEOMETRY];
   terakan_hw_config_sqk_emit_modified_kcache(
      command_writer, stage, TERAKAN_CONTEXT_REG_OFFSET(R_0281C0_ALU_CONST_BUFFER_SIZE_GS_0),
      TERAKAN_CONTEXT_REG_OFFSET(R_0289C0_ALU_CONST_CACHE_GS_0),
      TERASCALE_WDDM_PATCH_IDS_SQ_ALU_CONST_CACHE_GS, 0);
   terakan_hw_config_sqk_emit_modified_samplers(command_writer, stage,
                                                TERAKAN_DESCRIPTOR_HW_STAGE_GS);
   terakan_hw_config_sqk_emit_modified_resources_vertex(command_writer, stage,
                                                        command_writer->hw_config_sqk.resources_gs_,
                                                        TERAKAN_RESOURCE_HW_OFFSET_GS);
}

static void
terakan_hw_config_sqk_emit_modified_fs(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_sqk_stage * const stage =
      &command_writer->hw_config_sqk.stages_[MESA_SHADER_FRAGMENT];
   terakan_hw_config_sqk_emit_modified_kcache(
      command_writer, stage, TERAKAN_CONTEXT_REG_OFFSET(R_028140_ALU_CONST_BUFFER_SIZE_PS_0),
      TERAKAN_CONTEXT_REG_OFFSET(R_028940_ALU_CONST_CACHE_PS_0),
      TERASCALE_WDDM_PATCH_IDS_SQ_ALU_CONST_CACHE_PS, 0);
   terakan_hw_config_sqk_emit_modified_samplers(command_writer, stage,
                                                TERAKAN_DESCRIPTOR_HW_STAGE_PS);
   terakan_hw_config_sqk_emit_modified_resources_fragment(command_writer);
}

static terakan_hw_config_sqk_emit_modified_function const
   terakan_hw_config_sqk_emit_draw_stage_modified[MESA_SHADER_COMPUTE] = {
      [MESA_SHADER_VERTEX] = terakan_hw_config_sqk_emit_modified_vs,
      [MESA_SHADER_TESS_CTRL] = terakan_hw_config_sqk_emit_modified_tcs,
      [MESA_SHADER_TESS_EVAL] = terakan_hw_config_sqk_emit_modified_tes,
      [MESA_SHADER_GEOMETRY] = terakan_hw_config_sqk_emit_modified_gs,
      [MESA_SHADER_FRAGMENT] = terakan_hw_config_sqk_emit_modified_fs,
};

void
terakan_hw_config_sqk_emit_modified_for_draw(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_sqk * const config = &command_writer->hw_config_sqk;
   terakan_hw_config_sqk_emit_modified_resources_vertex_input(command_writer);
   u_foreach_bit (stage_index, config->draw_stages_used_ & config->draw_stages_pending_) {
      terakan_hw_config_sqk_emit_draw_stage_modified[stage_index](command_writer);
   }
   /* Reset all pending state regardless of which stages are used, because changing the usage marks
    * the stage as pending anyway.
    */
   config->draw_stages_pending_ = 0b0;
}

void
terakan_hw_config_sqk_emit_modified_for_compute(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_sqk_stage * const stage =
      &command_writer->hw_config_sqk.stages_[MESA_SHADER_COMPUTE];
   if (unlikely(stage->usage == NULL)) {
      /* Though dispatches shouldn't be done without a compute shader, allow this case for interface
       * simplicity and invalid usage stability.
       */
      return;
   }
   /* Unlike for the graphics stages, not tracking whether any binding was changed, because it's an
    * extremely unlikely situation that no bindings, even push constants, have been changed between
    * dispatches.
    */
   terakan_hw_config_sqk_emit_modified_kcache(
      command_writer, stage, TERAKAN_CONTEXT_REG_OFFSET(R_028FC0_ALU_CONST_BUFFER_SIZE_LS_0),
      TERAKAN_CONTEXT_REG_OFFSET(R_028F40_ALU_CONST_CACHE_LS_0),
      TERASCALE_WDDM_PATCH_IDS_SQ_ALU_CONST_CACHE_CS, TERAKAN_PACKET3_COMPUTE);
   terakan_hw_config_sqk_emit_modified_samplers(command_writer, stage,
                                                TERAKAN_DESCRIPTOR_HW_STAGE_CS);
   terakan_hw_config_sqk_emit_modified_resources_compute(command_writer);
}

void
terakan_hw_config_sqk_reset(struct terakan_hw_config_sqk * const config,
                            bool const sw_vs_always_uses_hw_ls_sqk)
{
   config->sw_vs_always_uses_hw_ls_sqk_ = sw_vs_always_uses_hw_ls_sqk;

   config->hw_vses_arbitration_needed_ = false;

   static_assert(
      CHAR_BIT * MIN2(sizeof(config->draw_stages_used_), sizeof(config->draw_stages_pending_)) >=
         MESA_SHADER_COMPUTE,
      "Stage flags must be wide enough to store flags for all supported graphics pipeline stages.");
   config->draw_stages_used_ = 0b0;
   config->draw_stages_pending_ = BITFIELD_MASK(MESA_SHADER_COMPUTE);

   config->vi_.resources_used = 0b0;
   /* The current hardware register contents are unknown. */
   config->vi_.resources_modified = BITFIELD_MASK(TERAKAN_RESOURCE_HW_COUNT_FETCH);
   /* Initialize to unbound. */
   config->vi_.resources_bound = 0b0;

   for (unsigned stage_index = 0; stage_index <= MESA_SHADER_COMPUTE; ++stage_index) {
      struct terakan_hw_config_sqk_stage * const stage = &config->stages_[stage_index];

      stage->usage = NULL;

      /* The current hardware register contents are unknown. */
      terakan_hw_config_sqk_stage_emission_flags_set_all(&stage->modified);

      /* Initialize to unbound. */
      memset(stage->kcache, 0, sizeof(stage->kcache));

      for (unsigned sampler_index = 0; sampler_index < TERAKAN_SAMPLER_HW_COUNT_PER_STAGE;
           ++sampler_index) {
         /* For simplicity, not storing unbound or invalid samplers (with `TYPE = 0`) in
          * `terakan_hw_config_sqk`. Vulkan doesn't have defined null sampler descriptors.
          * Initialize to the Direct3D 11 NULL sampler, see the `ID3D11DeviceContext::*SSetSamplers`
          * documentation on MSDN (disregarding the intended opaque white border color because no
          * clamping to the border is used in this sampler).
          */
         uint32_t * const sampler = stage->samplers[sampler_index].sampler;
         sampler[0] = S_03C000_CLAMP_X(V_03C000_SQ_TEX_CLAMP_LAST_TEXEL) |
                      S_03C000_CLAMP_Y(V_03C000_SQ_TEX_CLAMP_LAST_TEXEL) |
                      S_03C000_CLAMP_Z(V_03C000_SQ_TEX_CLAMP_LAST_TEXEL) |
                      S_03C000_XY_MAG_FILTER(V_03C000_SQ_TEX_XY_FILTER_BILINEAR) |
                      S_03C000_XY_MIN_FILTER(V_03C000_SQ_TEX_XY_FILTER_BILINEAR) |
                      S_03C000_MIP_FILTER(V_03C000_SQ_TEX_Z_FILTER_LINEAR);
         sampler[1] = S_03C004_MAX_LOD(0xFFF);
         sampler[2] = S_03C008_TYPE(1);
      }

      /* Initialize to unbound. */
      BITSET_ZERO(stage->resources_bound);
   }

   terakan_hw_config_sqk_stage_emission_flags_set_all(
      &config->modified_in_sw_vs_as_other_hw_stage_);

   if (sw_vs_always_uses_hw_ls_sqk) {
      /* Hardware VS/ES constants are used by the software TES exclusively. */
      terakan_hw_config_sqk_stage_emission_flags_set_all(&config->vses_last_used_by_tes_);
   } else {
      /* Expecting non-tessellated draws to be much more common. */
      config->vses_last_used_by_tes_ = (struct terakan_hw_config_sqk_stage_emission_flags){};
   }

   BITSET_ZERO(config->resources_last_used_as_uncached_fs_);
   BITSET_ZERO(config->resources_last_used_as_uncached_cs_);
}
