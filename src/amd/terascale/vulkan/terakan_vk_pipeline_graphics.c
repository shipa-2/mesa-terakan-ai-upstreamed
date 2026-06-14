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

#include "terakan_vk_pipeline_graphics.h"

#include "terakan_command_buffer.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_pipeline_layout.h"
#include "terakan_shader.h"
#include "terakan_vk_state.h"

#include "amd/terascale/common/terascale_format.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600_asm.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "util/u_math.h"

#include "vk_enum_to_str.h"
#include "vk_format.h"
#include "vk_graphics_state.h"
#include "vk_log.h"
#include "vk_shader_module.h"
#include "vk_util.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Note that when using pipeline objects, it's assumed that static state is the fastest path.
 * Therefore, for applying the pipeline state, to check if state is static, using conditionals
 * rather than a callback array, because the former are expected to be cheaper.
 */

/* The state structure initialization functions expect the structures to be preinitialized to 0,
 * since `vk_pipeline_zalloc` does that anyway.
 */

/* TODO(Triang3l): Possibly optimizations for not changing configuration if it's known from the
 * static state that it's irrelevant (for instance, the depth bias factors if the depth bias is
 * statically disabled).
 */

static void
terakan_vk_pipeline_graphics_vertex_input_apply(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_vk_pipeline_graphics_vertex_input const * const vertex_input)
{
   if (BITSET_TEST(vertex_input->static_state,
                   TERAKAN_VK_PIPELINE_GRAPHICS_VERTEX_INPUT_STATIC_VGT_PRIMITIVE_TYPE)) {
      terakan_app_config_draw_set_vgt_primitive_type(&command_writer->app_config_draw,
                                                     vertex_input->vgt_primitive_type);
   }

   if (vertex_input->sq_resources_fetch_stride.static_stride_needed_for_bindings_bits) {
      bool const use_2048_stride_as_1024 = terakan_vk_state_vertex_input_uses_2048_stride_as_1024(
         terakan_gfx_command_writer_physical_device(command_writer));
      u_foreach_bit (
         binding_index,
         vertex_input->sq_resources_fetch_stride.static_stride_needed_for_bindings_bits) {
         terakan_app_config_draw_set_sq_pgm_and_resource_fetch_stride(
            &command_writer->app_config_draw, binding_index,
            vertex_input->sq_resources_fetch_stride.stride[binding_index], use_2048_stride_as_1024);
      }
   }

   if (BITSET_TEST(vertex_input->static_state,
                   TERAKAN_VK_PIPELINE_GRAPHICS_VERTEX_INPUT_STATIC_SQ_PGM_FETCH)) {
      terakan_app_config_draw_set_sq_pgm_fetch_static_fs(&command_writer->app_config_draw,
                                                         &vertex_input->sq_pgm_fetch);
   }
}

/* `terakan_vertex_input_fs_layout::attributes_used` isn't populated, and the fetch shader isn't
 * created (only the layout is initialized), by this function.
 * `bindings_with_2048_stride_as_1024` is initialized regardless of whether the workaround is needed
 * (not known from the arguments).
 */
static void
terakan_vk_pipeline_graphics_vertex_input_init_without_fs_linkage_and_creation(
   struct terakan_vk_pipeline_graphics_vertex_input * const vertex_input,
   struct vk_graphics_pipeline_state const * const state)
{
   if (state->vi != NULL) {
      uint32_t const bindings_valid =
         state->vi->bindings_valid & BITFIELD_MASK(TERAKAN_VK_STATE_MAX_VERTEX_BINDINGS);

      static bool terakan_debug = false;
      static bool terakan_debug_initialized = false;
      if (!terakan_debug_initialized) {
         terakan_debug = getenv("TERAKAN_DEBUG") != NULL && getenv("TERAKAN_DEBUG")[0] != '\0';
         terakan_debug_initialized = true;
      }

      if (terakan_debug) {
         fprintf(stderr, "[TERAKAN] pipeline_vi_init: bindings_valid=0x%x attrs_valid=0x%x\n",
                 bindings_valid, state->vi->attributes_valid);
         u_foreach_bit (bi, bindings_valid) {
            fprintf(stderr, "[TERAKAN]   binding[%u] stride=%u\n",
                    bi, state->vi->bindings[bi].stride);
         }
      }

      /* Always initialize strides from the pipeline as defaults. When MESA_VK_DYNAMIC_VI is set,
       * the application can override strides later via vkCmdSetVertexInputEXT or
       * vkCmdBindVertexBuffers2.
       */
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VI_BINDING_STRIDES) ||
          BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VI)) {
         vertex_input->sq_resources_fetch_stride.static_stride_needed_for_bindings_bits =
            bindings_valid;
         u_foreach_bit (binding_index, bindings_valid) {
            uint16_t const stride = (uint16_t)state->vi->bindings[binding_index].stride;
            vertex_input->sq_resources_fetch_stride.stride[binding_index] = stride;
            if (stride == 2048) {
               vertex_input->sq_pgm_fetch.layout.bindings_with_2048_stride_as_1024 |=
                  BITFIELD_BIT(binding_index);
            }
         }
      }

      u_foreach_bit (attribute_index, state->vi->attributes_valid &
                                         BITFIELD_MASK(TERAKAN_VK_STATE_MAX_VERTEX_ATTRIBUTES)) {
         struct vk_vertex_attribute_state const * const attribute =
            &state->vi->attributes[attribute_index];
         if (!(bindings_valid & BITFIELD_BIT(attribute->binding))) {
            continue;
         }
         /* Disregarding whether the format is supported here, because `terakan_app_config` will
          * handle that anyway, and also VUID-VkVertexInputAttributeDescription-format-00623: "The
          * format features of `format` must contain `VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT`".
          */
         uint32_t const attribute_format_fetch_word1 = terakan_vertex_input_format_fetch_word1(
            &terascale_format_info_r8xx[vk_format_to_pipe_format(attribute->format)]);
         if (G_SQ_VTX_WORD1_DATA_FORMAT(attribute_format_fetch_word1) ==
             TERASCALE_FORMAT_INDEX_INVALID) {
            continue;
         }
          vertex_input->sq_pgm_fetch.layout.attribute_format_fetch_word1[attribute_index] =
             attribute_format_fetch_word1;
          vertex_input->sq_pgm_fetch.layout.attribute_bindings[attribute_index] = attribute->binding;
          vertex_input->sq_pgm_fetch.layout.attribute_offsets[attribute_index] =
             (uint16_t)attribute->offset;

          {
             static bool terakan_vi_debug = false;
             static bool terakan_vi_debug_init = false;
             if (!terakan_vi_debug_init) {
                terakan_vi_debug = getenv("TERAKAN_DEBUG") != NULL;
                terakan_vi_debug_init = true;
             }
             if (terakan_vi_debug) {
                fprintf(stderr, "[TERAKAN] PIPE_VI attr_idx=%u binding=%u offset=%u fmt_fetch_w1=0x%08x\n",
                        attribute_index, attribute->binding,
                        attribute->offset, attribute_format_fetch_word1);
             }
          }
         struct vk_vertex_binding_state const * const attribute_binding =
            &state->vi->bindings[attribute->binding];
         if (attribute_binding->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE) {
            vertex_input->sq_pgm_fetch.layout.instance_rate_attributes |=
               BITFIELD_BIT(attribute_index);
            vertex_input->sq_pgm_fetch.layout.attribute_instance_divisors[attribute_index] =
               attribute_binding->divisor;
         }
      }
      BITSET_SET(vertex_input->static_state,
                 TERAKAN_VK_PIPELINE_GRAPHICS_VERTEX_INPUT_STATIC_SQ_PGM_FETCH);
   }

   if (state->ia != NULL) {
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY)) {
         vertex_input->vgt_primitive_type =
            terakan_vk_state_primitive_topology_vgt_primitive_type(state->ia->primitive_topology);
         BITSET_SET(vertex_input->static_state,
                    TERAKAN_VK_PIPELINE_GRAPHICS_VERTEX_INPUT_STATIC_VGT_PRIMITIVE_TYPE);
      }
   }
}

static void
terakan_vk_pipeline_graphics_pre_rasterization_apply(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_vk_pipeline_graphics_pre_rasterization const * const pre_rasterization)
{
   if (BITSET_TEST(
          pre_rasterization->static_state,
          TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_CL_CLIP_CNTL_DX_RASTERIZATION_KILL)) {
      terakan_app_config_draw_set_pa_cl_clip_cntl_dx_rasterization_kill(
         &command_writer->app_config_draw,
         pre_rasterization->pa_cl_clip_cntl_dx_rasterization_kill);
   }

   if (BITSET_TEST(
          pre_rasterization->static_state,
          TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_CL_CLIP_CNTL_DX_CLIP_SPACE_DEF)) {
      terakan_app_config_draw_set_pa_cl_clip_cntl_dx_clip_space_def(
         &command_writer->app_config_draw, pre_rasterization->pa_cl_clip_cntl_dx_clip_space_def);
   }

   if (BITSET_TEST(
          pre_rasterization->static_state,
          TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_CL_CLIP_CNTL_Z_CLAMP_ENABLE)) {
      terakan_app_config_draw_set_pa_cl_clip_cntl_z_clamp_enable(
         &command_writer->app_config_draw, pre_rasterization->pa_cl_clip_cntl_z_clamp_enable);
   }

   if (BITSET_TEST(
          pre_rasterization->static_state,
          TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_CL_CLIP_CNTL_Z_CLIP_ENABLE_OVERRIDE)) {
      terakan_app_config_draw_set_pa_cl_clip_cntl_z_clip_enable_override(
         &command_writer->app_config_draw,
         pre_rasterization->pa_cl_clip_cntl_z_clip_enable_override);
   }

   if (BITSET_TEST(
          pre_rasterization->static_state,
          TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_USER_DEFINED_ZMIN_ZMAX)) {
      terakan_app_config_draw_set_pa_vport_user_defined_zmin_zmax_enable(
         &command_writer->app_config_draw,
         pre_rasterization->pa_vport_user_defined_zmin_zmax_enable);
      if (pre_rasterization->pa_vport_user_defined_zmin_zmax_enable) {
         terakan_app_config_draw_set_pa_vport_user_defined_zmin_zmax(
            &command_writer->app_config_draw, pre_rasterization->pa_vport_user_defined_zmin_zmax[0],
            pre_rasterization->pa_vport_user_defined_zmin_zmax[1]);
      }
   }

   if (BITSET_TEST(pre_rasterization->static_state,
                   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_VPORT_COUNT)) {
      terakan_app_config_draw_set_pa_vport_vport_count(&command_writer->app_config_draw,
                                                       pre_rasterization->pa_vport_vport_count);
   }

   if (BITSET_TEST(pre_rasterization->static_state,
                   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_VPORTS)) {
      for (unsigned vport_index = 0; vport_index < pre_rasterization->pa_vport_vport_count;
           ++vport_index) {
         terakan_app_config_draw_set_pa_vport_vport(
            &command_writer->app_config_draw, vport_index,
            &pre_rasterization->pa_vport_vports[vport_index]);
      }
   }

   if (BITSET_TEST(
          pre_rasterization->static_state,
          TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_EXPLICIT_SCISSOR_COUNT)) {
      terakan_app_config_draw_set_pa_vport_explicit_scissor_count(
         &command_writer->app_config_draw, pre_rasterization->pa_vport_explicit_scissor_count);
   }

   if (BITSET_TEST(
          pre_rasterization->static_state,
          TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_EXPLICIT_SCISSORS)) {
      for (unsigned scissor_index = 0;
           scissor_index < pre_rasterization->pa_vport_explicit_scissor_count; ++scissor_index) {
         terakan_app_config_draw_set_pa_vport_explicit_scissor(
            &command_writer->app_config_draw, scissor_index,
            pre_rasterization->pa_vport_explicit_scissors[scissor_index]);
      }
   }

   terakan_app_config_draw_set_pa_su_sc_mode_cntl(&command_writer->app_config_draw,
                                                  pre_rasterization->pa_su_sc_mode_cntl_keep,
                                                  pre_rasterization->pa_su_sc_mode_cntl);

   if (BITSET_TEST(pre_rasterization->static_state,
                   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_SU_POLY_OFFSET)) {
      terakan_app_config_draw_set_pa_su_poly_offset(&command_writer->app_config_draw,
                                                    pre_rasterization->pa_su_poly_offset.offset);
      terakan_app_config_draw_set_pa_su_poly_offset_db_fmt_cntl(
         &command_writer->app_config_draw, pre_rasterization->pa_su_poly_offset.representation,
         pre_rasterization->pa_su_poly_offset_exact);
   }

   if (BITSET_TEST(
          pre_rasterization->static_state,
          TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_SC_LINE_STIPPLE_PATTERN)) {
      terakan_app_config_draw_set_pa_sc_line_stipple_pattern(
         &command_writer->app_config_draw, pre_rasterization->pa_sc_line_stipple_pattern);
   }
}

static void
terakan_vk_pipeline_graphics_pre_rasterization_init(
   struct terakan_vk_pipeline_graphics_pre_rasterization * const pre_rasterization,
   struct vk_graphics_pipeline_state const * const state)
{
   pre_rasterization->pa_su_sc_mode_cntl_keep = ~(uint32_t)0;

   if (state->rs != NULL) {
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_RASTERIZER_DISCARD_ENABLE)) {
         pre_rasterization->pa_cl_clip_cntl_dx_rasterization_kill =
            state->rs->rasterizer_discard_enable;
         BITSET_SET(
            pre_rasterization->static_state,
            TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_CL_CLIP_CNTL_DX_RASTERIZATION_KILL);
         if (pre_rasterization->pa_cl_clip_cntl_dx_rasterization_kill) {
            /* The rest of the state initialized in this function has no effect and doesn't need to
             * be applied.
             */
            return;
         }
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_CLAMP_ENABLE)) {
         pre_rasterization->pa_cl_clip_cntl_z_clamp_enable = state->rs->depth_clamp_enable;
         BITSET_SET(
            pre_rasterization->static_state,
            TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_CL_CLIP_CNTL_Z_CLAMP_ENABLE);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_CLIP_ENABLE)) {
         pre_rasterization->pa_cl_clip_cntl_z_clip_enable_override =
            terakan_vk_state_depth_clip_enable_to_override(state->rs->depth_clip_enable);
         BITSET_SET(
            pre_rasterization->static_state,
            TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_CL_CLIP_CNTL_Z_CLIP_ENABLE_OVERRIDE);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE)) {
         pre_rasterization->pa_su_sc_mode_cntl_keep &=
            TERAKAN_VK_STATE_POLYGON_MODE_PA_SU_SC_MODE_CNTL_CLEAR;
         pre_rasterization->pa_su_sc_mode_cntl |=
            terakan_vk_state_polygon_mode_pa_su_sc_mode_cntl(state->rs->polygon_mode);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_CULL_MODE)) {
         pre_rasterization->pa_su_sc_mode_cntl_keep &= C_028814_CULL_FRONT & C_028814_CULL_BACK;
         pre_rasterization->pa_su_sc_mode_cntl |=
            S_028814_CULL_FRONT((state->rs->cull_mode & VK_CULL_MODE_FRONT_BIT) != 0) |
            S_028814_CULL_BACK((state->rs->cull_mode & VK_CULL_MODE_BACK_BIT) != 0);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_FRONT_FACE)) {
         pre_rasterization->pa_su_sc_mode_cntl_keep &= C_028814_FACE;
         pre_rasterization->pa_su_sc_mode_cntl |= S_028814_FACE(state->rs->front_face);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX)) {
         pre_rasterization->pa_su_sc_mode_cntl_keep &= C_028814_PROVOKING_VTX_LAST;
         pre_rasterization->pa_su_sc_mode_cntl |= S_028814_PROVOKING_VTX_LAST(
            state->rs->provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE)) {
         pre_rasterization->pa_su_sc_mode_cntl_keep &=
            TERAKAN_VK_STATE_DEPTH_BIAS_ENABLE_PA_SU_SC_MODE_CNTL_CLEAR;
         if (state->rs->depth_bias.enable) {
            pre_rasterization->pa_su_sc_mode_cntl |=
               TERAKAN_VK_STATE_DEPTH_BIAS_ENABLE_PA_SU_SC_MODE_CNTL;
         }
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS) &&
          (BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE) ||
           state->rs->depth_bias.enable)) {
         pre_rasterization->pa_su_poly_offset.offset.slope_scale_per_16th_subpixel =
            state->rs->depth_bias.slope_factor * 16.0f;
         pre_rasterization->pa_su_poly_offset.offset.constant_offset =
            state->rs->depth_bias.constant_factor;
         pre_rasterization->pa_su_poly_offset.offset.clamp = state->rs->depth_bias.clamp;
         pre_rasterization->pa_su_poly_offset.representation =
            terakan_vk_state_depth_bias_poly_offset_representation(
               state->rs->depth_bias.representation);
         pre_rasterization->pa_su_poly_offset_exact = state->rs->depth_bias.exact;
         BITSET_SET(pre_rasterization->static_state,
                    TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_SU_POLY_OFFSET);
      }

      bool const rs_line_stipple_enable_static =
         !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE_ENABLE);
      if (rs_line_stipple_enable_static) {
         pre_rasterization->pa_sc_line_stipple_enable = state->rs->line.stipple.enable;
         BITSET_SET(
            pre_rasterization->static_state,
            TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_SC_LINE_STIPPLE_ENABLE);
      }

      if ((!rs_line_stipple_enable_static || state->rs->line.stipple.enable) &&
          !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE)) {
         pre_rasterization->pa_sc_line_stipple_pattern =
            S_028A0C_LINE_PATTERN(state->rs->line.stipple.pattern) |
            S_028A0C_REPEAT_COUNT(state->rs->line.stipple.factor - 1);
         BITSET_SET(
            pre_rasterization->static_state,
            TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_SC_LINE_STIPPLE_PATTERN);
      }
   }

   if (state->vp != NULL) {
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VP_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE)) {
         pre_rasterization->pa_cl_clip_cntl_dx_clip_space_def =
            !state->vp->depth_clip_negative_one_to_one;
         BITSET_SET(
            pre_rasterization->static_state,
            TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_CL_CLIP_CNTL_DX_CLIP_SPACE_DEF);
      }

      bool const vp_viewports_static = !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VP_VIEWPORTS);
      if (vp_viewports_static || !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT)) {
         assert(state->vp->viewport_count <= TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
         /* #MemoryIntegrity: Prevent writing out of the array bounds in `terakan_app_config` in
          * case of invalid usage.
          */
         uint32_t const vport_count =
            MIN2(state->vp->viewport_count, TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
         pre_rasterization->pa_vport_vport_count = vport_count;
         BITSET_SET(pre_rasterization->static_state,
                    TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_VPORT_COUNT);
         if (vp_viewports_static) {
            for (uint32_t vport_index = 0; vport_index < vport_count; ++vport_index) {
               pre_rasterization->pa_vport_vports[vport_index] =
                  terakan_vk_state_viewport_to_hw(&state->vp->viewports[vport_index]);
            }
            BITSET_SET(pre_rasterization->static_state,
                       TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_VPORTS);
         }
      }

      bool const vp_scissors_static = !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VP_SCISSORS);
      if (vp_scissors_static || !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VP_SCISSOR_COUNT)) {
         assert(state->vp->scissor_count <= TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
         /* #MemoryIntegrity: Prevent writing out of the array bounds in `terakan_app_config` in
          * case of invalid usage.
          */
         uint32_t const scissor_count =
            MIN2(state->vp->scissor_count, TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
         pre_rasterization->pa_vport_explicit_scissor_count = scissor_count;
         BITSET_SET(
            pre_rasterization->static_state,
            TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_EXPLICIT_SCISSOR_COUNT);
         if (vp_scissors_static) {
            struct terakan_screen_rect const screen =
               terakan_screen_rect_square(TERAKAN_IMAGE_MAX_WIDTH_HEIGHT);
            for (uint32_t scissor_index = 0; scissor_index < scissor_count; ++scissor_index) {
               pre_rasterization->pa_vport_explicit_scissors[scissor_index] =
                  terakan_vk_rect_to_screen_rect(state->vp->scissors[scissor_index], screen);
            }
            BITSET_SET(
               pre_rasterization->static_state,
               TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_EXPLICIT_SCISSORS);
         }
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VP_DEPTH_CLAMP_RANGE)) {
         if (state->vp->depth_clamp_mode == VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT) {
            pre_rasterization->pa_vport_user_defined_zmin_zmax_enable = true;
            pre_rasterization->pa_vport_user_defined_zmin_zmax[0] =
               state->vp->depth_clamp_range.minDepthClamp;
            pre_rasterization->pa_vport_user_defined_zmin_zmax[1] =
               state->vp->depth_clamp_range.maxDepthClamp;
         }
         BITSET_SET(
            pre_rasterization->static_state,
            TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_USER_DEFINED_ZMIN_ZMAX);
      }
   }
}

static void
terakan_vk_pipeline_graphics_multisample_apply(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_vk_pipeline_graphics_multisample const * const multisample)
{
   if (BITSET_TEST(multisample->static_state,
                   TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_PA_SC_AA_MASK)) {
      terakan_app_config_draw_set_pa_sc_aa_mask(&command_writer->app_config_draw,
                                                multisample->pa_sc_aa_mask);
   }

   if (BITSET_TEST(
          multisample->static_state,
          TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_PA_SC_AA_CONFIG_MSAA_NUM_SAMPLES)) {
      terakan_app_config_draw_set_pa_sc_aa_config_msaa_num_samples_log2(
         &command_writer->app_config_draw, multisample->pa_sc_aa_config_msaa_num_samples_log2);
   }

   if (BITSET_TEST(
          multisample->static_state,
          TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_PA_SC_AA_SAMPLE_LOCS_CUSTOM_ENABLE)) {
      terakan_app_config_draw_set_pa_sc_aa_sample_locs_custom_enable(
         &command_writer->app_config_draw, multisample->pa_sc_aa_sample_locs_custom_enable);
   }

   if (BITSET_TEST(multisample->static_state,
                   TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_PA_SC_AA_SAMPLE_LOCS_CUSTOM)) {
      terakan_app_config_draw_set_pa_sc_aa_sample_locs_custom_16_samples_2x2_locs(
         &command_writer->app_config_draw, multisample->pa_sc_aa_sample_locs_custom[0]);
   }

   if (BITSET_TEST(multisample->static_state,
                   TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_DB_ALPHA_TO_MASK_ENABLE)) {
      terakan_app_config_draw_set_db_alpha_to_mask(
         &command_writer->app_config_draw, C_028B70_ALPHA_TO_MASK_ENABLE,
         S_028B70_ALPHA_TO_MASK_ENABLE(multisample->db_alpha_to_mask_enable));
   }
}

static VkResult
terakan_vk_pipeline_graphics_multisample_init_with_rasterization(
   struct terakan_vk_pipeline_graphics_multisample * const multisample,
   struct vk_graphics_pipeline_state const * const state,
   struct terakan_device const * const device)
{
   if (state->ms != NULL) {
      /* TODO(Triang3l): If the rasterization sample count becomes useful in shader compilation, but
       * it's dynamic, try to obtain it from the render pass attachments if it's available.
       */
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES)) {
         int const rasterization_samples_log2 = terakan_image_vk_sample_count_to_hw_log2(
            state->ms->rasterization_samples,
            terakan_device_physical_device(device)->chip_info.is_r9xx);
         if (unlikely(rasterization_samples_log2 < 0)) {
            /* #MemoryIntegrity. */
            return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                             "Static pipeline state rasterization sample count %s is not supported",
                             vk_SampleCountFlagBits_to_str(state->ms->rasterization_samples));
         }
         multisample->pa_sc_aa_config_msaa_num_samples_log2 = (uint8_t)rasterization_samples_log2;
         BITSET_SET(
            multisample->static_state,
            TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_PA_SC_AA_CONFIG_MSAA_NUM_SAMPLES);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_MASK)) {
         multisample->pa_sc_aa_mask = (uint16_t)state->ms->sample_mask;
         BITSET_SET(multisample->static_state,
                    TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_PA_SC_AA_MASK);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_ALPHA_TO_COVERAGE_ENABLE)) {
         multisample->db_alpha_to_mask_enable = state->ms->alpha_to_coverage_enable;
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_LOCATIONS_ENABLE)) {
         multisample->pa_sc_aa_sample_locs_custom_enable = state->ms->sample_locations_enable;
         BITSET_SET(
            multisample->static_state,
            TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_PA_SC_AA_SAMPLE_LOCS_CUSTOM_ENABLE);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_LOCATIONS)) {
         terakan_vk_state_sample_locations_mesa_to_hw(state->ms->sample_locations,
                                                      multisample->pa_sc_aa_sample_locs_custom[0]);
         BITSET_SET(multisample->static_state,
                    TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_PA_SC_AA_SAMPLE_LOCS_CUSTOM);
      }
   }

   return VK_SUCCESS;
}

static void
terakan_vk_pipeline_graphics_fragment_shading_apply(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_vk_pipeline_graphics_fragment_shading const * const fragment_shading)
{
   terakan_app_config_draw_set_db_stencilrefmask(&command_writer->app_config_draw, false,
                                                 fragment_shading->db_stencilrefmask.keep_mask,
                                                 fragment_shading->db_stencilrefmask.front);
   terakan_app_config_draw_set_db_stencilrefmask(&command_writer->app_config_draw, true,
                                                 fragment_shading->db_stencilrefmask.keep_mask,
                                                 fragment_shading->db_stencilrefmask.back);

   terakan_app_config_draw_set_db_depth_control(&command_writer->app_config_draw,
                                                fragment_shading->db_depth_control_keep,
                                                fragment_shading->db_depth_control);

   terakan_app_config_draw_set_db_eqaa_ps_iter_max_invocation_samples_log2(
      &command_writer->app_config_draw,
      fragment_shading->db_eqaa_ps_iter_max_invocation_samples_log2);
}

static void
terakan_vk_pipeline_graphics_fragment_shading_init_empty(
   struct terakan_vk_pipeline_graphics_fragment_shading * const fragment_shading)
{
   fragment_shading->db_stencilrefmask.keep_mask = ~(uint32_t)0b0;
   fragment_shading->db_depth_control_keep = ~(uint32_t)0b0;
}

static void
terakan_vk_pipeline_graphics_fragment_shading_init_with_rasterization(
   struct terakan_vk_pipeline_graphics_fragment_shading * const fragment_shading,
   struct vk_graphics_pipeline_state const * const state)
{
   terakan_vk_pipeline_graphics_fragment_shading_init_empty(fragment_shading);

   if (state->ds != NULL) {
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE)) {
         fragment_shading->db_depth_control_keep &= C_028800_Z_ENABLE;
         fragment_shading->db_depth_control |= S_028800_Z_ENABLE(state->ds->depth.test_enable);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE)) {
         fragment_shading->db_depth_control_keep &= C_028800_Z_WRITE_ENABLE;
         fragment_shading->db_depth_control |=
            S_028800_Z_WRITE_ENABLE(state->ds->depth.write_enable);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_COMPARE_OP)) {
         fragment_shading->db_depth_control_keep &= C_028800_ZFUNC;
         fragment_shading->db_depth_control |= S_028800_ZFUNC(state->ds->depth.compare_op);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE)) {
         fragment_shading->db_depth_control_keep &=
            TERAKAN_VK_STATE_STENCIL_TEST_ENABLE_DB_DEPTH_CONTROL_CLEAR;
         if (state->ds->stencil.test_enable) {
            fragment_shading->db_depth_control |=
               TERAKAN_VK_STATE_STENCIL_TEST_ENABLE_DB_DEPTH_CONTROL;
         }
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_OP)) {
         fragment_shading->db_depth_control_keep &=
            TERAKAN_VK_STATE_STENCIL_OP_DB_DEPTH_CONTROL_CLEAR;
         fragment_shading->db_depth_control |=
            S_028800_STENCILFUNC(state->ds->stencil.front.op.compare) |
            S_028800_STENCILFAIL(state->ds->stencil.front.op.fail) |
            S_028800_STENCILZPASS(state->ds->stencil.front.op.pass) |
            S_028800_STENCILZFAIL(state->ds->stencil.front.op.depth_fail) |
            S_028800_STENCILFUNC_BF(state->ds->stencil.back.op.compare) |
            S_028800_STENCILFAIL_BF(state->ds->stencil.back.op.fail) |
            S_028800_STENCILZPASS_BF(state->ds->stencil.back.op.pass) |
            S_028800_STENCILZFAIL_BF(state->ds->stencil.back.op.depth_fail);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_COMPARE_MASK)) {
         fragment_shading->db_stencilrefmask.keep_mask &= C_028430_STENCILMASK;
         fragment_shading->db_stencilrefmask.front |=
            S_028430_STENCILMASK(state->ds->stencil.front.compare_mask);
         fragment_shading->db_stencilrefmask.back |=
            S_028434_STENCILMASK_BF(state->ds->stencil.back.compare_mask);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK)) {
         fragment_shading->db_stencilrefmask.keep_mask &= C_028430_STENCILWRITEMASK;
         fragment_shading->db_stencilrefmask.front |=
            S_028430_STENCILWRITEMASK(state->ds->stencil.front.write_mask);
         fragment_shading->db_stencilrefmask.back |=
            S_028434_STENCILWRITEMASK_BF(state->ds->stencil.back.write_mask);
      }

      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_REFERENCE)) {
         fragment_shading->db_stencilrefmask.keep_mask &= C_028430_STENCILREF;
         fragment_shading->db_stencilrefmask.front |=
            S_028430_STENCILREF(state->ds->stencil.front.reference);
         fragment_shading->db_stencilrefmask.back |=
            S_028434_STENCILREF_BF(state->ds->stencil.back.reference);
      }
   }

   fragment_shading->db_eqaa_ps_iter_max_invocation_samples_log2 =
      state->ms != NULL && state->ms->sample_shading_enable
         ? terakan_hw_config_draw_db_eqaa_ps_iter_max_invocation_samples_log2(
              state->ms->min_sample_shading)
         : -1;
}

static void
terakan_vk_pipeline_graphics_fragment_output_apply(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_vk_pipeline_graphics_fragment_output const * const fragment_output)
{
   if (BITSET_TEST(fragment_output->static_state,
                   TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_ROP3_ENABLE)) {
      terakan_app_config_draw_set_cb_rop3_enable(&command_writer->app_config_draw,
                                                 fragment_output->cb_rop3_enable);
   }

   if (BITSET_TEST(fragment_output->static_state,
                   TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_ROP3)) {
      terakan_app_config_draw_set_cb_rop3(&command_writer->app_config_draw,
                                          fragment_output->cb_rop3);
   }

   if (BITSET_TEST(
          fragment_output->static_state,
          TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_COLOR_RTV_WRITE_ENABLE_MASK)) {
      terakan_app_config_draw_set_cb_color_rtv_write_enable_mask_all(
         &command_writer->app_config_draw,
         fragment_output->cb_color_rtv_write_potentially_enabled_mask);
   }

   if (BITSET_TEST(
          fragment_output->static_state,
          TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_COLOR_RTV_WRITE_COMPONENT_MASK)) {
      terakan_app_config_draw_set_cb_color_rtv_write_component_mask_all(
         &command_writer->app_config_draw, fragment_output->cb_color_rtv_write_component_mask);
   }

   if ((uint32_t)~fragment_output->cb_blend_control_keep) {
      u_foreach_bit (attachment_index,
                     fragment_output->cb_color_rtv_write_potentially_enabled_mask) {
         terakan_app_config_draw_set_cb_blend_control(
            &command_writer->app_config_draw, attachment_index,
            fragment_output->cb_blend_control_keep,
            fragment_output->cb_blend_control[attachment_index]);
      }
   }

   if (BITSET_TEST(fragment_output->static_state,
                   TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_BLEND_CONSTANTS)) {
      terakan_app_config_draw_set_cb_blend_constants(&command_writer->app_config_draw,
                                                     fragment_output->cb_blend_constants);
   }
}

static void
terakan_vk_pipeline_graphics_fragment_output_init_empty(
   struct terakan_vk_pipeline_graphics_fragment_output * const fragment_output)
{
   fragment_output->cb_blend_control_keep = ~(uint32_t)0b0;
}

static void
terakan_vk_pipeline_graphics_fragment_output_init_with_rasterization(
   struct terakan_vk_pipeline_graphics_fragment_output * const fragment_output,
   struct vk_graphics_pipeline_state const * const state)
{
   terakan_vk_pipeline_graphics_fragment_output_init_empty(fragment_output);

   if (state->cb != NULL) {
      fragment_output->cb_color_rtv_write_potentially_enabled_mask =
         BITFIELD_MASK(TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS);
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_ATTACHMENT_COUNT)) {
         fragment_output->cb_color_rtv_write_potentially_enabled_mask &= BITFIELD_MASK(
            MIN2(state->cb->attachment_count, TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS));
      }
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_COLOR_WRITE_ENABLES)) {
         fragment_output->cb_color_rtv_write_potentially_enabled_mask &=
            state->cb->color_write_enables;
         BITSET_SET(
            fragment_output->static_state,
            TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_COLOR_RTV_WRITE_ENABLE_MASK);
      }
      bool const cb_write_masks_dynamic =
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_WRITE_MASKS);
      if (!cb_write_masks_dynamic) {
         u_foreach_bit (attachment_index,
                        fragment_output->cb_color_rtv_write_potentially_enabled_mask) {
            uint8_t const attachment_write_mask =
               state->cb->attachments[attachment_index].write_mask & 0xF;
            if (attachment_write_mask) {
               fragment_output->cb_color_rtv_write_component_mask |= (uint32_t)attachment_write_mask
                                                                     << (4 * attachment_index);
            } else {
               fragment_output->cb_color_rtv_write_potentially_enabled_mask &=
                  ~BITFIELD_BIT(attachment_index);
            }
         }
         if (fragment_output->cb_color_rtv_write_potentially_enabled_mask) {
            BITSET_SET(
               fragment_output->static_state,
               TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_COLOR_RTV_WRITE_COMPONENT_MASK);
         }
      }

      if (fragment_output->cb_color_rtv_write_potentially_enabled_mask) {
         bool const logic_op_enable_dynamic =
            BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_LOGIC_OP_ENABLE);
         if (!logic_op_enable_dynamic) {
            fragment_output->cb_rop3_enable = state->cb->logic_op_enable;
            BITSET_SET(fragment_output->static_state,
                       TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_ROP3_ENABLE);
         }
         if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_LOGIC_OP) &&
             (logic_op_enable_dynamic || state->cb->logic_op_enable)) {
            fragment_output->cb_rop3 = terakan_vk_state_logic_op_rop3(state->cb->logic_op);
            BITSET_SET(fragment_output->static_state,
                       TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_ROP3);
         }

         /* Section "Logical Operations" of the Vulkan 1.4.344 specification says:
          *
          *     "If logicOpEnable is VK_TRUE, then a logical operation selected by logicOp is
          *     applied between each color attachment and the fragment’s corresponding output value,
          *     and blending of all attachments is treated as if it were disabled."
          */
         if (logic_op_enable_dynamic || !state->cb->logic_op_enable) {
            bool const blend_enables_dynamic =
               BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_BLEND_ENABLES);
            bool blend_potentially_enabled = blend_enables_dynamic;
            if (!blend_enables_dynamic) {
               fragment_output->cb_blend_control_keep &= C_028780_BLEND_CONTROL_ENABLE;
               u_foreach_bit (attachment_index,
                              fragment_output->cb_color_rtv_write_potentially_enabled_mask) {
                  if (!state->cb->attachments[attachment_index].blend_enable) {
                     continue;
                  }
                  blend_potentially_enabled = true;
                  fragment_output->cb_blend_control[attachment_index] |=
                     S_028780_BLEND_CONTROL_ENABLE(1);
               }
            }
            if (blend_potentially_enabled) {
               bool const blend_equations_dynamic =
                  BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_BLEND_EQUATIONS);
               uint32_t blend_factors_statically_used = 0b0;
               if (!blend_equations_dynamic) {
                  fragment_output->cb_blend_control_keep &=
                     C_028780_COLOR_SRCBLEND & C_028780_COLOR_DESTBLEND & C_028780_COLOR_COMB_FCN &
                     C_028780_ALPHA_SRCBLEND & C_028780_ALPHA_DESTBLEND & C_028780_ALPHA_COMB_FCN &
                     C_028780_SEPARATE_ALPHA_BLEND;
                  u_foreach_bit (attachment_index,
                                 fragment_output->cb_color_rtv_write_potentially_enabled_mask) {
                     struct vk_color_blend_attachment_state const * const attachment =
                        &state->cb->attachments[attachment_index];
                     if (!blend_enables_dynamic && !attachment->blend_enable) {
                        continue;
                     }
                     uint32_t blend_control_equation = S_028780_SEPARATE_ALPHA_BLEND(1);
                     /* Color blend equation. */
                     if (cb_write_masks_dynamic || (attachment->write_mask & 0b0111)) {
                        uint32_t const comb_fcn =
                           terakan_vk_state_blend_op_to_hw(attachment->color_blend_op);
                        blend_control_equation |= S_028780_COLOR_COMB_FCN(comb_fcn);
                        if (terakan_hw_config_draw_cb_blend_control_comb_fcn_uses_factors(
                               comb_fcn)) {
                           uint32_t const src_factor = terakan_vk_state_blend_factor_to_hw(
                              attachment->src_color_blend_factor);
                           uint32_t const dst_factor = terakan_vk_state_blend_factor_to_hw(
                              attachment->dst_color_blend_factor);
                           blend_control_equation |= S_028780_COLOR_SRCBLEND(src_factor) |
                                                     S_028780_COLOR_DESTBLEND(dst_factor);
                           blend_factors_statically_used |=
                              BITFIELD_BIT(src_factor) | BITFIELD_BIT(dst_factor);
                        } else {
                           blend_control_equation |= S_028780_COLOR_SRCBLEND(V_028780_BLEND_ONE) |
                                                     S_028780_COLOR_DESTBLEND(V_028780_BLEND_ZERO);
                        }
                     } else {
                        blend_control_equation |=
                           S_028780_COLOR_SRCBLEND(V_028780_BLEND_ONE) |
                           S_028780_COLOR_DESTBLEND(V_028780_BLEND_ZERO) |
                           S_028780_COLOR_COMB_FCN(V_028780_COMB_DST_PLUS_SRC);
                     }
                     /* Alpha blend equation. */
                     if (cb_write_masks_dynamic || (attachment->write_mask & 0b1000)) {
                        uint32_t const comb_fcn =
                           terakan_vk_state_blend_op_to_hw(attachment->alpha_blend_op);
                        blend_control_equation |= S_028780_ALPHA_COMB_FCN(comb_fcn);
                        if (terakan_hw_config_draw_cb_blend_control_comb_fcn_uses_factors(
                               comb_fcn)) {
                           uint32_t const src_factor =
                              terakan_hw_config_draw_cb_blend_control_color_factors_for_color_alpha
                                 [terakan_vk_state_blend_factor_to_hw(
                                    attachment->src_alpha_blend_factor)];
                           uint32_t const dst_factor =
                              terakan_hw_config_draw_cb_blend_control_color_factors_for_color_alpha
                                 [terakan_vk_state_blend_factor_to_hw(
                                    attachment->dst_alpha_blend_factor)];
                           blend_control_equation |= S_028780_ALPHA_SRCBLEND(src_factor) |
                                                     S_028780_ALPHA_DESTBLEND(dst_factor);
                           blend_factors_statically_used |=
                              BITFIELD_BIT(src_factor) | BITFIELD_BIT(dst_factor);
                        } else {
                           blend_control_equation |= S_028780_ALPHA_SRCBLEND(V_028780_BLEND_ONE) |
                                                     S_028780_ALPHA_DESTBLEND(V_028780_BLEND_ZERO);
                        }
                     } else {
                        blend_control_equation |=
                           S_028780_ALPHA_SRCBLEND(V_028780_BLEND_ONE) |
                           S_028780_ALPHA_DESTBLEND(V_028780_BLEND_ZERO) |
                           S_028780_ALPHA_COMB_FCN(V_028780_COMB_DST_PLUS_SRC);
                     }
                     fragment_output->cb_blend_control[attachment_index] |= blend_control_equation;
                  }
               }
               if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS) &&
                   (blend_equations_dynamic ||
                    (blend_factors_statically_used &
                     TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_CONST))) {
                  memcpy(fragment_output->cb_blend_constants, state->cb->blend_constants,
                         sizeof(float) * 4);
                  BITSET_SET(
                     fragment_output->static_state,
                     TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_BLEND_CONSTANTS);
               }
            }
         }
      }
   }
}

static void
terakan_vk_pipeline_graphics_cmd_bind(struct vk_command_buffer * const command_buffer_base,
                                      struct vk_pipeline * const pipeline_base)
{
   struct terakan_command_buffer * const command_buffer =
      container_of(command_buffer_base, struct terakan_command_buffer, vk);
   struct terakan_gfx_command_writer * const command_writer = command_buffer->command_writer.gfx;
   struct terakan_vk_pipeline_graphics const * const pipeline =
      container_of(pipeline_base, struct terakan_vk_pipeline_graphics const, vk);

   BITSET_COPY(command_buffer->graphics_state_is_dynamic, pipeline->dynamic_state);
   /* For consistency with the dynamic state in the `vk_command_buffer`, mark the state that's
    * static in the pipeline object as dirty in the command buffer's dynamic state so it's reapplied
    * when it becomes dynamic again.
    */
   for (unsigned word_index = 0; word_index < BITSET_WORDS(MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX);
        ++word_index) {
      command_buffer_base->dynamic_graphics_state.dirty[word_index] |=
         ~pipeline->dynamic_state[word_index];
   }

   terakan_vk_pipeline_graphics_vertex_input_apply(command_writer, &pipeline->vertex_input);

   assert(pipeline->shader_stages & VK_SHADER_STAGE_VERTEX_BIT);
   bool const pipeline_has_geometry_shader =
      (pipeline->shader_stages & VK_SHADER_STAGE_GEOMETRY_BIT) != 0;
   if (!(~pipeline->shader_stages & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                     VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT))) {
      terakan_app_config_draw_set_sq_pgm_pre_rasterization_vertex(
         &command_writer->app_config_draw, &pipeline->shaders[MESA_SHADER_VERTEX], NULL, NULL);
      terakan_app_config_draw_set_sq_pgm_pre_rasterization_tessellation_control(
         &command_writer->app_config_draw, &pipeline->shaders[MESA_SHADER_TESS_CTRL]);
      terakan_app_config_draw_set_sq_pgm_pre_rasterization_tessellation_evaluation(
         &command_writer->app_config_draw,
         pipeline_has_geometry_shader ? &pipeline->shaders[MESA_SHADER_TESS_EVAL] : NULL,
         pipeline_has_geometry_shader ? NULL : &pipeline->shaders[MESA_SHADER_TESS_EVAL]);
   } else {
      terakan_app_config_draw_set_sq_pgm_pre_rasterization_vertex(
         &command_writer->app_config_draw, NULL,
         pipeline_has_geometry_shader ? &pipeline->shaders[MESA_SHADER_VERTEX] : NULL,
         pipeline_has_geometry_shader ? NULL : &pipeline->shaders[MESA_SHADER_VERTEX]);
      terakan_app_config_draw_set_sq_pgm_pre_rasterization_tessellation_control(
         &command_writer->app_config_draw, NULL);
      terakan_app_config_draw_set_sq_pgm_pre_rasterization_tessellation_evaluation(
         &command_writer->app_config_draw, NULL, NULL);
   }
   terakan_app_config_draw_set_sq_pgm_pre_rasterization_geometry(
      &command_writer->app_config_draw,
      pipeline_has_geometry_shader ? &pipeline->shaders[MESA_SHADER_GEOMETRY] : NULL);
   terakan_vk_pipeline_graphics_pre_rasterization_apply(command_writer,
                                                        &pipeline->pre_rasterization);

   terakan_vk_pipeline_graphics_multisample_apply(command_writer, &pipeline->multisample);

   terakan_app_config_draw_set_sq_pgm_fragment(
      &command_writer->app_config_draw, pipeline->shader_stages & VK_SHADER_STAGE_FRAGMENT_BIT
                                           ? &pipeline->shaders[MESA_SHADER_FRAGMENT]
                                           : NULL);
   terakan_vk_pipeline_graphics_fragment_shading_apply(command_writer, &pipeline->fragment_shading);

   terakan_vk_pipeline_graphics_fragment_output_apply(command_writer, &pipeline->fragment_output);
}

static VkResult
terakan_vk_pipeline_graphics_get_executable_properties(
   UNUSED struct vk_device * const device_base, struct vk_pipeline * const pipeline_base,
   uint32_t * const executable_count, VkPipelineExecutablePropertiesKHR * const properties)
{
   /* TODO(Triang3l): Pipeline executables (fetch shader and other shaders). */
   *executable_count = 0;
   return VK_SUCCESS;
}

static void
terakan_vk_pipeline_graphics_destroy(struct vk_device * const device_base,
                                     struct vk_pipeline * const pipeline_base,
                                     VkAllocationCallbacks const * const allocator)
{
   struct terakan_vk_pipeline_graphics * const pipeline =
      container_of(pipeline_base, struct terakan_vk_pipeline_graphics, vk);

   u_foreach_bit (shader_stage_vk_bit_index, pipeline->shader_stages) {
      terakan_shader_impl_finish(&pipeline->shaders[vk_to_mesa_shader_stage(
         (VkShaderStageFlagBits)BITFIELD_BIT(shader_stage_vk_bit_index))]);
   }

   if (pipeline->shader_bo != NULL) {
      terakan_bo_free(pipeline->shader_bo, allocator);
   }

   vk_pipeline_free(device_base, allocator, &pipeline->vk);
}

struct vk_pipeline_ops const terakan_vk_pipeline_graphics_ops = {
   .destroy = terakan_vk_pipeline_graphics_destroy,
   .get_executable_properties = terakan_vk_pipeline_graphics_get_executable_properties,
   .cmd_bind = terakan_vk_pipeline_graphics_cmd_bind,
};

static VkResult
terakan_vk_pipeline_graphics_create(struct terakan_device * const device,
                                    VkGraphicsPipelineCreateInfo const * const create_info,
                                    VkAllocationCallbacks const * const allocator,
                                    struct terakan_vk_pipeline_graphics ** const pipeline_out)
{
   VkPipelineCreateFlags2KHR const create_flags = vk_graphics_pipeline_create_flags(create_info);
   struct terakan_vk_pipeline_graphics * const pipeline = vk_pipeline_zalloc(
      &device->vk, &terakan_vk_pipeline_graphics_ops, VK_PIPELINE_BIND_POINT_GRAPHICS, create_flags,
      allocator, sizeof(struct terakan_vk_pipeline_graphics));
   if (pipeline == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   VkResult result;

   /* TODO(Triang3l): Pipeline libraries. */

   /* TODO(Triang3l): Obtain the usable stages from the library state flags. */
   VkShaderStageFlags shader_stages_usable = VK_SHADER_STAGE_ALL_GRAPHICS;

   /* Initialize the static state. */
   struct vk_graphics_pipeline_all_state all_state;
   struct vk_graphics_pipeline_state state = {};
   result = vk_graphics_pipeline_state_fill(&device->vk, &state, create_info, NULL, 0, &all_state,
                                            NULL, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, NULL);
   if (result != VK_SUCCESS) {
      goto fail_pipeline;
   }
   BITSET_COPY(pipeline->dynamic_state, state.dynamic);
   terakan_vk_pipeline_graphics_vertex_input_init_without_fs_linkage_and_creation(
      &pipeline->vertex_input, &state);
   terakan_vk_pipeline_graphics_pre_rasterization_init(&pipeline->pre_rasterization, &state);
   if (pipeline->pre_rasterization.pa_cl_clip_cntl_dx_rasterization_kill) {
      terakan_vk_pipeline_graphics_fragment_shading_init_empty(&pipeline->fragment_shading);
      terakan_vk_pipeline_graphics_fragment_output_init_empty(&pipeline->fragment_output);
      shader_stages_usable &= ~VK_SHADER_STAGE_FRAGMENT_BIT;
   } else {
      result = terakan_vk_pipeline_graphics_multisample_init_with_rasterization(
         &pipeline->multisample, &state, device);
      if (result != VK_SUCCESS) {
         goto fail_pipeline;
      }
      terakan_vk_pipeline_graphics_fragment_shading_init_with_rasterization(
         &pipeline->fragment_shading, &state);
      terakan_vk_pipeline_graphics_fragment_output_init_with_rasterization(
         &pipeline->fragment_output, &state);
   }

   /* Gather the shader stage SPIR-V. */
   /* TODO(Triang3l): Shader caching. */

   struct shader_create_data {
      VkPipelineShaderStageCreateInfo const * info;
      size_t spirv_size_bytes;
      uint32_t const * spirv;
   } shaders_create_data[MESA_SHADER_FRAGMENT + 1] = {};

   for (uint32_t shader_stage_info_index = 0; shader_stage_info_index < create_info->stageCount;
        ++shader_stage_info_index) {
      VkPipelineShaderStageCreateInfo const * const shader_stage_info =
         &create_info->pStages[shader_stage_info_index];
      if (!util_is_power_of_two_nonzero(shader_stage_info->stage) ||
          !(shader_stages_usable & shader_stage_info->stage)) {
         continue;
      }
      struct shader_create_data * const shader_data =
         &shaders_create_data[vk_to_mesa_shader_stage(shader_stage_info->stage)];
      /* The `VkPipelineShaderStageCreateInfo` reference in the Vulkan 1.4.349 specification says:
       *
       *     "If `module` is not `VK_NULL_HANDLE`, the shader code used by the pipeline is defined
       *     by `module`. If `module` is `VK_NULL_HANDLE`, the shader code is defined by the chained
       *     `VkShaderModuleCreateInfo` if present."
       */
      if (shader_stage_info->module != VK_NULL_HANDLE) {
         struct vk_shader_module const * const shader_module =
            vk_shader_module_from_handle(shader_stage_info->module);
         shader_data->spirv_size_bytes = shader_module->size;
         shader_data->spirv = (uint32_t const *)shader_module->data;
      } else {
         VkShaderModuleCreateInfo const * const shader_module_create_info =
            vk_find_struct_const(create_info->pNext, SHADER_MODULE_CREATE_INFO);
         if (shader_module_create_info != NULL) {
            shader_data->spirv_size_bytes = shader_module_create_info->codeSize;
            shader_data->spirv = shader_module_create_info->pCode;
         } else {
            continue;
         }
      }
      shader_data->info = shader_stage_info;
      pipeline->shader_stages |= shader_stage_info->stage;
   }

   if (unlikely(!(pipeline->shader_stages & VK_SHADER_STAGE_VERTEX_BIT))) {
      result = vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                         "No vertex shader in the graphics pipeline");
      goto fail_pipeline;
   }

   /* Section "Tessellation" of the Vulkan 1.4.349 specification says:
    *
    *     "If a pipeline includes both tessellation shaders (control and evaluation), the
    *     tessellator consumes each input patch (after vertex shading) and produces a new set of
    *     independent primitives (points, lines, or triangles)."
    *
    * Disable tessellation if only one of the two tessellation shaders is provided. If none are
    * specified at all, this will do nothing.
    */
   if (~pipeline->shader_stages &
       (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)) {
      pipeline->shader_stages &=
         ~(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
   }

   /* Compile the shaders. */

   struct terakan_pipeline_layout const * const pipeline_layout =
      terakan_pipeline_layout_from_handle(create_info->layout);

   u_foreach_bit (shader_stage_vk_bit_index, pipeline->shader_stages) {
      gl_shader_stage const shader_stage =
         vk_to_mesa_shader_stage((VkShaderStageFlagBits)BITFIELD_BIT(shader_stage_vk_bit_index));
      struct shader_create_data const * const shader_data = &shaders_create_data[shader_stage];

      nir_shader * const nir = terakan_shader_spirv_to_nir(
         device, shader_data->spirv_size_bytes, shader_data->spirv, shader_stage,
         shader_data->info->pName, shader_data->info->pSpecializationInfo);
      if (nir == NULL) {
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         goto fail_shaders;
      }

      struct terakan_shader_impl * const shader = &pipeline->shaders[shader_stage];

      shader->push_constants_usage.app_extent_bytes =
         pipeline_layout->shader_app_push_constants_extents_bytes[shader_stage];

      terakan_shader_lower_and_optimize_post_link(
         nir, pipeline_layout, &shader->sqk_usage, shader->uavs_for_mutable_resources_needed,
         &shader->push_constants_usage.driver_constants, &shader->fs.rtv_dsb_uncompacted_exports);

      /* TODO(Triang3l): Construct the shader key from the NIR and, when available, the pipeline
       * state.
       */
      union r600_shader_key shader_key = {};
      if (shader_stage == MESA_SHADER_FRAGMENT) {
         shader_key.ps.nr_cbufs = util_bitcount(shader->fs.rtv_dsb_uncompacted_exports);
      }

      result = terakan_shader_impl_compile(shader, device, &shader_key, nir, allocator);
      ralloc_free(nir);
      if (result != VK_SUCCESS) {
         goto fail_shaders;
      }
   }

   bool const fetch_shader_is_static =
      BITSET_TEST(pipeline->vertex_input.static_state,
                  TERAKAN_VK_PIPELINE_GRAPHICS_VERTEX_INPUT_STATIC_SQ_PGM_FETCH);
   struct terakan_vertex_input_fs_code fetch_shader_code;
   bool fetch_shader_is_no_operation = true;
   if (fetch_shader_is_static) {
      pipeline->vertex_input.sq_pgm_fetch.layout.attributes_used =
         pipeline->shaders[MESA_SHADER_VERTEX].vs.vertex_attributes_needed;
      terakan_vertex_input_create_fs_code(&pipeline->vertex_input.sq_pgm_fetch.layout,
                                          terakan_device_physical_device(device)->chip_info.is_r9xx,
                                          &pipeline->vertex_input.sq_pgm_fetch.resource_usage,
                                          &fetch_shader_code);
      fetch_shader_is_no_operation =
         terakan_vertex_input_fs_code_is_no_operation(&fetch_shader_code);
   }

   /* Place the hardware shaders in the BO. */
   /* TODO(Triang3l): Use a global allocator and GPU uploading instead of creating one host-visible
    * device-local BO per pipeline.
    */

   /* Place the fetch shader close to the vertex shader. */
   uint32_t shader_bo_bytes_shr8 = 0;
   if (fetch_shader_is_static) {
      if (fetch_shader_is_no_operation) {
         pipeline->vertex_input.sq_pgm_fetch.bo = device->meta_shaders_bo;
         pipeline->vertex_input.sq_pgm_fetch.va_shr8 = device->meta_shaders_empty_fetch_va_shr8;
      } else {
         pipeline->vertex_input.sq_pgm_fetch.va_shr8 = shader_bo_bytes_shr8;
         shader_bo_bytes_shr8 += DIV_ROUND_UP(
            terakan_vertex_input_fs_code_qwords(&fetch_shader_code), (uint32_t)1 << (8 - 3));
      }
   }
   u_foreach_bit (shader_stage_vk_bit_index, pipeline->shader_stages) {
      struct terakan_shader_impl * const shader = &pipeline->shaders[vk_to_mesa_shader_stage(
         (VkShaderStageFlagBits)BITFIELD_BIT(shader_stage_vk_bit_index))];
      shader->static_state.program_va_shr8 = shader_bo_bytes_shr8;
      shader_bo_bytes_shr8 += DIV_ROUND_UP(shader->shader.bc.ndw, (uint32_t)1 << (8 - 2));
   }

   /* 32 bits are enough for the BO size, as qword addresses in shader control flow instructions are
    * 24-bit.
    */
   result = device->winsys_fn->bo->allocate_device_memory(
      device, shader_bo_bytes_shr8 << 8, TERAKAN_SHADER_PROGRAM_ALIGNMENT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      0, allocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &pipeline->shader_bo);
   if (result != VK_SUCCESS) {
      result = vk_error(device, result);
      goto fail_shaders;
   }
   {
      uint32_t * const shader_bo_mapping = terakan_bo_map(pipeline->shader_bo);
      if (shader_bo_mapping == NULL) {
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         goto fail_shader_bo;
      }
      uint32_t const shader_bo_va_shr8 = (uint32_t)(pipeline->shader_bo->va >> 8);
      if (!fetch_shader_is_no_operation) {
         assert(fetch_shader_is_static);
         terakan_vertex_input_combine_fs(
            &fetch_shader_code,
            shader_bo_mapping + (pipeline->vertex_input.sq_pgm_fetch.va_shr8 << (8 - 2)));
         pipeline->vertex_input.sq_pgm_fetch.bo = pipeline->shader_bo;
         pipeline->vertex_input.sq_pgm_fetch.va_shr8 += shader_bo_va_shr8;
      }
      u_foreach_bit (shader_stage_vk_bit_index, pipeline->shader_stages) {
         struct terakan_shader_impl * const shader = &pipeline->shaders[vk_to_mesa_shader_stage(
            (VkShaderStageFlagBits)BITFIELD_BIT(shader_stage_vk_bit_index))];
         util_memcpy_cpu_to_le32(
            shader_bo_mapping + (shader->static_state.program_va_shr8 << (8 - 2)),
            shader->shader.bc.bytecode, (uint32_t)sizeof(uint32_t) * shader->shader.bc.ndw);
         shader->static_state.program_bo = pipeline->shader_bo;
         shader->static_state.program_va_shr8 += shader_bo_va_shr8;
         /* Destroy the data needed only for creation. */
         r600_bytecode_clear(&shader->shader.bc);
      }
      terakan_bo_unmap(pipeline->shader_bo);
   }

   *pipeline_out = pipeline;
   return VK_SUCCESS;

fail_shader_bo:
   terakan_bo_free(pipeline->shader_bo, allocator);
fail_shaders:
   u_foreach_bit (shader_stage_vk_bit_index, pipeline->shader_stages) {
      gl_shader_stage const shader_stage =
         vk_to_mesa_shader_stage((VkShaderStageFlagBits)BITFIELD_BIT(shader_stage_vk_bit_index));
      struct terakan_shader_impl * const shader = &pipeline->shaders[shader_stage];
      r600_bytecode_clear(&shader->shader.bc);
      free(shader->shader.arrays);
   }
fail_pipeline:
   terakan_vk_pipeline_graphics_destroy(&device->vk, &pipeline->vk, allocator);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateGraphicsPipelines(VkDevice const deviceHandle, VkPipelineCache const pipelineCache,
                                uint32_t const createInfoCount,
                                VkGraphicsPipelineCreateInfo const * const pCreateInfos,
                                VkAllocationCallbacks const * const pAllocator,
                                VkPipeline * const pPipelines)
{
   VkResult result = VK_SUCCESS;

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   uint32_t pipeline_index;

   for (pipeline_index = 0; pipeline_index < createInfoCount; ++pipeline_index) {
      struct terakan_vk_pipeline_graphics * pipeline = NULL;
      VkGraphicsPipelineCreateInfo const * const create_info = &pCreateInfos[pipeline_index];
      VkResult const pipeline_result =
         terakan_vk_pipeline_graphics_create(device, create_info, pAllocator, &pipeline);
      if (pipeline_result != VK_SUCCESS) {
         result = pipeline_result;
         if (vk_graphics_pipeline_create_flags(create_info) &
             VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR) {
            break;
         }
         pPipelines[pipeline_index] = VK_NULL_HANDLE;
         continue;
      }
      pPipelines[pipeline_index] = vk_pipeline_to_handle(&pipeline->vk);
   }

   for (; pipeline_index < createInfoCount; ++pipeline_index) {
      pPipelines[pipeline_index] = VK_NULL_HANDLE;
   }

   return result;
}
