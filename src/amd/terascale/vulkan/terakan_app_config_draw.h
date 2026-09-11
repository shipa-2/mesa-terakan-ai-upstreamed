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

#ifndef TERAKAN_APP_CONFIG_DRAW_H
#define TERAKAN_APP_CONFIG_DRAW_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_hw_config_draw.h"
#include "terakan_screen_rect.h"
#include "terakan_shader.h"
#include "terakan_vertex_input.h"

#include "amd/terascale/common/terascale_format.h"
#include "gallium/drivers/r600/eg_sq.h"
#include "gallium/drivers/r600/evergreend.h"
#include "util/bitset.h"
#include "util/macros.h"

#include <assert.h>
#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `terakan_app_config_draw` is a layer on top of `terakan_hw_config` that specifically tracks the
 * graphics state configuration desired by the application (as opposed to the internal meta
 * shaders).
 *
 * It's not reapplied when switching to a new indirect buffer - `terakan_hw_config` is re-emitted
 * instead, for both application's work and internal draws / dispatches. Thus, no register writes
 * must be done directly from `terakan_app_config_draw` application - it must first go to
 * `terakan_hw_config`.
 *
 * It also can modify `terakan_push_constants_state`.
 */

struct terakan_app_config_draw_pa_vport {
   struct terakan_screen_rect implicit_scissor;
   float xy_scale_offset[2][2];
   float gb_vert_horz_clip_adj[2];
   float z_near_far[2];
};

#define TERAKAN_APP_CONFIG_DRAW_PA_VPORT_EMPTY                                                     \
   ((struct terakan_app_config_draw_pa_vport){                                                     \
      .gb_vert_horz_clip_adj = {FLT_MAX, FLT_MAX},                                                 \
      .z_near_far = {0.0f, 1.0f},                                                                  \
   })

static inline bool
terakan_app_config_draw_pa_vport_equal(struct terakan_app_config_draw_pa_vport const * const a,
                                       struct terakan_app_config_draw_pa_vport const * const b)
{
   static_assert(
      sizeof(*a) == sizeof(a->implicit_scissor) + sizeof(a->xy_scale_offset) +
                       sizeof(a->gb_vert_horz_clip_adj) + sizeof(a->z_near_far),
      "Using memcmp to compare viewports, expecting that there's no padding in the viewport "
      "structure.");
   return memcmp(a, b, sizeof(*a)) == 0;
}

enum terakan_app_config_draw_poly_offset_representation {
   TERAKAN_APP_CONFIG_DRAW_POLY_OFFSET_REPRESENTATION_FORMAT,
   TERAKAN_APP_CONFIG_DRAW_POLY_OFFSET_REPRESENTATION_FORCE_UNORM,
   TERAKAN_APP_CONFIG_DRAW_POLY_OFFSET_REPRESENTATION_FLOAT,
};

struct terakan_app_config_draw_cb_color_rtv {
   /* The target is bound if `terakan_color_descriptor_is_bound` is true for it. */
   struct terakan_bo const * bo;
   struct terakan_color_descriptor color;
   struct terakan_color_meta_descriptor meta;
   /* `BLEND_CONTROL_ENABLE` and the equation can be configured separately.
    * `terakan_hw_config_draw_cb_blend_control_color_factors_for_color_alpha` may be applied by the
    * client if desirable.
    */
   uint32_t blend_control;
};

static inline uint8_t
terakan_app_config_draw_cb_color_rtv_format_export_mask(
   struct terakan_app_config_draw_cb_color_rtv const * const rtv)
{
   if (rtv->bo == NULL) {
      return 0b0;
   }
   return terascale_format_cb_color_export_component_masks
      [terascale_format_channel_count[G_028C70_FORMAT(rtv->color.info)]]
      [G_028C70_COMP_SWAP(rtv->color.info)];
}

struct terakan_app_config_draw_cb_color_uav {
   struct terakan_bo const * bo;
   struct terakan_color_descriptor color;
};

enum terakan_app_config_draw_entry {
   /* Entries that are needed with rasterizer discard. */

   TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_PRIMITIVE_TYPE,
   TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_INDEX_OFFSET,
   TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER,

   /* Depends on VGT_PRIMITIVE_TYPE. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_PRE_RASTERIZATION,
   /* Depends on SQ_PGM_PRE_RASTERIZATION. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FETCH,
   /* Depends on SQ_PGM_FETCH. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_RESOURCES_FETCH,

   TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_CL_CLIP_CNTL,

   /* Depends on SQ_PGM_PRE_RASTERIZATION, PA_CL_CLIP_CNTL.
    * Needed even without rasterizer discard, but the hardware register depends on post-rasterizer-
    * discard configuration, therefore it's finally set by either the pre-discard or the
    * post-discard entry depending on whether rasterizer discard is enabled.
    */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_IA_MULTI_VGT_PARAM_PRE_RASTERIZER_DISCARD_R9XX,

   TERAKAN_APP_CONFIG_DRAW_ENTRIES_POST_RASTERIZER_DISCARD_FIRST,
   TERAKAN_APP_CONFIG_DRAW_ENTRIES_PRE_RASTERIZER_DISCARD_LAST =
      TERAKAN_APP_CONFIG_DRAW_ENTRIES_POST_RASTERIZER_DISCARD_FIRST - 1,

   /* Entries that have no effect with rasterizer discard. */

   TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_VPORT,
   TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_SC_MODE_CNTL,
   TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_CL_VTE_CNTL,
   /* Depends on VGT_PRIMITIVE_TYPE, SQ_PGM_PRE_RASTERIZATION. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_LINE_CNTL,
   TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_LINE_STIPPLE,
   /* Depends on PA_SU_SC_MODE_CNTL. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET,
   TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_AA_CONFIG_SAMPLE_LOCS,
   /* Depends on PA_SC_AA_CONFIG_SAMPLE_LOCS. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_AA_MASK,
   /* Depends on PA_SC_LINE_STIPPLE, PA_SC_AA_CONFIG_SAMPLE_LOCS. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_MODE_CNTL_0,

   /* Depends on IA_MULTI_VGT_PARAM_PRE_RASTERIZER_DISCARD_R9XX, PA_SC_LINE_STIPPLE.
    * See the corresponding pre-rasterizer-discard entry for more information.
    */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_IA_MULTI_VGT_PARAM_POST_RASTERIZER_DISCARD_R9XX,

   TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FRAGMENT,

   /* Depends on PA_SC_AA_CONFIG_SAMPLE_LOCS. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_COUNT_CONTROL,
   /* Depends on PA_SC_AA_CONFIG_SAMPLE_LOCS. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_BUFFER,
   /* Depends on PA_VPORT. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE,
   /* Depends on DB_DEPTH_STENCIL_BUFFER. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE2,
   /* Depends on DB_DEPTH_STENCIL_BUFFER. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_CONTROL_REF_MASK,
   /* Depends on PA_SU_SC_MODE_CNTL, DB_DEPTH_STENCIL_BUFFER. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET_DB_FMT_CNTL,
   /* Depends on PA_SC_AA_CONFIG_SAMPLE_LOCS, SQ_PGM_FRAGMENT, DB_DEPTH_STENCIL_BUFFER.
    * Also configures PA_SC_MODE_CNTL_1.
    */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_EQAA,
   TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_ALPHA_TO_MASK,

   TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_ROP3,
   /* Depends on SQ_PGM_FRAGMENT, CB_ROP3. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL,
   /* Depends on SQ_PGM_FRAGMENT. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_UAV_AND_UNUSED_MRT,
   /* Depends on CB_COLOR_RTV_AND_BLEND_CONTROL. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_BLEND_CONSTANTS,
   /* Depends on CB_ROP3, CB_COLOR_RTV_AND_BLEND_CONTROL, CB_COLOR_UAV_AND_UNUSED_MRT.
    * When rasterizer discard is enabled, CB_COLOR_CONTROL is set when applying PA_CL_CLIP_CNTL
    * instead as it has effect on UAV usage in pre-rasterization stages.
    */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_CONTROL,
   /* Depends on SQ_PGM_FRAGMENT, CB_COLOR_RTV_AND_BLEND_CONTROL. */
   TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_SHADER_CONTROL,

   TERAKAN_APP_CONFIG_DRAW_ENTRIES_COUNT,
};

#define TERAKAN_APP_CONFIG_DRAW_ASSERT_MAY_DEPEND_ON(dependent, dependency)                        \
   static_assert(                                                                                  \
      TERAKAN_APP_CONFIG_DRAW_ENTRY_##dependent > TERAKAN_APP_CONFIG_DRAW_ENTRY_##dependency,      \
      #dependent " depends on " #dependency " and thus must have a higher index.")

struct terakan_app_config_draw {
   /* Whether each entry has been modified, overridden by an internal draw, or had its dependencies
    * changed, and needs to be applied before the next draw.
    */
   BITSET_DECLARE(entries_pending_, TERAKAN_APP_CONFIG_DRAW_ENTRIES_COUNT);

   /* Vertex grouping and tessellation. */

   uint32_t vgt_primitive_type_;

   uint32_t vgt_index_offset_;

   struct {
      struct terakan_hw_config_draw_vgt_dma_index_buffer index_buffer;
      uint32_t index_type;

      /* If the primitive reset index is wider than the index type, it doesn't cause resets. */
      uint32_t multi_prim_reset_index;
      bool multi_prim_reset_enable;

      /* If false, the index buffer in `hw_config` is not configured, and primitive reset is
       * disabled.
       */
      bool draw_indexed;
   } vgt_dma_index_buffer_;

   /* Pre-rasterization shader stages. */

   struct {
      struct terakan_shader_impl const * vertex_as_local;
      struct terakan_shader_impl const * vertex_as_export;
      struct terakan_shader_impl const * vertex_as_vertex;
      struct terakan_shader_impl const * tessellation_control;
      struct terakan_shader_impl const * tessellation_evaluation_as_export;
      struct terakan_shader_impl const * tessellation_evaluation_as_vertex;
      struct terakan_shader_impl const * geometry;

      struct {
         bool tessellation_enable;
      } from_apply_vgt_primitive_type;
   } sq_pgm_pre_rasterization_;

   struct {
      struct {
         /* Must not include any attributes that need to be fetched to a GPR beyond the number of
          * GPRs available in the vertex shader, because index calculations in the fetch shader may
          * use the destination registers as temporary.
          */
         uint32_t attributes_used_by_vs;
      } from_apply_sq_pgm_pre_rasterization;

      /* `attributes_used` is not tracked, `attributes_used_by_vs` is used instead.
       * `bindings_with_2048_stride_as_1024` is used even with static vertex input layout
       * configuration.
       */
      struct terakan_vertex_input_fs_layout desired_2048_stride_as_1024_and_dynamic_fs_layout;

      /* If not NULL, the static vertex input layout will be used for binding and attribute
       * parameters, except for the #2048StrideAs1024 enablement, which is always taken from
       * `desired_2048_stride_as_1024_and_dynamic_fs_layout`.
       *
       * The precompiled fetch shader will be used if it's compatible with the actual vertex shader
       * attribute usage and with the 2048 stride as 1024 enablement in it, otherwise a new fetch
       * shader will be created for the specified configuration.
       */
      struct terakan_vertex_input_fs const * static_fs;

      struct terakan_vertex_input_fs last_transient_fs;
   } sq_pgm_fetch_;

   struct {
      /* Binding configuration (for application binding indices, not hardware resource indices). */
      struct terakan_bo const * bo[TERAKAN_RESOURCE_HW_COUNT_FETCH];
      uint64_t va[TERAKAN_RESOURCE_HW_COUNT_FETCH];
      uint32_t size_minus_1[TERAKAN_RESOURCE_HW_COUNT_FETCH];
      /* Must be 1024 if using the #2048StrideAs1024 stride workaround for the binding. */
      uint16_t hw_stride[TERAKAN_RESOURCE_HW_COUNT_FETCH];

      struct {
         struct terakan_vertex_input_fs_resource_usage usage;
         struct terakan_vertex_input_fs_layout layout;
      } from_apply_sq_pgm_fetch;
   } sq_resources_fetch_;

   /* Primitive assembly and scan conversion. */

   struct {
      bool dx_rasterization_kill; /* Rasterizer discard. */
      bool dx_clip_space_def;
      bool z_clamp_enable;
      /* < 0 - `!z_clamp_enable`, 0 - always disabled, > 0 - always enabled. */
      signed char z_clip_enable_override;
   } pa_cl_clip_cntl_;

   struct {
      struct {
         uint32_t ia_multi_vgt_param;
      } from_apply_sq_pgm_pre_rasterization;

      struct {
         bool dx_rasterization_kill;
      } from_apply_pa_cl_clip_cntl;
   } ia_multi_vgt_param_pre_rasterizer_discard_r9xx_;

   struct {
      /* TODO(Triang3l): Flag for whether the hardware vertex shader writes a viewport ID. */

      struct {
         /* Duplicated from the `PA_CL_CLIP_CNTL` entry when it's applied, so the setter for them
          * needs to update only one entry, and the dependency between the two entries is expressed
          * explicitly.
          */
         bool dx_clip_space_def;
         bool z_clamp_enable;
      } from_apply_pa_cl_clip_cntl;

      bool z_range_unrestricted;
      bool user_defined_zmin_zmax_enable;

      uint8_t vport_count;
      uint8_t explicit_scissor_count;

      /* The render area is treated just as an additional scissor rectangle for all viewports, and
       * for simplicity, doesn't have to actually be within the depth / stencil and color buffer
       * boundaries.
       */
      struct terakan_screen_rect render_area;
      float user_defined_zmin_zmax[2];

      struct terakan_app_config_draw_pa_vport vports[TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT];
      struct terakan_screen_rect explicit_scissors[TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT];
   } pa_vport_;

   uint32_t pa_su_sc_mode_cntl_;

   struct {
      /* `LINE_PATTERN`, `REPEAT_COUNT`, `PATTERN_BIT_ORDER`. */
      uint32_t pattern;

      bool enable;

      struct {
         bool per_primitive_reset;
      } from_apply_vgt_primitive_type;

      struct {
         bool geometry_shader_enable;
      } from_apply_sq_pgm_pre_rasterization;
   } pa_sc_line_stipple_;

   /* PA_SU_LINE_CNTL: the line width, in eighths of a pixel. */
   uint32_t pa_su_line_cntl_;

   struct {
      struct terakan_hw_config_draw_pa_su_poly_offset poly_offset;

      struct {
         bool poly_offset_enable;
      } from_apply_pa_su_sc_mode_cntl;
   } pa_su_poly_offset_;

   struct {
      /* `msaa_num_samples_log2` may be accessed by dependent entries. */
      uint8_t msaa_num_samples_log2;
      bool custom_sample_locs_enable;
      uint8_t custom_sample_locs[16][4];
   } pa_sc_aa_config_sample_locs_;

   /* May include unused samples, excluded when applied. */
   uint16_t pa_sc_aa_mask_;

   struct {
      struct {
         bool line_stipple_enable;
      } from_apply_pa_sc_line_stipple;

      struct {
         bool msaa_enable;
      } from_apply_pa_sc_aa_config_sample_locs;
   } pa_sc_mode_cntl_0_;

   struct {
      struct {
         bool line_stipple_enable;
      } from_apply_pa_sc_line_stipple;
   } ia_multi_vgt_param_post_rasterizer_discard_r9xx_;

   /* Fragment shader. */

   struct terakan_shader_impl const * sq_pgm_fragment_;

   /* Depth / stencil buffer. */

   struct {
      size_t zpass_query_active_count;
   } db_count_control_;

   struct {
      struct terakan_bo const * bo;
      struct terakan_depth_stencil_descriptor descriptor;
   } db_depth_stencil_buffer_;

   struct {
      struct {
         bool disable_viewport_clamp;
      } from_apply_pa_vport;
   } db_render_override_;

   struct {
      struct {
         bool decompress_z_on_flush_r9xx;
      } from_apply_db_depth_stencil_buffer;
   } db_render_override2_;

   struct {
      uint32_t stencil_ref_mask_front;
      uint32_t stencil_ref_mask_back;

      uint32_t depth_stencil_control;

      struct {
         bool depth_bound;
         bool stencil_bound;
      } from_apply_db_depth_stencil_buffer;
   } db_depth_stencil_control_ref_mask_;

   struct {
      int8_t ps_iter_max_invocation_samples_log2;

      uint8_t ps_iter_least_fragments_log2_r9xx;

      struct {
         bool ps_iter_full_sample_shading;
      } from_apply_sq_pgm_fragment;

      struct {
         uint8_t max_anchor_samples_log2_r9xx;
      } from_apply_db_depth_stencil_buffer;
   } db_eqaa_;

   uint32_t db_alpha_to_mask_;

   struct {
      enum terakan_app_config_draw_poly_offset_representation representation;
      bool exact;

      struct {
         bool poly_offset_enable;
      } from_apply_pa_su_sc_mode_cntl;

      struct {
         enum terascale_r8xx_depth_format depth_format;
      } from_apply_db_depth_stencil_buffer;
   } pa_su_poly_offset_db_fmt_cntl_;

   /* Color buffer. */

   struct {
      bool rop3_enable;
      enum terakan_hw_config_draw_cb_color_control_rop3 rop3;
   } cb_rop3_;

   struct {
      /* Vulkan 1.0 provides `colorWriteMask`, but `VK_EXT_color_write_enable` also adds
       * `colorWriteEnable` that can be used to disable writing to attachments regardless of the
       * component write mask.
       *
       * `write_enable_mask` defaults to all enabled because it's an optional feature in Vulkan.
       */
      uint8_t write_enable_mask;
      uint32_t write_component_mask;

      struct terakan_app_config_draw_cb_color_rtv rtv[TERAKAN_COLOR_HW_RTV_COUNT];

      struct {
         uint8_t rtv_dsb_uncompacted_exports;
      } from_apply_sq_pgm_fragment;

      struct {
         uint8_t sample_count_limit_log2;
      } from_apply_db_depth_stencil_buffer;

      struct {
         uint8_t min_fragments_log2_r9xx;
      } from_apply_db_eqaa;

      struct {
         bool blend_disable;
      } from_apply_cb_rop3;
   } cb_color_rtv_and_blend_control_;

   struct {
      /* For descriptor set binding simplicity, storing UAV descriptors in the configuration at the
       * same indices as the read-only resources corresponding to them, but to calculate UAV
       * indices, compacting the indices of the resources that have corresponding UAVs.
       *
       * The UAV array is large, so it's not initialized. Rather, if `uav_bound` doesn't have the
       * corresponding bit set for a UAV, the BO pointer and the color target descriptor are
       * undefined.
       */
      /* Indexed by the bind point: graphics first, compute second. The two share the hardware
       * CB/RAT targets but are separate pipeline bind points in Vulkan, each with its own bound
       * descriptor sets, so one array for both let whichever set was bound last decide the UAV
       * descriptors for the other's draw or dispatch as well.
       */
      BITSET_DECLARE(uav_bound[2], TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL);
      struct terakan_app_config_draw_cb_color_uav
         uav[2][TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL];

      struct {
         uint8_t rtv_dsb_export_count;
         BITSET_DECLARE(uav_used, TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL);
      } from_apply_sq_pgm_fragment;
   } cb_color_uav_and_unused_mrt_;

   struct {
      float constants[4];

      struct {
         bool constants_rgb_used;
         bool constants_alpha_used;
      } from_apply_cb_color_rtv_and_blend_control;
   } cb_blend_constants_;

   struct {
      struct {
         enum terakan_hw_config_draw_cb_color_control_rop3 rop3;
      } from_apply_cb_rop3;

      struct {
         bool any_rtv_written;
      } from_apply_cb_color_rtv_and_blend_control;

      struct {
         /* For determining whether CB needs to be enabled. Needs to be set if any UAVs are used
          * even if null descriptors are bound as all of them, because reading with acknowledge
          * awaiting and an expectation of a zero value may still be done for them.
          */
         bool any_uav_used;
      } from_apply_cb_color_uav_and_unused_mrt;
   } cb_color_control_;

   struct {
      struct {
         /* Should contain `DUAL_EXPORT_ENABLE` based purely on the DB configuration for the shader.
          */
         uint32_t db_shader_control;
      } from_apply_sq_pgm_fragment;

      struct {
         /* Force-disables dual export. */
         bool rtv_128bpp_export;
      } from_apply_cb_color_rtv_and_blend_control;
   } db_shader_control_;
};

static inline void
terakan_app_config_draw_set_pending(struct terakan_app_config_draw * const config,
                                    enum terakan_app_config_draw_entry const entry)
{
   BITSET_SET(config->entries_pending_, entry);
}

static inline void
terakan_app_config_draw_set_register_(struct terakan_app_config_draw * const config,
                                      enum terakan_app_config_draw_entry const entry,
                                      uint32_t * const entry_register, uint32_t const value)
{
   if (*entry_register == value) {
      return;
   }
   *entry_register = value;
   terakan_app_config_draw_set_pending(config, entry);
}

static inline void
terakan_app_config_draw_set_fields_(struct terakan_app_config_draw * const config,
                                    enum terakan_app_config_draw_entry const entry,
                                    uint32_t * const entry_register, uint32_t const keep_mask,
                                    uint32_t const value)
{
   assert(!(value & keep_mask));
   terakan_app_config_draw_set_register_(config, entry, entry_register,
                                         (*entry_register & keep_mask) | value);
}

static inline void
terakan_app_config_draw_set_vgt_primitive_type(struct terakan_app_config_draw * const config,
                                               uint32_t const value)
{
   terakan_app_config_draw_set_register_(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_PRIMITIVE_TYPE,
                                         &config->vgt_primitive_type_, value);
}

static inline void
terakan_app_config_draw_set_vgt_index_offset(struct terakan_app_config_draw * const config,
                                             uint32_t const value)
{
   terakan_app_config_draw_set_register_(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_INDEX_OFFSET,
                                         &config->vgt_index_offset_, value);
}

/* The passed `vgt_dma_index_type` may be ignored if the implementation determines that indices
 * aren't going to be read from this buffer.
 */
static inline void
terakan_app_config_draw_set_vgt_dma_index_buffer(
   struct terakan_app_config_draw * const config,
   struct terakan_hw_config_draw_vgt_dma_index_buffer const index_buffer, uint32_t const index_type)
{
   /* Applying isn't entirely trivial due to #MemoryIntegrity checks, however, this is not a part of
    * a pipeline object in Vulkan, and it's likely that a different index buffer needs to be bound
    * when the application sets one, so not checking if modified.
    */
   config->vgt_dma_index_buffer_.index_buffer = index_buffer;
   config->vgt_dma_index_buffer_.index_type = index_type;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER);
}

static inline void
terakan_app_config_draw_set_vgt_dma_index_buffer_multi_prim_reset_index(
   struct terakan_app_config_draw * const config, uint32_t const value)
{
   terakan_app_config_draw_set_register_(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER,
                                         &config->vgt_dma_index_buffer_.multi_prim_reset_index,
                                         value);
}

static inline void
terakan_app_config_draw_set_vgt_dma_index_buffer_multi_prim_reset_enable(
   struct terakan_app_config_draw * const config, bool const value)
{
   if (config->vgt_dma_index_buffer_.multi_prim_reset_enable == value) {
      return;
   }
   config->vgt_dma_index_buffer_.multi_prim_reset_enable = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER);
}

static inline void
terakan_app_config_draw_set_vgt_dma_index_buffer_draw_indexed(
   struct terakan_app_config_draw * const config, bool const value)
{
   if (config->vgt_dma_index_buffer_.draw_indexed == value) {
      return;
   }
   config->vgt_dma_index_buffer_.draw_indexed = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER);
}

static inline void
terakan_app_config_draw_set_sq_pgm_pre_rasterization_vertex(
   struct terakan_app_config_draw * const config, struct terakan_shader_impl const * const as_local,
   struct terakan_shader_impl const * const as_export,
   struct terakan_shader_impl const * const as_vertex)
{
   /* Compare likely the most frequently used first. */
   if (config->sq_pgm_pre_rasterization_.vertex_as_vertex == as_vertex &&
       config->sq_pgm_pre_rasterization_.vertex_as_local == as_local &&
       config->sq_pgm_pre_rasterization_.vertex_as_export == as_export) {
      return;
   }
   config->sq_pgm_pre_rasterization_.vertex_as_vertex = as_vertex;
   config->sq_pgm_pre_rasterization_.vertex_as_local = as_local;
   config->sq_pgm_pre_rasterization_.vertex_as_export = as_export;
   terakan_app_config_draw_set_pending(config,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_PRE_RASTERIZATION);
}

static inline void
terakan_app_config_draw_set_sq_pgm_pre_rasterization_tessellation_control(
   struct terakan_app_config_draw * const config, struct terakan_shader_impl const * const value)
{
   if (config->sq_pgm_pre_rasterization_.tessellation_control == value) {
      return;
   }
   config->sq_pgm_pre_rasterization_.tessellation_control = value;
   terakan_app_config_draw_set_pending(config,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_PRE_RASTERIZATION);
}

static inline void
terakan_app_config_draw_set_sq_pgm_pre_rasterization_tessellation_evaluation(
   struct terakan_app_config_draw * const config,
   struct terakan_shader_impl const * const as_export,
   struct terakan_shader_impl const * const as_vertex)
{
   /* Compare likely the most frequently used first. */
   if (config->sq_pgm_pre_rasterization_.tessellation_evaluation_as_vertex == as_vertex &&
       config->sq_pgm_pre_rasterization_.tessellation_evaluation_as_export == as_export) {
      return;
   }
   config->sq_pgm_pre_rasterization_.tessellation_evaluation_as_vertex = as_vertex;
   config->sq_pgm_pre_rasterization_.tessellation_evaluation_as_export = as_export;
   terakan_app_config_draw_set_pending(config,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_PRE_RASTERIZATION);
}

static inline void
terakan_app_config_draw_set_sq_pgm_pre_rasterization_geometry(
   struct terakan_app_config_draw * const config, struct terakan_shader_impl const * const value)
{
   if (config->sq_pgm_pre_rasterization_.geometry == value) {
      return;
   }
   config->sq_pgm_pre_rasterization_.geometry = value;
   terakan_app_config_draw_set_pending(config,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_PRE_RASTERIZATION);
}

static inline void
terakan_app_config_draw_set_sq_pgm_fetch_dynamic_attribute_unbound(
   struct terakan_app_config_draw * const config, unsigned const attribute_index)
{
   assert(attribute_index < TERAKAN_RESOURCE_HW_COUNT_FETCH);
   config->sq_pgm_fetch_.desired_2048_stride_as_1024_and_dynamic_fs_layout
      .attribute_format_fetch_word1[attribute_index] = 0;
   /* Making pending regardless of whether a static fetch shader layout is used, because in Vulkan,
    * this function is called only when applying dynamic state on the Vulkan side, and this means
    * that dynamic state is going to be used.
    * Also making pending unconditionally, without checking if the layout is being modified, because
    * this function may be called many times by `vkCmdSetVertexInputEXT`, and the comparison costs
    * are expected to be similar to the applying costs.
    */
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FETCH);
}

/* Arguments not relevant given the other arguments may be ignored by this function. */
static inline void
terakan_app_config_draw_set_sq_pgm_fetch_dynamic_attribute(
   struct terakan_app_config_draw * const config, unsigned const attribute_index,
   uint32_t const format_fetch_word1, uint8_t const binding, uint16_t const offset,
   bool const is_instance_rate, uint32_t const instance_divisor)
{
   assert(attribute_index < TERAKAN_RESOURCE_HW_COUNT_FETCH);

   if (G_SQ_VTX_WORD1_DATA_FORMAT(format_fetch_word1) == TERASCALE_FORMAT_INDEX_INVALID) {
      terakan_app_config_draw_set_sq_pgm_fetch_dynamic_attribute_unbound(config, attribute_index);
      return;
   }

   struct terakan_vertex_input_fs_layout * const dynamic_fs_layout =
      &config->sq_pgm_fetch_.desired_2048_stride_as_1024_and_dynamic_fs_layout;

   dynamic_fs_layout->attribute_format_fetch_word1[attribute_index] = format_fetch_word1;
   dynamic_fs_layout->attribute_bindings[attribute_index] = binding;
   dynamic_fs_layout->attribute_offsets[attribute_index] = offset;

   uint32_t const attribute_bit = BITFIELD_BIT(attribute_index);
   if (is_instance_rate) {
      dynamic_fs_layout->instance_rate_attributes |= attribute_bit;
      dynamic_fs_layout->attribute_instance_divisors[attribute_index] = instance_divisor;
   } else {
      dynamic_fs_layout->instance_rate_attributes &= ~attribute_bit;
   }

   /* Making pending regardless of whether a static fetch shader layout is used, because in Vulkan,
    * this function is called only when applying dynamic state on the Vulkan side, and this means
    * that dynamic state is going to be used.
    * Also making pending unconditionally, without checking if the layout is being modified, because
    * this function may be called many times by `vkCmdSetVertexInputEXT`, and the comparison costs
    * are expected to be similar to the applying costs.
    */
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FETCH);
}

/* Pass a NULL fetch shader to use the dynamic configuration. */
static inline void
terakan_app_config_draw_set_sq_pgm_fetch_static_fs(
   struct terakan_app_config_draw * const config,
   struct terakan_vertex_input_fs const * const value)
{
   if (config->sq_pgm_fetch_.static_fs == value) {
      return;
   }
   config->sq_pgm_fetch_.static_fs = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FETCH);
}

/* If `bo` is NULL, `va` and `size_minus_1` are ignored, and the buffer is unbound.
 * #MemoryIntegrity is expected to be ensured before calling.
 */
static inline void
terakan_app_config_draw_set_sq_resource_fetch_base_size(
   struct terakan_app_config_draw * const config, unsigned const binding,
   struct terakan_bo const * bo, uint64_t const va, uint32_t size_minus_1)
{
   assert(binding < TERAKAN_RESOURCE_HW_COUNT_FETCH);
   if (config->sq_resources_fetch_.bo[binding] != bo) {
      config->sq_resources_fetch_.bo[binding] = bo;
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_RESOURCES_FETCH);
   }
   if (bo != NULL) {
      if (config->sq_resources_fetch_.va[binding] != va ||
          config->sq_resources_fetch_.size_minus_1[binding] != size_minus_1) {
         config->sq_resources_fetch_.va[binding] = va;
         config->sq_resources_fetch_.size_minus_1[binding] = size_minus_1;
         terakan_app_config_draw_set_pending(config,
                                             TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_RESOURCES_FETCH);
      }
   }
}

static inline void
terakan_app_config_draw_set_sq_pgm_and_resource_fetch_stride(
   struct terakan_app_config_draw * const config, unsigned const binding, uint16_t const stride,
   bool const fetch_with_2048_stride_as_1024)
{
   assert(binding < TERAKAN_RESOURCE_HW_COUNT_FETCH);
   uint16_t hw_stride = stride;
   uint32_t const binding_bit = BITFIELD_BIT(binding);
   if (fetch_with_2048_stride_as_1024 && stride == 2048) {
      /* #2048StrideAs1024. */
      if (!(config->sq_pgm_fetch_.desired_2048_stride_as_1024_and_dynamic_fs_layout
               .bindings_with_2048_stride_as_1024 &
            binding_bit)) {
         config->sq_pgm_fetch_.desired_2048_stride_as_1024_and_dynamic_fs_layout
            .bindings_with_2048_stride_as_1024 |= binding_bit;
         terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FETCH);
      }
      hw_stride = stride >> 1;
   } else {
      if (config->sq_pgm_fetch_.desired_2048_stride_as_1024_and_dynamic_fs_layout
             .bindings_with_2048_stride_as_1024 &
          binding_bit) {
         config->sq_pgm_fetch_.desired_2048_stride_as_1024_and_dynamic_fs_layout
            .bindings_with_2048_stride_as_1024 &= ~binding_bit;
         terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FETCH);
      }
   }
   if (config->sq_resources_fetch_.hw_stride[binding] != hw_stride) {
      config->sq_resources_fetch_.hw_stride[binding] = hw_stride;
      if (config->sq_resources_fetch_.bo[binding] != NULL) {
         terakan_app_config_draw_set_pending(config,
                                             TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_RESOURCES_FETCH);
      }
   }
}

static inline void
terakan_app_config_draw_set_pa_cl_clip_cntl_dx_rasterization_kill(
   struct terakan_app_config_draw * const config, bool const value)
{
   if (config->pa_cl_clip_cntl_.dx_rasterization_kill == value) {
      return;
   }
   config->pa_cl_clip_cntl_.dx_rasterization_kill = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_CL_CLIP_CNTL);
}

static inline void
terakan_app_config_draw_set_pa_cl_clip_cntl_dx_clip_space_def(
   struct terakan_app_config_draw * const config, bool const value)
{
   if (config->pa_cl_clip_cntl_.dx_clip_space_def == value) {
      return;
   }
   config->pa_cl_clip_cntl_.dx_clip_space_def = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_CL_CLIP_CNTL);
}

static inline void
terakan_app_config_draw_set_pa_cl_clip_cntl_z_clamp_enable(
   struct terakan_app_config_draw * const config, bool const value)
{
   if (config->pa_cl_clip_cntl_.z_clamp_enable == value) {
      return;
   }
   config->pa_cl_clip_cntl_.z_clamp_enable = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_CL_CLIP_CNTL);
}

static inline void
terakan_app_config_draw_set_pa_cl_clip_cntl_z_clip_enable_override(
   struct terakan_app_config_draw * const config, signed char const value)
{
   if (config->pa_cl_clip_cntl_.z_clip_enable_override == value) {
      return;
   }
   config->pa_cl_clip_cntl_.z_clip_enable_override = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_CL_CLIP_CNTL);
}

static inline void
terakan_app_config_draw_set_pa_vport_render_area(struct terakan_app_config_draw * const config,
                                                 struct terakan_screen_rect const value)
{
   if (terakan_screen_rect_equal(config->pa_vport_.render_area, value)) {
      return;
   }
   config->pa_vport_.render_area = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_VPORT);
}

static inline struct terakan_screen_rect
terakan_app_config_draw_get_pa_vport_render_area(struct terakan_app_config_draw const * const config)
{
   return config->pa_vport_.render_area;
}

static inline void
terakan_app_config_draw_set_pa_vport_z_range_unrestricted(
   struct terakan_app_config_draw * const config, bool const value)
{
   if (config->pa_vport_.z_range_unrestricted == value) {
      return;
   }
   config->pa_vport_.z_range_unrestricted = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_VPORT);
}

static inline void
terakan_app_config_draw_set_pa_vport_user_defined_zmin_zmax_enable(
   struct terakan_app_config_draw * const config, bool const value)
{
   if (config->pa_vport_.user_defined_zmin_zmax_enable == value) {
      return;
   }
   config->pa_vport_.user_defined_zmin_zmax_enable = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_VPORT);
}

static inline void
terakan_app_config_draw_set_pa_vport_user_defined_zmin_zmax(
   struct terakan_app_config_draw * const config, float const zmin, float const zmax)
{
   if (terakan_hw_config_draw_float_equal(config->pa_vport_.user_defined_zmin_zmax[0], zmin) &&
       terakan_hw_config_draw_float_equal(config->pa_vport_.user_defined_zmin_zmax[1], zmax)) {
      return;
   }
   config->pa_vport_.user_defined_zmin_zmax[0] = zmin;
   config->pa_vport_.user_defined_zmin_zmax[1] = zmax;
   if (config->pa_vport_.user_defined_zmin_zmax_enable) {
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_VPORT);
   }
}

static inline void
terakan_app_config_draw_set_pa_vport_vport_count(struct terakan_app_config_draw * const config,
                                                 unsigned const value)
{
   assert(value <= TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   if (config->pa_vport_.vport_count == value) {
      return;
   }
   config->pa_vport_.vport_count = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_VPORT);
}

static inline uint8_t
terakan_app_config_draw_get_pa_vport_vport_count(struct terakan_app_config_draw const * const config)
{
   return config->pa_vport_.vport_count;
}

static inline void
terakan_app_config_draw_set_pa_vport_vport(
   struct terakan_app_config_draw * const config, unsigned const vport_index,
   struct terakan_app_config_draw_pa_vport const * const value)
{
   assert(vport_index < TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   if (terakan_app_config_draw_pa_vport_equal(&config->pa_vport_.vports[vport_index], value)) {
      return;
   }
   config->pa_vport_.vports[vport_index] = *value;
   if (vport_index < config->pa_vport_.vport_count) {
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_VPORT);
   }
}

static inline void
terakan_app_config_draw_set_pa_vport_explicit_scissor_count(
   struct terakan_app_config_draw * const config, unsigned const value)
{
   assert(value <= TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   if (config->pa_vport_.explicit_scissor_count == value) {
      return;
   }
   config->pa_vport_.explicit_scissor_count = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_VPORT);
}

static inline uint8_t
terakan_app_config_draw_get_pa_vport_explicit_scissor_count(
   struct terakan_app_config_draw const * const config)
{
   return config->pa_vport_.explicit_scissor_count;
}

static inline void
terakan_app_config_draw_set_pa_vport_explicit_scissor(struct terakan_app_config_draw * const config,
                                                      unsigned const scissor_index,
                                                      struct terakan_screen_rect const value)
{
   assert(scissor_index < TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   if (terakan_screen_rect_equal(config->pa_vport_.explicit_scissors[scissor_index], value)) {
      return;
   }
   config->pa_vport_.explicit_scissors[scissor_index] = value;
   if (scissor_index < config->pa_vport_.explicit_scissor_count) {
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_VPORT);
   }
}

static inline void
terakan_app_config_draw_set_pa_su_sc_mode_cntl(struct terakan_app_config_draw * const config,
                                               uint32_t const keep_mask, uint32_t const value)
{
   terakan_app_config_draw_set_fields_(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_SC_MODE_CNTL,
                                       &config->pa_su_sc_mode_cntl_, keep_mask, value);
}

/* `width` is VkPipelineRasterizationStateCreateInfo::lineWidth, or the value of
 * vkCmdSetLineWidth.
 */
static inline void
terakan_app_config_draw_set_pa_su_line_width(struct terakan_app_config_draw * const config,
                                             float const width)
{
   /* The hardware field counts eighths of a pixel and saturates at the widest line it can express;
    * maxLineWidth is reported from the same limit, so a legal width never reaches the clamp.
    */
   uint32_t const eighths =
      (uint32_t)MIN2(MAX2(width, 0.0f) * 8.0f + 0.5f, (float)0xFFFFu);
   uint32_t const value = S_028A08_WIDTH(eighths);
   if (config->pa_su_line_cntl_ == value) {
      return;
   }
   config->pa_su_line_cntl_ = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_LINE_CNTL);
}

static inline void
terakan_app_config_draw_set_pa_sc_line_stipple_pattern(
   struct terakan_app_config_draw * const config, uint32_t const value)
{
   terakan_app_config_draw_set_fields_(
      config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_LINE_STIPPLE,
      &config->pa_sc_line_stipple_.pattern,
      C_028A0C_LINE_PATTERN & C_028A0C_REPEAT_COUNT & C_028A0C_PATTERN_BIT_ORDER, value);
}

static inline void
terakan_app_config_draw_set_pa_sc_line_stipple_enable(struct terakan_app_config_draw * const config,
                                                      bool const value)
{
   if (config->pa_sc_line_stipple_.enable == value) {
      return;
   }
   config->pa_sc_line_stipple_.enable = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_LINE_STIPPLE);
}

static inline void
terakan_app_config_draw_set_pa_su_poly_offset(
   struct terakan_app_config_draw * const config,
   struct terakan_hw_config_draw_pa_su_poly_offset const value)
{
   /* Not checking if modified because that's not important and done on the `hw_config` level
    * anyway, applying this entry is cheap.
    */
   config->pa_su_poly_offset_.poly_offset = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET);
}

/* The `PA_SC_AA_CONFIG_SAMPLE_LOCS` entry setters don't make it pending if not changed because
 * applying isn't entirely trivial.
 */

static inline void
terakan_app_config_draw_set_pa_sc_aa_config_msaa_num_samples_log2(
   struct terakan_app_config_draw * const config, uint8_t const value)
{
   assert(value <= 4);
   if (config->pa_sc_aa_config_sample_locs_.msaa_num_samples_log2 == value) {
      return;
   }
   config->pa_sc_aa_config_sample_locs_.msaa_num_samples_log2 = value;
   terakan_app_config_draw_set_pending(config,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_AA_CONFIG_SAMPLE_LOCS);
}

static inline void
terakan_app_config_draw_set_pa_sc_aa_sample_locs_custom_enable(
   struct terakan_app_config_draw * const config, bool const value)
{
   if (config->pa_sc_aa_config_sample_locs_.custom_sample_locs_enable == value) {
      return;
   }
   config->pa_sc_aa_config_sample_locs_.custom_sample_locs_enable = value;
   terakan_app_config_draw_set_pending(config,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_AA_CONFIG_SAMPLE_LOCS);
}

/* `value` is [16][4], where the inner index is [2 * y + x] for the pixel in a quad, copied wholly,
 * so the sample count and the sample locations can be set in any order.
 */
static inline void
terakan_app_config_draw_set_pa_sc_aa_sample_locs_custom_16_samples_2x2_locs(
   struct terakan_app_config_draw * const config, uint8_t const * const value)
{
   /* Only make pending if the actually used sample locations are changed. Changing other values
    * involved in this check will make the entry pending too anyway.
    */
   if (config->pa_sc_aa_config_sample_locs_.custom_sample_locs_enable &&
       memcmp(config->pa_sc_aa_config_sample_locs_.custom_sample_locs, value,
              ((unsigned)sizeof(uint8_t) * 4)
                 << config->pa_sc_aa_config_sample_locs_.msaa_num_samples_log2) != 0) {
      terakan_app_config_draw_set_pending(
         config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_AA_CONFIG_SAMPLE_LOCS);
   }
   memcpy(config->pa_sc_aa_config_sample_locs_.custom_sample_locs, value,
          (unsigned)sizeof(uint8_t) * 4 * 16);
}

static inline void
terakan_app_config_draw_set_pa_sc_aa_mask(struct terakan_app_config_draw * const config,
                                          uint16_t const value)
{
   if (config->pa_sc_aa_mask_ == value) {
      return;
   }
   config->pa_sc_aa_mask_ = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SC_AA_MASK);
}

static inline void
terakan_app_config_draw_set_sq_pgm_fragment(struct terakan_app_config_draw * const config,
                                            struct terakan_shader_impl const * const value)
{
   if (config->sq_pgm_fragment_ == value) {
      return;
   }
   config->sq_pgm_fragment_ = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_SQ_PGM_FRAGMENT);
}

static inline void
terakan_app_config_draw_push_db_count_control_zpass_query_active(
   struct terakan_app_config_draw * const config)
{
   if (config->db_count_control_.zpass_query_active_count++ == 0) {
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_COUNT_CONTROL);
   }
}

static inline void
terakan_app_config_draw_pop_db_count_control_zpass_query_active(
   struct terakan_app_config_draw * const config)
{
   assert(config->db_count_control_.zpass_query_active_count != 0);
   if (--config->db_count_control_.zpass_query_active_count == 0) {
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_COUNT_CONTROL);
   }
}

/* If `bo` is NULL, `descriptor` is ignored, and the depth / stencil buffer is unbound.
 *
 * `S_028040_NUM_SAMPLES` must be provided regardless of the architecture generation for making sure
 * that framebuffer attachments with incompatible sample counts aren't bound.
 *
 * #MemoryIntegrity is expected to be ensured before calling.
 */
static inline void
terakan_app_config_draw_set_db_depth_stencil_buffer(
   struct terakan_app_config_draw * const config, struct terakan_bo const * const bo,
   struct terakan_depth_stencil_descriptor const * const descriptor)
{
   /* Not checking if modified because that isn't entirely trivial and done on the `hw_config` level
    * anyway, don't repeat its work. This is expected to be called infrequently anyway.
    */
   bool depth_bound, stencil_bound;
   terakan_depth_stencil_descriptor_is_bound(bo, descriptor, &depth_bound, &stencil_bound);
   if (depth_bound || stencil_bound) {
      config->db_depth_stencil_buffer_.bo = bo;
      config->db_depth_stencil_buffer_.descriptor = *descriptor;
   } else {
      config->db_depth_stencil_buffer_.bo = NULL;
      config->db_depth_stencil_buffer_.descriptor = (struct terakan_depth_stencil_descriptor){};
   }
   terakan_app_config_draw_set_pending(config,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_BUFFER);
}

static inline struct terakan_depth_stencil_descriptor const *
terakan_app_config_draw_get_db_depth_stencil_buffer(
   struct terakan_app_config_draw const * const config, struct terakan_bo const ** const bo_out)
{
   assert(bo_out != NULL &&
          "The BO output pointer must be provided because the BO pointer is needed for checking "
          "whether the depth / stencil buffer is bound");
   *bo_out = config->db_depth_stencil_buffer_.bo;
   return &config->db_depth_stencil_buffer_.descriptor;
}

static inline void
terakan_app_config_draw_set_db_stencilrefmask(struct terakan_app_config_draw * const config,
                                              bool const back, uint32_t const keep_mask,
                                              uint32_t const value)
{
   terakan_app_config_draw_set_fields_(
      config, TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_CONTROL_REF_MASK,
      back ? &config->db_depth_stencil_control_ref_mask_.stencil_ref_mask_back
           : &config->db_depth_stencil_control_ref_mask_.stencil_ref_mask_front,
      keep_mask, value);
}

static inline void
terakan_app_config_draw_set_db_depth_control(struct terakan_app_config_draw * const config,
                                             uint32_t const keep_mask, uint32_t const value)
{
   terakan_app_config_draw_set_fields_(
      config, TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_CONTROL_REF_MASK,
      &config->db_depth_stencil_control_ref_mask_.depth_stencil_control, keep_mask, value);
}

/* Pass log2(maximum number of samples per shader invocation) if sample shading is needed, or a
 * negative value to disable it.
 * See `terakan_hw_config_draw_db_eqaa_ps_iter_max_invocation_samples_log2`.
 */
static inline void
terakan_app_config_draw_set_db_eqaa_ps_iter_max_invocation_samples_log2(
   struct terakan_app_config_draw * const config, int8_t const value)
{
   if (config->db_eqaa_.ps_iter_max_invocation_samples_log2 == value) {
      return;
   }
   config->db_eqaa_.ps_iter_max_invocation_samples_log2 = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_EQAA);
}

/* Sets log2 of the number of color attachment fragments that sample shading will be limited to on
 * R9xx. Pass log2(16), which is the default value of this parameter in `terakan_app_config_draw`,
 * or at least the rasterization sample count, if the subpass doesn't use color attachments.
 *
 * Prior to R9xx, this function has no effect.
 *
 * In EQAA, `PS_ITER_SAMPLES` - the sample shading amount - is not intended to be greater than the
 * minimum across the rasterization sample count and the fragment counts of all RTVs.
 *
 * Also, section "Sample Shading" of the Vulkan 1.4.352 specification says:
 *
 *     "If the `VK_AMD_mixed_attachment_samples` extension is enabled and the subpass uses color
 *     attachments, the `samples` value used to create each color attachment is used instead of
 *     `rasterizationSamples`."
 *
 * The minimum number of color attachment fragments is specified explicitly here instead of being
 * derived from the RTV descriptors to make sure the amount of sample shading stays explicitly and
 * consistently controlled by the application regardless of the actual RTV descriptors that end up
 * being bound.
 *
 * RTVs with a fragment count greater than this may be unbound implicitly.
 *
 * This limit has higher priority than `ps_iter_max_invocation_samples_log2`.
 */
static inline void
terakan_app_config_draw_set_db_eqaa_ps_iter_least_fragments_log2_r9xx(
   struct terakan_app_config_draw * const config, int8_t const value)
{
   if (config->db_eqaa_.ps_iter_least_fragments_log2_r9xx == value) {
      return;
   }
   config->db_eqaa_.ps_iter_least_fragments_log2_r9xx = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_EQAA);
}

static inline void
terakan_app_config_draw_set_db_alpha_to_mask(struct terakan_app_config_draw * const config,
                                             uint32_t const keep_mask, uint32_t const value)
{
   terakan_app_config_draw_set_fields_(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_ALPHA_TO_MASK,
                                       &config->db_alpha_to_mask_, keep_mask, value);
}

static inline void
terakan_app_config_draw_set_pa_su_poly_offset_db_fmt_cntl(
   struct terakan_app_config_draw * const config,
   enum terakan_app_config_draw_poly_offset_representation const representation, bool const exact)
{
   /* Applying isn't entirely trivial, and changes are rare, so don't make pending if not modified.
    */
   if (config->pa_su_poly_offset_db_fmt_cntl_.representation == representation &&
       config->pa_su_poly_offset_db_fmt_cntl_.exact == exact) {
      return;
   }
   config->pa_su_poly_offset_db_fmt_cntl_.representation = representation;
   config->pa_su_poly_offset_db_fmt_cntl_.exact = exact;
   if (config->pa_su_poly_offset_db_fmt_cntl_.from_apply_pa_su_sc_mode_cntl.poly_offset_enable) {
      terakan_app_config_draw_set_pending(
         config, TERAKAN_APP_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET_DB_FMT_CNTL);
   }
}

static inline void
terakan_app_config_draw_set_cb_rop3_enable(struct terakan_app_config_draw * const config,
                                           bool const value)
{
   if (config->cb_rop3_.rop3_enable == value) {
      return;
   }
   config->cb_rop3_.rop3_enable = value;
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_ROP3);
}

static inline void
terakan_app_config_draw_set_cb_rop3(struct terakan_app_config_draw * const config,
                                    enum terakan_hw_config_draw_cb_color_control_rop3 const value)
{
   if (config->cb_rop3_.rop3 == value) {
      return;
   }
   config->cb_rop3_.rop3 = value;
   if (config->cb_rop3_.rop3_enable) {
      terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_ROP3);
   }
}

static inline void
terakan_app_config_draw_set_cb_color_rtv_write_enable_mask_all(
   struct terakan_app_config_draw * const config, uint8_t const value)
{
   if (config->cb_color_rtv_and_blend_control_.write_enable_mask == value) {
      return;
   }
   config->cb_color_rtv_and_blend_control_.write_enable_mask = value;
   terakan_app_config_draw_set_pending(
      config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL);
}

static inline void
terakan_app_config_draw_set_cb_color_rtv_write_component_mask_all(
   struct terakan_app_config_draw * const config, uint32_t const value)
{
   terakan_app_config_draw_set_register_(
      config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL,
      &config->cb_color_rtv_and_blend_control_.write_component_mask, value);
}

/* If `terakan_color_descriptor_is_bound` for the provided arguments is false, the RTV is unbound.
 * `NUM_SAMPLES` must be provided regardless of the architecture generation.
 * If the meta descriptor pointer is NULL, the meta surfaces will be disabled for this target.
 * #MemoryIntegrity is expected to be ensured before calling.
 */
void terakan_app_config_draw_set_cb_color_rtv(struct terakan_app_config_draw * config,
                                              unsigned color_index, struct terakan_bo const * bo,
                                              struct terakan_color_descriptor const * color,
                                              struct terakan_color_meta_descriptor const * meta);

static inline struct terakan_app_config_draw_cb_color_rtv const *
terakan_app_config_draw_get_cb_color_rtv(struct terakan_app_config_draw const * const config,
                                         unsigned const color_index)
{
   assert(color_index < TERAKAN_COLOR_HW_RTV_COUNT);
   return &config->cb_color_rtv_and_blend_control_.rtv[color_index];
}

/* If `terakan_color_descriptor_is_bound` for the provided arguments is false, the UAV is unbound.
 * #MemoryIntegrity is expected to be ensured before calling.
 */
#define TERAKAN_APP_CONFIG_DRAW_UAV_BIND_POINT_GRAPHICS 0
#define TERAKAN_APP_CONFIG_DRAW_UAV_BIND_POINT_COMPUTE  1

void terakan_app_config_draw_set_cb_color_uav(struct terakan_app_config_draw * config,
                                              unsigned bind_point,
                                              unsigned mutable_resource_index,
                                              struct terakan_bo const * bo,
                                              struct terakan_color_descriptor const * color);

static inline void
terakan_app_config_draw_set_cb_blend_control(struct terakan_app_config_draw * const config,
                                             unsigned const color_index, uint32_t const keep_mask,
                                             uint32_t const value)
{
   assert(color_index < TERAKAN_COLOR_HW_RTV_COUNT);
   terakan_app_config_draw_set_fields_(
      config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_RTV_AND_BLEND_CONTROL,
      &config->cb_color_rtv_and_blend_control_.rtv[color_index].blend_control, keep_mask, value);
}

static inline void
terakan_app_config_draw_set_cb_blend_constants(struct terakan_app_config_draw * const config,
                                               float const value[4])
{
   /* Not checking if modified because that's not important and done on the `hw_config` level
    * anyway, applying this entry is cheap.
    */
   memcpy(config->cb_blend_constants_.constants, value, sizeof(float) * 4);
   terakan_app_config_draw_set_pending(config, TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_BLEND_CONSTANTS);
}

struct terakan_gfx_command_writer;

void terakan_app_config_draw_apply_pending(struct terakan_gfx_command_writer * command_writer,
                                           bool for_compute);

void terakan_app_config_draw_restore_cb_state_after_compute(
   struct terakan_gfx_command_writer * command_writer);

void terakan_app_config_draw_reset(struct terakan_app_config_draw * config);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_APP_CONFIG_DRAW_H */
