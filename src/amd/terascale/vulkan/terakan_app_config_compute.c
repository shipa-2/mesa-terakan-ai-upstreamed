/*
 * Copyright © 2026 Terakan contributors
 */

#include "terakan_app_config_compute.h"

#include "terakan_app_config_draw.h"
#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_hw_config_compute.h"
#include "terakan_hw_config_draw.h"
#include "terakan_hw_config_shared.h"
#include "terakan_hw_config_sqk.h"
#include "terakan_push_constants.h"

#include "util/bitscan.h"
#include "util/macros.h"

#include <string.h>

void
terakan_app_config_compute_reset(struct terakan_app_config_compute * const config)
{
   config->shader = NULL;
   config->block_size_[0] = 1;
   config->block_size_[1] = 1;
   config->block_size_[2] = 1;
   config->lds_dwords_ = 0;
   config->num_waves_ = 1;
   config->cb_target_mask_ = 0;
   config->pending_ = false;
}

void
terakan_app_config_compute_clear_binding(struct terakan_app_config_compute * const config)
{
   config->shader = NULL;
   config->cb_target_mask_ = 0;
   config->pending_ = false;
}

void
terakan_app_config_compute_bind_shader(struct terakan_gfx_command_writer * const command_writer,
                                       struct terakan_shader_impl const * const shader,
                                       uint32_t const block_size_x, uint32_t const block_size_y,
                                       uint32_t const block_size_z, uint32_t const lds_dwords,
                                       uint32_t const num_waves, uint32_t const cb_target_mask)
{
   struct terakan_app_config_compute * const config = &command_writer->app_config_compute;

   config->shader = shader;
   config->block_size_[0] = block_size_x;
   config->block_size_[1] = block_size_y;
   config->block_size_[2] = block_size_z;
   config->lds_dwords_ = lds_dwords;
   config->num_waves_ = num_waves;
   config->cb_target_mask_ = cb_target_mask;
   config->pending_ = true;

   /* Storage buffer UAVs reuse the fragment CB/RAT path; mark which slots the compute shader
    * needs so vkCmdBindDescriptorSets can populate `app_config_draw` before dispatch.
    */
   struct terakan_app_config_draw * const draw = &command_writer->app_config_draw;
   static BITSET_WORD const uav_empty[BITSET_WORDS(TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL)];
   BITSET_WORD const * const uav_used =
      shader != NULL ? shader->uavs_for_mutable_resources_needed : uav_empty;
   size_t const uav_bitset_size =
      sizeof(BITSET_WORD) * BITSET_WORDS(TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL);
   if (draw->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.rtv_dsb_export_count != 0 ||
       memcmp(draw->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.uav_used, uav_used,
              uav_bitset_size) != 0) {
      draw->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.rtv_dsb_export_count = 0;
      memcpy(draw->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.uav_used, uav_used,
             uav_bitset_size);
      terakan_app_config_draw_set_pending(draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_UAV_AND_UNUSED_MRT);
   }
}

void
terakan_app_config_compute_apply_pending(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_compute * const config = &command_writer->app_config_compute;
   if (!config->pending_) {
      return;
   }
   config->pending_ = false;

   struct terakan_shader_impl const * const shader = config->shader;

   if (shader != NULL && shader->scratch_item_size_dwords != 0) {
      terakan_hw_config_draw_set_sq_ring_itemsize_dwords(
         &command_writer->hw_config_draw, TERAKAN_SHADER_RING_INDEX_LSTMP,
         shader->scratch_item_size_dwords);
      terakan_hw_config_shared_compute_set_sq_ring_usage_lstmp(&command_writer->hw_config_shared,
                                                               true);
   }

   terakan_hw_config_compute_set_sq_pgm_ls(
      &command_writer->hw_config_compute, shader != NULL ? &shader->static_state : NULL);
   terakan_hw_config_compute_set_dispatch_params(
      &command_writer->hw_config_compute, config->block_size_[0], config->block_size_[1],
      config->block_size_[2], config->lds_dwords_, config->num_waves_);

   terakan_hw_config_draw_set_cb_target_mask(&command_writer->hw_config_draw,
                                             config->cb_target_mask_);

   command_writer->push_constants_state.graphics_stages_using_push_constants &=
      ~VK_SHADER_STAGE_COMPUTE_BIT;
   command_writer->push_constants_state.usage_compute = (struct terakan_push_constants_usage){};
   if (shader != NULL && !terakan_push_constants_usage_empty(shader->push_constants_usage)) {
      command_writer->push_constants_state.usage_compute = shader->push_constants_usage;
      command_writer->push_constants_state.graphics_stages_using_push_constants |=
         VK_SHADER_STAGE_COMPUTE_BIT;
   }

   terakan_hw_config_sqk_set_usage_cs(&command_writer->hw_config_sqk,
                                      shader != NULL ? &shader->sqk_usage : NULL);
}
