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

#include "terakan_app_config_draw.h"

#include "terakan_command_buffer.h"
#include "terakan_physical_device.h"

#include "amd/terascale/common/terascale_format.h"
#include "gallium/drivers/r600/evergreend.h"
#include "util/bitscan.h"
#include "util/macros.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Section "Isoline Tessellation" of the Vulkan 1.4.337 specification says:
 *
 *     "Each of the n isolines is then subdivided according to the second outer tessellation level
 *     and the tessellation spacing, resulting in m line segments. Each segment of each line is
 *     emitted by the tessellator. These line segments are generated with a topology similar to line
 *     lists, except that the order in which each line is generated, and the order in which the
 *     vertices are generated for each line segment, are implementation-dependent."
 */
#define TERAKAN_APP_CONFIG_DRAW_PA_SC_LINE_STIPPLE_PER_PRIMITIVE_RESET_PRIMITIVE_TYPES             \
   (BITFIELD64_BIT(V_008958_DI_PT_LINELIST) | BITFIELD64_BIT(V_008958_DI_PT_LINELIST_ADJ) |        \
    BITFIELD64_BIT(V_008958_DI_PT_PATCH))

void
terakan_app_config_draw_set_cb_color_rtv(struct terakan_app_config_draw * const config,
                                         unsigned const color_index,
                                         struct terakan_bo const * const bo,
                                         struct terakan_color_descriptor const * const color,
                                         struct terakan_color_meta_descriptor const * const meta)
{
   assert(color_index < TERAKAN_COLOR_HW_RTV_COUNT);
   struct terakan_app_config_draw_cb_color_rtv * const rtv =
      &config->cb_color_rtv_and_blend_control_.rtv[color_index];
   if (terakan_color_descriptor_is_bound(bo, color)) {
      assert(!G_028C70_RAT(color->info));
      /* Not checking if the new RTV is the same, even though applying is complex, because in
       * Vulkan, the same color attachments may be rebound only via `vkCmdNextSubpass`, but when
       * subpasses are implemented on top of dynamic rendering, it ends rendering anyway, causing
       * all attachments to be unbound and bound between subpasses.
       */
      rtv->bo = bo;
      memcpy(&rtv->color, color, sizeof(struct terakan_color_descriptor));
      if (meta != NULL) {
         memcpy(&rtv->meta, meta, sizeof(struct terakan_color_meta_descriptor));
      } else {
         rtv->meta = terakan_color_meta_descriptor_create_disabled(color);
      }
   } else {
      if (!terakan_color_descriptor_is_bound(rtv->bo, &rtv->color)) {
         return;
      }
      rtv->bo = NULL;
      rtv->color.info = 0;
   }
   terakan_app_config_draw_set_pending(
      config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL);
}

void
terakan_app_config_draw_set_cb_color_uav(struct terakan_app_config_draw * const config,
                                         unsigned const mutable_resource_index,
                                         struct terakan_bo const * const bo,
                                         struct terakan_color_descriptor const * const color)
{
   assert(mutable_resource_index < TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL);
   if (terakan_color_descriptor_is_bound(bo, color)) {
      assert(G_028C70_RAT(color->info));
      struct terakan_app_config_draw_cb_color_uav * const uav =
         &config->cb_color_uav_and_unused_mrt_.uav[mutable_resource_index];
      if (BITSET_TEST(config->cb_color_uav_and_unused_mrt_.uav_bound, mutable_resource_index) &&
          uav->bo == bo &&
          memcmp(&uav->color, color, sizeof(struct terakan_color_descriptor)) == 0) {
         return;
      }
      BITSET_SET(config->cb_color_uav_and_unused_mrt_.uav_bound, mutable_resource_index);
      uav->bo = bo;
      uav->color = *color;
   } else {
      if (!BITSET_TEST(config->cb_color_uav_and_unused_mrt_.uav_bound, mutable_resource_index)) {
         return;
      }
      BITSET_CLEAR(config->cb_color_uav_and_unused_mrt_.uav_bound, mutable_resource_index);
   }
   if (BITSET_TEST(config->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.uav_used,
                   mutable_resource_index)) {
      terakan_app_config_draw_set_pending(
         config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_UAV_AND_UNUSED_MRT);
   }
}

typedef void (*terakan_app_config_apply_function)(
   struct terakan_gfx_command_writer * command_writer);

static void
terakan_app_config_draw_apply_vgt_primitive_type(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;

   terakan_hw_config_shared_draw_set_vgt_primitive_type(&command_writer->hw_config_shared,
                                                        config->vgt_primitive_type_);

   uint32_t const primitive_type = G_008958_PRIM_TYPE(config->vgt_primitive_type_);

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(SQ_PGM_PRE_RASTERIZATION, VGT_PRIMITIVE_TYPE);
   bool const tessellation_enable = primitive_type == V_008958_DI_PT_PATCH;
   if (config->sq_pgm_pre_rasterization_.from_apply_vgt_primitive_type.tessellation_enable !=
       tessellation_enable) {
      config->sq_pgm_pre_rasterization_.from_apply_vgt_primitive_type.tessellation_enable =
         tessellation_enable;
      terakan_app_config_draw_set_pending(config,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_PRE_RASTERIZATION);
   }

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(PA_SC_LINE_STIPPLE, VGT_PRIMITIVE_TYPE);
   bool const line_stipple_per_primitive_reset =
      (TERAKAN_APP_CONFIG_DRAW_PA_SC_LINE_STIPPLE_PER_PRIMITIVE_RESET_PRIMITIVE_TYPES &
       BITFIELD64_BIT(primitive_type)) != 0;
   if (config->pa_sc_line_stipple_.from_apply_vgt_primitive_type.per_primitive_reset !=
       line_stipple_per_primitive_reset) {
      config->pa_sc_line_stipple_.from_apply_vgt_primitive_type.per_primitive_reset =
         line_stipple_per_primitive_reset;
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_LINE_STIPPLE);
   }
}

static void
terakan_app_config_draw_apply_vgt_index_offset(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_set_vgt_index_offset(&command_writer->hw_config_draw,
                                               command_writer->app_config_draw.vgt_index_offset_);
}

static void
terakan_app_config_draw_apply_vgt_dma_index_buffer(
   struct terakan_gfx_command_writer * const command_writer)
{
   if (!command_writer->app_config_draw.vgt_dma_index_buffer_.draw_indexed) {
      terakan_hw_config_draw_set_vgt_multi_prim_ib_reset_en(&command_writer->hw_config_draw, false);
      return;
   }

   /* #MemoryIntegrity is handled by `hw_config`. */
   terakan_hw_config_draw_set_vgt_dma_index_buffer(
      &command_writer->hw_config_draw,
      command_writer->app_config_draw.vgt_dma_index_buffer_.index_buffer,
      command_writer->app_config_draw.vgt_dma_index_buffer_.index_type);

   bool const multi_prim_reset_enable =
      command_writer->app_config_draw.vgt_dma_index_buffer_.multi_prim_reset_enable &&
      ((command_writer->app_config_draw.vgt_dma_index_buffer_.index_type & VGT_INDEX_32) ||
       command_writer->app_config_draw.vgt_dma_index_buffer_.multi_prim_reset_index <= UINT16_MAX);
   terakan_hw_config_draw_set_vgt_multi_prim_ib_reset_en(&command_writer->hw_config_draw,
                                                         multi_prim_reset_enable);
   if (multi_prim_reset_enable) {
      terakan_hw_config_draw_set_vgt_multi_prim_ib_reset_index(
         &command_writer->hw_config_draw,
         command_writer->app_config_draw.vgt_dma_index_buffer_.multi_prim_reset_index);
   }
}

static void
terakan_app_config_draw_apply_sq_pgm_pre_rasterization(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;

   bool const tessellation_enable =
      config->sq_pgm_pre_rasterization_.from_apply_vgt_primitive_type.tessellation_enable;

   /* Get the shaders actually needed for each software stage, and check if all of them are bound.
    */
   /* TODO(Triang3l): Research fragment shader primitive ID input (does it require separate shaders,
    * and `GS_SCENARIO_A` and `PRIMITIVEID_EN`?)
    */
   bool any_stage_unbound = false;
   struct terakan_shader_impl const * sw_stages[MESA_SHADER_FRAGMENT] = {};
   sw_stages[MESA_SHADER_GEOMETRY] = config->sq_pgm_pre_rasterization_.geometry;
   if (tessellation_enable) {
      sw_stages[MESA_SHADER_VERTEX] = config->sq_pgm_pre_rasterization_.vertex_as_local;
      sw_stages[MESA_SHADER_TESS_CTRL] = config->sq_pgm_pre_rasterization_.tessellation_control;
      sw_stages[MESA_SHADER_TESS_EVAL] =
         sw_stages[MESA_SHADER_GEOMETRY] != NULL
            ? config->sq_pgm_pre_rasterization_.tessellation_evaluation_as_export
            : config->sq_pgm_pre_rasterization_.tessellation_evaluation_as_vertex;
      if (unlikely(sw_stages[MESA_SHADER_TESS_CTRL] == NULL ||
                   sw_stages[MESA_SHADER_TESS_EVAL] == NULL)) {
         any_stage_unbound = true;
      }
   } else {
      sw_stages[MESA_SHADER_VERTEX] = sw_stages[MESA_SHADER_GEOMETRY] != NULL
                                         ? config->sq_pgm_pre_rasterization_.vertex_as_export
                                         : config->sq_pgm_pre_rasterization_.vertex_as_vertex;
   }
   if (unlikely(sw_stages[MESA_SHADER_VERTEX] == NULL)) {
      any_stage_unbound = true;
   }

   /* Make sure `terakan_hw_config_draw` sets up shaders that result in all geometry discarded if
    * the shader combination is invalid.
    * Note that the SQK usage of all pre-rasterization shaders must be set to NULL in this case, so
    * that it doesn't matter how `terakan_hw_config_sqk` checks whether tessellation is enabled and
    * which software stages need to use the hardware LS and VS/ES constants.
    */
   if (unlikely(any_stage_unbound)) {
      memset(sw_stages, 0, sizeof(sw_stages));
   }
   bool const geometry_shader_enable = sw_stages[MESA_SHADER_GEOMETRY] != NULL;

   /* Map the hardware stages to the software shaders. */
   struct terakan_shader_impl const * const hw_ls =
      tessellation_enable ? sw_stages[MESA_SHADER_VERTEX] : NULL;
   gl_shader_stage const hw_vses_sw_stage =
      tessellation_enable ? MESA_SHADER_TESS_EVAL : MESA_SHADER_VERTEX;
   struct terakan_shader_impl const * const hw_es =
      sw_stages[MESA_SHADER_GEOMETRY] != NULL ? sw_stages[hw_vses_sw_stage] : NULL;
   /* TODO(Triang3l): Copy shader for the geometry shader. */
   struct terakan_shader_impl const * const hw_vs = sw_stages[hw_vses_sw_stage];

   struct terakan_physical_device_chip_info const * const chip_info =
      &terakan_gfx_command_writer_physical_device(command_writer)->chip_info;

   /* Set hardware resource usage. */
   /* R9xx doesn't have `SQ_THREAD_RESOURCE_MGMT`, use the maximum possible thread counts for ring
    * sizes.
    */
   /* TODO(Triang3l): Research smaller sizes for different stages. */
   uint32_t const ring_num_threads_sq_thread_resource_mgmt_r9xx[2] = {
      S_008C18_NUM_PS_THREADS(256 >> 3) | S_008C18_NUM_VS_THREADS(256 >> 3) |
         S_008C18_NUM_GS_THREADS(256 >> 3) | S_008C18_NUM_ES_THREADS(256 >> 3),
      S_008C1C_NUM_HS_THREADS(256 >> 3) | S_008C1C_NUM_LS_THREADS(256 >> 3)};
   uint32_t const * const sq_thread_resource_mgmt =
      chip_info->is_r9xx
         ? ring_num_threads_sq_thread_resource_mgmt_r9xx
         : chip_info->sq_thread_resource_mgmt_ts_gs_r8xx[(unsigned)tessellation_enable]
                                                        [(unsigned)geometry_shader_enable];
   if (!chip_info->is_r9xx) {
      terakan_hw_config_shared_draw_set_sq_thread_resource_mgmt(&command_writer->hw_config_shared,
                                                                sq_thread_resource_mgmt);
      unsigned const hw_vertex_stage_count =
         (tessellation_enable ? 2 : 0) + (geometry_shader_enable ? 2 : 0) + 1;
      unsigned const vertex_stage_stack_entries =
         chip_info->sq_max_stack_entries / (hw_vertex_stage_count + 1);
      uint32_t const sq_stack_resource_mgmt[3] = {
         S_008C20_NUM_PS_STACK_ENTRIES(chip_info->sq_max_stack_entries -
                                       vertex_stage_stack_entries * hw_vertex_stage_count) |
            S_008C20_NUM_VS_STACK_ENTRIES(vertex_stage_stack_entries),
         S_008C24_NUM_GS_STACK_ENTRIES(vertex_stage_stack_entries) |
            S_008C24_NUM_ES_STACK_ENTRIES(vertex_stage_stack_entries),
         S_008C28_NUM_HS_STACK_ENTRIES(vertex_stage_stack_entries) |
            S_008C28_NUM_LS_STACK_ENTRIES(vertex_stage_stack_entries),
      };
      terakan_hw_config_shared_draw_set_sq_stack_resource_mgmt(&command_writer->hw_config_shared,
                                                               sq_stack_resource_mgmt);
   }

   /* Configure the pipeline shader sequence. */
   terakan_hw_config_draw_set_vgt_shader_stages_en(
      &command_writer->hw_config_draw,
      (tessellation_enable ? S_028B54_LS_EN(V_028B54_LS_STAGE_ON) | S_028B54_HS_EN(true) : 0) |
         (geometry_shader_enable
             ? S_028B54_ES_EN(tessellation_enable ? V_028B54_ES_STAGE_DS : V_028B54_ES_STAGE_REAL) |
                  S_028B54_GS_EN(true) | S_028B54_VS_EN(V_028B54_VS_STAGE_COPY_SHADER)
             : S_028B54_VS_EN(tessellation_enable ? V_028B54_VS_STAGE_DS
                                                  : V_028B54_VS_STAGE_REAL)));

   /* Set the shader code. */
   terakan_hw_config_draw_set_sq_pgm_vs(&command_writer->hw_config_draw,
                                        hw_vs ? &hw_vs->static_state : NULL);
   /* TODO(Triang3l): Bind all the shaders. */

   /* Configure the ring buffers.
    * 1 block in `SQ_THREAD_RESOURCE_MGMT` is 8 wavefronts.
    */
   unsigned const lanes_per_thread_block_log2 = chip_info->wave_lanes_log2 + 3;
   struct ring_config {
      enum terakan_shader_ring_index ring_index;
      uint16_t item_size_dwords;
      uint32_t items_needed;
   } const rings[] = {
      {
         TERAKAN_SHADER_RING_INDEX_LSTMP,
         hw_ls != NULL ? hw_ls->scratch_item_size_dwords : 0,
         G_008C1C_NUM_LS_THREADS(sq_thread_resource_mgmt[1]) << lanes_per_thread_block_log2,
      },
      {
         TERAKAN_SHADER_RING_INDEX_HSTMP,
         sw_stages[MESA_SHADER_TESS_CTRL] != NULL
            ? sw_stages[MESA_SHADER_TESS_CTRL]->scratch_item_size_dwords
            : 0,
         G_008C1C_NUM_HS_THREADS(sq_thread_resource_mgmt[1]) << lanes_per_thread_block_log2,
      },
      {
         TERAKAN_SHADER_RING_INDEX_ESTMP,
         hw_es != NULL ? hw_es->scratch_item_size_dwords : 0,
         G_008C18_NUM_ES_THREADS(sq_thread_resource_mgmt[0]) << lanes_per_thread_block_log2,
      },
      {
         TERAKAN_SHADER_RING_INDEX_GSTMP,
         sw_stages[MESA_SHADER_GEOMETRY] != NULL
            ? sw_stages[MESA_SHADER_GEOMETRY]->scratch_item_size_dwords
            : 0,
         G_008C18_NUM_GS_THREADS(sq_thread_resource_mgmt[0]) << lanes_per_thread_block_log2,
      },
      {
         TERAKAN_SHADER_RING_INDEX_VSTMP,
         hw_vs != NULL ? hw_vs->scratch_item_size_dwords : 0,
         G_008C18_NUM_VS_THREADS(sq_thread_resource_mgmt[0]) << lanes_per_thread_block_log2,
      },
   };
   uint32_t rings_configured_for_entry = 0b0;
   uint32_t rings_used = 0b0;
   for (unsigned ring_config_index = 0; ring_config_index < ARRAY_SIZE(rings);
        ++ring_config_index) {
      struct ring_config const * const ring = &rings[ring_config_index];
      uint32_t const ring_bit = BITFIELD_BIT(ring->ring_index);
      rings_configured_for_entry |= ring_bit;
      terakan_hw_config_draw_set_sq_ring_itemsize_dwords(&command_writer->hw_config_draw,
                                                         ring->ring_index, ring->item_size_dwords);
      if (ring->item_size_dwords == 0) {
         continue;
      }
      rings_used |= ring_bit;
      uint32_t const ring_bytes_needed_shr8 = DIV_ROUND_UP(
         ((uint32_t)ring->item_size_dwords << 2) * ring->items_needed, (uint32_t)0x100);
      uint32_t * const command_buffer_ring_bytes_needed_shr8 =
         &command_writer->base.command_buffer
             ->shader_ring_bytes_needed_for_se_shr8[ring->ring_index];
      *command_buffer_ring_bytes_needed_shr8 =
         MAX2(ring_bytes_needed_shr8, *command_buffer_ring_bytes_needed_shr8);
   }
   terakan_hw_config_shared_draw_set_sq_ring_usage(&command_writer->hw_config_shared,
                                                   rings_configured_for_entry, rings_used);

   /* Set the constant usage. */

   command_writer->push_constants_state.graphics_stages_using_push_constants &=
      ~(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT);
   command_writer->push_constants_state.usage_pre_rasterization =
      (struct terakan_push_constants_usage){};
   for (unsigned sw_stage_index = 0; sw_stage_index < ARRAY_SIZE(sw_stages); ++sw_stage_index) {
      struct terakan_shader_impl const * const stage_shader = sw_stages[sw_stage_index];
      if (stage_shader == NULL ||
          terakan_push_constants_usage_empty(stage_shader->push_constants_usage)) {
         continue;
      }
      command_writer->push_constants_state.usage_pre_rasterization =
         terakan_push_constants_usage_union(
            command_writer->push_constants_state.usage_pre_rasterization,
            stage_shader->push_constants_usage);
      command_writer->push_constants_state.graphics_stages_using_push_constants |=
         mesa_to_vk_shader_stage((gl_shader_stage)sw_stage_index);
   }

   terakan_hw_config_sqk_set_usage_vs_tes(
      &command_writer->hw_config_sqk,
      sw_stages[MESA_SHADER_VERTEX] ? &sw_stages[MESA_SHADER_VERTEX]->sqk_usage : NULL,
      sw_stages[MESA_SHADER_TESS_EVAL] ? &sw_stages[MESA_SHADER_TESS_EVAL]->sqk_usage : NULL);
   terakan_hw_config_sqk_set_usage_tcs(
      &command_writer->hw_config_sqk,
      sw_stages[MESA_SHADER_TESS_CTRL] ? &sw_stages[MESA_SHADER_TESS_CTRL]->sqk_usage : NULL);
   terakan_hw_config_sqk_set_usage_gs(
      &command_writer->hw_config_sqk,
      sw_stages[MESA_SHADER_GEOMETRY] ? &sw_stages[MESA_SHADER_GEOMETRY]->sqk_usage : NULL);

   /* Update dependent configuration entries. */

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(SQ_PGM_FETCH, SQ_PGM_PRE_RASTERIZATION);
   uint32_t const vertex_attributes_used =
      sw_stages[MESA_SHADER_VERTEX] != NULL
         ? sw_stages[MESA_SHADER_VERTEX]->vs.vertex_attributes_needed
         : 0b0;
   if (config->sq_pgm_fetch_.from_apply_sq_pgm_pre_rasterization.attributes_used_by_vs !=
       vertex_attributes_used) {
      config->sq_pgm_fetch_.from_apply_sq_pgm_pre_rasterization.attributes_used_by_vs =
         vertex_attributes_used;
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FETCH);
   }

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(PA_SC_LINE_STIPPLE, SQ_PGM_PRE_RASTERIZATION);
   if (config->pa_sc_line_stipple_.from_apply_sq_pgm_pre_rasterization.geometry_shader_enable !=
       geometry_shader_enable) {
      config->pa_sc_line_stipple_.from_apply_sq_pgm_pre_rasterization.geometry_shader_enable =
         geometry_shader_enable;
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_LINE_STIPPLE);
   }
}

static void
terakan_app_config_draw_apply_sq_pgm_fetch(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;

   struct terakan_vertex_input_fs_layout const * fs_layout;
   struct terakan_vertex_input_fs_layout const * const static_fs_layout =
      config->sq_pgm_fetch_.static_fs != NULL ? &config->sq_pgm_fetch_.static_fs->layout : NULL;
   struct terakan_vertex_input_fs_layout static_fs_layout_with_overrides;
   if (static_fs_layout != NULL) {
      fs_layout = &static_fs_layout_with_overrides;
      /* Check if the precompiled fetch shader can be used with the current configuration. */
      if (static_fs_layout->attributes_used ==
          config->sq_pgm_fetch_.from_apply_sq_pgm_pre_rasterization.attributes_used_by_vs) {
         uint32_t const bindings_with_2048_stride_as_1024_different_bits =
            static_fs_layout->bindings_with_2048_stride_as_1024 ^
            config->sq_pgm_fetch_.desired_2048_stride_as_1024_and_dynamic_fs_layout
               .bindings_with_2048_stride_as_1024;
         /* Fast path - check if all bits of the bit array of bindings with #2048StrideAs1024 match
          * since a 2048 stride is rare, and the workaround is not needed on R9xx unless it's
          * explicitly used (such as for regression testing purposes). If they don't, check only
          * relevant bindings.
          */
         bool used_bindings_with_2048_stride_as_1024_different =
            bindings_with_2048_stride_as_1024_different_bits != 0;
         if (used_bindings_with_2048_stride_as_1024_different) {
            used_bindings_with_2048_stride_as_1024_different = false;
            u_foreach_bit (attribute_index, static_fs_layout->attributes_used) {
               if (G_SQ_VTX_WORD1_DATA_FORMAT(
                      static_fs_layout->attribute_format_fetch_word1[attribute_index]) ==
                   TERASCALE_FORMAT_INDEX_INVALID) {
                  continue;
               }
               if (bindings_with_2048_stride_as_1024_different_bits &
                   BITFIELD_BIT(static_fs_layout->attribute_bindings[attribute_index])) {
                  used_bindings_with_2048_stride_as_1024_different = true;
                  break;
               }
            }
         }
         if (!used_bindings_with_2048_stride_as_1024_different) {
            fs_layout = static_fs_layout;
         }
      }
      if (fs_layout != static_fs_layout) {
         /* The precompiled shader can't be used with the current configuration, need to build a
          * compatible one.
          */
         assert(fs_layout == &static_fs_layout_with_overrides);
         static_fs_layout_with_overrides = *static_fs_layout;
         static_fs_layout_with_overrides.attributes_used =
            config->sq_pgm_fetch_.from_apply_sq_pgm_pre_rasterization.attributes_used_by_vs;
         static_fs_layout_with_overrides.bindings_with_2048_stride_as_1024 =
            config->sq_pgm_fetch_.desired_2048_stride_as_1024_and_dynamic_fs_layout
               .bindings_with_2048_stride_as_1024;
      }
   } else {
      fs_layout = &config->sq_pgm_fetch_.desired_2048_stride_as_1024_and_dynamic_fs_layout;
      config->sq_pgm_fetch_.desired_2048_stride_as_1024_and_dynamic_fs_layout.attributes_used =
         config->sq_pgm_fetch_.from_apply_sq_pgm_pre_rasterization.attributes_used_by_vs;
   }

   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);

   struct terakan_vertex_input_fs const * fs;
   if (fs_layout == static_fs_layout) {
      /* The static fetch shader is compatible, use it. */
      fs = config->sq_pgm_fetch_.static_fs;
   } else {
      /* Build the static fetch shader with overrides or the dynamic fetch shader. */
      fs = &config->sq_pgm_fetch_.last_transient_fs;
      if (!terakan_vertex_input_fs_layout_identical(&config->sq_pgm_fetch_.last_transient_fs.layout,
                                                    fs_layout)) {
         config->sq_pgm_fetch_.last_transient_fs.bo = NULL;
         config->sq_pgm_fetch_.last_transient_fs.layout = *fs_layout;
      }
      if (config->sq_pgm_fetch_.last_transient_fs.bo == NULL) {
         struct terakan_vertex_input_fs_code fs_code;
         terakan_vertex_input_create_fs_code(
            fs_layout, terakan_device_physical_device(device)->chip_info.is_r9xx,
            &config->sq_pgm_fetch_.last_transient_fs.resource_usage, &fs_code);
         if (terakan_vertex_input_fs_code_is_no_operation(&fs_code)) {
            /* Don't allocate a fetch shader for draws that don't need vertex input. */
            config->sq_pgm_fetch_.last_transient_fs.bo = device->meta_shaders_bo;
            config->sq_pgm_fetch_.last_transient_fs.va_shr8 =
               device->meta_shaders_empty_fetch_va_shr8;
         } else {
            uint64_t transient_fs_va;
            uint32_t * const transient_fs_mapping = terakan_push_buffer_allocate(
               command_writer->base.command_buffer,
               (uint32_t)(sizeof(uint32_t) * 2) * terakan_vertex_input_fs_code_qwords(&fs_code),
               TERAKAN_SHADER_PROGRAM_ALIGNMENT, &config->sq_pgm_fetch_.last_transient_fs.bo,
               &transient_fs_va);
            if (unlikely(!transient_fs_mapping)) {
               /* Bind an existing shader program without leaving `terakan_app_config_draw` in an
                * invalid state if allocation has failed, because it may still be used until the
                * allocation error is handled (in Vulkan, it's reported when ending a command
                * buffer).
                */
               config->sq_pgm_fetch_.last_transient_fs.bo = device->meta_shaders_bo;
               config->sq_pgm_fetch_.last_transient_fs.va_shr8 =
                  device->meta_shaders_empty_fetch_va_shr8;
            } else {
               config->sq_pgm_fetch_.last_transient_fs.va_shr8 = (uint32_t)(transient_fs_va >> 8);
               terakan_vertex_input_combine_fs(&fs_code, transient_fs_mapping);
            }
         }
      }
   }

   /* Apply the actual fetch shader to the `terakan_hw_config`, regardless of whether the actual
    * fetch shader was changed since the last time one was applied because `terakan_app_config_draw`
    * entries may be made pending without changing the application configuration, in particular,
    * when setting up meta draws.
    */

   /* `terakan_hw_config_draw` implicitly uses the empty fetch shader from the when NULL is passed,
    * but in the tracking of whether the hardware configuration entry is modified and needs to be
    * emitted, it may possibly treat NULL and the explicitly specified empty shader as different
    * shaders.
    */
   terakan_hw_config_draw_set_sq_pgm_fs(
      &command_writer->hw_config_draw,
      fs->bo == device->meta_shaders_bo && fs->va_shr8 == device->meta_shaders_empty_fetch_va_shr8
         ? NULL
         : fs->bo,
      fs->va_shr8);
   terakan_hw_config_sqk_set_usage_vi(&command_writer->hw_config_sqk,
                                      fs->resource_usage.resources_used);

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(SQ_RESOURCES_FETCH, SQ_PGM_FETCH);
   if (!terakan_vertex_input_fs_resource_usage_equal(
          &config->sq_resources_fetch_.from_apply_sq_pgm_fetch.usage, &fs->resource_usage)) {
      config->sq_resources_fetch_.from_apply_sq_pgm_fetch.usage = fs->resource_usage;
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_RESOURCES_FETCH);
   }
}

static void
terakan_app_config_draw_apply_sq_resources_fetch(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;

   uint32_t unbound_resources = BITFIELD_MASK(TERAKAN_RESOURCE_HW_COUNT_FETCH);

   struct terakan_resource_descriptor descriptor = {
      .resource = {
         [3] = S_03000C_DST_SEL_X(TERASCALE_SWIZZLE_X) | S_03000C_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
               S_03000C_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_03000C_DST_SEL_W(TERASCALE_SWIZZLE_W),
         [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
         [TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_VERTEX_BUFFER,
      }};

   u_foreach_bit (resource_index,
                  config->sq_resources_fetch_.from_apply_sq_pgm_fetch.usage.resources_used) {
      uint8_t const binding_and_truncation =
         config->sq_resources_fetch_.from_apply_sq_pgm_fetch.usage
            .resource_bindings_and_truncation[resource_index];
      uint8_t const binding = binding_and_truncation & 0x1Fu;
      struct terakan_bo const * const bo = config->sq_resources_fetch_.bo[binding];
      if (bo == NULL) {
         continue;
      }
      uint64_t const va = config->sq_resources_fetch_.va[binding];
      uint32_t const app_size_minus_1 = config->sq_resources_fetch_.size_minus_1[binding];
      uint8_t const truncation_bytes = sizeof(uint32_t) * (binding_and_truncation >> 5);
      descriptor.resource[0] = (uint32_t)va;
      descriptor.resource[1] = MAX2(app_size_minus_1, truncation_bytes) - truncation_bytes;
      /* Not using `S_030008_STRIDE` because R9xx increased the width of the stride from 11 to 12
       * bits. On architecture generations prior to R9xx, the bit above the stride is `CLAMP_X`
       * (special filtering to avoid setting it to `CLAMP_NAN` instead of `CLAMP_ZERO` in case of
       * invalid usage is unimportant).
       */
      descriptor.resource[2] =
         S_030008_BASE_ADDRESS_HI(va >> 32) |
         ((uint32_t)(config->sq_resources_fetch_.hw_stride[binding] & 0xFFF) << 8);
      descriptor.resource[4] = app_size_minus_1 + 1;
      terakan_hw_config_sqk_set_resource_vi(&command_writer->hw_config_sqk, resource_index, bo,
                                            &descriptor);
      unbound_resources &= ~BITFIELD_BIT(resource_index);
   }

   u_foreach_bit (resource_index, unbound_resources) {
      terakan_hw_config_sqk_set_resource_vi(&command_writer->hw_config_sqk, resource_index, NULL,
                                            NULL);
   }
}

static void
terakan_app_config_draw_apply_pa_cl_clip_cntl(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;

   if (terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(IA_MULTI_VGT_PARAM_PRE_RASTERIZER_DISCARD_R9XX,
                                                   PA_CL_CLIP_CNTL);
      if (config->ia_multi_vgt_param_pre_rasterizer_discard_r9xx_.from_apply_pa_cl_clip_cntl
             .dx_rasterization_kill != config->pa_cl_clip_cntl_.dx_rasterization_kill) {
         config->ia_multi_vgt_param_pre_rasterizer_discard_r9xx_.from_apply_pa_cl_clip_cntl
            .dx_rasterization_kill = config->pa_cl_clip_cntl_.dx_rasterization_kill;
         terakan_app_config_draw_set_pending(
            config, TERAKAN_APP_CONFIG_DRAW_ENTRY_IA_MULTI_VGT_PARAM_PRE_RASTERIZER_DISCARD_R9XX);
      }
   }

   if (config->pa_cl_clip_cntl_.dx_rasterization_kill) {
      terakan_hw_config_draw_set_pa_cl_clip_cntl(&command_writer->hw_config_draw,
                                                 S_028810_DX_RASTERIZATION_KILL(true));

      /* Disable emission of fragment shader constants modified in `hw_config` outside
       * `terakan_app_config_draw`.
       * Make `SQ_PGM_FRAGMENT` pending so the actual usage is applied when rasterizer discard is
       * disabled later.
       */
      assert(TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FRAGMENT >=
             TERAKAN_APP_CONFIG_DRAW_ENTRIES_POST_RASTERIZER_DISCARD_FIRST);
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FRAGMENT);
      terakan_hw_config_sqk_set_usage_fs(&command_writer->hw_config_sqk, NULL);

      /* Disable CB as pre-rasterization stages in application pipelines don't use UAVs in the
       * driver.
       * Make `CB_COLOR_CONTROL` pending so the actual value is applied when rasterizer discard is
       * disabled later.
       */
      assert(TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_CONTROL >=
             TERAKAN_APP_CONFIG_DRAW_ENTRIES_POST_RASTERIZER_DISCARD_FIRST);
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_CONTROL);
      terakan_hw_config_draw_set_cb_color_control(
         &command_writer->hw_config_draw,
         S_028808_MODE(V_028808_CB_DISABLE) |
            S_028808_ROP3(TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_COPY));

      /* Other parameters have no observable effect with `DX_RASTERIZATION_KILL`, and the dependent
       * state entries are not applied either.
       * Clipping invocations and primitives pipeline statistics are also not counted by the
       * hardware when it's enabled.
       */
      return;
   }

   uint32_t pa_cl_clip_cntl =
      S_028810_DX_CLIP_SPACE_DEF(config->pa_cl_clip_cntl_.dx_clip_space_def) |
      S_028810_DX_LINEAR_ATTR_CLIP_ENA(true);
   if ((config->pa_cl_clip_cntl_.z_clip_enable_override < 0 &&
        config->pa_cl_clip_cntl_.z_clamp_enable) ||
       config->pa_cl_clip_cntl_.z_clip_enable_override == 0) {
      pa_cl_clip_cntl |= S_028810_ZCLIP_NEAR_DISABLE(true) | S_028810_ZCLIP_FAR_DISABLE(true);
   }
   terakan_hw_config_draw_set_pa_cl_clip_cntl(&command_writer->hw_config_draw, pa_cl_clip_cntl);

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(PA_VPORT, PA_CL_CLIP_CNTL);
   if (config->pa_vport_.from_apply_pa_cl_clip_cntl.dx_clip_space_def !=
          config->pa_cl_clip_cntl_.dx_clip_space_def ||
       config->pa_vport_.from_apply_pa_cl_clip_cntl.z_clamp_enable !=
          config->pa_cl_clip_cntl_.z_clamp_enable) {
      config->pa_vport_.from_apply_pa_cl_clip_cntl.dx_clip_space_def =
         config->pa_cl_clip_cntl_.dx_clip_space_def;
      config->pa_vport_.from_apply_pa_cl_clip_cntl.z_clamp_enable =
         config->pa_cl_clip_cntl_.z_clamp_enable;
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_VPORT);
   }
}

static void
terakan_app_config_draw_apply_ia_multi_vgt_param_pre_rasterizer_discard_r9xx(
   struct terakan_gfx_command_writer * const command_writer)
{
   if (!terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      return;
   }
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;
   if (config->ia_multi_vgt_param_pre_rasterizer_discard_r9xx_.from_apply_pa_cl_clip_cntl
          .dx_rasterization_kill) {
      terakan_hw_config_draw_set_ia_multi_vgt_param(
         &command_writer->hw_config_draw,
         config->ia_multi_vgt_param_pre_rasterizer_discard_r9xx_.from_apply_sq_pgm_pre_rasterization
            .ia_multi_vgt_param);
   } else {
      TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(IA_MULTI_VGT_PARAM_POST_RASTERIZER_DISCARD_R9XX,
                                                   IA_MULTI_VGT_PARAM_PRE_RASTERIZER_DISCARD_R9XX);
      terakan_app_config_draw_set_pending(
         config, TERAKAN_APP_CONFIG_DRAW_ENTRY_IA_MULTI_VGT_PARAM_POST_RASTERIZER_DISCARD_R9XX);
   }
}

static void
terakan_app_config_draw_apply_pa_vport(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const app_config = &command_writer->app_config_draw;
   struct terakan_hw_config_draw * const hw_config = &command_writer->hw_config_draw;

   assert(app_config->pa_vport_.vport_count <= TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);

   /* TODO(Triang3l): Skip applying viewports 1... except for the guard band if the hardware vertex
    * shader doesn't write a viewport ID.
    */
   uint8_t const vport_addressable_count = TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT;
   uint8_t const vport_needed_count =
      MIN2(app_config->pa_vport_.vport_count, vport_addressable_count);

   /* Scissor.
    * Unused viewports receive an empty scissor to avoid indeterminate rendering results if the
    * viewport index specified by the shader is out of bounds.
    */
   bool const is_r9xx =
      terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx;
   terakan_hw_config_draw_set_pa_sc_vport_scissor_needed_count(hw_config, vport_addressable_count);
   for (unsigned vport_index = 0; vport_index < vport_needed_count; ++vport_index) {
      /* Render area scissoring is not necessary, but done for more consistent rendering results,
       * similarly to how it's used in other Mesa Vulkan drivers.
       * Using `VPORT_SCISSOR`, not `WINDOW_SCISSOR` or `GENERIC_SCISSOR`, for the render area to
       * properly apply the hardware bug workarounds to the actual scissor rectangle that will be
       * used in the end.
       */
      struct terakan_screen_rect scissor =
         terakan_screen_rect_intersect(app_config->pa_vport_.vports[vport_index].implicit_scissor,
                                       app_config->pa_vport_.render_area);
      if (vport_index < app_config->pa_vport_.explicit_scissor_count) {
         scissor = terakan_screen_rect_intersect(
            app_config->pa_vport_.explicit_scissors[vport_index], scissor);
      }
      if (unlikely(terakan_screen_rect_is_empty(scissor))) {
         /* Apply the hardware bug workaround described in the
          * `TERAKAN_HW_CONFIG_DRAW_PA_SC_SCISSOR_EMPTY_TL` comment.
          */
         terakan_hw_config_draw_set_pa_sc_vport_scissor(
            hw_config, vport_index, TERAKAN_HW_CONFIG_DRAW_PA_SC_SCISSOR_EMPTY_TL(true),
            TERAKAN_HW_CONFIG_DRAW_PA_SC_SCISSOR_EMPTY_BR);
         continue;
      }
      /* On R9xx, (0, 0) to (1, 1) scissor is treated as empty due to a hardware bug
       * (#0To1ScissorOnR9xx #hashtag).
       */
      /* TODO(Triang3l): Implement a correct workaround using a 2x1 scissor and a clip rectangle. In
       * the worst case, where there are both viewports with (0, 0) to (1, 1) scissors and viewports
       * where the top-left pixel may be covered, duplicate the draw with and without the pixel
       * (1, 0) discarded using a clip rectangle, with only subsets of the viewports having
       * non-empty scissors in each of the two draws. It's okay if geometry pipeline statistics are
       * counted twice as that also may happen on tile-based devices, and
       * `vertexPipelineStoresAndAtomics` is not supported.
       */
      if (is_r9xx && scissor.bounds[1][0] == 1 && scissor.bounds[1][1] == 1) {
         scissor.bounds[1][0] = 2;
      }
      /* TODO(Triang3l): Research the R9xx 1x1 scissor hardware bug (reported to be handled as
       * empty - #0To1ScissorOnR9xx #hashtag.
       */
      terakan_hw_config_draw_set_pa_sc_vport_scissor(
         hw_config, vport_index,
         S_028250_TL_X(scissor.bounds[0][0]) | S_028250_TL_Y(scissor.bounds[0][1]) |
            S_028250_WINDOW_OFFSET_DISABLE(true),
         S_028254_BR_X(scissor.bounds[1][0]) | S_028254_BR_Y(scissor.bounds[1][1]));
   }
   for (unsigned vport_index = vport_needed_count; vport_index < vport_addressable_count;
        ++vport_index) {
      terakan_hw_config_draw_set_pa_sc_vport_scissor(
         hw_config, vport_index, TERAKAN_HW_CONFIG_DRAW_PA_SC_SCISSOR_EMPTY_TL(true),
         TERAKAN_HW_CONFIG_DRAW_PA_SC_SCISSOR_EMPTY_BR);
   }

   /* Z clamping range.
    * When not using an unrestricted depth range, always clamp to [0, 1] to implement the
    * `depthClampZeroOne` Vulkan feature and to avoid the performance impact of disabling clamping.
    */
   bool disable_viewport_z_clamp = false;
   if (app_config->pa_vport_.from_apply_pa_cl_clip_cntl.z_clamp_enable) {
      if (app_config->pa_vport_.user_defined_zmin_zmax_enable) {
         float zmin_zmax[] = {
            app_config->pa_vport_.user_defined_zmin_zmax[0],
            app_config->pa_vport_.user_defined_zmin_zmax[1],
         };
         if (!app_config->pa_vport_.z_range_unrestricted) {
            for (unsigned bound = 0; bound <= 1; ++bound) {
               zmin_zmax[bound] = fmaxf(zmin_zmax[bound], 0.0f);
               zmin_zmax[bound] = MIN2(zmin_zmax[bound], 1.0f);
            }
         }
         for (unsigned vport_index = 0; vport_index < vport_needed_count; ++vport_index) {
            terakan_hw_config_draw_set_pa_sc_vport_zmin_zmax(hw_config, vport_index, zmin_zmax[0],
                                                             zmin_zmax[1]);
         }
      } else {
         for (unsigned vport_index = 0; vport_index < vport_needed_count; ++vport_index) {
            float const * const z_near_far = app_config->pa_vport_.vports[vport_index].z_near_far;
            float zmin_zmax[] = {
               fminf(z_near_far[0], z_near_far[1]),
               fmaxf(z_near_far[0], z_near_far[1]),
            };
            if (!app_config->pa_vport_.z_range_unrestricted) {
               for (unsigned bound = 0; bound <= 1; ++bound) {
                  zmin_zmax[bound] = fmaxf(zmin_zmax[bound], 0.0f);
                  zmin_zmax[bound] = MIN2(zmin_zmax[bound], 1.0f);
               }
            }
            terakan_hw_config_draw_set_pa_sc_vport_zmin_zmax(hw_config, vport_index, zmin_zmax[0],
                                                             zmin_zmax[1]);
         }
      }
   } else {
      if (app_config->pa_vport_.z_range_unrestricted) {
         disable_viewport_z_clamp = true;
      } else {
         for (unsigned vport_index = 0; vport_index < vport_needed_count; ++vport_index) {
            terakan_hw_config_draw_set_pa_sc_vport_zmin_zmax(hw_config, vport_index, 0.0f, 1.0f);
         }
      }
   }
   terakan_hw_config_draw_set_pa_sc_vport_zmin_zmax_needed_count(
      hw_config, disable_viewport_z_clamp ? 0 : vport_needed_count);
   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(DB_RENDER_OVERRIDE, PA_VPORT);
   if (app_config->db_render_override_.from_apply_pa_vport.disable_viewport_clamp !=
       disable_viewport_z_clamp) {
      app_config->db_render_override_.from_apply_pa_vport.disable_viewport_clamp =
         disable_viewport_z_clamp;
      terakan_app_config_draw_set_pending(app_config,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE);
   }

   /* Transform. */
   terakan_hw_config_draw_set_pa_cl_vport_scale_offset_needed_count(hw_config, vport_needed_count);
   if (app_config->pa_vport_.from_apply_pa_cl_clip_cntl.dx_clip_space_def) {
      for (unsigned vport_index = 0; vport_index < vport_needed_count; ++vport_index) {
         struct terakan_app_config_draw_pa_vport const * const vport =
            &app_config->pa_vport_.vports[vport_index];
         terakan_hw_config_draw_set_pa_cl_vport_scale_offset(
            hw_config, vport_index, vport->xy_scale_offset[0],
            vport->z_near_far[1] - vport->z_near_far[0], vport->z_near_far[0]);
      }
   } else {
      for (unsigned vport_index = 0; vport_index < vport_needed_count; ++vport_index) {
         struct terakan_app_config_draw_pa_vport const * const vport =
            &app_config->pa_vport_.vports[vport_index];
         terakan_hw_config_draw_set_pa_cl_vport_scale_offset(
            hw_config, vport_index, vport->xy_scale_offset[0],
            (vport->z_near_far[1] - vport->z_near_far[0]) * 0.5f,
            (vport->z_near_far[1] + vport->z_near_far[0]) * 0.5f);
      }
   }

   /* Guard band. */
   float gb_vert_horz_clip_adj[2] = {FLT_MAX, FLT_MAX};
   for (unsigned vport_index = 0; vport_index < vport_needed_count; ++vport_index) {
      float const * const vport_gb_vert_horz_clip_adj =
         app_config->pa_vport_.vports[vport_index].gb_vert_horz_clip_adj;
      for (unsigned vert_horz = 0; vert_horz <= 1; ++vert_horz) {
         gb_vert_horz_clip_adj[vert_horz] =
            fminf(vport_gb_vert_horz_clip_adj[vert_horz], gb_vert_horz_clip_adj[vert_horz]);
      }
   }
   terakan_hw_config_draw_set_pa_cl_gb_clip_adj(hw_config, gb_vert_horz_clip_adj[0],
                                                gb_vert_horz_clip_adj[1]);
   /* TODO(Triang3l): Primitive discard guard band for points and lines (as a separate configuration
    * entry).
    */
   terakan_hw_config_draw_set_pa_cl_gb_disc_adj(hw_config, 1.0f, 1.0f);
}

static void
terakan_app_config_draw_apply_pa_su_sc_mode_cntl(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;

   /* Many fields of this register don't apply to non-polygonal primitives, but not checking for
    * simplicity, especially because that depends not solely on `VGT_PRIMITIVE_TYPE`, but also on
    * tessellation and geometry shader output topology.
    */

   terakan_hw_config_draw_set_pa_su_sc_mode_cntl(&command_writer->hw_config_draw,
                                                 config->pa_su_sc_mode_cntl_);

   bool const poly_offset_enable =
      (config->pa_su_sc_mode_cntl_ &
       ~(uint32_t)(C_028814_POLY_OFFSET_FRONT_ENABLE & C_028814_POLY_OFFSET_BACK_ENABLE &
                   C_028814_POLY_OFFSET_PARA_ENABLE)) != 0;

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(PA_SU_POLY_OFFSET, PA_SU_SC_MODE_CNTL);
   if (config->pa_su_poly_offset_.from_apply_pa_su_sc_mode_cntl.poly_offset_enable !=
       poly_offset_enable) {
      config->pa_su_poly_offset_.from_apply_pa_su_sc_mode_cntl.poly_offset_enable =
         poly_offset_enable;
      if (poly_offset_enable) {
         terakan_app_config_draw_set_pending(config,
                                             TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET);
      }
   }

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(PA_SU_POLY_OFFSET_DB_FMT_CNTL, PA_SU_SC_MODE_CNTL);
   if (config->pa_su_poly_offset_db_fmt_cntl_.from_apply_pa_su_sc_mode_cntl.poly_offset_enable !=
       poly_offset_enable) {
      config->pa_su_poly_offset_db_fmt_cntl_.from_apply_pa_su_sc_mode_cntl.poly_offset_enable =
         poly_offset_enable;
      if (poly_offset_enable) {
         terakan_app_config_draw_set_pending(
            config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET_DB_FMT_CNTL);
      }
   }
}

static void
terakan_app_config_draw_apply_pa_cl_vte_cntl(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_set_pa_cl_vte_cntl(&command_writer->hw_config_draw,
                                             TERAKAN_HW_CONFIG_DRAW_PA_CL_VTE_CNTL_3D);
}

static void
terakan_app_config_draw_apply_pa_sc_line_stipple(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(PA_SC_MODE_CNTL_0, PA_SC_LINE_STIPPLE);
   if (config->pa_sc_mode_cntl_0_.from_apply_pa_sc_line_stipple.line_stipple_enable !=
       config->pa_sc_line_stipple_.enable) {
      config->pa_sc_mode_cntl_0_.from_apply_pa_sc_line_stipple.line_stipple_enable =
         config->pa_sc_line_stipple_.enable;
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_MODE_CNTL_0);
   }

   if (terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(IA_MULTI_VGT_PARAM_POST_RASTERIZER_DISCARD_R9XX,
                                                   PA_SC_LINE_STIPPLE);
      if (config->ia_multi_vgt_param_post_rasterizer_discard_r9xx_.from_apply_pa_sc_line_stipple
             .line_stipple_enable != config->pa_sc_line_stipple_.enable) {
         config->ia_multi_vgt_param_post_rasterizer_discard_r9xx_.from_apply_pa_sc_line_stipple
            .line_stipple_enable = config->pa_sc_line_stipple_.enable;
         terakan_app_config_draw_set_pending(
            config, TERAKAN_APP_CONFIG_DRAW_ENTRY_IA_MULTI_VGT_PARAM_POST_RASTERIZER_DISCARD_R9XX);
      }
   }

   if (!config->pa_sc_line_stipple_.enable) {
      return;
   }

   /* Geometry shaders output strips. */
   terakan_hw_config_draw_set_pa_sc_line_stipple(
      &command_writer->hw_config_draw,
      config->pa_sc_line_stipple_.pattern |
         (!config->pa_sc_line_stipple_.from_apply_sq_pgm_pre_rasterization.geometry_shader_enable &&
                config->pa_sc_line_stipple_.from_apply_vgt_primitive_type.per_primitive_reset
             ? 1
             : 2));
}

static void
terakan_app_config_draw_apply_pa_su_poly_offset(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw const * const config = &command_writer->app_config_draw;
   if (!config->pa_su_poly_offset_.from_apply_pa_su_sc_mode_cntl.poly_offset_enable) {
      return;
   }
   terakan_hw_config_draw_set_pa_su_poly_offset(&command_writer->hw_config_draw,
                                                config->pa_su_poly_offset_.poly_offset);
}

static void
terakan_app_config_draw_apply_pa_sc_aa_config_sample_locs(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;

   unsigned const sample_count_log2 = config->pa_sc_aa_config_sample_locs_.msaa_num_samples_log2;
   assert(sample_count_log2 <=
          (terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx ? 4 : 3));

   uint8_t const * sample_locs;
   uint8_t max_sample_dist;
   if (config->pa_sc_aa_config_sample_locs_.custom_sample_locs_enable) {
      sample_locs = config->pa_sc_aa_config_sample_locs_.custom_sample_locs[0];
      max_sample_dist = 0;
      for (unsigned sample_loc_index = 0; sample_loc_index < 4u << sample_count_log2;
           ++sample_loc_index) {
         uint8_t const current_sample_max_dist =
            terakan_hw_config_draw_pa_sc_aa_sample_max_dist(sample_locs[sample_loc_index]);
         max_sample_dist = MAX2(current_sample_max_dist, max_sample_dist);
      }
   } else {
      sample_locs = terakan_hw_config_draw_pa_sc_aa_standard_sample_locs[sample_count_log2][0];
      max_sample_dist =
         terakan_hw_config_draw_pa_sc_aa_standard_max_sample_dists[sample_count_log2];
   }

   terakan_hw_config_draw_set_pa_sc_aa_config_sample_locs(
      &command_writer->hw_config_draw,
      S_028BE0_MSAA_NUM_SAMPLES(sample_count_log2) | S_028BE0_MAX_SAMPLE_DIST(max_sample_dist) |
         S_028BE0_MSAA_EXPOSED_SAMPLES(sample_count_log2),
      sample_locs);

   /* Update other entries that depend on the sample count.
    * This entry can be made pending even when the sample locations are modified without changing
    * the sample count, but that's expected to be done rarely, and mainly roughly at render pass
    * boundaries, because HTile depends on the sample locations too.
    */
   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(PA_SC_AA_MASK, PA_SC_AA_CONFIG_SAMPLE_LOCS);
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_AA_MASK);
   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(DB_COUNT_CONTROL, PA_SC_AA_CONFIG_SAMPLE_LOCS);
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_COUNT_CONTROL);
   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(DB_DEPTH_STENCIL_BUFFER,
                                                PA_SC_AA_CONFIG_SAMPLE_LOCS);
   terakan_app_config_draw_set_pending(config,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_BUFFER);
   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(DB_EQAA, PA_SC_AA_CONFIG_SAMPLE_LOCS);
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_EQAA);

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(PA_SC_MODE_CNTL_0, PA_SC_AA_CONFIG_SAMPLE_LOCS);
   /* Enable MSAA if using multiple samples, or single sample with custom sample locations. */
   bool const pa_sc_mode_cntl_0_msaa_enable = sample_count_log2 != 0 || max_sample_dist != 0;
   if (config->pa_sc_mode_cntl_0_.from_apply_pa_sc_aa_config_sample_locs.msaa_enable !=
       pa_sc_mode_cntl_0_msaa_enable) {
      config->pa_sc_mode_cntl_0_.from_apply_pa_sc_aa_config_sample_locs.msaa_enable =
         pa_sc_mode_cntl_0_msaa_enable;
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_MODE_CNTL_0);
   }
}

static void
terakan_app_config_draw_apply_pa_sc_aa_mask(struct terakan_gfx_command_writer * const command_writer)
{
   /* Exclude unused samples. Using a type wider than 16 bits for the shift for the 16-sample case.
    */
   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(PA_SC_AA_MASK, PA_SC_AA_CONFIG_SAMPLE_LOCS);
   terakan_hw_config_draw_set_pa_sc_aa_mask(
      &command_writer->hw_config_draw,
      command_writer->app_config_draw.pa_sc_aa_mask_ &
         (((uint32_t)1 << (1u << command_writer->app_config_draw.pa_sc_aa_config_sample_locs_
                                    .msaa_num_samples_log2)) -
          1));
}

static void
terakan_app_config_draw_apply_pa_sc_mode_cntl_0(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_set_pa_sc_mode_cntl_0(
      &command_writer->hw_config_draw,
      S_028A48_MSAA_ENABLE(command_writer->app_config_draw.pa_sc_mode_cntl_0_
                              .from_apply_pa_sc_aa_config_sample_locs.msaa_enable) |
         S_028A48_VPORT_SCISSOR_ENABLE(1) |
         S_028A48_LINE_STIPPLE_ENABLE(command_writer->app_config_draw.pa_sc_mode_cntl_0_
                                         .from_apply_pa_sc_line_stipple.line_stipple_enable));
}

static void
terakan_app_config_draw_apply_ia_multi_vgt_param_post_rasterizer_discard_r9xx(
   struct terakan_gfx_command_writer * const command_writer)
{
   if (!terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      return;
   }

   struct terakan_app_config_draw const * const config = &command_writer->app_config_draw;

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(IA_MULTI_VGT_PARAM_POST_RASTERIZER_DISCARD_R9XX,
                                                IA_MULTI_VGT_PARAM_PRE_RASTERIZER_DISCARD_R9XX);
   uint32_t ia_multi_vgt_param = config->ia_multi_vgt_param_pre_rasterizer_discard_r9xx_
                                    .from_apply_sq_pgm_pre_rasterization.ia_multi_vgt_param;

   if (config->ia_multi_vgt_param_post_rasterizer_discard_r9xx_.from_apply_pa_sc_line_stipple
          .line_stipple_enable) {
      ia_multi_vgt_param |= S_028AA8_SWITCH_ON_EOP(true);
   }

   terakan_hw_config_draw_set_ia_multi_vgt_param(&command_writer->hw_config_draw,
                                                 ia_multi_vgt_param);
}

static void
terakan_app_config_draw_apply_sq_pgm_fragment(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;
   struct terakan_shader_impl const * const fs = config->sq_pgm_fragment_;

   terakan_hw_config_draw_set_sq_pgm_ps(&command_writer->hw_config_draw,
                                        fs != NULL ? &fs->static_state : NULL);

   if (fs != NULL && fs->scratch_item_size_dwords != 0) {
      terakan_hw_config_shared_draw_set_sq_ring_usage(
         &command_writer->hw_config_shared, BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_PSTMP),
         BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_PSTMP));
      terakan_hw_config_draw_set_sq_ring_itemsize_dwords(&command_writer->hw_config_draw,
                                                         TERAKAN_SHADER_RING_INDEX_PSTMP,
                                                         fs->scratch_item_size_dwords);
      uint32_t const pstmp_ring_bytes_needed_shr8 =
         fs->scratch_item_size_dwords *
         (uint32_t)terakan_gfx_command_writer_physical_device(command_writer)
            ->chip_info.sq_pstmp_ring_bytes_per_item_dword_shr8;
      uint32_t * const command_buffer_pstmp_ring_bytes_needed_shr8 =
         &command_writer->base.command_buffer
             ->shader_ring_bytes_needed_for_se_shr8[TERAKAN_SHADER_RING_INDEX_PSTMP];
      *command_buffer_pstmp_ring_bytes_needed_shr8 =
         MAX2(pstmp_ring_bytes_needed_shr8, *command_buffer_pstmp_ring_bytes_needed_shr8);
   } else {
      terakan_hw_config_shared_draw_set_sq_ring_usage(
         &command_writer->hw_config_shared, BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_PSTMP), 0b0);
      terakan_hw_config_draw_set_sq_ring_itemsize_dwords(&command_writer->hw_config_draw,
                                                         TERAKAN_SHADER_RING_INDEX_PSTMP, 0);
   }

   command_writer->push_constants_state.usage_fragment =
      fs != NULL ? fs->push_constants_usage : (struct terakan_push_constants_usage){};
   if (terakan_push_constants_usage_empty(command_writer->push_constants_state.usage_fragment)) {
      command_writer->push_constants_state.graphics_stages_using_push_constants &=
         ~VK_SHADER_STAGE_FRAGMENT_BIT;
   } else {
      command_writer->push_constants_state.graphics_stages_using_push_constants |=
         VK_SHADER_STAGE_FRAGMENT_BIT;
   }

   terakan_hw_config_sqk_set_usage_fs(&command_writer->hw_config_sqk,
                                      fs != NULL ? &fs->sqk_usage : NULL);

   uint8_t const rtv_dsb_uncompacted_exports =
      fs != NULL ? fs->fs.rtv_dsb_uncompacted_exports : 0b0;

   /* TODO(Triang3l): Update `ps_iter_full_sample_shading`. */

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(CB_COLOR_RTV_AND_BLEND_CONTROL, SQ_PGM_FRAGMENT);
   if (config->cb_color_rtv_and_blend_control_.from_apply_sq_pgm_fragment
          .rtv_dsb_uncompacted_exports != rtv_dsb_uncompacted_exports) {
      config->cb_color_rtv_and_blend_control_.from_apply_sq_pgm_fragment
         .rtv_dsb_uncompacted_exports = rtv_dsb_uncompacted_exports;
      terakan_app_config_draw_set_pending(
         config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL);
   }

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(CB_COLOR_UAV_AND_UNUSED_MRT, SQ_PGM_FRAGMENT);
   uint8_t const rtv_dsb_export_count = util_bitcount(rtv_dsb_uncompacted_exports);
   static BITSET_WORD const
      uav_empty_bitset[BITSET_WORDS(TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL)];
   BITSET_WORD const * const uav_used =
      fs != NULL ? fs->uavs_for_mutable_resources_needed : uav_empty_bitset;
   size_t const uav_bitset_size =
      sizeof(BITSET_WORD) * BITSET_WORDS(TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL);
   if (config->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.rtv_dsb_export_count !=
          rtv_dsb_export_count ||
       memcmp(config->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.uav_used, uav_used,
              uav_bitset_size) != 0) {
      config->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.rtv_dsb_export_count =
         rtv_dsb_export_count;
      memcpy(config->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.uav_used, uav_used,
             uav_bitset_size);
      terakan_app_config_draw_set_pending(
         config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_UAV_AND_UNUSED_MRT);
   }

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(DB_SHADER_CONTROL, SQ_PGM_FRAGMENT);
   uint32_t const db_shader_control =
      fs != NULL ? fs->fs.db_shader_control : TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY;
   if (config->db_shader_control_.from_apply_sq_pgm_fragment.db_shader_control !=
       db_shader_control) {
      config->db_shader_control_.from_apply_sq_pgm_fragment.db_shader_control = db_shader_control;
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_SHADER_CONTROL);
   }
}

static void
terakan_app_config_draw_apply_db_count_control(
   struct terakan_gfx_command_writer * const command_writer)
{
   if (!command_writer->app_config_draw.db_count_control_.zpass_query_active_count) {
      terakan_hw_config_draw_set_db_count_control(&command_writer->hw_config_draw,
                                                  S_028004_ZPASS_INCREMENT_DISABLE(true));
      return;
   }

   /* Even if all active occlusion queries aren't precise, enable perfect Z pass counts, because
    * otherwise samples may not be counted in depth-only render passes with depth writing
    * disabled (see Gallium RadeonSI and PAL), or may be counted even if they're discarded as a
    * result of the fragment shader (see https://gitlab.freedesktop.org/mesa/mesa/-/issues/3218).
    */
   uint32_t db_count_control = S_028004_PERFECT_ZPASS_COUNTS(true);
   if (terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(DB_COUNT_CONTROL, PA_SC_AA_CONFIG_SAMPLE_LOCS);
      db_count_control |= S_028004_SAMPLE_RATE(
         command_writer->app_config_draw.pa_sc_aa_config_sample_locs_.msaa_num_samples_log2);
   }
   terakan_hw_config_draw_set_db_count_control(&command_writer->hw_config_draw, db_count_control);
}

static void
terakan_app_config_draw_apply_db_depth_stencil_buffer(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;

   bool const is_r9xx =
      terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx;

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(DB_DEPTH_STENCIL_BUFFER,
                                                PA_SC_AA_CONFIG_SAMPLE_LOCS);
   uint8_t const msaa_num_samples_log2 = config->pa_sc_aa_config_sample_locs_.msaa_num_samples_log2;

   bool decompress_z_on_flush = false;
   uint8_t anchor_samples_log2 = msaa_num_samples_log2;

   bool depth_bound, stencil_bound;
   terakan_depth_stencil_descriptor_is_bound(config->db_depth_stencil_buffer_.bo,
                                             &config->db_depth_stencil_buffer_.descriptor,
                                             &depth_bound, &stencil_bound);
   if (depth_bound || stencil_bound) {
      /* #MemoryIntegrity: Don't allow depth / stencil targets not compatible with the current
       * rasterization sample count. Prior to R9xx, `PA_SC_AA_CONFIG.MSAA_NUM_SAMPLES` also directly
       * specifies the depth / stencil and color target sample count. Expecting that the descriptor
       * inside the driver has the sample count specified regardless of the architecture generation.
       */
      unsigned const depth_stencil_num_samples_log2 =
         G_028040_NUM_SAMPLES(config->db_depth_stencil_buffer_.descriptor.z_info);
      if (is_r9xx ? depth_stencil_num_samples_log2 > msaa_num_samples_log2
                  : depth_stencil_num_samples_log2 != msaa_num_samples_log2) {
         depth_bound = false;
         stencil_bound = false;
      } else {
         anchor_samples_log2 = depth_stencil_num_samples_log2;
         /* According to the Cayman 3D Register Reference Guide. */
         decompress_z_on_flush = depth_stencil_num_samples_log2 > 1;
      }
   }

   if (depth_bound || stencil_bound) {
      terakan_hw_config_draw_set_db_depth_stencil_buffer(
         &command_writer->hw_config_draw, config->db_depth_stencil_buffer_.bo,
         &config->db_depth_stencil_buffer_.descriptor);
   } else {
      terakan_hw_config_draw_set_db_depth_stencil_buffer(&command_writer->hw_config_draw, NULL,
                                                         NULL);
   }

   if (is_r9xx) {
      TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(DB_RENDER_OVERRIDE2, DB_DEPTH_STENCIL_BUFFER);
      if (config->db_render_override2_.from_apply_db_depth_stencil_buffer
             .decompress_z_on_flush_r9xx != decompress_z_on_flush) {
         config->db_render_override2_.from_apply_db_depth_stencil_buffer.decompress_z_on_flush_r9xx =
            decompress_z_on_flush;
         terakan_app_config_draw_set_pending(config,
                                             TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE2);
      }

      TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(DB_EQAA, DB_DEPTH_STENCIL_BUFFER);
      if (config->db_eqaa_.from_apply_db_depth_stencil_buffer.max_anchor_samples_log2_r9xx !=
          anchor_samples_log2) {
         config->db_eqaa_.from_apply_db_depth_stencil_buffer.max_anchor_samples_log2_r9xx =
            anchor_samples_log2;
         terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_EQAA);
      }
   }

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(DB_DEPTH_STENCIL_CONTROL_REF_MASK,
                                                DB_DEPTH_STENCIL_BUFFER);
   if (config->db_depth_stencil_control_ref_mask_.from_apply_db_depth_stencil_buffer.depth_bound !=
          depth_bound ||
       config->db_depth_stencil_control_ref_mask_.from_apply_db_depth_stencil_buffer.stencil_bound !=
          stencil_bound) {
      config->db_depth_stencil_control_ref_mask_.from_apply_db_depth_stencil_buffer.depth_bound =
         depth_bound;
      config->db_depth_stencil_control_ref_mask_.from_apply_db_depth_stencil_buffer.stencil_bound =
         stencil_bound;
      terakan_app_config_draw_set_pending(
         config, TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_CONTROL_REF_MASK);
   }

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(PA_SU_POLY_OFFSET_DB_FMT_CNTL,
                                                DB_DEPTH_STENCIL_BUFFER);
   /* Treating an unbound depth buffer like 32-bit floating point in polygon offset calculations to
    * match the precision of `gl_FragCoord.z` in fragment shaders.
    */
   uint32_t const poly_offset_depth_format =
      depth_bound ? G_028040_FORMAT(config->db_depth_stencil_buffer_.descriptor.z_info)
                  : V_028040_Z_32_FLOAT;
   if (config->pa_su_poly_offset_db_fmt_cntl_.from_apply_db_depth_stencil_buffer.depth_format !=
       poly_offset_depth_format) {
      config->pa_su_poly_offset_db_fmt_cntl_.from_apply_db_depth_stencil_buffer.depth_format =
         poly_offset_depth_format;
      terakan_app_config_draw_set_pending(
         config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET_DB_FMT_CNTL);
   }

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(CB_COLOR_RTV_AND_BLEND_CONTROL,
                                                DB_DEPTH_STENCIL_BUFFER);
   if (config->cb_color_rtv_and_blend_control_.from_apply_db_depth_stencil_buffer
          .sample_count_limit_log2 != anchor_samples_log2) {
      config->cb_color_rtv_and_blend_control_.from_apply_db_depth_stencil_buffer
         .sample_count_limit_log2 = anchor_samples_log2;
      terakan_app_config_draw_set_pending(
         config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL);
   }
}

static void
terakan_app_config_draw_apply_db_render_override(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_set_db_render_override(
      &command_writer->hw_config_draw,
      S_02800C_DISABLE_VIEWPORT_CLAMP(command_writer->app_config_draw.db_render_override_
                                         .from_apply_pa_vport.disable_viewport_clamp));
}

static void
terakan_app_config_draw_apply_db_render_override2(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t db_render_override2 = 0;
   if (terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      /* By setting `DISABLE_COLOR_ON_VALIDATION` to 1, prevent disabling of the fragment shader in
       * UAV-only rendering cases, because `CB_TARGET_MASK` bits are not set for UAVs (and it
       * doesn't have storage for bits for UAVs starting from 8). CB is toggled explicitly and
       * precisely via `CB_COLOR_CONTROL` in the driver, so the optimization isn't needed.
       */
      db_render_override2 |= S_028010_DISABLE_COLOR_ON_VALIDATION(true) |
                             S_028010_DECOMPRESS_Z_ON_FLUSH(
                                command_writer->app_config_draw.db_render_override2_
                                   .from_apply_db_depth_stencil_buffer.decompress_z_on_flush_r9xx);
   }
   terakan_hw_config_draw_set_db_render_override2(&command_writer->hw_config_draw,
                                                  db_render_override2);
}

static void
terakan_app_config_draw_apply_db_depth_stencil_control_ref_mask(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw const * const config = &command_writer->app_config_draw;

   uint32_t depth_stencil_control =
      config->db_depth_stencil_control_ref_mask_.depth_stencil_control;

   if (!config->db_depth_stencil_control_ref_mask_.from_apply_db_depth_stencil_buffer.depth_bound) {
      depth_stencil_control &= C_028800_Z_ENABLE;
   }
   if (!config->db_depth_stencil_control_ref_mask_.from_apply_db_depth_stencil_buffer.stencil_bound) {
      depth_stencil_control &= C_028800_STENCIL_ENABLE;
   }

   /* Avoid emitting irrelevant state changes. */

   /* TODO(Triang3l): Perform optimizations similar to vk_optimize_depth_stencil_state, possibly
    * based on the depth / stencil test configuration, which aspects are currently bound, and maybe
    * the face culling configuration (although that's more difficult because of multiple places
    * where the primitive type may be specified).
    */

   if (!G_028800_Z_ENABLE(depth_stencil_control)) {
      depth_stencil_control &= C_028800_Z_WRITE_ENABLE & C_028800_ZFUNC;
   }

   if (G_028800_STENCIL_ENABLE(depth_stencil_control)) {
      terakan_hw_config_draw_set_db_stencilrefmask(
         &command_writer->hw_config_draw, false,
         command_writer->app_config_draw.db_depth_stencil_control_ref_mask_.stencil_ref_mask_front);
      if (G_028800_BACKFACE_ENABLE(depth_stencil_control)) {
         terakan_hw_config_draw_set_db_stencilrefmask(
            &command_writer->hw_config_draw, true,
            command_writer->app_config_draw.db_depth_stencil_control_ref_mask_
               .stencil_ref_mask_back);
      } else {
         depth_stencil_control &= C_028800_BACKFACE_ENABLE & C_028800_STENCILFUNC_BF &
                                  C_028800_STENCILFAIL_BF & C_028800_STENCILZPASS_BF &
                                  C_028800_STENCILZFAIL_BF;
      }
   } else {
      depth_stencil_control &=
         C_028800_BACKFACE_ENABLE & C_028800_STENCILFUNC & C_028800_STENCILFAIL &
         C_028800_STENCILZPASS & C_028800_STENCILZFAIL & C_028800_STENCILFUNC_BF &
         C_028800_STENCILFAIL_BF & C_028800_STENCILZPASS_BF & C_028800_STENCILZFAIL_BF;
   }

   terakan_hw_config_draw_set_db_depth_control(&command_writer->hw_config_draw,
                                               depth_stencil_control);
}

static void
terakan_app_config_draw_apply_db_eqaa(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;

   bool const is_r9xx =
      terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx;

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(DB_EQAA, PA_SC_AA_CONFIG_SAMPLE_LOCS);
   uint8_t const msaa_num_samples_log2 = config->pa_sc_aa_config_sample_locs_.msaa_num_samples_log2;

   uint8_t fragments_log2 = msaa_num_samples_log2;
   if (is_r9xx) {
      /* Section "Sample Shading" of the Vulkan 1.4.352 specification says:
       *
       *     "If the `VK_AMD_mixed_attachment_samples` extension is enabled and the subpass uses
       *     color attachments, the `samples` value used to create each color attachment is used
       *     instead of `rasterizationSamples`."
       */
      fragments_log2 = MIN2(fragments_log2, config->db_eqaa_.ps_iter_least_fragments_log2_r9xx);
   }
   uint8_t ps_iter_samples_log2 = 0;
   if (fragments_log2 != 0) {
      if (config->db_eqaa_.from_apply_sq_pgm_fragment.ps_iter_full_sample_shading) {
         ps_iter_samples_log2 = fragments_log2;
      } else if (config->db_eqaa_.ps_iter_max_invocation_samples_log2 >= 0) {
         /* The clamping ensures at least 2 samples per invocation when `minSampleShading` is
          * greater than 0.0f.
          */
         ps_iter_samples_log2 =
            fragments_log2 - MIN2((uint8_t)config->db_eqaa_.ps_iter_max_invocation_samples_log2,
                                  fragments_log2 - 1u);
      }
   }

   terakan_hw_config_draw_set_pa_sc_mode_cntl_1(
      &command_writer->hw_config_draw, TERAKAN_HW_CONFIG_DRAW_PA_SC_MODE_CNTL_1_CONSTANT |
                                          EG_S_028A4C_PS_ITER_SAMPLE(ps_iter_samples_log2 != 0));

   if (!is_r9xx) {
      return;
   }

   terakan_hw_config_draw_set_db_eqaa(
      &command_writer->hw_config_draw,
      TERAKAN_HW_CONFIG_DRAW_DB_EQAA_CONSTANT |
         S_028804_MAX_ANCHOR_SAMPLES(
            config->db_eqaa_.from_apply_db_depth_stencil_buffer.max_anchor_samples_log2_r9xx) |
         S_028804_PS_ITER_SAMPLES(ps_iter_samples_log2) |
         S_028804_MASK_EXPORT_NUM_SAMPLES(msaa_num_samples_log2) |
         S_028804_ALPHA_TO_MASK_NUM_SAMPLES(msaa_num_samples_log2));

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(CB_COLOR_RTV_AND_BLEND_CONTROL, DB_EQAA);
   if (config->cb_color_rtv_and_blend_control_.from_apply_db_eqaa.min_fragments_log2_r9xx !=
       ps_iter_samples_log2) {
      config->cb_color_rtv_and_blend_control_.from_apply_db_eqaa.min_fragments_log2_r9xx =
         ps_iter_samples_log2;
      terakan_app_config_draw_set_pending(
         config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL);
   }
}

static void
terakan_app_config_draw_apply_db_alpha_to_mask(
   struct terakan_gfx_command_writer * const command_writer)
{
   /* TODO(Triang3l): Disable alpha to mask if the fragment shader output is missing or integer, and
    * also make sure the `2C_32BPC_GR` export format is not used with alpha to coverage for RTV 0
    * and the dual-source blending target 1 on R9xx.
    *
    * Section "Multisample Coverage" of the Vulkan 1.4.351 specification says:
    *
    *     "All alpha values in this section refer only to the alpha component of the fragment shader
    *     output that has a Location and Index decoration of zero (see the Fragment Output Interface
    *     section). If that shader output has an integer or unsigned integer type, then these
    *     operations are skipped."
    */

   uint32_t const db_alpha_to_mask = command_writer->app_config_draw.db_alpha_to_mask_;
   /* Ignore the offsets if disabled to avoid modifying the hardware register when they're not
    * relevant.
    */
   terakan_hw_config_draw_set_db_alpha_to_mask(
      &command_writer->hw_config_draw,
      G_028B70_ALPHA_TO_MASK_ENABLE(db_alpha_to_mask) ? db_alpha_to_mask : 0);
}

static void
terakan_app_config_draw_apply_pa_su_poly_offset_db_fmt_cntl(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw const * const config = &command_writer->app_config_draw;
   if (!config->pa_su_poly_offset_db_fmt_cntl_.from_apply_pa_su_sc_mode_cntl.poly_offset_enable) {
      return;
   }
   /* Section "Rasterization" of the Vulkan 1.4.334 specification says:
    *
    *     "depthBiasConstantFactor scales the parameter r of the depth attachment
    *
    *     In a pipeline with a depth bias representation of VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT,
    *     r, for the given primitive is defined as
    *
    *        r = 1
    *
    *     Otherwise r is the minimum resolvable difference that depends on the depth attachment
    *     representation. If VkDepthBiasRepresentationInfoEXT::depthBiasExact is VK_FALSE it is the
    *     smallest difference in a sample’s depth z_f values that is guaranteed to remain distinct
    *     throughout polygon rasterization and in the depth attachment. All pairs of fragments
    *     generated by the rasterization of two polygons with otherwise identical vertices, but z_f
    *     values that differ by r, will have distinct depth values.
    *
    *     For fixed-point depth attachment representations, or in a pipeline with a depth bias
    *     representation of VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT,
    *     r is constant throughout the range of the entire depth attachment. If
    *     VkDepthBiasRepresentationInfoEXT::depthBiasExact is VK_TRUE, then its value must be
    *
    *        r = 2^-n
    *
    *     Otherwise its value is implementation-dependent but must be at most
    *
    *        r = 2 × 2^-n
    *
    *     where n is the number of bits used for the depth aspect when using a fixed-point
    *     attachment, or the number of mantissa bits plus one when using a floating-point
    *     attachment.
    *
    *     Otherwise for floating-point depth attachment, there is no single minimum resolvable
    *     difference. In this case, the minimum resolvable difference for a given polygon is
    *     dependent on the maximum exponent, e, in the range of z values spanned by the primitive.
    *     If n is the number of bits in the floating-point mantissa, the minimum resolvable
    *     difference, r, for the given primitive is defined as
    *
    *        r = 2^(e-n)
    *
    *     [...]
    *
    *     If no depth attachment is present, r is undefined."
    *
    * When `DB_IS_FLOAT_FMT` is 0, the hardware interprets `NEG_NUM_DB_BITS` as an exponent bias.
    *
    * If `NEG_NUM_DB_BITS = -n`, the `r` is computed by the hardware as:
    *    r = 2^-n
    * Alternatively, this can be written as:
    *    r = 1 / 2^n
    * This is exactly the `depthBiasExact = VK_TRUE` behavior for a fixed-point representation.
    *
    * However, it's not sufficient to guarantee that polygons with identical vertices, but `z_f`
    * values that differ by `r`, will have distinct depth values. This requires that:
    *    r >= 1 / (2^n - 1)
    *
    * Therefore, for a fixed-point representation, when exact depth bias is not enabled, using:
    *    r = 2 × 2^-n
    * by specifying that `NEG_NUM_DB_BITS = -n + 1`.
    *
    * For `FORCE_UNORM`, the behavior for 32-bit floating-point should match 24-bit fixed-point,
    * whose emulation is one of the problems that `VK_EXT_depth_bias_control` was proposed for
    * solving.
    *
    * Treating an unbound depth buffer like 32-bit floating point, even though in that case `r` is
    * undefined, to match the precision of `gl_FragCoord.z` in fragment shaders.
    */
   uint32_t pa_su_poly_offset_db_fmt_cntl;
   enum terakan_app_config_draw_poly_offset_representation const representation =
      command_writer->app_config_draw.pa_su_poly_offset_db_fmt_cntl_.representation;
   if (representation == TERAKAN_APP_CONFIG_DRAW_POLY_OFFSET_REPRESENTATION_FLOAT) {
      pa_su_poly_offset_db_fmt_cntl = 0;
   } else {
      switch (command_writer->app_config_draw.pa_su_poly_offset_db_fmt_cntl_
                 .from_apply_db_depth_stencil_buffer.depth_format) {
      case TERASCALE_R8XX_DEPTH_FORMAT_16:
         pa_su_poly_offset_db_fmt_cntl = S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-16);
         break;
      case TERASCALE_R8XX_DEPTH_FORMAT_24:
         pa_su_poly_offset_db_fmt_cntl = S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-24);
         break;
      default:
         pa_su_poly_offset_db_fmt_cntl =
            representation == TERAKAN_APP_CONFIG_DRAW_POLY_OFFSET_REPRESENTATION_FORCE_UNORM
               ? S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-24)
               : S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-23) |
                    S_028B78_POLY_OFFSET_DB_IS_FLOAT_FMT(true);
         break;
      }
      if (!G_028B78_POLY_OFFSET_DB_IS_FLOAT_FMT(pa_su_poly_offset_db_fmt_cntl) &&
          !command_writer->app_config_draw.pa_su_poly_offset_db_fmt_cntl_.exact) {
         pa_su_poly_offset_db_fmt_cntl =
            (pa_su_poly_offset_db_fmt_cntl & C_028B78_POLY_OFFSET_NEG_NUM_DB_BITS) |
            S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(
               G_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(pa_su_poly_offset_db_fmt_cntl) + 1);
      }
   }
   terakan_hw_config_draw_set_pa_su_poly_offset_db_fmt_cntl(&command_writer->hw_config_draw,
                                                            pa_su_poly_offset_db_fmt_cntl);
}

static void
terakan_app_config_draw_apply_cb_rop3(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;

   bool const rop3_enable = config->cb_rop3_.rop3_enable;

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(CB_COLOR_RTV_AND_BLEND_CONTROL, CB_ROP3);
   /* Section "Logical Operations" of the Vulkan 1.4.344 specification says:
    *
    *     "If logicOpEnable is VK_TRUE, then a logical operation selected by logicOp is applied
    *     between each color attachment and the fragment’s corresponding output value, and blending
    *     of all attachments is treated as if it were disabled."
    *
    * Section 8.1.2 "Prohibited combinations" of Radeon Evergreen / Northern Islands Acceleration
    * says:
    *
    *     "Special rop3 modes cannot be used when any MRT is using the blender. If any MRT is using
    *     the blender, ROP3 must be set to the value 0xCC."
    */
   if (config->cb_color_rtv_and_blend_control_.from_apply_cb_rop3.blend_disable != rop3_enable) {
      config->cb_color_rtv_and_blend_control_.from_apply_cb_rop3.blend_disable = rop3_enable;
      terakan_app_config_draw_set_pending(
         config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL);
   }

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(CB_COLOR_CONTROL, CB_ROP3);
   enum terakan_hw_config_draw_cb_color_control_rop3 const rop3 =
      rop3_enable ? config->cb_rop3_.rop3 : TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_COPY;
   if (config->cb_color_control_.from_apply_cb_rop3.rop3 != rop3) {
      config->cb_color_control_.from_apply_cb_rop3.rop3 = rop3;
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_CONTROL);
   }
}

static void
terakan_app_config_draw_apply_cb_color_rtv_and_blend_control(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const app_config = &command_writer->app_config_draw;
   struct terakan_hw_config_draw * const hw_config = &command_writer->hw_config_draw;

   bool const is_r9xx =
      terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx;

   /* The hardware color target indices are compacted, the application ones aren't. */
   unsigned hw_rtv_count = 0;
   uint32_t hw_cb_target_mask = 0b0;

   bool dual_source_blend = false;

   uint32_t color_blend_factors_used = 0b0, alpha_blend_factors_used = 0b0;

   bool export_128bpp = false;

   u_foreach_bit (app_rtv_index, app_config->cb_color_rtv_and_blend_control_
                                    .from_apply_sq_pgm_fragment.rtv_dsb_uncompacted_exports) {
      struct terakan_app_config_draw_cb_color_rtv const * const rtv =
         &app_config->cb_color_rtv_and_blend_control_.rtv[app_rtv_index];
      if (!dual_source_blend && terakan_color_descriptor_is_bound(rtv->bo, &rtv->color) &&
          (app_config->cb_color_rtv_and_blend_control_.write_enable_mask &
           BITFIELD_BIT(app_rtv_index))) {
         uint8_t const rtv_format_export_mask =
            terakan_app_config_draw_cb_color_rtv_format_export_mask(rtv);
         uint8_t const rtv_write_mask =
            (app_config->cb_color_rtv_and_blend_control_.write_component_mask >>
             (4 * app_rtv_index)) &
            rtv_format_export_mask;
         if (rtv_write_mask) {
            /* #MemoryIntegrity: Don't allow color targets not compatible with the current
             * rasterization sample count and depth / stencil target sample count. Prior to R9xx,
             * `PA_SC_AA_CONFIG.MSAA_NUM_SAMPLES` also directly specifies the depth / stencil and
             * color target sample count. Expecting that the descriptor inside the driver has the
             * sample count specified regardless of the architecture generation.
             */
            uint8_t const rtv_samples_log2 = G_028C74_NUM_SAMPLES(rtv->color.attrib);
            if (is_r9xx ? rtv_samples_log2 <=
                                app_config->cb_color_rtv_and_blend_control_
                                   .from_apply_db_depth_stencil_buffer.sample_count_limit_log2 &&
                             G_028C74_NUM_FRAGMENTS(rtv->color.attrib) >=
                                app_config->cb_color_rtv_and_blend_control_.from_apply_db_eqaa
                                   .min_fragments_log2_r9xx
                        : rtv_samples_log2 ==
                             app_config->cb_color_rtv_and_blend_control_
                                .from_apply_db_depth_stencil_buffer.sample_count_limit_log2) {
               /* The color target needs to be bound. */

               terakan_hw_config_draw_set_cb_color(hw_config, hw_rtv_count, rtv->bo, &rtv->color,
                                                   &rtv->meta);

               /* Along with the components in the write mask, enable components missing from the
                * format to more clearly specify that the destination data for those components
                * isn't needed for write masking purposes.
                */
               hw_cb_target_mask |= (rtv_write_mask | (rtv_format_export_mask ^ 0xF))
                                    << (4 * app_rtv_index);

               uint32_t hw_blend_control = 0;
               if (!app_config->cb_color_rtv_and_blend_control_.from_apply_cb_rop3.blend_disable &&
                   !G_028C70_BLEND_BYPASS(rtv->color.info)) {
                  uint32_t const app_blend_control = rtv->blend_control;
                  if (G_028780_BLEND_CONTROL_ENABLE(app_blend_control)) {
                     hw_blend_control =
                        S_028780_BLEND_CONTROL_ENABLE(true) | S_028780_SEPARATE_ALPHA_BLEND(true);

                     bool const dual_source_blend_allowed =
                        app_rtv_index == 0 &&
                        (app_config->cb_color_rtv_and_blend_control_.from_apply_sq_pgm_fragment
                            .rtv_dsb_uncompacted_exports &
                         0b10) != 0;

                     if (rtv_write_mask & 0b0111) {
                        uint32_t const comb_fcn = G_028780_COLOR_COMB_FCN(app_blend_control);
                        hw_blend_control |= S_028780_COLOR_COMB_FCN(comb_fcn);
                        if (terakan_hw_config_draw_cb_blend_control_comb_fcn_uses_factors(
                               comb_fcn)) {
                           uint32_t src_factor = G_028780_COLOR_SRCBLEND(app_blend_control);
                           uint32_t dest_factor = G_028780_COLOR_DESTBLEND(app_blend_control);
                           if (dual_source_blend_allowed) {
                              if ((BITFIELD_BIT(src_factor) | BITFIELD_BIT(dest_factor)) &
                                  TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_SRC1) {
                                 dual_source_blend = true;
                              }
                           } else {
                              /* Disable dual-source blending for targets not supporting it. */
                              if (BITFIELD_BIT(src_factor) &
                                  TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_SRC1) {
                                 src_factor = V_028780_BLEND_ZERO;
                              }
                              if (BITFIELD_BIT(dest_factor) &
                                  TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_SRC1) {
                                 dest_factor = V_028780_BLEND_ZERO;
                              }
                           }
                           hw_blend_control |= S_028780_COLOR_SRCBLEND(src_factor) |
                                               S_028780_COLOR_DESTBLEND(dest_factor);
                           color_blend_factors_used |=
                              BITFIELD_BIT(src_factor) | BITFIELD_BIT(dest_factor);
                        } else {
                           hw_blend_control |= S_028780_COLOR_SRCBLEND(V_028780_BLEND_ONE) |
                                               S_028780_COLOR_DESTBLEND(V_028780_BLEND_ZERO);
                        }
                     }

                     if (rtv_write_mask & 0b1000) {
                        uint32_t const app_blend_control_alpha =
                           app_blend_control
                           << (G_028780_SEPARATE_ALPHA_BLEND(app_blend_control) ? 0 : 16);
                        uint32_t const comb_fcn = G_028780_ALPHA_COMB_FCN(app_blend_control_alpha);
                        hw_blend_control |= S_028780_ALPHA_COMB_FCN(comb_fcn);
                        if (terakan_hw_config_draw_cb_blend_control_comb_fcn_uses_factors(
                               comb_fcn)) {
                           /* terakan_hw_config_draw_cb_blend_control_color_factors_for_color_alpha
                            * not applied, because it's expected to have been applied by the client
                            * (while creating the pipeline object, for instance).
                            */
                           uint32_t src_factor = G_028780_ALPHA_SRCBLEND(app_blend_control_alpha);
                           uint32_t dest_factor = G_028780_ALPHA_DESTBLEND(app_blend_control_alpha);
                           if (dual_source_blend_allowed) {
                              if ((BITFIELD_BIT(src_factor) | BITFIELD_BIT(dest_factor)) &
                                  TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_SRC1) {
                                 dual_source_blend = true;
                              }
                           } else {
                              /* Disable dual-source blending for targets not supporting it. */
                              if (BITFIELD_BIT(src_factor) &
                                  TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_SRC1) {
                                 src_factor = V_028780_BLEND_ZERO;
                              }
                              if (BITFIELD_BIT(dest_factor) &
                                  TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_SRC1) {
                                 dest_factor = V_028780_BLEND_ZERO;
                              }
                           }
                           hw_blend_control |= S_028780_ALPHA_SRCBLEND(src_factor) |
                                               S_028780_ALPHA_DESTBLEND(dest_factor);
                           alpha_blend_factors_used |=
                              BITFIELD_BIT(src_factor) | BITFIELD_BIT(dest_factor);
                        } else {
                           hw_blend_control |= S_028780_ALPHA_SRCBLEND(V_028780_BLEND_ONE) |
                                               S_028780_ALPHA_DESTBLEND(V_028780_BLEND_ZERO);
                        }
                     } else {
                        /* Replicate the color blend equation into alpha if it's not written, for
                         * the optimizations below to handle further.
                         */
                        hw_blend_control |= (hw_blend_control & BITFIELD_MASK(13)) << 16;
                     }

                     if (!(rtv_write_mask & 0b0111)) {
                        /* Replicate the alpha blend equation into color if it's not written, for
                         * the optimizations below to handle further.
                         */
                        hw_blend_control |= (hw_blend_control >> 16) & BITFIELD_MASK(13);
                     }

                     /* For simplicity, removing the separate alpha blend flag later, but initially
                      * always enabling it.
                      */
                     assert(G_028780_SEPARATE_ALPHA_BLEND(hw_blend_control));
                     if (unlikely(hw_blend_control ==
                                     (S_028780_COLOR_SRCBLEND(V_028780_BLEND_ONE) |
                                      S_028780_COLOR_COMB_FCN(V_028780_COMB_DST_PLUS_SRC) |
                                      S_028780_COLOR_DESTBLEND(V_028780_BLEND_ZERO) |
                                      S_028780_ALPHA_SRCBLEND(V_028780_BLEND_ONE) |
                                      S_028780_ALPHA_COMB_FCN(V_028780_COMB_DST_PLUS_SRC) |
                                      S_028780_ALPHA_DESTBLEND(V_028780_BLEND_ZERO) |
                                      S_028780_SEPARATE_ALPHA_BLEND(true) |
                                      S_028780_BLEND_CONTROL_ENABLE(true)) &&
                                  G_028C70_SIMPLE_FLOAT(rtv->color.info))) {
                        /* Disable blending if it's no-operation. */
                        hw_blend_control = 0;
                     } else {
                        if (!((hw_blend_control ^ (hw_blend_control >> 16)) & BITFIELD_MASK(13))) {
                           /* Same equation used for color and alpha, or either is not written. */
                           hw_blend_control &= C_028780_SEPARATE_ALPHA_BLEND;
                        }
                     }
                  }
               }
               terakan_hw_config_draw_set_cb_blend_control(hw_config, hw_rtv_count,
                                                           hw_blend_control);

               if (G_028C70_SOURCE_FORMAT(rtv->color.info) == V_028C70_EXPORT_4C_32BPC) {
                  export_128bpp = true;
               }

               ++hw_rtv_count;
               continue;
            }
         }
      }

      /* The color target doesn't need to be bound. */
      terakan_hw_config_draw_set_cb_color_unbound(
         hw_config, hw_rtv_count,
         hw_rtv_count == 1 && dual_source_blend
            ? G_028C70_SOURCE_FORMAT(app_config->cb_color_rtv_and_blend_control_.rtv[0].color.info)
            : V_028C70_EXPORT_4C_16BPC);
      terakan_hw_config_draw_set_cb_blend_control(hw_config, hw_rtv_count, 0);
      ++hw_rtv_count;
   }

   /* Disable blending for unused RTVs. */
   for (; hw_rtv_count < TERAKAN_COLOR_HW_RTV_COUNT; ++hw_rtv_count) {
      terakan_hw_config_draw_set_cb_blend_control(hw_config, hw_rtv_count, 0);
   }

   terakan_hw_config_draw_set_cb_target_mask(hw_config, hw_cb_target_mask);

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(CB_BLEND_CONSTANTS, CB_COLOR_RTV_AND_BLEND_CONTROL);
   bool const blend_constants_rgb_used =
      (color_blend_factors_used & TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_CONST_COLOR) != 0;
   bool const blend_constants_alpha_used =
      (color_blend_factors_used & TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_CONST_ALPHA) ||
      (alpha_blend_factors_used & TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_CONST);
   if (app_config->cb_blend_constants_.from_apply_cb_color_rtv_and_blend_control
             .constants_rgb_used != blend_constants_rgb_used ||
       app_config->cb_blend_constants_.from_apply_cb_color_rtv_and_blend_control
             .constants_alpha_used != blend_constants_alpha_used) {
      app_config->cb_blend_constants_.from_apply_cb_color_rtv_and_blend_control.constants_rgb_used =
         blend_constants_rgb_used;
      app_config->cb_blend_constants_.from_apply_cb_color_rtv_and_blend_control
         .constants_alpha_used = blend_constants_alpha_used;
      terakan_app_config_draw_set_pending(app_config,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_BLEND_CONSTANTS);
   }

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(CB_COLOR_CONTROL, CB_COLOR_RTV_AND_BLEND_CONTROL);
   bool const any_rtv_written = hw_cb_target_mask != 0b0;
   if (app_config->cb_color_control_.from_apply_cb_color_rtv_and_blend_control.any_rtv_written !=
       any_rtv_written) {
      app_config->cb_color_control_.from_apply_cb_color_rtv_and_blend_control.any_rtv_written =
         any_rtv_written;
      terakan_app_config_draw_set_pending(app_config,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_CONTROL);
   }

   TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(DB_SHADER_CONTROL, CB_COLOR_RTV_AND_BLEND_CONTROL);
   if (app_config->db_shader_control_.from_apply_cb_color_rtv_and_blend_control.rtv_128bpp_export !=
       export_128bpp) {
      app_config->db_shader_control_.from_apply_cb_color_rtv_and_blend_control.rtv_128bpp_export =
         export_128bpp;
      terakan_app_config_draw_set_pending(app_config,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_SHADER_CONTROL);
   }
}

static void
terakan_app_config_draw_apply_cb_color_uav_and_unused_mrt(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const app_config = &command_writer->app_config_draw;
   struct terakan_hw_config_draw * const hw_config = &command_writer->hw_config_draw;
   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);

   unsigned const uav_color_index_base =
      app_config->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.rtv_dsb_export_count;

   unsigned uav_count = 0;

   unsigned uav_uncompacted_index;
   BITSET_FOREACH_SET (uav_uncompacted_index,
                       app_config->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.uav_used,
                       TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL) {
      unsigned const color_index = uav_color_index_base + uav_count;
      assert(color_index < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);

      /* The color export count is intentionally not added to the immediate buffer resource indices
       * in the driver's shader ABI.
       */
      unsigned const uav_immediate_resource_index =
         TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL + uav_count;

      if (BITSET_SET(app_config->cb_color_uav_and_unused_mrt_.uav_bound, uav_uncompacted_index)) {
         struct terakan_app_config_draw_cb_color_uav const * const uav =
            &app_config->cb_color_uav_and_unused_mrt_.uav[uav_uncompacted_index];

         if (G_028C70_RESOURCE_TYPE(uav->color.info) == V_028C70_BUFFER) {
            uint32_t * const uav_base_granularity_offset_constant =
               &command_writer->push_constants_state.driver_constants
                   .buffer_uav_base_granularity_offset[uav_count];
            if (*uav_base_granularity_offset_constant != uav->color.view) {
               *uav_base_granularity_offset_constant = uav->color.view;
               command_writer->push_constants_state.driver_constants_modified |= BITFIELD_BIT(
                  TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_BUFFER_UAV_BASE_GRANULARITY_OFFSET);
            }
            /* The driver-internal `view` field of the descriptor will be eliminated by `hw_config`.
             */
         }

         terakan_hw_config_draw_set_cb_color(hw_config, color_index, uav->bo, &uav->color, NULL);

         terakan_hw_config_draw_set_cb_immed(
            hw_config, color_index,
            util_logbase2(terascale_format_bytes_per_block[G_028C70_FORMAT(uav->color.info)]));
         struct terakan_resource_descriptor const uav_immediate_resource =
            terakan_color_descriptor_info_to_uav_immediate_resource(device, uav->color.info);
         terakan_hw_config_sqk_set_resource_fs(&command_writer->hw_config_sqk,
                                               uav_immediate_resource_index,
                                               device->uav_immediate_bo, &uav_immediate_resource);
      } else {
         terakan_hw_config_draw_set_cb_color_unbound(hw_config, color_index,
                                                     V_028C70_EXPORT_4C_16BPC);

         /* Make 0 the return value of all operations using the unbound UAV. */
         terakan_hw_config_sqk_set_resource_fs(&command_writer->hw_config_sqk,
                                               uav_immediate_resource_index, NULL, NULL);
      }

      ++uav_count;
   }

   for (unsigned color_index = uav_color_index_base + uav_count;
        color_index < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT; ++color_index) {
      terakan_hw_config_draw_set_cb_color_unbound(hw_config, color_index, V_028C70_EXPORT_4C_16BPC);
   }

   bool const any_uav_used = uav_count != 0;
   if (app_config->cb_color_control_.from_apply_cb_color_uav_and_unused_mrt.any_uav_used !=
       any_uav_used) {
      app_config->cb_color_control_.from_apply_cb_color_uav_and_unused_mrt.any_uav_used =
         any_uav_used;
      terakan_app_config_draw_set_pending(app_config,
                                          TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_CONTROL);
   }
}

static void
terakan_app_config_draw_apply_cb_blend_constants(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw const * const config = &command_writer->app_config_draw;
   if (config->cb_blend_constants_.from_apply_cb_color_rtv_and_blend_control.constants_rgb_used) {
      terakan_hw_config_draw_set_cb_blend_constants_rgb(&command_writer->hw_config_draw,
                                                        config->cb_blend_constants_.constants);
   }
   if (config->cb_blend_constants_.from_apply_cb_color_rtv_and_blend_control.constants_alpha_used) {
      terakan_hw_config_draw_set_cb_blend_constants_alpha(&command_writer->hw_config_draw,
                                                          config->cb_blend_constants_.constants[3]);
   }
}

static void
terakan_app_config_draw_apply_cb_color_control(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw const * const config = &command_writer->app_config_draw;
   terakan_hw_config_draw_set_cb_color_control(
      &command_writer->hw_config_draw,
      S_028808_MODE(
         config->cb_color_control_.from_apply_cb_color_rtv_and_blend_control.any_rtv_written ||
               config->cb_color_control_.from_apply_cb_color_uav_and_unused_mrt.any_uav_used
            ? V_028808_CB_NORMAL
            : V_028808_CB_DISABLE) |
         S_028808_ROP3(
            config->cb_color_control_.from_apply_cb_color_rtv_and_blend_control.any_rtv_written
               ? config->cb_color_control_.from_apply_cb_rop3.rop3
               : TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_COPY));
}

static void
terakan_app_config_draw_apply_db_shader_control(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw const * const config = &command_writer->app_config_draw;
   uint32_t db_shader_control =
      config->db_shader_control_.from_apply_sq_pgm_fragment.db_shader_control;
   if (config->db_shader_control_.from_apply_cb_color_rtv_and_blend_control.rtv_128bpp_export) {
      db_shader_control &= C_02880C_DUAL_EXPORT_ENABLE;
   }
   terakan_hw_config_draw_set_db_shader_control(&command_writer->hw_config_draw, db_shader_control);
}

static terakan_app_config_apply_function const
   terakan_app_config_apply_functions[TERAKAN_APP_CONFIG_DRAW_ENTRIES_COUNT] = {
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_PRIMITIVE_TYPE] =
         terakan_app_config_draw_apply_vgt_primitive_type,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_INDEX_OFFSET] =
         terakan_app_config_draw_apply_vgt_index_offset,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER] =
         terakan_app_config_draw_apply_vgt_dma_index_buffer,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_PRE_RASTERIZATION] =
         terakan_app_config_draw_apply_sq_pgm_pre_rasterization,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FETCH] = terakan_app_config_draw_apply_sq_pgm_fetch,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_RESOURCES_FETCH] =
         terakan_app_config_draw_apply_sq_resources_fetch,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_CL_CLIP_CNTL] =
         terakan_app_config_draw_apply_pa_cl_clip_cntl,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_IA_MULTI_VGT_PARAM_PRE_RASTERIZER_DISCARD_R9XX] =
         terakan_app_config_draw_apply_ia_multi_vgt_param_pre_rasterizer_discard_r9xx,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_VPORT] = terakan_app_config_draw_apply_pa_vport,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_SC_MODE_CNTL] =
         terakan_app_config_draw_apply_pa_su_sc_mode_cntl,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_CL_VTE_CNTL] = terakan_app_config_draw_apply_pa_cl_vte_cntl,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_LINE_STIPPLE] =
         terakan_app_config_draw_apply_pa_sc_line_stipple,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET] =
         terakan_app_config_draw_apply_pa_su_poly_offset,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_AA_CONFIG_SAMPLE_LOCS] =
         terakan_app_config_draw_apply_pa_sc_aa_config_sample_locs,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_AA_MASK] = terakan_app_config_draw_apply_pa_sc_aa_mask,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_MODE_CNTL_0] =
         terakan_app_config_draw_apply_pa_sc_mode_cntl_0,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_IA_MULTI_VGT_PARAM_POST_RASTERIZER_DISCARD_R9XX] =
         terakan_app_config_draw_apply_ia_multi_vgt_param_post_rasterizer_discard_r9xx,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FRAGMENT] =
         terakan_app_config_draw_apply_sq_pgm_fragment,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_COUNT_CONTROL] =
         terakan_app_config_draw_apply_db_count_control,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_BUFFER] =
         terakan_app_config_draw_apply_db_depth_stencil_buffer,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE] =
         terakan_app_config_draw_apply_db_render_override,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE2] =
         terakan_app_config_draw_apply_db_render_override2,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_CONTROL_REF_MASK] =
         terakan_app_config_draw_apply_db_depth_stencil_control_ref_mask,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_EQAA] = terakan_app_config_draw_apply_db_eqaa,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_ALPHA_TO_MASK] =
         terakan_app_config_draw_apply_db_alpha_to_mask,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET_DB_FMT_CNTL] =
         terakan_app_config_draw_apply_pa_su_poly_offset_db_fmt_cntl,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_ROP3] = terakan_app_config_draw_apply_cb_rop3,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL] =
         terakan_app_config_draw_apply_cb_color_rtv_and_blend_control,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_UAV_AND_UNUSED_MRT] =
         terakan_app_config_draw_apply_cb_color_uav_and_unused_mrt,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_BLEND_CONSTANTS] =
         terakan_app_config_draw_apply_cb_blend_constants,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_CONTROL] =
         terakan_app_config_draw_apply_cb_color_control,
      [TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_SHADER_CONTROL] =
         terakan_app_config_draw_apply_db_shader_control,
};

void
terakan_app_config_draw_apply_pending(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;
   /* Because the applying functions may make dependent entries (which must have greater indices)
    * pending, `BITSET_FOREACH_SET` isn't used, rather always iterating the current pending entries.
    */
   unsigned const apply_entry_count =
      config->pa_cl_clip_cntl_.dx_rasterization_kill
         ? TERAKAN_APP_CONFIG_DRAW_ENTRIES_PRE_RASTERIZER_DISCARD_LAST + 1
         : TERAKAN_APP_CONFIG_DRAW_ENTRIES_COUNT;
   unsigned const apply_word_count = BITSET_WORDS(apply_entry_count);
   for (unsigned apply_word_index = 0; apply_word_index < apply_word_count; ++apply_word_index) {
      BITSET_WORD * const apply_word = &config->entries_pending_[apply_word_index];
      unsigned const apply_word_first_entry_index = BITSET_WORDBITS * apply_word_index;
      while (*apply_word) {
         static_assert(
            sizeof(BITSET_WORD) == sizeof(int),
            "Using `ffs` with `BITSET_WORD`, expecting the bitset word size to match the argument "
            "size.");
         unsigned const apply_word_bit_index = (unsigned)(ffs(*apply_word) - 1);
         unsigned const apply_entry_index = apply_word_first_entry_index + apply_word_bit_index;
         if (apply_entry_index >= apply_entry_count) {
            break;
         }
         terakan_app_config_apply_functions[apply_entry_index](command_writer);
         /* Not using `BITSET_ZERO`, instead excluding individual bits, because with rasterizer
          * discard, applying only a subset of entries.
          */
         *apply_word &= ~((BITSET_WORD)1 << apply_word_bit_index);
      }
   }
}

void
terakan_app_config_draw_reset(struct terakan_app_config_draw * config)
{
   BITSET_ONES(config->entries_pending_);

   /* Safe defaults for registers, based on the values expected when the respective state is
    * zero-initialized in Vulkan structures or commands, or when the corresponding Vulkan optional
    * features or pipeline stages are disabled, or where there's no such default for Vulkan, on the
    * default OpenGL state, falling back to the Direct3D 11 default if not defined, similarly to the
    * `terakan_hw_config` defaults.
    */

   config->vgt_primitive_type_ = TERAKAN_HW_CONFIG_SHARED_DRAW_DEFAULT_VGT_PRIMITIVE_TYPE;
   config->sq_pgm_pre_rasterization_.from_apply_vgt_primitive_type.tessellation_enable =
      G_008958_PRIM_TYPE(TERAKAN_HW_CONFIG_SHARED_DRAW_DEFAULT_VGT_PRIMITIVE_TYPE) ==
      V_008958_DI_PT_PATCH;
   config->pa_sc_line_stipple_.from_apply_vgt_primitive_type.per_primitive_reset =
      (TERAKAN_APP_CONFIG_DRAW_PA_SC_LINE_STIPPLE_PER_PRIMITIVE_RESET_PRIMITIVE_TYPES &
       BITFIELD64_BIT(
          G_008958_PRIM_TYPE(TERAKAN_HW_CONFIG_SHARED_DRAW_DEFAULT_VGT_PRIMITIVE_TYPE))) != 0;

   config->vgt_index_offset_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_INDEX_OFFSET;

   config->vgt_dma_index_buffer_.index_buffer = TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_DMA_INDEX_BUFFER;
   config->vgt_dma_index_buffer_.index_type = TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_DMA_INDEX_TYPE;
   config->vgt_dma_index_buffer_.multi_prim_reset_index =
      TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_MULTI_PRIM_IB_RESET_INDEX;
   /* VkPipelineInputAssemblyStateCreateInfo primitiveRestartEnable = VK_FALSE */
   config->vgt_dma_index_buffer_.multi_prim_reset_enable = false;
   config->vgt_dma_index_buffer_.draw_indexed = false;

   config->sq_pgm_pre_rasterization_.vertex_as_local = NULL;
   config->sq_pgm_pre_rasterization_.vertex_as_export = NULL;
   config->sq_pgm_pre_rasterization_.vertex_as_vertex = NULL;
   config->sq_pgm_pre_rasterization_.tessellation_control = NULL;
   config->sq_pgm_pre_rasterization_.tessellation_evaluation_as_export = NULL;
   config->sq_pgm_pre_rasterization_.tessellation_evaluation_as_vertex = NULL;
   config->sq_pgm_pre_rasterization_.geometry = NULL;
   config->ia_multi_vgt_param_pre_rasterizer_discard_r9xx_.from_apply_sq_pgm_pre_rasterization
      .ia_multi_vgt_param = TERAKAN_HW_CONFIG_DRAW_DEFAULT_IA_MULTI_VGT_PARAM;
   config->pa_sc_line_stipple_.from_apply_sq_pgm_pre_rasterization.geometry_shader_enable = false;

   memset(&config->sq_pgm_fetch_, 0, sizeof(config->sq_pgm_fetch_));

   memset(&config->sq_resources_fetch_, 0, sizeof(config->sq_resources_fetch_));

   /* No VkPipelineViewportDepthClipControlCreateInfoEXT
    * VkPipelineRasterizationStateCreateInfo:
    * - depthClampEnable = VK_FALSE
    * - rasterizerDiscardEnable = VK_FALSE
    * No VkPipelineRasterizationDepthClipStateCreateInfoEXT
    */
   config->pa_cl_clip_cntl_.dx_rasterization_kill = false;
   config->ia_multi_vgt_param_pre_rasterizer_discard_r9xx_.from_apply_pa_cl_clip_cntl
      .dx_rasterization_kill = config->pa_cl_clip_cntl_.dx_rasterization_kill;
   config->pa_cl_clip_cntl_.dx_clip_space_def = true;
   config->pa_cl_clip_cntl_.z_clamp_enable = false;
   config->pa_cl_clip_cntl_.z_clip_enable_override = -1;
   config->pa_vport_.from_apply_pa_cl_clip_cntl.dx_clip_space_def =
      config->pa_cl_clip_cntl_.dx_clip_space_def;
   config->pa_vport_.from_apply_pa_cl_clip_cntl.z_clamp_enable =
      config->pa_cl_clip_cntl_.z_clamp_enable;

   /* VK_EXT_depth_range_unrestricted not enabled
    * VkPipelineViewportStateCreateInfo:
    * - viewportCount = 0
    * - pViewports[0 ... TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT - 1] = (VkViewport){}
    * - scissorCount = 0
    * No VkPipelineViewportDepthClampControlCreateInfoEXT
    * Not in a render pass instance
    */
   config->pa_vport_.z_range_unrestricted = false;
   config->pa_vport_.user_defined_zmin_zmax_enable = false;
   config->pa_vport_.vport_count = 0;
   config->pa_vport_.explicit_scissor_count = 0;
   config->pa_vport_.render_area = (struct terakan_screen_rect){};
   memset(config->pa_vport_.user_defined_zmin_zmax, 0,
          sizeof(config->pa_vport_.user_defined_zmin_zmax));
   memset(config->pa_vport_.vports, 0, sizeof(config->pa_vport_.vports));
   memset(config->pa_vport_.explicit_scissors, 0, sizeof(config->pa_vport_.explicit_scissors));
   config->db_render_override_.from_apply_pa_vport.disable_viewport_clamp = false;

   config->pa_su_sc_mode_cntl_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SU_SC_MODE_CNTL;
   config->pa_su_poly_offset_.from_apply_pa_su_sc_mode_cntl.poly_offset_enable =
      (TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SU_SC_MODE_CNTL &
       ~(uint32_t)(C_028814_POLY_OFFSET_FRONT_ENABLE & C_028814_POLY_OFFSET_BACK_ENABLE &
                   C_028814_POLY_OFFSET_PARA_ENABLE)) != 0;
   config->pa_su_poly_offset_db_fmt_cntl_.from_apply_pa_su_sc_mode_cntl.poly_offset_enable =
      config->pa_su_poly_offset_.from_apply_pa_su_sc_mode_cntl.poly_offset_enable;

   config->pa_sc_line_stipple_.pattern =
      TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_LINE_STIPPLE &
      ~(uint32_t)(C_028A0C_LINE_PATTERN & C_028A0C_REPEAT_COUNT & C_028A0C_PATTERN_BIT_ORDER);
   /* No VkPipelineRasterizationLineStateCreateInfo */
   config->pa_sc_line_stipple_.enable = false;
   config->pa_sc_mode_cntl_0_.from_apply_pa_sc_line_stipple.line_stipple_enable =
      config->pa_sc_line_stipple_.enable;
   config->ia_multi_vgt_param_post_rasterizer_discard_r9xx_.from_apply_pa_sc_line_stipple
      .line_stipple_enable = config->pa_sc_line_stipple_.enable;

   /* VkPipelineMultisampleStateCreateInfo rasterizationSamples = min (VK_SAMPLE_COUNT_1_BIT)
    * No VkPipelineSampleLocationsStateCreateInfoEXT
    */
   config->pa_sc_aa_config_sample_locs_.msaa_num_samples_log2 = 0;
   config->pa_sc_aa_config_sample_locs_.custom_sample_locs_enable = false;
   memset(config->pa_sc_aa_config_sample_locs_.custom_sample_locs, 0,
          sizeof(config->pa_sc_aa_config_sample_locs_.custom_sample_locs));
   config->pa_sc_mode_cntl_0_.from_apply_pa_sc_aa_config_sample_locs.msaa_enable = false;

   config->pa_su_poly_offset_.poly_offset = TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SU_POLY_OFFSET;

   /* VkPipelineMultisampleStateCreateInfo pSampleMask = NULL, rasterizationSamples handled
    * dynamically.
    */
   config->pa_sc_aa_mask_ = 0xFFFF;

   config->sq_pgm_fragment_ = NULL;
   config->db_eqaa_.from_apply_sq_pgm_fragment.ps_iter_full_sample_shading = false;
   config->cb_color_rtv_and_blend_control_.from_apply_sq_pgm_fragment.rtv_dsb_uncompacted_exports =
      0b0;
   config->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.rtv_dsb_export_count = 0;
   BITSET_ZERO(config->cb_color_uav_and_unused_mrt_.from_apply_sq_pgm_fragment.uav_used);
   config->db_shader_control_.from_apply_sq_pgm_fragment.db_shader_control =
      TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY;

   config->db_count_control_.zpass_query_active_count = 0;

   config->db_depth_stencil_buffer_.bo = NULL;
   config->db_depth_stencil_buffer_.descriptor = (struct terakan_depth_stencil_descriptor){};
   config->db_render_override2_.from_apply_db_depth_stencil_buffer.decompress_z_on_flush_r9xx =
      false;
   config->db_depth_stencil_control_ref_mask_.from_apply_db_depth_stencil_buffer.depth_bound =
      false;
   config->db_depth_stencil_control_ref_mask_.from_apply_db_depth_stencil_buffer.stencil_bound =
      false;
   config->pa_su_poly_offset_db_fmt_cntl_.from_apply_db_depth_stencil_buffer.depth_format =
      TERASCALE_R8XX_DEPTH_FORMAT_32_FLOAT;
   /* VkPipelineMultisampleStateCreateInfo rasterizationSamples = min (VK_SAMPLE_COUNT_1_BIT) */
   config->db_eqaa_.from_apply_db_depth_stencil_buffer.max_anchor_samples_log2_r9xx = 0;
   config->cb_color_rtv_and_blend_control_.from_apply_db_depth_stencil_buffer
      .sample_count_limit_log2 = 0;

   config->db_depth_stencil_control_ref_mask_.stencil_ref_mask_front =
      TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_STENCILREFMASK;
   config->db_depth_stencil_control_ref_mask_.stencil_ref_mask_back =
      TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_STENCILREFMASK;

   config->db_depth_stencil_control_ref_mask_.depth_stencil_control =
      TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_DEPTH_CONTROL;

   /* VkPipelineMultisampleStateCreateInfo minSampleShading = 0.0f, no fragment shader */
   config->db_eqaa_.ps_iter_max_invocation_samples_log2 = -1;
   /* Disable limiting of sample shading to the fragment count by default, like when not in a render
    * pass instance (no color attachments used), regardless of the rasterization sample count.
    */
   config->db_eqaa_.ps_iter_least_fragments_log2_r9xx = 4;
   config->cb_color_rtv_and_blend_control_.from_apply_db_eqaa.min_fragments_log2_r9xx = 0;

   /* VkPipelineMultisampleStateCreateInfo alphaToCoverageEnable = VK_FALSE, treat
    * GL_ALPHA_TO_COVERAGE_DITHER_DEFAULT_NV as dithered for more distinct transparency amounts.
    */
   config->db_alpha_to_mask_ = TERAKAN_HW_CONFIG_DRAW_DB_ALPHA_TO_MASK_OFFSETS_DITHERED;

   config->pa_su_poly_offset_db_fmt_cntl_.representation =
      TERAKAN_APP_CONFIG_DRAW_POLY_OFFSET_REPRESENTATION_FORMAT;
   config->pa_su_poly_offset_db_fmt_cntl_.exact = false;

   /* VkPipelineColorBlendStateCreateInfo:
    * - logicOpEnable = VK_FALSE
    * - logicOp = VK_LOGIC_OP_CLEAR
    */
   config->cb_rop3_.rop3_enable = false;
   config->cb_rop3_.rop3 = TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_CLEAR;
   config->cb_color_rtv_and_blend_control_.from_apply_cb_rop3.blend_disable = false;
   config->cb_color_control_.from_apply_cb_rop3.rop3 =
      TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_COPY;

   /* VkPipelineColorBlendAttachmentState:
    * - blendEnable = VK_FALSE
    * - colorWriteMask = 0b0
    * No VkPipelineColorWriteCreateInfoEXT
    */
   config->cb_color_rtv_and_blend_control_.write_component_mask = 0b0;
   config->cb_color_rtv_and_blend_control_.write_enable_mask =
      BITFIELD_MASK(TERAKAN_COLOR_HW_RTV_COUNT);
   memset(config->cb_color_rtv_and_blend_control_.rtv, 0,
          sizeof(config->cb_color_rtv_and_blend_control_.rtv));
   config->cb_blend_constants_.from_apply_cb_color_rtv_and_blend_control.constants_rgb_used = false;
   config->cb_blend_constants_.from_apply_cb_color_rtv_and_blend_control.constants_alpha_used =
      false;
   config->cb_color_control_.from_apply_cb_color_rtv_and_blend_control.any_rtv_written = false;
   config->db_shader_control_.from_apply_cb_color_rtv_and_blend_control.rtv_128bpp_export = false;

   BITSET_ZERO(config->cb_color_uav_and_unused_mrt_.uav_bound);
   config->cb_color_control_.from_apply_cb_color_uav_and_unused_mrt.any_uav_used = false;

   /* VkPipelineColorBlendStateCreateInfo blendConstants[0...3] = 0.0f */
   memset(config->cb_blend_constants_.constants, 0, sizeof(config->cb_blend_constants_.constants));
}
