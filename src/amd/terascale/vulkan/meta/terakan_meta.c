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

#include "terakan_meta_impl.h"

#include "terakan_device.h"
#include "terakan_physical_device.h"

#include "amd/terascale/common/terascale_format.h"
#include "util/bitscan.h"
#include "util/macros.h"

#include <assert.h>

struct terakan_meta_shader const * const terakan_meta_shaders[TERAKAN_META_SHADER_COUNT] = {
   [TERAKAN_META_SHADER_DUMMY_NAN_VS] = &terakan_meta_dummy_nan_vs,
   [TERAKAN_META_SHADER_DUMMY_OPAQUE_PS] = &terakan_meta_dummy_opaque_ps,
   [TERAKAN_META_SHADER_POSITION_FROM_INDEX_VS] = &terakan_meta_position_from_index_vs,
   [TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS] =
      &terakan_meta_position_and_layer_from_index_vs,
   [TERAKAN_META_SHADER_CLEAR_DEPTH_VS] = &terakan_meta_clear_depth_vs,
   [TERAKAN_META_SHADER_CLEAR_COLOR_PS] = &terakan_meta_clear_color_ps,
   [TERAKAN_META_SHADER_COPY_BUFFER_TO_IMAGE_PS] = &terakan_meta_copy_buffer_to_image_ps,
   [TERAKAN_META_SHADER_COPY_IMAGE_TO_BUFFER_PS] = &terakan_meta_copy_image_to_buffer_ps,
   [TERAKAN_META_SHADER_COPY_IMAGE_PS] = &terakan_meta_copy_image_ps,
   [TERAKAN_META_SHADER_BLIT_IMAGE_PS] = &terakan_meta_blit_image_ps,
   [TERAKAN_META_SHADER_COPY_EXPAND_3X_PS] = &terakan_meta_copy_expand_3x_ps,
   [TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_1_RB_VS] = &terakan_meta_query_accum_zpass_1_rb_vs,
   [TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_2_RB_VS] = &terakan_meta_query_accum_zpass_2_rb_vs,
   [TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_4_RB_VS] = &terakan_meta_query_accum_zpass_4_rb_vs,
   [TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_8_RB_VS] = &terakan_meta_query_accum_zpass_8_rb_vs,
   [TERAKAN_META_SHADER_QUERY_ACCUM_PIPELINESTAT_VS] = &terakan_meta_query_accum_pipelinestat_vs,
   [TERAKAN_META_SHADER_QUERY_ACCUM_STREAMOUTSTATS_VS] =
      &terakan_meta_query_accum_streamoutstats_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_1_RB_VS] =
      &terakan_meta_query_copy_zpass_32_bit_1_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_2_RB_VS] =
      &terakan_meta_query_copy_zpass_32_bit_2_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_4_RB_VS] =
      &terakan_meta_query_copy_zpass_32_bit_4_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_8_RB_VS] =
      &terakan_meta_query_copy_zpass_32_bit_8_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_1_RB_VS] =
      &terakan_meta_query_copy_zpass_64_bit_1_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_2_RB_VS] =
      &terakan_meta_query_copy_zpass_64_bit_2_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_4_RB_VS] =
      &terakan_meta_query_copy_zpass_64_bit_4_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_8_RB_VS] =
      &terakan_meta_query_copy_zpass_64_bit_8_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_PIPELINESTAT_32_BIT_VS] =
      &terakan_meta_query_copy_pipelinestat_32_bit_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_PIPELINESTAT_64_BIT_VS] =
      &terakan_meta_query_copy_pipelinestat_64_bit_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_TIMESTAMP_32_BIT_VS] =
      &terakan_meta_query_copy_timestamp_32_bit_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_TIMESTAMP_64_BIT_VS] =
      &terakan_meta_query_copy_timestamp_64_bit_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_STREAMOUTSTATS_32_BIT_VS] =
      &terakan_meta_query_copy_streamoutstats_32_bit_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_STREAMOUTSTATS_64_BIT_VS] =
      &terakan_meta_query_copy_streamoutstats_64_bit_vs,
};

void
terakan_meta_config_draw_set_sq_pgm_vs(struct terakan_gfx_command_writer * const command_writer,
                                       enum terakan_meta_shader_index const shader_index)
{
   assert(
      shader_index != TERAKAN_META_SHADER_DUMMY_NAN_VS &&
      "The dummy NaN vertex shader is used for NULL fallback special case handling in "
      "`terakan_hw_config_draw`, and it produces no observable effects in meta draws, so there's "
      "no reason to use it there");
   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_PRE_RASTERIZATION);
   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);
   terakan_hw_config_draw_set_sq_pgm_vs(&command_writer->hw_config_draw,
                                        &device->meta_shaders[shader_index]);
   terakan_hw_config_sqk_set_usage_vs_tes(&command_writer->hw_config_sqk,
                                          &device->meta_shader_sqk_usage[shader_index], NULL);
}

void
terakan_meta_config_draw_set_sq_pgm_ps(struct terakan_gfx_command_writer * const command_writer,
                                       enum terakan_meta_shader_index const shader_index)
{
   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FRAGMENT);
   if (shader_index == TERAKAN_META_SHADER_DUMMY_OPAQUE_PS) {
      /* Already used in `terakan_hw_config_draw` as the NULL fallback, but the setter may not
       * distinguish between NULL and the explicitly provided dummy shader, and may mark the shader
       * binding as modified when switching between the two.
       */
      terakan_hw_config_draw_set_sq_pgm_ps(&command_writer->hw_config_draw, NULL);
      terakan_hw_config_sqk_set_usage_fs(&command_writer->hw_config_sqk, NULL);
   } else {
      struct terakan_device const * const device =
         terakan_gfx_command_writer_device(command_writer);
      terakan_hw_config_draw_set_sq_pgm_ps(&command_writer->hw_config_draw,
                                           &device->meta_shaders[shader_index]);
      terakan_hw_config_sqk_set_usage_fs(&command_writer->hw_config_sqk,
                                         &device->meta_shader_sqk_usage[shader_index]);
   }
}

void
terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const cb_target_mask,
   struct terakan_bo const * const * const rtvs_bo,
   struct terakan_color_descriptor const * const rtvs_color,
   struct terakan_color_meta_descriptor const * const rtvs_meta_opt, uint32_t db_shader_control)
{
   terakan_app_config_draw_set_pending(
      &command_writer->app_config_draw,
      TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL);
   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_UAV_AND_UNUSED_MRT);
   terakan_hw_config_draw_set_cb_target_mask(&command_writer->hw_config_draw, cb_target_mask);
   uint32_t cb_target_mask_remaining = cb_target_mask;
   while (cb_target_mask_remaining) {
      unsigned const rtv_index = (unsigned)(ffs(cb_target_mask_remaining) - 1) / 4;
      cb_target_mask_remaining &= ~((uint32_t)0xF << (4 * rtv_index));
      struct terakan_bo const * const rtv_bo = rtvs_bo[rtv_index];
      struct terakan_color_descriptor const * const rtv_color = &rtvs_color[rtv_index];
      if (!terakan_color_descriptor_is_bound(rtv_bo, rtv_color)) {
         terakan_hw_config_draw_set_cb_color_unbound(&command_writer->hw_config_draw, rtv_index,
                                                     V_028C70_EXPORT_4C_16BPC);
         continue;
      }
      assert(!G_028C70_RAT(rtv_color->info) &&
             "Use `terakan_meta_config_draw_set_cb_uav` for UAVs");
      terakan_hw_config_draw_set_cb_color(&command_writer->hw_config_draw, rtv_index, rtv_bo,
                                          rtv_color,
                                          rtvs_meta_opt != NULL ? &rtvs_meta_opt[rtv_index] : NULL);
      terakan_hw_config_draw_set_cb_blend_control(&command_writer->hw_config_draw, rtv_index, 0);
      if (G_028C70_SOURCE_FORMAT(rtv_color->info) == V_028C70_EXPORT_4C_32BPC) {
         db_shader_control &= C_02880C_DUAL_EXPORT_ENABLE;
      }
   }
   terakan_meta_config_draw_set_db_shader_control(command_writer, db_shader_control);
}

void
terakan_meta_config_draw_set_cb_uav(struct terakan_gfx_command_writer * const command_writer,
                                    unsigned const uav_index, struct terakan_bo const * const bo,
                                    struct terakan_color_descriptor const * const color)
{
   terakan_app_config_draw_set_pending(
      &command_writer->app_config_draw,
      TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL);
   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_UAV_AND_UNUSED_MRT);
   if (!terakan_color_descriptor_is_bound(bo, color)) {
      terakan_hw_config_draw_set_cb_color_unbound(&command_writer->hw_config_draw, uav_index,
                                                  V_028C70_EXPORT_4C_16BPC);
      return;
   }
   assert(G_028C70_RAT(color->info) &&
          "Use `terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control` for RTVs");
   terakan_hw_config_draw_set_cb_color(&command_writer->hw_config_draw, uav_index, bo, color, NULL);
}

void
terakan_meta_config_draw_begin(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_meta_config_draw_begin_options const * const begin_options)
{
   command_writer->meta_blit_draw_session_active = false;

   /* Disable query counter incrementing. */
   terakan_hw_config_shared_set_pipelinestat_streamoutstats_enable(
      &command_writer->hw_config_shared, false);
   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_COUNT_CONTROL);
   terakan_hw_config_draw_set_db_count_control(&command_writer->hw_config_draw,
                                               S_028004_ZPASS_INCREMENT_DISABLE(true));

   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_PRIMITIVE_TYPE);
   terakan_hw_config_shared_draw_set_vgt_primitive_type(&command_writer->hw_config_shared,
                                                        begin_options->vgt_primitive_type);

   if (!begin_options->vgt_index_offset_explicit) {
      terakan_meta_config_draw_set_vgt_index_offset(command_writer, 0);
   }

   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER);
   terakan_hw_config_draw_set_vgt_multi_prim_ib_reset_en(&command_writer->hw_config_draw, false);

   struct terakan_physical_device_chip_info const * const chip_info =
      &terakan_gfx_command_writer_physical_device(command_writer)->chip_info;

   /* Use only the vertex pre-rasterization shader stage, and disable ring buffers in the
    * pre-rasterization shaders for them not to limit occupancy.
    * Note that disabling fetch shader resource usage is not needed because fetch shader resources
    * are set by `terakan_app_config_draw`, which is not applied for meta draws.
    */

   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_PRE_RASTERIZATION);

   terakan_hw_config_draw_set_vgt_shader_stages_en(&command_writer->hw_config_draw,
                                                   S_028B54_VS_EN(V_028B54_VS_STAGE_REAL));

   uint32_t const pre_rasterization_shader_rings = BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_LSTMP) |
                                                   BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_HSTMP) |
                                                   BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_ESTMP) |
                                                   BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_GSTMP) |
                                                   BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_VSTMP);
   terakan_hw_config_shared_draw_set_sq_ring_usage(&command_writer->hw_config_shared,
                                                   pre_rasterization_shader_rings, 0b0);
   u_foreach_bit (shader_ring_index, pre_rasterization_shader_rings) {
      terakan_hw_config_draw_set_sq_ring_itemsize_dwords(
         &command_writer->hw_config_draw, (enum terakan_shader_ring_index)shader_ring_index, 0);
   }

   terakan_hw_config_sqk_set_usage_tcs(&command_writer->hw_config_sqk, NULL);
   terakan_hw_config_sqk_set_usage_gs(&command_writer->hw_config_sqk, NULL);

   if (chip_info->is_r9xx) {
      terakan_app_config_draw_set_pending(
         &command_writer->app_config_draw,
         TERAKAN_APP_CONFIG_DRAW_ENTRY_IA_MULTI_VGT_PARAM_PRE_RASTERIZER_DISCARD_R9XX);
      terakan_hw_config_draw_set_ia_multi_vgt_param(&command_writer->hw_config_draw,
                                                    S_028AA8_PRIMGROUP_SIZE(128 - 1));
   } else {
      terakan_hw_config_shared_draw_set_sq_thread_resource_mgmt(
         &command_writer->hw_config_shared, chip_info->sq_thread_resource_mgmt_ts_gs_r8xx[0][0]);
      uint32_t const sq_stack_resource_mgmt[3] = {
         S_008C20_NUM_PS_STACK_ENTRIES(chip_info->sq_max_stack_entries -
                                       chip_info->sq_max_stack_entries / 2) |
         S_008C20_NUM_VS_STACK_ENTRIES(chip_info->sq_max_stack_entries / 2)};
      terakan_hw_config_shared_draw_set_sq_stack_resource_mgmt(&command_writer->hw_config_shared,
                                                               sq_stack_resource_mgmt);
   }

   /* Either disable rasterization if it's not needed, or disable clipping as meta shaders normally
    * work with pixel coordinates.
    */
   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_CL_CLIP_CNTL);
   terakan_hw_config_draw_set_pa_cl_clip_cntl(&command_writer->hw_config_draw,
                                              begin_options->rasterization.enable
                                                 ? S_028810_CLIP_DISABLE(true)
                                                 : S_028810_DX_RASTERIZATION_KILL(true));

   switch (begin_options->cb_and_db_shader_control_mode) {
   case TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_DISABLE:
      if (begin_options->rasterization.enable) {
         terakan_meta_config_draw_set_db_shader_control(command_writer,
                                                        TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);
      }
      terakan_meta_config_draw_set_cb_color_control_for_mode(command_writer, V_028808_CB_DISABLE);
      break;
   case TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_DYNAMIC:
      break;
   case TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_UAV_ONLY:
      if (begin_options->rasterization.enable) {
         terakan_meta_config_draw_set_db_shader_control(
            command_writer, TERAKAN_META_CONFIG_DRAW_DB_SHADER_CONTROL_PS_MEMORY_EXPORT);
         terakan_app_config_draw_set_pending(
            &command_writer->app_config_draw,
            TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL);
         terakan_hw_config_draw_set_cb_target_mask(&command_writer->hw_config_draw, 0b0);
      }
      terakan_meta_config_draw_set_cb_color_control_for_mode(command_writer, V_028808_CB_NORMAL);
      break;
   case TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_WITH_RTV_AND_DYNAMIC_DB_SHADER_CONTROL:
      terakan_meta_config_draw_set_cb_color_control_for_mode(command_writer, V_028808_CB_NORMAL);
      break;
   }

   if (begin_options->rasterization.enable) {
      /* Configure rasterization and the subsequent pipeline stages. */

      unsigned const msaa_num_samples_log2 = begin_options->rasterization.msaa_num_samples_log2;
      assert(begin_options->rasterization.msaa_num_anchor_samples_log2 <= msaa_num_samples_log2);

      terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_VPORT);
      terakan_hw_config_draw_set_pa_sc_vport_scissor_needed_count(&command_writer->hw_config_draw,
                                                                  1);
      terakan_hw_config_draw_set_pa_sc_vport_zmin_zmax_needed_count(&command_writer->hw_config_draw,
                                                                    0);
      terakan_hw_config_draw_set_pa_cl_vport_scale_offset_needed_count(
         &command_writer->hw_config_draw, 0);

      terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_SC_MODE_CNTL);
      terakan_hw_config_draw_set_pa_su_sc_mode_cntl(
         &command_writer->hw_config_draw,
         S_028814_POLY_MODE(V_028814_X_DISABLE_POLY_MODE) |
            S_028814_POLYMODE_FRONT_PTYPE(V_028814_X_DRAW_TRIANGLES) |
            S_028814_POLYMODE_BACK_PTYPE(V_028814_X_DRAW_TRIANGLES));

      terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_CL_VTE_CNTL);
      terakan_hw_config_draw_set_pa_cl_vte_cntl(
         &command_writer->hw_config_draw, S_028818_VTX_XY_FMT(true) | S_028818_VTX_Z_FMT(true));

      terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_MODE_CNTL_0);
      terakan_hw_config_draw_set_pa_sc_mode_cntl_0(
         &command_writer->hw_config_draw, S_028A48_MSAA_ENABLE(msaa_num_samples_log2 != 0));

      terakan_app_config_draw_set_pending(
         &command_writer->app_config_draw,
         TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_AA_CONFIG_SAMPLE_LOCS);
      /* TODO(Triang3l): Custom sample locations, such as for HTile expansion. */
      terakan_hw_config_draw_set_pa_sc_aa_config_sample_locs(
         &command_writer->hw_config_draw,
         S_028BE0_MSAA_NUM_SAMPLES(msaa_num_samples_log2) |
            S_028BE0_MAX_SAMPLE_DIST(
               terakan_hw_config_draw_pa_sc_aa_standard_max_sample_dists[msaa_num_samples_log2]) |
            S_028BE0_MSAA_EXPOSED_SAMPLES(msaa_num_samples_log2),
         terakan_hw_config_draw_pa_sc_aa_standard_sample_locs[msaa_num_samples_log2][0]);

      terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_AA_MASK);
      terakan_hw_config_draw_set_pa_sc_aa_mask(&command_writer->hw_config_draw,
                                               ((uint32_t)1 << (1u << msaa_num_samples_log2)) - 1u);

      /* Disable the pixel shader scratch ring buffer for it not to limit occupancy. */
      terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FRAGMENT);
      terakan_hw_config_shared_draw_set_sq_ring_usage(
         &command_writer->hw_config_shared, BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_PSTMP), 0b0);
      terakan_hw_config_draw_set_sq_ring_itemsize_dwords(&command_writer->hw_config_draw,
                                                         TERAKAN_SHADER_RING_INDEX_PSTMP, 0);

      terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE);
      /* Not using viewports, including Z clamping, in meta draws. */
      terakan_hw_config_draw_set_db_render_override(&command_writer->hw_config_draw,
                                                    S_02800C_DISABLE_VIEWPORT_CLAMP(true));

      terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE2);
      terakan_hw_config_draw_set_db_render_override2(
         &command_writer->hw_config_draw,
         chip_info->is_r9xx ? S_028010_DISABLE_COLOR_ON_VALIDATION(true) |
                                 S_028010_DECOMPRESS_Z_ON_FLUSH(
                                    begin_options->rasterization.msaa_num_anchor_samples_log2 > 1)
                            : 0);

      terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_EQAA);
      /* `hw_config_draw` `PA_SC_MODE_CNTL_1` is updated by `app_config_draw` `DB_EQAA`. */
      terakan_hw_config_draw_set_pa_sc_mode_cntl_1(
         &command_writer->hw_config_draw, TERAKAN_HW_CONFIG_DRAW_PA_SC_MODE_CNTL_1_CONSTANT);
      if (chip_info->is_r9xx) {
         terakan_hw_config_draw_set_db_eqaa(
            &command_writer->hw_config_draw,
            TERAKAN_HW_CONFIG_DRAW_DB_EQAA_CONSTANT |
               S_028804_MAX_ANCHOR_SAMPLES(
                  begin_options->rasterization.msaa_num_anchor_samples_log2) |
               S_028804_MASK_EXPORT_NUM_SAMPLES(msaa_num_samples_log2) |
               S_028804_ALPHA_TO_MASK_NUM_SAMPLES(msaa_num_samples_log2));
      }

      terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_ALPHA_TO_MASK);
      terakan_hw_config_draw_set_db_alpha_to_mask(&command_writer->hw_config_draw, 0);

      if (!begin_options->rasterization.db_explicit) {
         terakan_app_config_draw_set_pending(
            &command_writer->app_config_draw,
            TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_CONTROL_REF_MASK);
         terakan_hw_config_draw_set_db_depth_control(&command_writer->hw_config_draw, 0);
      }
   } else {
      /* Don't emit application's pixel shader constants modified outside `terakan_app_config_draw`.
       */
      terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FRAGMENT);
      terakan_hw_config_sqk_set_usage_fs(&command_writer->hw_config_sqk, NULL);
   }
}

uint32_t *
terakan_meta_draw_immediate_32_bit_indexed(struct terakan_gfx_command_writer * const command_writer,
                                           uint32_t const index_count,
                                           uint32_t const instance_count)
{
   assert(index_count != 0);

   terakan_meta_config_draw_set_vgt_num_instances(command_writer, instance_count);

   uint32_t * packet;

   if (instance_count > 1) {
      /* `PKT3_DRAW_INDEX_IMMD` with multiple instances causes a hang (tested with meta rectangles
       * on Barts with the firmware used by DRM Radeon 2.50.0).
       */

      struct terakan_hw_config_draw_vgt_dma_index_buffer index_buffer;
      uint32_t * const index_buffer_mapping = terakan_push_buffer_allocate(
         command_writer->base.command_buffer, (uint32_t)sizeof(uint32_t) * index_count,
         sizeof(uint32_t), &index_buffer.bo, &index_buffer.va);
      if (unlikely(index_buffer_mapping == NULL)) {
         return NULL;
      }
      index_buffer.size_indices = index_count;
      terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER);
      terakan_hw_config_draw_set_vgt_dma_index_buffer(
         &command_writer->hw_config_draw, index_buffer,
         TERAKAN_HW_CONFIG_DRAW_VGT_DMA_INDEX_TYPE_32_HOST_ENDIAN);

      terakan_meta_before_draw(command_writer);
      packet = terakan_gfx_command_writer_emit(command_writer,
                                               TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 4);
      if (unlikely(packet == NULL)) {
         return NULL;
      }
      *packet++ = PKT3(EG_PKT3_DRAW_INDEX_OFFSET, 4 - 2, 0);
      *packet++ = 0;
      *packet++ = index_count;
      *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_DMA);
      terakan_gfx_command_writer_emit_done(command_writer, packet);

      return index_buffer_mapping;
   }

   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER);
   terakan_hw_config_draw_set_vgt_dma_index_type_for_immediate(&command_writer->hw_config_draw,
                                                               VGT_INDEX_32);

   terakan_meta_before_draw(command_writer);
   packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 3 + index_count);
   if (unlikely(packet == NULL)) {
      return NULL;
   }
   *packet++ = PKT3(PKT3_DRAW_INDEX_IMMD, 3 - 2 + index_count, 0);
   *packet++ = index_count;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_IMMEDIATE);
   uint32_t * const packet_indices = packet;
   packet += index_count;
   terakan_gfx_command_writer_emit_done(command_writer, packet);

   return packet_indices;
}
