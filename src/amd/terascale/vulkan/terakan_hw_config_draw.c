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

#include "terakan_hw_config_draw.h"

#include "terakan_command_buffer.h"
#include "terakan_device.h"
#include "terakan_image.h"
#include "terakan_physical_device.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_endian.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TERAKAN_STANDARD_SAMPLE_LOCS(s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13,   \
                                     s14, s15)                                                     \
   {                                                                                               \
      {0x##s0, 0x##s0, 0x##s0, 0x##s0}, {0x##s1, 0x##s1, 0x##s1, 0x##s1},                          \
         {0x##s2, 0x##s2, 0x##s2, 0x##s2}, {0x##s3, 0x##s3, 0x##s3, 0x##s3},                       \
         {0x##s4, 0x##s4, 0x##s4, 0x##s4}, {0x##s5, 0x##s5, 0x##s5, 0x##s5},                       \
         {0x##s6, 0x##s6, 0x##s6, 0x##s6}, {0x##s7, 0x##s7, 0x##s7, 0x##s7},                       \
         {0x##s8, 0x##s8, 0x##s8, 0x##s8}, {0x##s9, 0x##s9, 0x##s9, 0x##s9},                       \
         {0x##s10, 0x##s10, 0x##s10, 0x##s10}, {0x##s11, 0x##s11, 0x##s11, 0x##s11},               \
         {0x##s12, 0x##s12, 0x##s12, 0x##s12}, {0x##s13, 0x##s13, 0x##s13, 0x##s13},               \
         {0x##s14, 0x##s14, 0x##s14, 0x##s14}, {0x##s15, 0x##s15, 0x##s15, 0x##s15},               \
   }

/* Direct3D 10.1 and Vulkan standard sample locations.
 * Note that these are ordered as defined in the API specifications. For EQAA with mixed coverage
 * and attachment sample counts, samples 23, 4567, 89ABCDEF should refine samples 01, 0123, 01234567
 * respectively, and samples 0 and 1 should be in diametrically opposed quadrants. This is not the
 * case for these standard sample locations, which are ordered by distance to the center.
 * See Gallium RadeonSI `si_state_msaa.c` for more information.
 */
uint8_t const terakan_hw_config_draw_pa_sc_aa_standard_sample_locs[5][16][4] = {
   TERAKAN_STANDARD_SAMPLE_LOCS(00, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00),
   TERAKAN_STANDARD_SAMPLE_LOCS(44, CC, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00),
   TERAKAN_STANDARD_SAMPLE_LOCS(AE, E6, 2A, 62, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00, 00),
   TERAKAN_STANDARD_SAMPLE_LOCS(D1, 3F, 15, BD, 5B, F9, 73, 97, 00, 00, 00, 00, 00, 00, 00, 00),
   TERAKAN_STANDARD_SAMPLE_LOCS(11, DF, 2D, F4, EB, 52, 35, B3, 6E, 90, AC, 4A, 08, C7, 76, 89),
};

uint8_t const terakan_hw_config_draw_pa_sc_aa_standard_max_sample_dists[5] = {0, 4, 6, 7, 8};

#undef TERAKAN_STANDARD_SAMPLE_LOCS

uint8_t const terakan_hw_config_draw_cb_blend_control_color_factors_for_color_alpha[0x20] = {
   V_028780_BLEND_ZERO,
   V_028780_BLEND_ONE,
   V_028780_BLEND_SRC_COLOR,
   V_028780_BLEND_ONE_MINUS_SRC_COLOR,
   V_028780_BLEND_SRC_COLOR,
   V_028780_BLEND_ONE_MINUS_SRC_COLOR,
   V_028780_BLEND_DST_COLOR,
   V_028780_BLEND_ONE_MINUS_DST_COLOR,
   V_028780_BLEND_DST_COLOR,
   V_028780_BLEND_ONE_MINUS_DST_COLOR,
   V_028780_BLEND_SRC_ALPHA_SATURATE,
   V_028780_BLEND_BOTH_SRC_ALPHA,
   V_028780_BLEND_BOTH_INV_SRC_ALPHA,
   V_028780_BLEND_CONST_COLOR,
   V_028780_BLEND_ONE_MINUS_CONST_COLOR,
   V_028780_BLEND_SRC1_COLOR,
   V_028780_BLEND_INV_SRC1_COLOR,
   V_028780_BLEND_SRC1_COLOR,
   V_028780_BLEND_INV_SRC1_COLOR,
   V_028780_BLEND_CONST_COLOR,
   V_028780_BLEND_ONE_MINUS_CONST_COLOR,
   0x15,
   0x16,
   0x17,
   0x18,
   0x19,
   0x1A,
   0x1B,
   0x1C,
   0x1D,
   0x1E,
   0x1F,
};

void
terakan_hw_config_draw_set_vgt_dma_index_buffer(
   struct terakan_hw_config_draw * const config,
   struct terakan_hw_config_draw_vgt_dma_index_buffer index_buffer, uint32_t const index_type)
{
   if (index_buffer.bo == NULL) {
      index_buffer.size_indices = 0;
   } else {
      /* Ensure #MemoryIntegrity while gracefully recovering from misalignment and an out of bounds
       * binding.
       * VUID-vkCmdBindIndexBuffer-offset-08783:
       *     "The sum of offset and the base address of the range of VkDeviceMemory object that is
       *     backing buffer, must be a multiple of the size of the type indicated by indexType"
       * `INDEX_BASE` is word-aligned according to Radeon Evergreen / Northern Islands Acceleration.
       */
      unsigned const index_size_bytes_log2 = index_type & 0b1 ? 2 : 1;
      index_buffer.va = index_buffer.va >> index_size_bytes_log2 << index_size_bytes_log2;
      if (unlikely(index_buffer.va < index_buffer.bo->va ||
                   index_buffer.va - index_buffer.bo->va > index_buffer.bo->size)) {
         index_buffer.size_indices = 0;
      } else {
         index_buffer.size_indices =
            MIN2((index_buffer.bo->size - (index_buffer.va - index_buffer.bo->va)) >>
                    index_size_bytes_log2,
                 index_buffer.size_indices);
      }
   }

   if (index_buffer.size_indices == 0) {
      /* Unbind. */
      if (terakan_hw_config_draw_vgt_dma_index_buffer_is_bound(config->vgt_dma_index_buffer_)) {
         config->vgt_dma_index_buffer_.size_indices = 0;
         BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER);
      }
      return;
   }

   if (config->vgt_dma_index_buffer_.bo != index_buffer.bo ||
       config->vgt_dma_index_buffer_.va != index_buffer.va ||
       config->vgt_dma_index_buffer_.size_indices != index_buffer.size_indices) {
      config->vgt_dma_index_buffer_.bo = index_buffer.bo;
      config->vgt_dma_index_buffer_.va = index_buffer.va;
      config->vgt_dma_index_buffer_.size_indices = index_buffer.size_indices;
      BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER);
   }

   terakan_hw_config_draw_set_single_register_(config,
                                               TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_TYPE,
                                               &config->vgt_dma_index_type_, index_type);
}

void
terakan_hw_config_draw_set_db_depth_stencil_buffer(
   struct terakan_hw_config_draw * const config, struct terakan_bo const * const bo,
   struct terakan_depth_stencil_descriptor const * const descriptor)
{
   bool new_depth_bound, new_stencil_bound;
   terakan_depth_stencil_descriptor_is_bound(bo, descriptor, &new_depth_bound, &new_stencil_bound);

   bool old_depth_bound, old_stencil_bound;
   terakan_depth_stencil_descriptor_is_bound(config->db_depth_stencil_buffer_.bo,
                                             &config->db_depth_stencil_buffer_.descriptor,
                                             &old_depth_bound, &old_stencil_bound);

   if (!new_depth_bound && !new_stencil_bound) {
      if (old_depth_bound || old_stencil_bound) {
         config->db_depth_stencil_buffer_.bo = NULL;
         config->db_depth_stencil_buffer_.descriptor = (struct terakan_depth_stencil_descriptor){};
         BITSET_SET(config->entries_modified_,
                    TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_BUFFER);
      }
      return;
   }

   /* `DB_Z_INFO` contains fields used not only by the depth aspect, but by the stencil aspect too.
    */
   uint32_t z_info = descriptor->z_info;
   if (!new_depth_bound) {
      z_info &= C_028040_FORMAT & C_028040_ZRANGE_PRECISION & C_028040_TILE_SPLIT;
   }

   if (old_depth_bound == new_depth_bound && old_stencil_bound == new_stencil_bound &&
       config->db_depth_stencil_buffer_.bo == bo &&
       config->db_depth_stencil_buffer_.descriptor.z_info == z_info &&
       config->db_depth_stencil_buffer_.descriptor.size == descriptor->size &&
       config->db_depth_stencil_buffer_.descriptor.slice == descriptor->slice &&
       (!new_depth_bound ||
        config->db_depth_stencil_buffer_.descriptor.z_base == descriptor->z_base) &&
       (!new_stencil_bound ||
        (config->db_depth_stencil_buffer_.descriptor.stencil_info == descriptor->stencil_info &&
         config->db_depth_stencil_buffer_.descriptor.stencil_base == descriptor->stencil_base))) {
      return;
   }
   config->db_depth_stencil_buffer_.bo = bo;
   config->db_depth_stencil_buffer_.descriptor.view = descriptor->view;
   config->db_depth_stencil_buffer_.descriptor.z_info = z_info;
   config->db_depth_stencil_buffer_.descriptor.z_base =
      new_depth_bound ? descriptor->z_base : descriptor->stencil_base;
   config->db_depth_stencil_buffer_.descriptor.stencil_info =
      new_stencil_bound ? descriptor->stencil_info : S_028044_FORMAT(V_028044_STENCIL_INVALID);
   config->db_depth_stencil_buffer_.descriptor.stencil_base =
      new_depth_bound ? descriptor->stencil_base : descriptor->z_base;
   config->db_depth_stencil_buffer_.descriptor.size = descriptor->size;
   config->db_depth_stencil_buffer_.descriptor.slice = descriptor->slice;
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_BUFFER);
}

void
terakan_hw_config_draw_set_cb_color(struct terakan_hw_config_draw * const config,
                                    unsigned const color_index, struct terakan_bo const * const bo,
                                    struct terakan_color_descriptor const * const color,
                                    struct terakan_color_meta_descriptor const * meta)
{
   assert(color_index < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);

   if (!terakan_color_descriptor_is_bound(bo, color)) {
      terakan_hw_config_draw_set_cb_color_unbound(config, color_index, V_028C70_EXPORT_4C_16BPC);
      return;
   }

   /* Convert the intermediate representation of the descriptor to the actual hardware descriptor,
    * with irrelevant fields changed to a constant. This is done when setting, not when emitting, so
    * changes only in irrelevant fields don't cause the registers to be marked as modified and to be
    * re-emitted.
    * `NUM_SAMPLES` is needed by the emission logic internally regardless of the architecture
    * generation, so it's not eliminated (and also because the architecture generation is not known
    * in this function, thus `NUM_FRAGMENTS` is not removed either).
    */

   struct terakan_color_descriptor color_normalized = *color;
   if (G_028C70_RAT(color->info)) {
      terakan_color_descriptor_to_hw_uav(&color_normalized);
      meta = NULL;
   } else {
      terakan_color_descriptor_to_hw_rtv(&color_normalized);
   }

   bool const has_meta = color_index < TERAKAN_COLOR_HW_RTV_COUNT;
   struct terakan_color_meta_descriptor disabled_meta;
   if (has_meta && meta == NULL) {
      disabled_meta = terakan_color_meta_descriptor_create_disabled(color);
      meta = &disabled_meta;
   }

   if (config->cb_color_.bo[color_index] == bo &&
       memcmp(&config->cb_color_.color[color_index], &color_normalized,
              sizeof(struct terakan_color_descriptor)) == 0 &&
       (!has_meta || memcmp(&config->cb_color_.meta[color_index], meta,
                            sizeof(struct terakan_color_meta_descriptor)) == 0)) {
      return;
   }

   config->cb_color_.bo[color_index] = bo;
   config->cb_color_.color[color_index] = color_normalized;
   if (has_meta) {
      config->cb_color_.meta[color_index] = *meta;
   }

   config->cb_color_.modified_bits |= BITFIELD_BIT(color_index);
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_COLOR);
}

static bool
terakan_hw_config_draw_emit_context_register(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const register_address_bytes,
   uint32_t const register_value)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1);
   if (unlikely(packet == NULL)) {
      return false;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(register_address_bytes);
   *packet++ = register_value;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
   return true;
}

void
terakan_hw_config_draw_emit_constant(struct terakan_gfx_command_writer * const command_writer)
{
#define TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(address, value)                                \
   PKT3(PKT3_SET_CONTEXT_REG, 1, 0), TERAKAN_CONTEXT_REG_OFFSET(address), (value)

#define TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_RANGE(address_first, address_last)                    \
   PKT3(PKT3_SET_CONTEXT_REG, ((address_last) - (address_first)) / sizeof(uint32_t) + 1, 0),       \
      TERAKAN_CONTEXT_REG_OFFSET(address_first)

   /* This array contains only compile-time constants to avoid copying. */
   uint32_t const common_config[] = {
      /* TODO(Triang3l): Expose DB and CB metadata configuration. */
      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(R_028000_DB_RENDER_CONTROL, 0),

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_RANGE(R_028030_PA_SC_SCREEN_SCISSOR_TL,
                                                 R_028034_PA_SC_SCREEN_SCISSOR_BR),
      /* R_028030_PA_SC_SCREEN_SCISSOR_TL */
      0,
      /* R_028034_PA_SC_SCREEN_SCISSOR_BR */
      S_028034_BR_X(TERAKAN_IMAGE_MAX_WIDTH_HEIGHT) | S_028034_BR_Y(TERAKAN_IMAGE_MAX_WIDTH_HEIGHT),

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_RANGE(R_028204_PA_SC_WINDOW_SCISSOR_TL,
                                                 R_02820C_PA_SC_CLIPRECT_RULE),
      /* R_028204_PA_SC_WINDOW_SCISSOR_TL */
      S_028204_WINDOW_OFFSET_DISABLE(true),
      /* R_028208_PA_SC_WINDOW_SCISSOR_BR */
      S_028208_BR_X(TERAKAN_IMAGE_MAX_WIDTH_HEIGHT) | S_028208_BR_Y(TERAKAN_IMAGE_MAX_WIDTH_HEIGHT),
      /* R_02820C_PA_SC_CLIPRECT_RULE */
      /* TODO(Triang3l): Expose clip rectangle configuration. */
      0xFFFF,

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_RANGE(R_028230_PA_SC_EDGERULE,
                                                 R_028234_PA_SU_HARDWARE_SCREEN_OFFSET),
      /* R_028230_PA_SC_EDGERULE
       * Direct3D top-left rule, also compatible with Direct3D line rasterization diamond test.
       */
      S_028230_ER_TRI(0b1010) | S_028230_ER_POINT(0b1010) | S_028230_ER_RECT(0b1010) |
         S_028230_ER_LINE_LR(0b011010) | S_028230_ER_LINE_RL(0b100110) |
         S_028230_ER_LINE_TB(0b1010) | S_028230_ER_LINE_BT(0b1010),
      /* R_028234_PA_SU_HARDWARE_SCREEN_OFFSET */
      0,

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_RANGE(R_028240_PA_SC_GENERIC_SCISSOR_TL,
                                                 R_028244_PA_SC_GENERIC_SCISSOR_BR),
      /* R_028240_PA_SC_GENERIC_SCISSOR_TL */
      S_028240_WINDOW_OFFSET_DISABLE(true),
      /* R_028244_PA_SC_GENERIC_SCISSOR_BR */
      S_028244_BR_X(TERAKAN_IMAGE_MAX_WIDTH_HEIGHT) | S_028244_BR_Y(TERAKAN_IMAGE_MAX_WIDTH_HEIGHT),

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(R_028350_SX_MISC, 0),

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_RANGE(R_028400_VGT_MAX_VTX_INDX,
                                                 R_028404_VGT_MIN_VTX_INDX),
      UINT32_MAX, /* R_028400_VGT_MAX_VTX_INDX */
      0,          /* R_028404_VGT_MIN_VTX_INDX */

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(
         R_028410_SX_ALPHA_TEST_CONTROL,
         S_028410_ALPHA_FUNC(0b111) | S_028410_ALPHA_TEST_BYPASS(true)),

      /* `SX_ALPHA_REF` doesn't have an effect on rendering when alpha test is disabled, but make
       * sure everything related to the reference in the GPU behaves the same regardless of what the
       * GPU executes prior to this indirect buffer.
       */
      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(R_028438_SX_ALPHA_REF, 0),

      /* `SPI_THREAD_GROUPING` same as in the Gallium R600 driver as of May 2026 and in WDDM Radeon
       * Software 15.301.1901, though different from DRM Radeon 2.50.0 `cleanstate_evergreen.h` and
       * `cleanstate_cayman.h`, where it's 1.
       */
      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(R_0286C8_SPI_THREAD_GROUPING, 0),

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(
         R_0286D4_SPI_INTERP_CONTROL_0,
         S_0286D4_FLAT_SHADE_ENA(1) | S_0286D4_PNT_SPRITE_ENA(1) |
            S_0286D4_PNT_SPRITE_OVRD_X(V_0286D4_SPI_PNT_SPRITE_SEL_S) |
            S_0286D4_PNT_SPRITE_OVRD_Y(V_0286D4_SPI_PNT_SPRITE_SEL_T) |
            S_0286D4_PNT_SPRITE_OVRD_Z(V_0286D4_SPI_PNT_SPRITE_SEL_0) |
            S_0286D4_PNT_SPRITE_OVRD_W(V_0286D4_SPI_PNT_SPRITE_SEL_1)),

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(R_0286DC_SPI_FOG_CNTL, 0),

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(R_0286E4_SPI_PS_IN_CONTROL_2, 0),

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(R_028820_PA_CL_NANINF_CNTL, 0),

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(R_0288A8_SQ_PGM_RESOURCES_FS,
                                                  S_0288A8_DX10_CLAMP(true)),

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_RANGE(R_0288E8_SQ_LDS_ALLOC, R_0288EC_SQ_LDS_ALLOC_PS),
      0, /* R_0288E8_SQ_LDS_ALLOC */
      0, /* R_0288EC_SQ_LDS_ALLOC_PS */

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_RANGE(R_028A00_PA_SU_POINT_SIZE,
                                                 R_028A08_PA_SU_LINE_CNTL),
      /* R_028A00_PA_SU_POINT_SIZE */
      S_028A00_HEIGHT((uint32_t)1 << 3) | S_028A00_WIDTH((uint32_t)1 << 3),
      /* R_028A04_PA_SU_POINT_MINMAX */
      S_028A04_MAX_SIZE(0xFFFF),
      /* R_028A08_PA_SU_LINE_CNTL */
      /* TODO(Triang3l): Expose line width configuration. */
      S_028A08_WIDTH((uint32_t)1 << 3),

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_RANGE(R_028A14_VGT_HOS_CNTL,
                                                 R_028A3C_VGT_GROUP_VECT_1_FMT_CNTL),
      0,                          /* R_028A14_VGT_HOS_CNTL */
      (uint32_t)(0x7F + 6) << 23, /* R_028A18_VGT_HOS_MAX_TESS_LEVEL = 64.0f */
      0,                          /* R_028A1C_VGT_HOS_MIN_TESS_LEVEL */
      16,                         /* R_028A20_VGT_HOS_REUSE_DEPTH */
      0,                          /* R_028A24_VGT_GROUP_PRIM_TYPE */
      0,                          /* R_028A28_VGT_GROUP_FIRST_DECR */
      0,                          /* R_028A2C_VGT_GROUP_DECR */
      0,                          /* R_028A30_VGT_GROUP_VECT_0_CNTL */
      0,                          /* R_028A34_VGT_GROUP_VECT_1_CNTL */
      0,                          /* R_028A38_VGT_GROUP_VECT_0_FMT_CNTL */
      0,                          /* R_028A3C_VGT_GROUP_VECT_1_FMT_CNTL */

      /* TODO(Triang3l): Expose geometry shader configuration. */
      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(R_028A40_VGT_GS_MODE,
                                                  S_028A40_MODE(V_028A40_GS_OFF)),

      /* TODO(Triang3l): Expose geometry shader configuration. */
      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(R_028A84_VGT_PRIMITIVEID_EN, 0),

      /* TODO(Triang3l): Expose stream output configuration and viewport index output. */
      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(R_028AB4_VGT_REUSE_OFF, 0),

      /* TODO(Triang3l): Expose tessellation and geometry shader configuration. */
      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(R_028AB8_VGT_VTX_CNT_EN, 0),

      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_RANGE(R_028AC0_DB_SRESULTS_COMPARE_STATE0,
                                                 R_028AC8_DB_PRELOAD_CONTROL),
      0, /* R_028AC0_DB_SRESULTS_COMPARE_STATE0 */
      0, /* R_028AC4_DB_SRESULTS_COMPARE_STATE1 */
      0, /* R_028AC8_DB_PRELOAD_CONTROL */

      /* TODO(Triang3l): Expose stream output configuration. */
      TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_RANGE(R_028B94_VGT_STRMOUT_CONFIG,
                                                 R_028B98_VGT_STRMOUT_BUFFER_CONFIG),
      0, /* R_028B94_VGT_STRMOUT_CONFIG */
      0, /* R_028B98_VGT_STRMOUT_BUFFER_CONFIG */
   };

   {
      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
         ARRAY_SIZE(common_config));
      if (unlikely(packet == NULL)) {
         return;
      }
      memcpy(packet, common_config, sizeof(common_config));
      packet += ARRAY_SIZE(common_config);
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }

   if (terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      /* This array contains only compile-time constants to avoid copying. */
      uint32_t const r9xx_config[] = {
         TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(CM_R_0286FC_SPI_LDS_MGMT, 0),

         /* PAL gives higher priority to samples with a shorter distance to the center, but
          * according to the ID3D12GraphicsCommandList1::SetSamplePositions reference on MSDN, this
          * is not valid in Direct3D 12:
          *
          *     "If centroid interpolation is used during rendering, the order of positions for each
          *     pixel determines centroid-sampling priority. That is, the first covered sample in
          *     the order specified is chosen as the centroid sample location."
          *
          * In addition, custom centroid priority is not supported before R9xx, thus sorting would
          * introduce visual differences between the architecture generations, and unlike the sample
          * locations themselves, the priority can't be specified for each pixel in a quad
          * separately.
          *
          * Vulkan, according to the "Interpolation Decorations" section of the Vulkan 1.4.335
          * specification, only requires that:
          *
          *     "If the Centroid decoration is used, the interpolation position used for the
          *     variable must also fall within the bounds of the primitive being rasterized."
          */
         TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_RANGE(CM_R_028BD4_PA_SC_CENTROID_PRIORITY_0,
                                                    CM_R_028BD8_PA_SC_CENTROID_PRIORITY_1),
         0x76543210,
         0xFEDCBA98,

         /* Direct3D 10 fixed-point vertex positions. */
         TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(
            CM_R_028BE4_PA_SU_VTX_CNTL, S_028C08_PIX_CENTER_HALF(true) |
                                           S_028C08_ROUND_MODE(V_028C08_X_ROUND_TO_EVEN) |
                                           S_028C08_QUANT_MODE(V_028C08_X_1_256TH)),
      };

      {
         uint32_t * packet = terakan_gfx_command_writer_emit(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
            ARRAY_SIZE(r9xx_config));
         if (unlikely(packet == NULL)) {
            return;
         }
         memcpy(packet, r9xx_config, sizeof(r9xx_config));
         packet += ARRAY_SIZE(r9xx_config);
         terakan_gfx_command_writer_emit_done(command_writer, packet);
      }
   } else {
      /* This array contains only compile-time constants to avoid copying. */
      uint32_t const r8xx_config[] = {
         /* Workaround for hardware issues with dynamic GPRs - must set all limits to 240 (in units
          * of 8 registers) instead of 0.
          */
         TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(
            R_028838_SQ_DYN_GPR_RESOURCE_LIMIT_1,
            S_028838_PS_GPRS(0x1E) | S_028838_VS_GPRS(0x1E) | S_028838_GS_GPRS(0x1E) |
               S_028838_ES_GPRS(0x1E) | S_028838_HS_GPRS(0x1E) | S_028838_LS_GPRS(0x1E)),

         /* Direct3D 10 fixed-point vertex positions. */
         TERAKAN_HW_CONFIG_DRAW_EMIT_CONSTANT_SINGLE(
            R_028C08_PA_SU_VTX_CNTL, S_028C08_PIX_CENTER_HALF(true) |
                                        S_028C08_ROUND_MODE(V_028C08_X_ROUND_TO_EVEN) |
                                        S_028C08_QUANT_MODE(V_028C08_X_1_256TH)),
      };

      {
         uint32_t * packet = terakan_gfx_command_writer_emit(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
            ARRAY_SIZE(r8xx_config));
         if (unlikely(packet == NULL)) {
            return;
         }
         memcpy(packet, r8xx_config, sizeof(r8xx_config));
         packet += ARRAY_SIZE(r8xx_config);
         terakan_gfx_command_writer_emit_done(command_writer, packet);
      }
   }
}

/* Note that dependencies between entries in emissions are not supported. An emit function must
 * access only the fields belonging to the entry it's emitting.
 *
 * If some registers require complex management, it must done on the `terakan_app_config` level
 * instead.
 */

typedef void (*terakan_hw_config_draw_emit_function)(
   struct terakan_gfx_command_writer * command_writer);

static void
terakan_hw_config_draw_emit_vgt_index_offset(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_028408_VGT_INDX_OFFSET,
                                                command_writer->hw_config_draw.vgt_index_offset_);
}

static void
terakan_hw_config_draw_emit_vgt_multi_prim_ib_reset_index(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(
      command_writer, R_02840C_VGT_MULTI_PRIM_IB_RESET_INDX,
      command_writer->hw_config_draw.vgt_multi_prim_ib_reset_index_);
}

static void
terakan_hw_config_draw_emit_vgt_multi_prim_ib_reset_en(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(
      command_writer, R_028A94_VGT_MULTI_PRIM_IB_RESET_EN,
      S_028A94_RESET_EN(command_writer->hw_config_draw.vgt_multi_prim_ib_reset_en_));
}

static void
terakan_hw_config_draw_emit_vgt_dma_index_buffer(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet;

   struct terakan_hw_config_draw_vgt_dma_index_buffer const index_buffer =
      command_writer->hw_config_draw.vgt_dma_index_buffer_;

   if (!terakan_hw_config_draw_vgt_dma_index_buffer_is_bound(index_buffer)) {
      /* TODO(Triang3l): Does GFX6 `has_null_index_buffer_clamping_bug` (`INDEX_BASE = 0` resulting
       * in overflow check upper bound address wraparound) need to be handled by binding a dummy
       * buffer (such as the meta shader buffer) at least once per indirect buffer? Check on R9xx
       * with virtual memory.
       */
      packet = terakan_gfx_command_writer_emit(command_writer,
                                               TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(EG_PKT3_INDEX_BUFFER_SIZE, 0, 0);
      *packet++ = 0;
      terakan_gfx_command_writer_emit_done(command_writer, packet);
      return;
   }

   /* #MemoryIntegrity is handled by the setter. */

   packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 3, 1, 0, 1);
   if (unlikely(packet == NULL)) {
      return;
   }

   *packet++ = PKT3(EG_PKT3_INDEX_BUFFER_SIZE, 0, 0);
   *packet++ = index_buffer.size_indices;

   /* Emitted last because in DRM Radeon 2.50.0 and 2.51.0, an `INDEX_BASE` packet spuriously
    * invokes binding validation.
    */
   *packet++ = PKT3(EG_PKT3_INDEX_BASE, 2 - 1, 0);
   uint32_t const * const packet_index_base = packet;
   *packet++ = (uint32_t)index_buffer.va;
   *packet++ = (index_buffer.va >> 32) & 0xFF;
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_index_base, packet_index_base + 1,
      TERASCALE_WDDM_PATCH_IDS_INDEX_BASE_LO, TERASCALE_WDDM_PATCH_IDS_INDEX_BASE_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                index_buffer.bo, true, false,
                                                TERAKAN_BO_PRIORITY_INDEX_BUFFER));

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_vgt_dma_index_type(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_INDEX_TYPE, 0, 0);
   *packet++ = command_writer->hw_config_draw.vgt_dma_index_type_;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_ia_multi_vgt_param(
   struct terakan_gfx_command_writer * const command_writer)
{
   if (!terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      return;
   }
   terakan_hw_config_draw_emit_context_register(command_writer, CM_R_028AA8_IA_MULTI_VGT_PARAM,
                                                command_writer->hw_config_draw.ia_multi_vgt_param_);
}

static void
terakan_hw_config_draw_emit_vgt_shader_stages_en(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(
      command_writer, R_028B54_VGT_SHADER_STAGES_EN,
      command_writer->hw_config_draw.vgt_shader_stages_en_);
}

static void
terakan_hw_config_draw_emit_sq_pgm_fs(struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1, 1, 1, 0);
   if (unlikely(packet == NULL)) {
      return;
   }
   struct terakan_bo const * bo = command_writer->hw_config_draw.sq_pgm_fs_.bo;
   uint32_t va_shr8 = command_writer->hw_config_draw.sq_pgm_fs_.va_shr8;
   if (bo == NULL) {
      struct terakan_device const * const device =
         terakan_gfx_command_writer_device(command_writer);
      bo = device->meta_shaders_bo;
      va_shr8 = device->meta_shaders_empty_fetch_va_shr8;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_0288A4_SQ_PGM_START_FS);
   uint32_t const * const packet_pgm_start = packet;
   *packet++ = va_shr8;
   terakan_gfx_command_writer_add_relocation(
      command_writer, &packet, packet_pgm_start, *packet_pgm_start,
      TERASCALE_WDDM_PATCH_IDS_SQ_PGM_START_FS,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer, bo, true,
                                                false, TERAKAN_BO_PRIORITY_SHADER_BINARY));
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_sq_pgm_vs(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_shader_static const * shader = command_writer->hw_config_draw.sq_pgm_vs_;
   if (shader == NULL) {
      shader = &terakan_gfx_command_writer_device(command_writer)
                   ->meta_shaders[TERAKAN_META_SHADER_DUMMY_NAN_VS];
   }

   /* `VS_EXPORT_COUNT` is the highest parameter export index. */
   uint32_t const spi_vs_out_id_count =
      G_0286C4_VS_EXPORT_COUNT(shader->stage.vs.spi_vs_out_config) / 4 + 1;

   uint32_t const packet_dwords =
      2 + ((R_028864_SQ_PGM_RESOURCES_2_VS - R_02885C_SQ_PGM_START_VS) / sizeof(uint32_t) + 1) +
      /* R_02861C_SPI_VS_OUT_ID_[0-9] */
      2 + spi_vs_out_id_count +
      /* R_0286C4_SPI_VS_OUT_CONFIG */
      2 + 1 +
      /* R_02881C_PA_CL_VS_OUT_CNTL */
      2 + 1;

   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, packet_dwords, 1, 1, 0);
   if (unlikely(packet == NULL)) {
      return;
   }

   *packet++ =
      PKT3(PKT3_SET_CONTEXT_REG,
           (R_028864_SQ_PGM_RESOURCES_2_VS - R_02885C_SQ_PGM_START_VS) / sizeof(uint32_t) + 1, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_02885C_SQ_PGM_START_VS);
   uint32_t const * const packet_pgm_start = packet;
   *packet++ = shader->program_va_shr8;
   /* TODO(Triang3l): `USE_LS_CONSTS`. */
   *packet++ = shader->sq_pgm_resources[0];
   *packet++ = shader->sq_pgm_resources[1];
   terakan_gfx_command_writer_add_relocation(
      command_writer, &packet, packet_pgm_start, *packet_pgm_start,
      TERASCALE_WDDM_PATCH_IDS_SQ_PGM_START_VS,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                shader->program_bo, true, false,
                                                TERAKAN_BO_PRIORITY_SHADER_BINARY));

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, spi_vs_out_id_count, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_02861C_SPI_VS_OUT_ID_0);
   memcpy(packet, shader->stage.vs.spi_vs_out_id, sizeof(uint32_t) * spi_vs_out_id_count);
   packet += spi_vs_out_id_count;

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_0286C4_SPI_VS_OUT_CONFIG);
   *packet++ = shader->stage.vs.spi_vs_out_config;

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_02881C_PA_CL_VS_OUT_CNTL);
   *packet++ = shader->stage.vs.pa_cl_vs_out_cntl;

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_sq_pgm_ps(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_shader_static const * shader = command_writer->hw_config_draw.sq_pgm_ps_;
   if (shader == NULL) {
      shader = &terakan_gfx_command_writer_device(command_writer)
                   ->meta_shaders[TERAKAN_META_SHADER_DUMMY_OPAQUE_PS];
   }

   uint32_t const interpolator_count = G_0286CC_NUM_INTERP(shader->stage.ps.spi_ps_in_control[0]);

   uint32_t const packet_dwords =
      2 + ((R_02884C_SQ_PGM_EXPORTS_PS - R_028840_SQ_PGM_START_PS) / sizeof(uint32_t) + 1) +
      /* R_028644_SPI_PS_INPUT_CNTL_[0-31] */
      (interpolator_count != 0 ? 2 + interpolator_count : 0) +
      /* R_0286CC_SPI_PS_IN_CONTROL_0, R_0286D0_SPI_PS_IN_CONTROL_1 */
      2 + 2 +
      /* R_0286D8_SPI_INPUT_Z */
      2 + 1 +
      /* R_0286E0_SPI_BARYC_CNTL */
      2 + 1 +
      /* R_02823C_CB_SHADER_MASK */
      2 + 1;

   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, packet_dwords, 1, 1, 0);
   if (unlikely(packet == NULL)) {
      return;
   }

   *packet++ =
      PKT3(PKT3_SET_CONTEXT_REG,
           (R_02884C_SQ_PGM_EXPORTS_PS - R_028840_SQ_PGM_START_PS) / sizeof(uint32_t) + 1, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028840_SQ_PGM_START_PS);
   uint32_t const * const packet_pgm_start = packet;
   *packet++ = shader->program_va_shr8;
   *packet++ = shader->sq_pgm_resources[0];
   *packet++ = shader->sq_pgm_resources[1];
   *packet++ = shader->stage.ps.sq_pgm_exports_ps;
   terakan_gfx_command_writer_add_relocation(
      command_writer, &packet, packet_pgm_start, *packet_pgm_start,
      TERASCALE_WDDM_PATCH_IDS_SQ_PGM_START_PS,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                shader->program_bo, true, false,
                                                TERAKAN_BO_PRIORITY_SHADER_BINARY));

   if (interpolator_count != 0) {
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, interpolator_count, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028644_SPI_PS_INPUT_CNTL_0);
      memcpy(packet, shader->stage.ps.spi_ps_input_cntl, sizeof(uint32_t) * interpolator_count);
      packet += interpolator_count;
   }

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_0286CC_SPI_PS_IN_CONTROL_0);
   *packet++ = shader->stage.ps.spi_ps_in_control[0];
   *packet++ = shader->stage.ps.spi_ps_in_control[1];

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_0286D8_SPI_INPUT_Z);
   *packet++ = shader->stage.ps.spi_input_z;

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_0286E0_SPI_BARYC_CNTL);
   *packet++ = shader->stage.ps.spi_baryc_cntl;

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_02823C_CB_SHADER_MASK);
   *packet++ = shader->stage.ps.cb_shader_mask;

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_sq_ring_itemsize(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_draw * const config = &command_writer->hw_config_draw;
   if (unlikely(!config->sq_ring_itemsize_.modified_bits)) {
      /* Don't do an empty emission. */
      return;
   }
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
      (2 + 1) * util_bitcount(config->sq_ring_itemsize_.modified_bits));
   if (unlikely(packet == NULL)) {
      return;
   }
   while (config->sq_ring_itemsize_.modified_bits) {
      unsigned const ring_index = ffs(config->sq_ring_itemsize_.modified_bits) - 1;
      config->sq_ring_itemsize_.modified_bits &= ~BITFIELD_BIT(ring_index);
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
      *packet++ = terakan_shader_rings[ring_index].item_size_context_reg_offset;
      *packet++ = config->sq_ring_itemsize_.itemsize_dwords[ring_index];
   }
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_sq_bool_const_vses(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_BOOL_CONST, 1, 0);
   *packet++ = (uint32_t)TERAKAN_DESCRIPTOR_HW_STAGE_VSES;
   *packet++ = command_writer->hw_config_draw.sq_bool_const_vses_;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_sq_bool_const_ls(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_BOOL_CONST, 1, 0);
   *packet++ = (uint32_t)TERAKAN_DESCRIPTOR_HW_STAGE_LS;
   *packet++ = command_writer->hw_config_draw.sq_bool_const_ls_;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_pa_vport_register_array(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const needed_count,
   uint16_t * const modified_bits, uint32_t const base_context_register_offset,
   uint32_t const registers_per_vport, void const * const register_values)
{
   assert(needed_count <= TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);

   /* Viewport register arrays are not preserved between contexts partially in the hardware, fully
    * set the registers for all the needed viewports if any has its values modified, and invalidate
    * the values for all other viewports.
    */

   uint16_t const needed_mask = BITFIELD_MASK(needed_count);
   if (!(*modified_bits & needed_mask)) {
      return;
   }

   uint32_t const needed_register_count = registers_per_vport * needed_count;
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + needed_register_count);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, needed_register_count, 0);
   *packet++ = base_context_register_offset;
   memcpy(packet, register_values, sizeof(uint32_t) * needed_register_count);
   packet += needed_register_count;
   terakan_gfx_command_writer_emit_done(command_writer, packet);

   *modified_bits = ~needed_mask;
}

static void
terakan_hw_config_draw_emit_pa_sc_vport_scissor(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_draw * const config = &command_writer->hw_config_draw;
   terakan_hw_config_draw_emit_pa_vport_register_array(
      command_writer, config->pa_sc_vport_scissor_.needed_count,
      &config->pa_sc_vport_scissor_.modified_bits,
      TERAKAN_CONTEXT_REG_OFFSET(R_028250_PA_SC_VPORT_SCISSOR_0_TL), 2,
      config->pa_sc_vport_scissor_.tl_br);
}

static void
terakan_hw_config_draw_emit_pa_sc_vport_zmin_zmax(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_draw * const config = &command_writer->hw_config_draw;
   terakan_hw_config_draw_emit_pa_vport_register_array(
      command_writer, config->pa_sc_vport_zmin_zmax_.needed_count,
      &config->pa_sc_vport_zmin_zmax_.modified_bits,
      TERAKAN_CONTEXT_REG_OFFSET(R_0282D0_PA_SC_VPORT_ZMIN_0), 2,
      config->pa_sc_vport_zmin_zmax_.zmin_zmax);
}

static void
terakan_hw_config_draw_emit_pa_cl_vport_scale_offset(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_draw * const config = &command_writer->hw_config_draw;
   terakan_hw_config_draw_emit_pa_vport_register_array(
      command_writer, config->pa_cl_vport_scale_offset_.needed_count,
      &config->pa_cl_vport_scale_offset_.modified_bits,
      TERAKAN_CONTEXT_REG_OFFSET(R_02843C_PA_CL_VPORT_XSCALE_0), 2 * 3,
      config->pa_cl_vport_scale_offset_.scale_offset);
}

static void
terakan_hw_config_draw_emit_pa_cl_clip_cntl(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_028810_PA_CL_CLIP_CNTL,
                                                command_writer->hw_config_draw.pa_cl_clip_cntl_);
}

static void
terakan_hw_config_draw_emit_pa_su_sc_mode_cntl(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_028814_PA_SU_SC_MODE_CNTL,
                                                command_writer->hw_config_draw.pa_su_sc_mode_cntl_);
}

static void
terakan_hw_config_draw_emit_pa_cl_vte_cntl(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_028818_PA_CL_VTE_CNTL,
                                                command_writer->hw_config_draw.pa_cl_vte_cntl_);
}

static void
terakan_hw_config_draw_emit_pa_sc_line_stipple(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_028A0C_PA_SC_LINE_STIPPLE,
                                                command_writer->hw_config_draw.pa_sc_line_stipple_);
}

static void
terakan_hw_config_draw_emit_pa_sc_mode_cntl_0(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_028A48_PA_SC_MODE_CNTL_0,
                                                command_writer->hw_config_draw.pa_sc_mode_cntl_0_);
}

static void
terakan_hw_config_draw_emit_pa_sc_mode_cntl_1(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_028A4C_PA_SC_MODE_CNTL_1,
                                                command_writer->hw_config_draw.pa_sc_mode_cntl_1_);
}

static void
terakan_hw_config_draw_emit_pa_su_poly_offset_db_fmt_cntl(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(
      command_writer, R_028B78_PA_SU_POLY_OFFSET_DB_FMT_CNTL,
      command_writer->hw_config_draw.pa_su_poly_offset_db_fmt_cntl_);
}

static void
terakan_hw_config_draw_emit_pa_su_poly_offset(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1 + 2 * 2);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1 + 2 * 2, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028B7C_PA_SU_POLY_OFFSET_CLAMP);
   struct terakan_hw_config_draw_pa_su_poly_offset const * const poly_offset =
      &command_writer->hw_config_draw.pa_su_poly_offset_;
   memcpy(packet++, &poly_offset->clamp, sizeof(float));
   for (unsigned face = 0; face <= 1; ++face) {
      memcpy(packet++, &poly_offset->slope_scale_per_16th_subpixel, sizeof(float));
      memcpy(packet++, &poly_offset->constant_offset, sizeof(float));
   }
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_pa_sc_line_cntl(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(
      command_writer,
      terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx
         ? CM_R_028BDC_PA_SC_LINE_CNTL
         : R_028C00_PA_SC_LINE_CNTL,
      command_writer->hw_config_draw.pa_sc_line_cntl_);
}

static void
terakan_hw_config_draw_emit_pa_sc_aa_config_sample_locs(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_draw const * const config = &command_writer->hw_config_draw;

   bool const is_r9xx =
      terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx;

   uint32_t pa_sc_aa_config = config->pa_sc_aa_config_sample_locs_.config;
   if (!is_r9xx) {
      pa_sc_aa_config &= TERAKAN_HW_CONFIG_DRAW_PA_SC_AA_CONFIG_FIELDS_R8XX;
   }

   uint32_t const sample_locs_context_reg_offset =
      is_r9xx ? TERAKAN_CONTEXT_REG_OFFSET(CM_R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0)
              : TERAKAN_CONTEXT_REG_OFFSET(R_028C1C_PA_SC_AA_SAMPLE_LOCS_0);

   unsigned const sample_count_log2 = G_028BE0_MSAA_NUM_SAMPLES(pa_sc_aa_config);
   unsigned const sample_count = 1u << sample_count_log2;

   unsigned const sample_locs_dwords_per_pixel_log2 = MAX2(sample_count_log2, 2) - 2;
   unsigned const sample_locs_dwords_per_pixel = 1 << sample_locs_dwords_per_pixel_log2;

   /* Emitting only `sample_count` sample locations for a more compact command buffer, because the
    * rest are unused, and the registers contain only arbitrary values in all of their bits, so
    * there are no values that can be interpreted in an invalid way.
    *
    * On R8xx, consecutive sample location registers contain 1 dword per pixel for up to 4x MSAA,
    * or 2 dwords per pixel for 8x MSAA.
    * Emitting all locations in a single `SET_CONTEXT_REG` packet.
    *
    * On R9xx, consecutive registers always store 4 dwords per pixel.
    * For 16x MSAA, emitting all locations in a single `SET_CONTEXT_REG` packet (1 header, 4 dwords
    * per pixel - 18 dwords in total).
    * For 8x MSAA or less, emitting the locations for each pixel in separate packets (4 headers, up
    * to 2 dwords per pixel - up to 16 dwords in total).
    */
   bool const separate_set_sample_locs_per_pixel =
      is_r9xx && sample_locs_dwords_per_pixel_log2 != 2;

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
      3 + (separate_set_sample_locs_per_pixel ? 2 * 4 : 2) +
         (4 << sample_locs_dwords_per_pixel_log2));
   if (unlikely(packet == NULL)) {
      return;
   }

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = is_r9xx ? TERAKAN_CONTEXT_REG_OFFSET(CM_R_028BE0_PA_SC_AA_CONFIG)
                       : TERAKAN_CONTEXT_REG_OFFSET(R_028C04_PA_SC_AA_CONFIG);
   *packet++ = pa_sc_aa_config;

   if (!separate_set_sample_locs_per_pixel) {
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 4 << sample_locs_dwords_per_pixel_log2, 0);
      *packet++ = sample_locs_context_reg_offset;
   }
   for (unsigned pixel_index = 0; pixel_index < 4; ++pixel_index) {
      if (separate_set_sample_locs_per_pixel) {
         *packet++ = PKT3(PKT3_SET_CONTEXT_REG, sample_locs_dwords_per_pixel, 0);
         *packet++ = sample_locs_context_reg_offset +
                     (pixel_index << (is_r9xx ? 2 : sample_locs_dwords_per_pixel_log2));
      }
      /* For less than 4 samples, initialize the unused bytes to a consistent value. */
      *packet = 0;
      static_assert(
         CHAR_BIT == 8,
         "Using character type aliasing to write 8-bit MSAA sample locations assuming that `char` "
         "is 8-bit");
      unsigned char * const emitted_sample_locs = (unsigned char *)packet;
      for (unsigned sample_index = 0; sample_index < sample_count; ++sample_index) {
         emitted_sample_locs[sample_index ^ (UTIL_ARCH_BIG_ENDIAN ? 3 : 0)] =
            config->pa_sc_aa_config_sample_locs_.sample_locs[sample_index][pixel_index];
      }
      packet += sample_locs_dwords_per_pixel;
   }

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_pa_cl_gb(struct terakan_gfx_command_writer * const command_writer)
{
   /* Guard band registers are not preserved between contexts partially in the hardware, both clip
    * and discard adjustments for both axes must be emitted if any needs to be set.
    */
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 4);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 4, 0);
   *packet++ = terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx
                  ? TERAKAN_CONTEXT_REG_OFFSET(CM_R_028BE8_PA_CL_GB_VERT_CLIP_ADJ)
                  : TERAKAN_CONTEXT_REG_OFFSET(R_028C0C_PA_CL_GB_VERT_CLIP_ADJ);
   memcpy(packet, command_writer->hw_config_draw.pa_cl_gb_.vert_horz_clip_disc_adj,
          sizeof(float) * 4);
   packet += 4;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_pa_sc_aa_mask(struct terakan_gfx_command_writer * const command_writer)
{
   /* 2 dwords with 16 bits per pixel on R9xx, 1 dword with 8 bits per pixel on the earlier
    * architecture generations. Per-pixel setting is not exposed because it's currently not needed.
    */

   bool const is_r9xx =
      terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx;

   unsigned const quad_dword_count = is_r9xx ? 2 : 1;

   uint32_t quad_dword = command_writer->hw_config_draw.pa_sc_aa_mask_;
   if (!is_r9xx) {
      quad_dword &= 0xFF;
      quad_dword |= quad_dword << 8;
   }
   quad_dword |= quad_dword << 16;

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + quad_dword_count);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, quad_dword_count, 0);
   *packet++ = is_r9xx ? TERAKAN_CONTEXT_REG_OFFSET(CM_R_028C38_PA_SC_AA_MASK_X0Y0_X1Y0)
                       : TERAKAN_CONTEXT_REG_OFFSET(R_028C3C_PA_SC_AA_MASK);
   for (unsigned dword_index = 0; dword_index < quad_dword_count; ++dword_index) {
      *packet++ = quad_dword;
   }
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_db_count_control(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_028004_DB_COUNT_CONTROL,
                                                command_writer->hw_config_draw.db_count_control_);
}

static void
terakan_hw_config_draw_emit_db_render_override(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_02800C_DB_RENDER_OVERRIDE,
                                                command_writer->hw_config_draw.db_render_override_);
}

static void
terakan_hw_config_draw_emit_db_render_override2(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(
      command_writer, R_028010_DB_RENDER_OVERRIDE2,
      command_writer->hw_config_draw.db_render_override2_);
}

static void
terakan_hw_config_draw_emit_db_depth_stencil_buffer(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet;

   struct terakan_bo const * const bo = command_writer->hw_config_draw.db_depth_stencil_buffer_.bo;
   struct terakan_depth_stencil_descriptor const * const descriptor =
      &command_writer->hw_config_draw.db_depth_stencil_buffer_.descriptor;

   bool depth_bound, stencil_bound;
   terakan_depth_stencil_descriptor_is_bound(bo, descriptor, &depth_bound, &stencil_bound);

   if (!depth_bound && !stencil_bound) {
      /* Neither is bound. */
      packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 2);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028040_DB_Z_INFO);
      *packet++ = S_028040_FORMAT(V_028040_Z_INVALID);
      *packet++ = S_028044_FORMAT(V_028044_STENCIL_INVALID);
      terakan_gfx_command_writer_emit_done(command_writer, packet);
      return;
   }

   terakan_hw_config_draw_emit_context_register(command_writer, R_028008_DB_DEPTH_VIEW,
                                                descriptor->view);

   uint32_t hw_z_info = descriptor->z_info;
   if (!terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      hw_z_info &= C_028040_NUM_SAMPLES;
   }

   enum terakan_bo_priority const bo_priority = G_028040_NUM_SAMPLES(descriptor->z_info) != 0
                                                   ? TERAKAN_BO_PRIORITY_DEPTH_BUFFER_MS
                                                   : TERAKAN_BO_PRIORITY_DEPTH_BUFFER;

   if (depth_bound && stencil_bound) {
      /* Both are bound - set all registers sequentially. */
      uint32_t const depth_and_stencil_register_count =
         (R_02805C_DB_DEPTH_SLICE - R_028040_DB_Z_INFO) / sizeof(uint32_t) + 1;
      packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
         2 + depth_and_stencil_register_count, 1, 4, 0);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, depth_and_stencil_register_count, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028040_DB_Z_INFO);
      *packet++ = hw_z_info;
      *packet++ = descriptor->stencil_info;
      uint32_t const * const packet_bases = packet;
      /* Read bases. */
      *packet++ = descriptor->z_base;
      *packet++ = descriptor->stencil_base;
      /* Write bases. */
      *packet++ = descriptor->z_base;
      *packet++ = descriptor->stencil_base;
      *packet++ = descriptor->size;
      *packet++ = descriptor->slice;
      /* BO references can be added only after `terakan_gfx_command_writer_emit_with_bo`. */
      uint32_t const bo_reference = terakan_bo_reference_writer_add_reference(
         &command_writer->base.bo_reference_writer, bo, true, true, bo_priority);
      for (uint32_t relocation_index = 0; relocation_index < 4; ++relocation_index) {
         terakan_gfx_command_writer_add_relocation(
            command_writer, &packet, &packet_bases[relocation_index],
            packet_bases[relocation_index], TERASCALE_WDDM_PATCH_IDS_DB_Z_STENCIL_BASE,
            bo_reference);
      }
      terakan_gfx_command_writer_emit_done(command_writer, packet);
      return;
   }

   /* Either a depth buffer or a stencil buffer is bound. */
   assert(depth_bound != stencil_bound);
   /* {Z info, stencil info}, {read base}, {write base}, {size, slice}. */
   packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 * 4 + 6, 1, 4, 0);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028040_DB_Z_INFO);
   /* `DB_Z_INFO` contains fields used not only by the depth aspect, but by the stencil aspect too.
    */
   assert(depth_bound || G_028040_FORMAT(hw_z_info) == TERASCALE_R8XX_DEPTH_FORMAT_INVALID);
   *packet++ = hw_z_info;
   assert(stencil_bound || G_028044_FORMAT(descriptor->stencil_info) == V_028044_STENCIL_INVALID);
   *packet++ = descriptor->stencil_info;
   /* BO references can be added only after `terakan_gfx_command_writer_emit_with_bo`. */
   uint32_t const bo_reference = terakan_bo_reference_writer_add_reference(
      &command_writer->base.bo_reference_writer, bo, true, true, bo_priority);
   /* Read and write bases. */
   uint32_t const base = stencil_bound ? descriptor->stencil_base : descriptor->z_base;
   uint32_t const base_register_offset =
      stencil_bound ? TERAKAN_CONTEXT_REG_OFFSET(R_02804C_DB_STENCIL_READ_BASE)
                    : TERAKAN_CONTEXT_REG_OFFSET(R_028048_DB_Z_READ_BASE);
   for (uint32_t base_index = 0; base_index <= 1; ++base_index) {
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
      *packet++ = base_register_offset + 2 * base_index;
      uint32_t const * const packet_base = packet;
      *packet++ = base;
      terakan_gfx_command_writer_add_relocation(command_writer, &packet, packet_base, *packet_base,
                                                TERASCALE_WDDM_PATCH_IDS_DB_Z_STENCIL_BASE,
                                                bo_reference);
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028058_DB_DEPTH_SIZE);
   *packet++ = descriptor->size;
   *packet++ = descriptor->slice;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_db_stencilrefmask(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 2);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028430_DB_STENCILREFMASK);
   *packet++ = command_writer->hw_config_draw.db_stencilrefmask_.front;
   *packet++ = command_writer->hw_config_draw.db_stencilrefmask_.back;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_db_depth_control(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_028800_DB_DEPTH_CONTROL,
                                                command_writer->hw_config_draw.db_depth_control_);
}

static void
terakan_hw_config_draw_emit_db_eqaa(struct terakan_gfx_command_writer * const command_writer)
{
   if (!terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      return;
   }
   terakan_hw_config_draw_emit_context_register(command_writer, CM_R_028804_DB_EQAA,
                                                command_writer->hw_config_draw.db_eqaa_);
}

static void
terakan_hw_config_draw_emit_db_shader_control(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_02880C_DB_SHADER_CONTROL,
                                                command_writer->hw_config_draw.db_shader_control_);
}

static void
terakan_hw_config_draw_emit_db_alpha_to_mask(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_028B70_DB_ALPHA_TO_MASK,
                                                command_writer->hw_config_draw.db_alpha_to_mask_);
}

static void
terakan_hw_config_draw_emit_cb_target_mask(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_028238_CB_TARGET_MASK,
                                                command_writer->hw_config_draw.cb_target_mask_);
}

static void
terakan_hw_config_draw_emit_cb_blend_constants(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 4);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 4, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028414_CB_BLEND_RED);
   memcpy(packet, command_writer->hw_config_draw.cb_blend_constants_, sizeof(float) * 4);
   packet += 4;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_draw_emit_cb_blend_control(
   struct terakan_gfx_command_writer * const command_writer)
{
   unsigned modified_remaining = command_writer->hw_config_draw.cb_blend_control_.modified_bits;
   while (modified_remaining) {
      int range_start, range_length;
      u_bit_scan_consecutive_range(&modified_remaining, &range_start, &range_length);
      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + range_length);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, range_length, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028780_CB_BLEND0_CONTROL) + range_start;
      memcpy(packet, &command_writer->hw_config_draw.cb_blend_control_.blend_control[range_start],
             sizeof(uint32_t) * range_length);
      packet += range_length;
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }
   command_writer->hw_config_draw.cb_blend_control_.modified_bits = 0b0;
}

static void
terakan_hw_config_draw_emit_cb_color_control(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_draw_emit_context_register(command_writer, R_028808_CB_COLOR_CONTROL,
                                                command_writer->hw_config_draw.cb_color_control_);
}

static void
terakan_hw_config_draw_emit_cb_immed(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);
   uint64_t const uav_bytes_per_element_log2 =
      command_writer->hw_config_draw.cb_immed_.uav_bytes_per_element_log2;
   unsigned modified_remaining = command_writer->hw_config_draw.cb_immed_.modified_bits;
   assert(!(modified_remaining & ~BITFIELD_MASK(TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT)));
   while (modified_remaining) {
      int range_start, range_length;
      u_bit_scan_consecutive_range(&modified_remaining, &range_start, &range_length);
      uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + range_length, 1,
         range_length, 0);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, range_length, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028B9C_CB_IMMED0_BASE) + range_start;
      uint32_t * const packet_bases = packet;
      for (unsigned range_uav_index = 0; range_uav_index < (unsigned)range_length;
           ++range_uav_index) {
         *packet++ = device->uav_immediate_va_shr8[(uav_bytes_per_element_log2 >>
                                                    (3 * (range_start + range_uav_index))) &
                                                   0b111];
      }
      /* BO references can be added only after `terakan_gfx_command_writer_emit_with_bo`. */
      uint32_t const bo_reference = terakan_bo_reference_writer_add_reference(
         &command_writer->base.bo_reference_writer, device->uav_immediate_bo, true, true,
         TERAKAN_BO_PRIORITY_SHADER_RW_BUFFER);
      for (unsigned range_uav_index = 0; range_uav_index < (unsigned)range_length;
           ++range_uav_index) {
         terakan_gfx_command_writer_add_relocation(
            command_writer, &packet, &packet_bases[range_uav_index], packet_bases[range_uav_index],
            TERASCALE_WDDM_PATCH_IDS_CB_IMMED_BASE | (range_start + range_uav_index), bo_reference);
      }
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }
   command_writer->hw_config_draw.cb_immed_.modified_bits = 0b0;
}

static void
terakan_hw_config_draw_emit_cb_color(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_draw * const config = &command_writer->hw_config_draw;

   /* DRM Radeon requires `ATTRIB` relocations regardless of `RADEON_CS_KEEP_TILING_FLAGS`. */
   bool const need_attrib_relocation =
      terakan_gfx_command_writer_physical_device(command_writer)
         ->submission_info_gfx.base.relocation_type == TERAKAN_QUEUE_RELOCATION_TYPE_DRM_NOP;

   assert(!(config->cb_color_.modified_bits & ~BITFIELD_MASK(TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT)));
   while (config->cb_color_.modified_bits) {
      unsigned const color_index = (unsigned)(ffs(config->cb_color_.modified_bits) - 1);
      config->cb_color_.modified_bits &= ~(uint16_t)BITFIELD_BIT(color_index);

      bool const has_meta = color_index < TERAKAN_COLOR_HW_RTV_COUNT;
      uint32_t const register_offset_dwords =
         has_meta
            ? (R_028C9C_CB_COLOR1_BASE - R_028C60_CB_COLOR0_BASE) / sizeof(uint32_t) * color_index
            : (R_028E40_CB_COLOR8_BASE - R_028C60_CB_COLOR0_BASE) / sizeof(uint32_t) +
                 (R_028E5C_CB_COLOR9_BASE - R_028E40_CB_COLOR8_BASE) / sizeof(uint32_t) *
                    (color_index - TERAKAN_COLOR_HW_RTV_COUNT);

      struct terakan_color_descriptor const * const color = &config->cb_color_.color[color_index];

      if (G_028C70_FORMAT(color->info) == V_028C70_COLOR_INVALID) {
         /* Emit the `INFO` register with `SOURCE_FORMAT` (dual-source blending needs RTV 0's export
          * format in the target 1).
          */
         uint32_t * packet = terakan_gfx_command_writer_emit(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1);
         if (unlikely(packet == NULL)) {
            return;
         }
         *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
         *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028C70_CB_COLOR0_INFO) + register_offset_dwords;
         *packet++ = color->info;
         terakan_gfx_command_writer_emit_done(command_writer, packet);
         continue;
      }

      unsigned const register_count =
         sizeof(struct terakan_color_descriptor) / sizeof(uint32_t) +
         (has_meta ? sizeof(struct terakan_color_meta_descriptor) / sizeof(uint32_t) : 0);

      uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + register_count, 1,
         1 + (need_attrib_relocation ? 1 : 0) + (has_meta ? 2 : 0), 0);
      if (unlikely(packet == NULL)) {
         return;
      }

      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, register_count, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028C60_CB_COLOR0_BASE) + register_offset_dwords;

      uint32_t * const packet_descriptor = packet;
      memcpy(packet_descriptor, color, sizeof(struct terakan_color_descriptor));
      if (!terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
         packet_descriptor[offsetof(struct terakan_color_descriptor, attrib) / sizeof(uint32_t)] &=
            C_028C74_NUM_SAMPLES & C_028C74_NUM_FRAGMENTS;
      }
      packet += sizeof(struct terakan_color_descriptor) / sizeof(uint32_t);
      uint32_t * const packet_meta_descriptor = packet;
      if (has_meta) {
         memcpy(packet_meta_descriptor, &config->cb_color_.meta[color_index],
                sizeof(struct terakan_color_meta_descriptor));
         packet += sizeof(struct terakan_color_meta_descriptor) / sizeof(uint32_t);
      }

      bool const is_multisampled = G_028C74_NUM_SAMPLES(color->attrib) != 0;
      assert(config->cb_color_.bo[color_index] != NULL);
      /* TODO(Triang3l): Possibly accept the priority explicitly, at least for buffer UAVs.
       * Meta functions may use CB targets for wildly different purposes (such as query operations).
       */
      uint32_t const bo_reference = terakan_bo_reference_writer_add_reference(
         &command_writer->base.bo_reference_writer, config->cb_color_.bo[color_index], true, true,
         G_028C70_RAT(color->info) ? TERAKAN_BO_PRIORITY_SHADER_RW_IMAGE
                                   : (is_multisampled ? TERAKAN_BO_PRIORITY_COLOR_BUFFER_MS
                                                      : TERAKAN_BO_PRIORITY_COLOR_BUFFER));
      terakan_gfx_command_writer_add_relocation(
         command_writer, &packet,
         &packet_descriptor[offsetof(struct terakan_color_descriptor, base) / sizeof(uint32_t)],
         packet_descriptor[offsetof(struct terakan_color_descriptor, base) / sizeof(uint32_t)],
         TERASCALE_WDDM_PATCH_IDS_CB_COLOR_BASE | color_index, bo_reference);
      if (need_attrib_relocation) {
         terakan_gfx_command_writer_add_relocation(
            command_writer, &packet,
            &packet_descriptor[offsetof(struct terakan_color_descriptor, attrib) / sizeof(uint32_t)],
            packet_descriptor[offsetof(struct terakan_color_descriptor, attrib) / sizeof(uint32_t)],
            0, bo_reference);
      }
      if (has_meta) {
         terakan_gfx_command_writer_add_relocation(
            command_writer, &packet,
            &packet_meta_descriptor[offsetof(struct terakan_color_meta_descriptor, cmask) /
                                    sizeof(uint32_t)],
            packet_meta_descriptor[offsetof(struct terakan_color_meta_descriptor, cmask) /
                                   sizeof(uint32_t)],
            TERASCALE_WDDM_PATCH_IDS_CB_COLOR_CMASK | color_index, bo_reference);
         terakan_gfx_command_writer_add_relocation(
            command_writer, &packet,
            &packet_meta_descriptor[offsetof(struct terakan_color_meta_descriptor, fmask) /
                                    sizeof(uint32_t)],
            packet_meta_descriptor[offsetof(struct terakan_color_meta_descriptor, fmask) /
                                   sizeof(uint32_t)],
            (is_multisampled ? TERASCALE_WDDM_PATCH_IDS_CB_COLOR_FMASK
                             : TERASCALE_WDDM_PATCH_IDS_CB_COLOR_BASE) |
               color_index,
            bo_reference);
      }

      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }
}

static terakan_hw_config_draw_emit_function const
   terakan_hw_config_draw_emit_functions[TERAKAN_HW_CONFIG_DRAW_ENTRY_COUNT] = {
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_INDEX_OFFSET] =
         terakan_hw_config_draw_emit_vgt_index_offset,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_MULTI_PRIM_IB_RESET_INDEX] =
         terakan_hw_config_draw_emit_vgt_multi_prim_ib_reset_index,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_MULTI_PRIM_IB_RESET_EN] =
         terakan_hw_config_draw_emit_vgt_multi_prim_ib_reset_en,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_TYPE] =
         terakan_hw_config_draw_emit_vgt_dma_index_type,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_IA_MULTI_VGT_PARAM] =
         terakan_hw_config_draw_emit_ia_multi_vgt_param,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_SHADER_STAGES_EN] =
         terakan_hw_config_draw_emit_vgt_shader_stages_en,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_FS] = terakan_hw_config_draw_emit_sq_pgm_fs,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_VS] = terakan_hw_config_draw_emit_sq_pgm_vs,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_PS] = terakan_hw_config_draw_emit_sq_pgm_ps,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_RING_ITEMSIZE] =
         terakan_hw_config_draw_emit_sq_ring_itemsize,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_BOOL_CONST_VSES] =
         terakan_hw_config_draw_emit_sq_bool_const_vses,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_BOOL_CONST_LS] =
         terakan_hw_config_draw_emit_sq_bool_const_ls,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_VPORT_SCISSOR] =
         terakan_hw_config_draw_emit_pa_sc_vport_scissor,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_VPORT_ZMIN_ZMAX] =
         terakan_hw_config_draw_emit_pa_sc_vport_zmin_zmax,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_VPORT_SCALE_OFFSET] =
         terakan_hw_config_draw_emit_pa_cl_vport_scale_offset,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_CLIP_CNTL] = terakan_hw_config_draw_emit_pa_cl_clip_cntl,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SU_SC_MODE_CNTL] =
         terakan_hw_config_draw_emit_pa_su_sc_mode_cntl,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_VTE_CNTL] = terakan_hw_config_draw_emit_pa_cl_vte_cntl,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_LINE_STIPPLE] =
         terakan_hw_config_draw_emit_pa_sc_line_stipple,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_MODE_CNTL_0] =
         terakan_hw_config_draw_emit_pa_sc_mode_cntl_0,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_MODE_CNTL_1] =
         terakan_hw_config_draw_emit_pa_sc_mode_cntl_1,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET_DB_FMT_CNTL] =
         terakan_hw_config_draw_emit_pa_su_poly_offset_db_fmt_cntl,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET] =
         terakan_hw_config_draw_emit_pa_su_poly_offset,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_LINE_CNTL] = terakan_hw_config_draw_emit_pa_sc_line_cntl,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_AA_CONFIG_SAMPLE_LOCS] =
         terakan_hw_config_draw_emit_pa_sc_aa_config_sample_locs,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_GB] = terakan_hw_config_draw_emit_pa_cl_gb,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_AA_MASK] = terakan_hw_config_draw_emit_pa_sc_aa_mask,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_COUNT_CONTROL] =
         terakan_hw_config_draw_emit_db_count_control,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE] =
         terakan_hw_config_draw_emit_db_render_override,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE2] =
         terakan_hw_config_draw_emit_db_render_override2,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_BUFFER] =
         terakan_hw_config_draw_emit_db_depth_stencil_buffer,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_STENCILREFMASK] =
         terakan_hw_config_draw_emit_db_stencilrefmask,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_DEPTH_CONTROL] =
         terakan_hw_config_draw_emit_db_depth_control,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_EQAA] = terakan_hw_config_draw_emit_db_eqaa,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_SHADER_CONTROL] =
         terakan_hw_config_draw_emit_db_shader_control,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_ALPHA_TO_MASK] =
         terakan_hw_config_draw_emit_db_alpha_to_mask,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_TARGET_MASK] = terakan_hw_config_draw_emit_cb_target_mask,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_BLEND_CONSTANTS] =
         terakan_hw_config_draw_emit_cb_blend_constants,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_BLEND_CONTROL] =
         terakan_hw_config_draw_emit_cb_blend_control,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_COLOR_CONTROL] =
         terakan_hw_config_draw_emit_cb_color_control,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_IMMED] = terakan_hw_config_draw_emit_cb_immed,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_COLOR] = terakan_hw_config_draw_emit_cb_color,
      [TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER] =
         terakan_hw_config_draw_emit_vgt_dma_index_buffer,
};

void
terakan_hw_config_draw_emit_modified(struct terakan_gfx_command_writer * const command_writer)
{
   unsigned entry_index;
   BITSET_FOREACH_SET (entry_index, command_writer->hw_config_draw.entries_modified_,
                       TERAKAN_HW_CONFIG_DRAW_ENTRY_COUNT) {
      terakan_hw_config_draw_emit_functions[entry_index](command_writer);
   }
   BITSET_ZERO(command_writer->hw_config_draw.entries_modified_);
}

void
terakan_hw_config_draw_set_all_modified(struct terakan_hw_config_draw * const config)
{
   BITSET_ONES(config->entries_modified_);

   config->sq_ring_itemsize_.modified_bits = BITFIELD_MASK(TERAKAN_SHADER_RING_INDEX_COUNT);

   config->pa_sc_vport_scissor_.modified_bits = BITSET_MASK(TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   config->pa_sc_vport_zmin_zmax_.modified_bits =
      BITSET_MASK(TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   config->pa_cl_vport_scale_offset_.modified_bits =
      BITSET_MASK(TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);

   config->cb_blend_control_.modified_bits = BITFIELD_MASK(TERAKAN_COLOR_HW_RTV_COUNT);

   config->cb_immed_.modified_bits = BITFIELD_MASK(TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);

   config->cb_color_.modified_bits = BITFIELD_MASK(TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);
}

void
terakan_hw_config_draw_reset(struct terakan_hw_config_draw * const config)
{
   config->vgt_index_offset_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_INDEX_OFFSET;

   config->vgt_multi_prim_ib_reset_index_ =
      TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_MULTI_PRIM_IB_RESET_INDEX;

   config->vgt_multi_prim_ib_reset_en_ = false;

   config->vgt_dma_index_buffer_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_DMA_INDEX_BUFFER;

   config->vgt_dma_index_type_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_DMA_INDEX_TYPE;

   config->ia_multi_vgt_param_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_IA_MULTI_VGT_PARAM;

   config->vgt_shader_stages_en_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_SHADER_STAGES_EN;

   config->sq_pgm_fs_.bo = NULL;
   config->sq_pgm_fs_.va_shr8 = 0;

   config->sq_pgm_vs_ = NULL;

   config->sq_pgm_ps_ = NULL;

   memset(config->sq_ring_itemsize_.itemsize_dwords, 0,
          sizeof(config->sq_ring_itemsize_.itemsize_dwords));

   config->sq_bool_const_vses_ = 0b0;

   config->sq_bool_const_ls_ = 0b0;

   /* VkPipelineViewportStateCreateInfo:
    * - viewportCount = 0
    * - pViewports[0 ... TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT - 1] = (VkViewport){}
    * - scissorCount = 0
    * VkPipelineRasterizationStateCreateInfo:
    * - depthClampEnable = VK_FALSE
    * VK_EXT_depth_range_unrestricted not enabled
    *
    * Don't even emit the scissor for the first viewport initially, for instance, if the first draw
    * in the command buffer uses a meta shader with rasterizer discard, and thus doesn't set the
    * needed scissor count to 0 explicitly.
    */
   config->pa_sc_vport_scissor_.needed_count = 0;
   config->pa_sc_vport_zmin_zmax_.needed_count = 0;
   config->pa_cl_vport_scale_offset_.needed_count = 0;
   for (unsigned vport_index = 0; vport_index < TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT;
        ++vport_index) {
      config->pa_sc_vport_scissor_.tl_br[vport_index][0] =
         TERAKAN_HW_CONFIG_DRAW_PA_SC_SCISSOR_EMPTY_TL(true);
      config->pa_sc_vport_scissor_.tl_br[vport_index][1] =
         TERAKAN_HW_CONFIG_DRAW_PA_SC_SCISSOR_EMPTY_BR;
      config->pa_sc_vport_zmin_zmax_.zmin_zmax[vport_index][0] = 0.0f;
      config->pa_sc_vport_zmin_zmax_.zmin_zmax[vport_index][1] = 1.0f;
   }
   memset(config->pa_cl_vport_scale_offset_.scale_offset, 0,
          sizeof(config->pa_cl_vport_scale_offset_.scale_offset));

   config->pa_cl_clip_cntl_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_CL_CLIP_CNTL;

   config->pa_su_sc_mode_cntl_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SU_SC_MODE_CNTL;

   config->pa_cl_vte_cntl_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_CL_VTE_CNTL;

   config->pa_sc_line_stipple_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_LINE_STIPPLE;

   config->pa_sc_mode_cntl_0_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_MODE_CNTL_0;

   config->pa_sc_mode_cntl_1_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_MODE_CNTL_1;

   config->pa_su_poly_offset_db_fmt_cntl_ =
      TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SU_POLY_OFFSET_DB_FMT_CNTL;

   config->pa_su_poly_offset_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SU_POLY_OFFSET;

   config->pa_sc_line_cntl_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_LINE_CNTL;

   config->pa_sc_aa_config_sample_locs_.config = TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_AA_CONFIG;
   memset(config->pa_sc_aa_config_sample_locs_.sample_locs, 0,
          sizeof(config->pa_sc_aa_config_sample_locs_.sample_locs));

   /* VkPipelineViewportStateCreateInfo viewportCount = 0 (initialize to safe values, which disable
    * the guard band).
    */
   for (unsigned vert_horz = 0; vert_horz <= 1; ++vert_horz) {
      for (unsigned clip_disc = 0; clip_disc <= 1; ++clip_disc) {
         config->pa_cl_gb_.vert_horz_clip_disc_adj[vert_horz][clip_disc] = 1.0f;
      }
   }

   config->pa_sc_aa_mask_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_AA_MASK;

   config->db_count_control_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_COUNT_CONTROL;

   config->db_render_override_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_RENDER_OVERRIDE;

   config->db_render_override2_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_RENDER_OVERRIDE2;

   config->db_depth_stencil_buffer_.bo = NULL;
   config->db_depth_stencil_buffer_.descriptor = (struct terakan_depth_stencil_descriptor){};

   config->db_stencilrefmask_.front = TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_STENCILREFMASK;
   config->db_stencilrefmask_.back = TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_STENCILREFMASK;

   config->db_depth_control_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_DEPTH_CONTROL;

   config->db_eqaa_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_EQAA;

   config->db_shader_control_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_SHADER_CONTROL;

   config->db_alpha_to_mask_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_ALPHA_TO_MASK;

   config->cb_target_mask_ = 0b0;

   /* VkPipelineColorBlendStateCreateInfo blendConstants[0...3] = 0.0f */
   memset(config->cb_blend_constants_, 0, sizeof(config->cb_blend_constants_));

   for (unsigned color_index = 0; color_index < TERAKAN_COLOR_HW_RTV_COUNT; ++color_index) {
      config->cb_blend_control_.blend_control[color_index] =
         TERAKAN_HW_CONFIG_DRAW_DEFAULT_CB_BLEND_CONTROL;
   }

   config->cb_color_control_ = TERAKAN_HW_CONFIG_DRAW_DEFAULT_CB_COLOR_CONTROL;

   /* Expecting UAVs to have 4 bytes per element (such as storage buffers) frequently. */
   config->cb_immed_.uav_bytes_per_element_log2 = UINT64_C(0222222222222);

   memset(config->cb_color_.bo, 0, sizeof(config->cb_color_.bo));
   for (unsigned color_index = 0; color_index < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT; ++color_index) {
      config->cb_color_.color[color_index] = (struct terakan_color_descriptor){
         .info = S_028C70_SOURCE_FORMAT(V_028C70_EXPORT_4C_16BPC)};
   }
   memset(config->cb_color_.meta, 0, sizeof(config->cb_color_.meta));

   terakan_hw_config_draw_set_all_modified(config);
}
