/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Kept in its own translation unit including only r600d.h/r600d_common.h, deliberately never
 * evergreend.h -- see the identical rationale in terakan_hw_config_shared_terascale_1.c.
 */

#include "terakan_hw_config_draw_terascale_1.h"

#include "gallium/drivers/r600/r600d.h"
#include "gallium/drivers/r600/r600d_common.h"

#include <assert.h>
#include <stddef.h>

bool
terakan_hw_config_draw_terascale_1_gprs_fit_baseline(
   struct terakan_hw_config_draw_terascale_1_gpr_counts const * const required,
   struct terakan_hw_config_draw_terascale_1_gpr_counts const * const baseline)
{
   return required != NULL && baseline != NULL && required->ps <= baseline->ps &&
          required->vs <= baseline->vs && required->gs <= baseline->gs &&
          required->es <= baseline->es;
}

static uint32_t *
write_config_reg(uint32_t * packet, uint32_t const reg, uint32_t const value)
{
   *packet++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
   *packet++ = (reg - R600_CONFIG_REG_OFFSET) >> 2;
   *packet++ = value;
   return packet;
}

static uint32_t *
write_context_reg(uint32_t * packet, uint32_t const reg, uint32_t const value)
{
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = (reg - R600_CONTEXT_REG_OFFSET) >> 2;
   *packet++ = value;
   return packet;
}

bool
terakan_hw_config_draw_terascale_1_spi_ps_encode(
   struct terakan_hw_config_draw_terascale_1_spi_ps_input const * const inputs,
   uint32_t const input_count,
   struct terakan_hw_config_draw_terascale_1_spi_ps * const spi_out)
{
   if (input_count > TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_SPI_PS_INPUT_COUNT ||
       (input_count && !inputs)) {
      return false;
   }

   *spi_out = (struct terakan_hw_config_draw_terascale_1_spi_ps){.input_count = input_count};

   bool have_position = false, have_front_face = false, have_sample_id = false;
   uint32_t front_face_gpr = 0;
   bool need_linear = false;
   for (uint32_t input_index = 0; input_index < input_count; ++input_index) {
      struct terakan_hw_config_draw_terascale_1_spi_ps_input const * const input =
         &inputs[input_index];
      if (input->semantic > 0xFF || input->gpr > 0x1F ||
          (input->position && have_position) ||
          (input->front_face_or_sample_mask && have_front_face &&
           input->gpr != front_face_gpr) ||
          (input->sample_id && have_sample_id)) {
         return false;
      }

      spi_out->input_control[input_index] =
         S_028644_SEMANTIC(input->semantic) |
         S_028644_FLAT_SHADE(input->flat || input->position) |
         S_028644_SEL_CENTROID(input->centroid) | S_028644_SEL_LINEAR(input->linear) |
         S_028644_PT_SPRITE_TEX(input->point_sprite) | S_028644_SEL_SAMPLE(input->sample);
      need_linear |= input->linear;

      if (input->position) {
         have_position = true;
         /* This matches r600_update_ps_state(). BARYC_SAMPLE_CNTL is a two-bit R600/R700 field
          * absent from evergreend.h, and is set to the classic driver's value 1 when position is
          * requested.
          */
         spi_out->in_control_0 |=
            S_0286CC_POSITION_ENA(1) | S_0286CC_POSITION_CENTROID(input->centroid) |
            S_0286CC_POSITION_ADDR(input->gpr) | S_0286CC_BARYC_SAMPLE_CNTL(1) |
            S_0286CC_POSITION_SAMPLE(input->sample);
         spi_out->input_z = S_0286D8_PROVIDE_Z_TO_SPI(1);
      }
      if (input->front_face_or_sample_mask) {
         have_front_face = true;
         front_face_gpr = input->gpr;
         spi_out->in_control_1 |=
            S_0286D0_FRONT_FACE_ENA(1) | S_0286D0_FRONT_FACE_ADDR(input->gpr);
      }
      if (input->sample_id) {
         have_sample_id = true;
         spi_out->in_control_1 |= S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                  S_0286D0_FIXED_PT_POSITION_ADDR(input->gpr);
      }
   }

   /* Classic r600_update_ps_state() enables perspective gradients unconditionally and linear
    * gradients iff a linear input exists. Unlike Evergreen, NUM_INTERP is the complete shader
    * input count, including system inputs.
    */
   spi_out->in_control_0 |= S_0286CC_NUM_INTERP(input_count) |
                            S_0286CC_PERSP_GRADIENT_ENA(1) |
                            S_0286CC_LINEAR_GRADIENT_ENA(need_linear);
   return true;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_spi_ps(
   uint32_t * packet, struct terakan_hw_config_draw_terascale_1_spi_ps const * const spi)
{
   if (spi->input_count) {
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, spi->input_count, 0);
      *packet++ = (R_028644_SPI_PS_INPUT_CNTL_0 - R600_CONTEXT_REG_OFFSET) >> 2;
      for (uint32_t input_index = 0; input_index < spi->input_count; ++input_index) {
         *packet++ = spi->input_control[input_index];
      }
   }

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
   *packet++ = (R_0286CC_SPI_PS_IN_CONTROL_0 - R600_CONTEXT_REG_OFFSET) >> 2;
   *packet++ = spi->in_control_0;
   *packet++ = spi->in_control_1;
   return write_context_reg(packet, R_0286D8_SPI_INPUT_Z, spi->input_z);
}

uint32_t
terakan_hw_config_draw_terascale_1_pa_sc_aa_mask_encode(uint32_t const sample_mask)
{
   uint32_t const pixel_mask = sample_mask & 0xFF;
   return pixel_mask | (pixel_mask << 8) | (pixel_mask << 16) | (pixel_mask << 24);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_pa_sc_aa_mask(uint32_t * const packet,
                                                        uint32_t const value)
{
   return write_context_reg(packet, R_028C48_PA_SC_AA_MASK, value);
}

bool
terakan_hw_config_draw_terascale_1_pa_sc_aa_encode(
   uint32_t const sample_count_log2, uint32_t const max_sample_dist,
   bool const aa_mask_centroid_determine, uint8_t const sample_locs[16][4],
   struct terakan_hw_config_draw_terascale_1_pa_sc_aa * const aa_out)
{
   if (sample_count_log2 > 3 || max_sample_dist > 0xF || !sample_locs || !aa_out) {
      return false;
   }

   *aa_out = (struct terakan_hw_config_draw_terascale_1_pa_sc_aa){
      .config = S_028C04_MSAA_NUM_SAMPLES(sample_count_log2) |
                S_028C04_AA_MASK_CENTROID_DTMN(aa_mask_centroid_determine) |
                S_028C04_MAX_SAMPLE_DIST(max_sample_dist),
   };

   uint32_t const sample_count = UINT32_C(1) << sample_count_log2;
   for (uint32_t sample_index = 0; sample_index < sample_count; ++sample_index) {
      for (uint32_t pixel_index = 1; pixel_index < 4; ++pixel_index) {
         if (sample_locs[sample_index][pixel_index] != sample_locs[sample_index][0]) {
            return false;
         }
      }
      aa_out->sample_locs[sample_index >> 2] |=
         (uint32_t)sample_locs[sample_index][0] << (8 * (sample_index & 3));
   }
   return true;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_pa_sc_aa(
   uint32_t * packet, bool const is_original_r600,
   struct terakan_hw_config_draw_terascale_1_pa_sc_aa const * const aa)
{
   if (is_original_r600) {
      switch (G_028C04_MSAA_NUM_SAMPLES(aa->config)) {
      case 0:
         break;
      case 1:
         packet = write_config_reg(packet, R_008B40_PA_SC_AA_SAMPLE_LOCS_2S,
                                   aa->sample_locs[0]);
         break;
      case 2:
         packet = write_config_reg(packet, R_008B44_PA_SC_AA_SAMPLE_LOCS_4S,
                                   aa->sample_locs[0]);
         break;
      case 3:
         *packet++ = PKT3(PKT3_SET_CONFIG_REG, 2, 0);
         *packet++ = (R_008B48_PA_SC_AA_SAMPLE_LOCS_8S_WD0 - R600_CONFIG_REG_OFFSET) >> 2;
         *packet++ = aa->sample_locs[0];
         *packet++ = aa->sample_locs[1];
         break;
      }
   } else {
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
      *packet++ = (R_028C1C_PA_SC_AA_SAMPLE_LOCS_MCTX - R600_CONTEXT_REG_OFFSET) >> 2;
      *packet++ = aa->sample_locs[0];
      *packet++ = aa->sample_locs[1];
   }
   return write_context_reg(packet, R_028C04_PA_SC_AA_CONFIG, aa->config);
}

uint32_t
terakan_hw_config_draw_terascale_1_pa_sc_aa_packet_dwords(
   bool const is_original_r600,
   struct terakan_hw_config_draw_terascale_1_pa_sc_aa const * const aa)
{
   if (!is_original_r600) {
      return 7;
   }
   return G_028C04_MSAA_NUM_SAMPLES(aa->config) == 0 ? 3
          : G_028C04_MSAA_NUM_SAMPLES(aa->config) == 3 ? 7
                                                       : 6;
}

bool
terakan_hw_config_draw_terascale_1_pa_sc_mode_encode(
   struct terakan_hw_config_draw_terascale_1_pa_sc_mode_input const * const input,
   uint32_t * const mode_out)
{
   if (!input || !mode_out || input->unknown_mode_0_bits || input->unknown_mode_1_bits) {
      return false;
   }

   /* Both baselines and the RV770-only sample-shading workaround are transcribed from
    * r600_create_rs_state(). R700 adds FORCE_EOV_REZ, ZMM_LINE_OFFSET and its viewport-scissor bit;
    * R600 uses WALK_ALIGN8_PRIM_FITS_ST instead. The common Evergreen software value has no
    * trustworthy representation of these generation-specific constants, so they are supplied
    * here. R600 does not consume `viewport_scissor_enable` in this register; the classic path uses
    * its unconditional WALK_ALIGN8 baseline and programs the actual scissor rectangles separately.
    */
   *mode_out = S_028A4C_MSAA_ENABLE(input->msaa_enable) |
               S_028A4C_LINE_STIPPLE_ENABLE(input->line_stipple_enable) |
               S_028A4C_FORCE_EOV_CNTDWN_ENABLE(1) |
               S_028A4C_PS_ITER_SAMPLE(input->ps_iter_sample);
   if (input->is_r700) {
      *mode_out |= S_028A4C_FORCE_EOV_REZ_ENABLE(1) | S_028A4C_R700_ZMM_LINE_OFFSET(1) |
                   S_028A4C_R700_VPORT_SCISSOR_ENABLE(input->viewport_scissor_enable) |
                   S_028A4C_TILE_COVER_DISABLE(input->is_rv770 && input->msaa_enable &&
                                               input->ps_iter_sample);
   } else {
      *mode_out |= S_028A4C_WALK_ALIGN8_PRIM_FITS_ST(1);
   }
   return true;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_pa_sc_mode(uint32_t * const packet,
                                                     uint32_t const value)
{
   return write_context_reg(packet, R_028A4C_PA_SC_MODE_CNTL, value);
}

bool
terakan_hw_config_draw_terascale_1_pa_cl_clip_encode(
   uint32_t const evergreen_value, bool const is_r700,
   struct terakan_hw_config_draw_terascale_1_pa_cl_clip * const clip_out)
{
   uint32_t const known_bits =
      S_028810_CLIP_DISABLE(1) | S_028810_DX_CLIP_SPACE_DEF(1) |
      S_028810_DX_RASTERIZATION_KILL(1) | S_028810_DX_LINEAR_ATTR_CLIP_ENA(1) |
      S_028810_ZCLIP_NEAR_DISABLE(1) | S_028810_ZCLIP_FAR_DISABLE(1);
   if (!clip_out || (evergreen_value & ~known_bits)) {
      return false;
   }

   bool const rasterization_kill = G_028810_DX_RASTERIZATION_KILL(evergreen_value) != 0;
   clip_out->clip_cntl =
      is_r700 ? evergreen_value : evergreen_value & C_028810_DX_RASTERIZATION_KILL;
   clip_out->sx_misc = is_r700 ? 0 : S_028350_MULTIPASS(rasterization_kill);
   return true;
}

uint32_t
terakan_hw_config_draw_terascale_1_pa_cl_clip_packet_dwords(bool const is_r700)
{
   return is_r700 ? 3 : 6;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_pa_cl_clip(
   uint32_t * packet, bool const is_r700,
   struct terakan_hw_config_draw_terascale_1_pa_cl_clip const * const clip)
{
   packet = write_context_reg(packet, R_028810_PA_CL_CLIP_CNTL, clip->clip_cntl);
   if (!is_r700) {
      packet = write_context_reg(packet, R_028350_SX_MISC, clip->sx_misc);
   }
   return packet;
}

bool
terakan_hw_config_draw_terascale_1_pa_su_poly_offset_db_fmt_encode(
   uint32_t const evergreen_value, uint32_t * const r700_value_out)
{
   uint32_t const known_bits = S_028DF8_POLY_OFFSET_NEG_NUM_DB_BITS(UINT8_MAX) |
                               S_028DF8_POLY_OFFSET_DB_IS_FLOAT_FMT(1);
   if (!r700_value_out || (evergreen_value & ~known_bits)) {
      return false;
   }
   *r700_value_out = evergreen_value;
   return true;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_pa_su_poly_offset_db_fmt(uint32_t * const packet,
                                                                  uint32_t const value)
{
   return write_context_reg(packet, R_028DF8_PA_SU_POLY_OFFSET_DB_FMT_CNTL, value);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_pa_su_poly_offset(
   uint32_t * packet, uint32_t const clamp, uint32_t const scale, uint32_t const offset)
{
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 5, 0);
   *packet++ = (R_028DFC_PA_SU_POLY_OFFSET_CLAMP - R600_CONTEXT_REG_OFFSET) >> 2;
   *packet++ = clamp;
   *packet++ = scale;
   *packet++ = offset;
   *packet++ = scale;
   *packet++ = offset;
   return packet;
}

bool
terakan_hw_config_draw_terascale_1_sq_pgm_resources_encode(
   uint32_t const evergreen_resources, bool const force_uncached_first_inst,
   uint32_t * const r700_resources_out)
{
   uint32_t const known_bits = S_028850_NUM_GPRS(UINT8_MAX) |
                               S_028850_STACK_SIZE(UINT8_MAX) | S_028850_DX10_CLAMP(1) |
                               S_028850_UNCACHED_FIRST_INST(1) | S_028850_CLAMP_CONSTS(1);
   if (!r700_resources_out || (evergreen_resources & ~known_bits)) {
      return false;
   }
   *r700_resources_out =
      evergreen_resources | S_028850_UNCACHED_FIRST_INST(force_uncached_first_inst);
   return true;
}

uint32_t
terakan_hw_config_draw_terascale_1_constant_packet_dwords(void)
{
   return 0;
}

bool
terakan_hw_config_draw_terascale_1_absent_vgt_control_encode(
   uint32_t const value, uint32_t * const packet_dwords_out)
{
   if (value != 0 || !packet_dwords_out) {
      return false;
   }
   *packet_dwords_out = 0;
   return true;
}

bool
terakan_hw_config_draw_terascale_1_ring_itemsize_encode(
   uint32_t const * const itemsize_dwords, uint32_t const itemsize_count,
   uint32_t * const packet_dwords_out)
{
   if (!itemsize_dwords || !packet_dwords_out) {
      return false;
   }
   for (uint32_t index = 0; index < itemsize_count; ++index) {
      if (itemsize_dwords[index] != 0) {
         return false;
      }
   }
   *packet_dwords_out = 0;
   return true;
}

bool
terakan_hw_config_draw_terascale_1_absent_ls_bool_const_encode(
   uint32_t const value, uint32_t * const packet_dwords_out)
{
   if (value != 0 || !packet_dwords_out) {
      return false;
   }
   *packet_dwords_out = 0;
   return true;
}

uint32_t
terakan_hw_config_draw_terascale_1_cb_immed_packet_dwords(void)
{
   return 0;
}

uint32_t
terakan_hw_config_draw_terascale_1_index_buffer_unbind_packet_dwords(void)
{
   return 0;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_zero_count_draw(uint32_t * packet)
{
   *packet++ = PKT3(PKT3_DRAW_INDEX_AUTO, 1, 0);
   *packet++ = 0;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX);
   return packet;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_draw_index(uint32_t * packet,
                                                    uint64_t const index_va,
                                                    uint32_t const index_count)
{
   *packet++ = PKT3(PKT3_DRAW_INDEX, 3, 0);
   *packet++ = (uint32_t)index_va;
   *packet++ = (uint32_t)(index_va >> 32) & 0xFF;
   *packet++ = index_count;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_DMA);
   return packet;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_sq_pgm_fs(uint32_t * const packet,
                                                    uint32_t const program_va_shr8)
{
   return write_context_reg(packet, R_028894_SQ_PGM_START_FS, program_va_shr8);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_sq_pgm_vs(
   uint32_t * packet, uint32_t const program_va_shr8, uint32_t const resources)
{
   packet = terakan_hw_config_draw_terascale_1_write_sq_pgm_vs_start(packet, program_va_shr8);
   return terakan_hw_config_draw_terascale_1_write_sq_pgm_vs_resources(packet, resources);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_sq_pgm_vs_start(uint32_t * const packet,
                                                           uint32_t const program_va_shr8)
{
   return write_context_reg(packet, R_028858_SQ_PGM_START_VS, program_va_shr8);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_sq_pgm_vs_resources(uint32_t * const packet,
                                                               uint32_t const resources)
{
   return write_context_reg(packet, R_028868_SQ_PGM_RESOURCES_VS, resources);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_sq_pgm_ps(
   uint32_t * packet, uint32_t const program_va_shr8, uint32_t const resources,
   uint32_t const exports)
{
   packet = terakan_hw_config_draw_terascale_1_write_sq_pgm_ps_start(packet, program_va_shr8);
   return terakan_hw_config_draw_terascale_1_write_sq_pgm_ps_resources(packet, resources, exports);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_sq_pgm_ps_start(uint32_t * const packet,
                                                           uint32_t const program_va_shr8)
{
   return write_context_reg(packet, R_028840_SQ_PGM_START_PS, program_va_shr8);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_sq_pgm_ps_resources(
   uint32_t * packet, uint32_t const resources, uint32_t const exports)
{
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
   *packet++ = (R_028850_SQ_PGM_RESOURCES_PS - R600_CONTEXT_REG_OFFSET) >> 2;
   *packet++ = resources;
   *packet++ = exports;
   return packet;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_sq_pgm_es(
   uint32_t * packet, uint32_t const program_va_shr8, uint32_t const resources)
{
   packet = write_context_reg(packet, R_028880_SQ_PGM_START_ES, program_va_shr8);
   return write_context_reg(packet, R_028890_SQ_PGM_RESOURCES_ES, resources);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_sq_pgm_gs(
   uint32_t * packet, uint32_t const program_va_shr8, uint32_t const resources)
{
   packet = write_context_reg(packet, R_02886C_SQ_PGM_START_GS, program_va_shr8);
   return write_context_reg(packet, R_02887C_SQ_PGM_RESOURCES_GS, resources);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_spi_vs_out_id(
   uint32_t * packet, uint32_t const count, uint32_t const * const values)
{
   assert(count != 0 && count <= 10 && values != NULL);
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, count, 0);
   *packet++ = (R_028614_SPI_VS_OUT_ID_0 - R600_CONTEXT_REG_OFFSET) >> 2;
   for (uint32_t index = 0; index < count; ++index) {
      *packet++ = values[index];
   }
   return packet;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_depth_view(uint32_t * const packet,
                                                        uint32_t const value)
{
   return write_context_reg(packet, R_028004_DB_DEPTH_VIEW, value);
}

bool
terakan_hw_config_draw_terascale_1_db_render_control_override_encode(
   uint32_t const evergreen_db_render_control, uint32_t const evergreen_db_render_override,
   bool const is_r700, uint32_t const conservative_z_export,
   uint32_t * const db_render_control_out, uint32_t * const db_render_override_out)
{
   if (evergreen_db_render_control || evergreen_db_render_override ||
       conservative_z_export > V_028D0C_EXPORT_GREATER_THAN_Z ||
       (!is_r700 && conservative_z_export != V_028D0C_EXPORT_ANY_Z)) {
      return false;
   }

   /* Exact no-query, no-HTILE branch of r600_emit_db_misc_state(). Its R700 conservative-Z branch
    * runs before the no-query ZPASS baseline; R600 deliberately skips it. Occlusion queries, HTILE
    * and depth/stencil copy remain separate unported state transitions.
    */
   *db_render_control_out = S_028D0C_ZPASS_INCREMENT_DISABLE(1) |
                            (is_r700
                                ? S_028D0C_CONSERVATIVE_Z_EXPORT(conservative_z_export)
                                : 0);
   *db_render_override_out =
      S_028D10_FORCE_HIZ_ENABLE(V_028D10_FORCE_DISABLE) |
      S_028D10_FORCE_HIS_ENABLE0(V_028D10_FORCE_DISABLE) |
      S_028D10_FORCE_HIS_ENABLE1(V_028D10_FORCE_DISABLE);
   return true;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_render_control_override(
   uint32_t * packet, uint32_t const db_render_control, uint32_t const db_render_override)
{
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
   *packet++ = (R_028D0C_DB_RENDER_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2;
   *packet++ = db_render_control;
   *packet++ = db_render_override;
   return packet;
}

bool
terakan_hw_config_draw_terascale_1_db_shader_control_encode(
   struct terakan_hw_config_draw_terascale_1_db_shader_control_input const * const input,
   uint32_t * const db_shader_control_out)
{
   /* r600d.h and evergreend.h agree on the positions of the fields emitted below. The source
    * format is Evergreen shader-export metadata rather than an R700 register field; values 0..2
    * are the complete non-reserved Evergreen range. Classic r600_update_db_shader_control() only
    * selects DUAL_EXPORT_ENABLE for the corresponding R700 export choice.
    */
   if (input->z_order > V_02880C_EARLY_Z_THEN_RE_Z || input->source_format > 2 ||
       input->exec_on_hier_fail || input->exec_on_noop || input->alpha_to_mask_disable ||
       input->depth_before_shader ||
       input->conservative_z_export > V_028D0C_EXPORT_GREATER_THAN_Z || input->unknown_bits) {
      return false;
   }

   *db_shader_control_out =
      S_02880C_Z_EXPORT_ENABLE(input->z_export_enable) |
      S_02880C_STENCIL_REF_EXPORT_ENABLE(input->stencil_ref_export_enable) |
      S_02880C_Z_ORDER(input->z_order) | S_02880C_KILL_ENABLE(input->kill_enable) |
      S_02880C_MASK_EXPORT_ENABLE(input->mask_export_enable) |
      S_02880C_DUAL_EXPORT_ENABLE(input->dual_export_enable);
   return true;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_shader_control(uint32_t * const packet,
                                                            uint32_t const value)
{
   return write_context_reg(packet, R_02880C_DB_SHADER_CONTROL, value);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_depth_size(uint32_t * const packet,
                                                        uint32_t const pitch_tile_max,
                                                        uint32_t const slice_tile_max)
{
   return write_context_reg(packet, R_028000_DB_DEPTH_SIZE,
                            S_028000_PITCH_TILE_MAX(pitch_tile_max) |
                               S_028000_SLICE_TILE_MAX(slice_tile_max));
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_depth_base_info(uint32_t * packet,
                                                             uint32_t const db_depth_base,
                                                             uint32_t const db_depth_info)
{
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
   *packet++ = (R_02800C_DB_DEPTH_BASE - R600_CONTEXT_REG_OFFSET) >> 2;
   *packet++ = db_depth_base;
   *packet++ = db_depth_info;
   return packet;
}

bool
terakan_hw_config_draw_terascale_1_db_depth_encode(
   struct terakan_hw_config_draw_terascale_1_db_depth_input const * const input,
   struct terakan_hw_config_draw_terascale_1_db_depth * const depth_out)
{
   if (input->pitch_tile_max > 0x3FF || input->height_tile_max > 0x3FF ||
       input->slice_tile_max > 0xFFFFF || input->samples_log2 > 3 ||
       input->slice_start > 0x7FF || input->slice_max > 0x7FF ||
       input->slice_start > input->slice_max ||
       (input->array_mode != V_0280A0_ARRAY_1D_TILED_THIN1 &&
        input->array_mode != V_0280A0_ARRAY_2D_TILED_THIN1)) {
      return false;
   }

   uint32_t hw_format;
   switch (input->format) {
   case TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_16:
      hw_format = V_028010_DEPTH_16;
      break;
   case TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_24:
      /* This encoder is depth-only, so the unused high byte is X rather than stencil. */
      hw_format = V_028010_DEPTH_X8_24;
      break;
   case TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_32_FLOAT:
      hw_format = V_028010_DEPTH_32_FLOAT;
      break;
   default:
      return false;
   }

   *depth_out = (struct terakan_hw_config_draw_terascale_1_db_depth){
      .base = input->base,
      .size = S_028000_PITCH_TILE_MAX(input->pitch_tile_max) |
              S_028000_SLICE_TILE_MAX(input->slice_tile_max),
      .view = S_028004_SLICE_START(input->slice_start) |
              S_028004_SLICE_MAX(input->slice_max),
      .info = S_028010_FORMAT(hw_format) | S_028010_ARRAY_MODE(input->array_mode) |
              S_028010_ZRANGE_PRECISION(input->zrange_precision),
      /* r600_init_depth_surface() derives this directly from nblk_y / 8 - 1, which is the
       * Evergreen-shaped HEIGHT_TILE_MAX already carried in the intermediate descriptor.
       */
      .prefetch_limit = S_028D34_DEPTH_HEIGHT_TILE_MAX(input->height_tile_max),
   };
   return true;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_depth(
   uint32_t * packet, struct terakan_hw_config_draw_terascale_1_db_depth const * const depth)
{
   packet = write_context_reg(packet, R_028000_DB_DEPTH_SIZE, depth->size);
   packet = write_context_reg(packet, R_028004_DB_DEPTH_VIEW, depth->view);
   packet = terakan_hw_config_draw_terascale_1_write_db_depth_base_info(
      packet, depth->base, depth->info);
   return write_context_reg(packet, R_028D34_DB_PREFETCH_LIMIT, depth->prefetch_limit);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_depth_prefetch_limit(uint32_t * const packet,
                                                                 uint32_t const value)
{
   return write_context_reg(packet, R_028D34_DB_PREFETCH_LIMIT, value);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_depth_unbound(uint32_t * const packet)
{
   return write_context_reg(packet, R_028010_DB_DEPTH_INFO,
                            S_028010_FORMAT(V_028010_DEPTH_INVALID));
}

uint32_t
terakan_hw_config_draw_terascale_1_db_alpha_to_mask_encode(bool const enable)
{
   return S_028D44_ALPHA_TO_MASK_ENABLE(enable) | S_028D44_ALPHA_TO_MASK_OFFSET0(2) |
          S_028D44_ALPHA_TO_MASK_OFFSET1(2) | S_028D44_ALPHA_TO_MASK_OFFSET2(2) |
          S_028D44_ALPHA_TO_MASK_OFFSET3(2);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_alpha_to_mask(uint32_t * const packet,
                                                          uint32_t const value)
{
   return write_context_reg(packet, R_028D44_DB_ALPHA_TO_MASK, value);
}

bool
terakan_hw_config_draw_terascale_1_cb_color_encode(
   struct terakan_hw_config_draw_terascale_1_cb_color_input const * const input,
   struct terakan_hw_config_draw_terascale_1_cb_color * const color_out)
{
   if (input->is_uav || input->format == 0 ||
       input->pitch_tile_max > 0x3FF || input->slice_tile_max > 0xFFFFF ||
       input->slice_start > 0x7FF || input->slice_max > 0x7FF ||
       input->slice_start > input->slice_max || input->endian > 0x3 || input->format > 0x3F ||
       (input->array_mode != V_0280A0_ARRAY_LINEAR_ALIGNED &&
        input->array_mode != V_0280A0_ARRAY_1D_TILED_THIN1 &&
        input->array_mode != V_0280A0_ARRAY_2D_TILED_THIN1) ||
       input->number_type > 0x7 || input->comp_swap > 0x3 || input->source_format > 1) {
      return false;
   }
   /* A multisampled R6xx/R7xx target needs both metadata addresses and their paired mask. The
    * classic driver allocates the pair together; accepting only one would be a known hang path,
    * not a graceful loss of compression. Conversely, a single-sample target with metadata is not
    * representable by this port yet (the R600 resolve-destination dummy allocation is separate).
    */
   if (input->is_multisampled != input->metadata_enabled ||
       input->cmask_tile_max > 0xFFF || input->fmask_tile_max > 0xFFFFF) {
      return false;
   }

   color_out->base = input->base;
   color_out->size = S_028060_PITCH_TILE_MAX(input->pitch_tile_max) |
                     S_028060_SLICE_TILE_MAX(input->slice_tile_max);
   color_out->view =
      S_028080_SLICE_START(input->slice_start) | S_028080_SLICE_MAX(input->slice_max);
   color_out->info =
      S_0280A0_ENDIAN(input->endian) | S_0280A0_FORMAT(input->format) |
      S_0280A0_ARRAY_MODE(input->array_mode) | S_0280A0_NUMBER_TYPE(input->number_type) |
      S_0280A0_COMP_SWAP(input->comp_swap) | S_0280A0_BLEND_CLAMP(input->blend_clamp) |
      S_0280A0_BLEND_BYPASS(input->blend_bypass) | S_0280A0_SIMPLE_FLOAT(input->simple_float) |
      S_0280A0_SOURCE_FORMAT(input->source_format == 1 ? V_0280A0_EXPORT_NORM
                                                       : V_0280A0_EXPORT_FULL);
   if (input->metadata_enabled) {
      color_out->info |= S_0280A0_TILE_MODE(V_0280A0_FRAG_ENABLE);
      color_out->fmask = input->fmask;
      color_out->cmask = input->cmask;
      color_out->mask = S_028100_CMASK_BLOCK_MAX(input->cmask_tile_max) |
                        S_028100_FMASK_TILE_MAX(input->fmask_tile_max);
   } else {
      /* The no-metadata branch is exactly r600_init_color_surface(): alias both metadata bases to
       * BASE and clear MASK. It is valid only for a single-sampled target, checked above.
       */
      color_out->fmask = input->base;
      color_out->cmask = input->base;
      color_out->mask = 0;
   }
   return true;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_cb_color(
   uint32_t * packet, uint32_t const color_index,
   struct terakan_hw_config_draw_terascale_1_cb_color const * const color)
{
   assert(color_index < 8);
   packet = write_context_reg(packet, R_0280A0_CB_COLOR0_INFO + color_index * sizeof(uint32_t),
                              color->info);
   packet = write_context_reg(packet, R_028040_CB_COLOR0_BASE + color_index * sizeof(uint32_t),
                              color->base);
   packet = write_context_reg(packet, R_0280E0_CB_COLOR0_FRAG + color_index * sizeof(uint32_t),
                              color->fmask);
   packet = write_context_reg(packet, R_0280C0_CB_COLOR0_TILE + color_index * sizeof(uint32_t),
                              color->cmask);
   packet = write_context_reg(packet, R_028060_CB_COLOR0_SIZE + color_index * sizeof(uint32_t),
                              color->size);
   packet = write_context_reg(packet, R_028080_CB_COLOR0_VIEW + color_index * sizeof(uint32_t),
                              color->view);
   return write_context_reg(packet, R_028100_CB_COLOR0_MASK + color_index * sizeof(uint32_t),
                            color->mask);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_cb_color_drm_relocations_prefix(
   uint32_t * packet, uint32_t const color_index,
   struct terakan_hw_config_draw_terascale_1_cb_color const * const color,
   uint32_t const bo_reference, uint32_t const field_count)
{
   assert(color_index < 8);
   assert(field_count >= 1 && field_count <= 7);

   /* r600_cs_packet_parse() calls radeon_cs_packet_next_reloc while parsing each of BASE, FRAG
    * and TILE. It therefore cannot consume a batched NOP list after all seven CB registers. */
   packet = write_context_reg(packet, R_0280A0_CB_COLOR0_INFO + color_index * sizeof(uint32_t),
                              color->info);
   if (field_count == 1) {
      return packet;
   }
   packet = write_context_reg(packet, R_028040_CB_COLOR0_BASE + color_index * sizeof(uint32_t),
                              color->base);
   *packet++ = PKT3(PKT3_NOP, 0, 0);
   *packet++ = 4 * bo_reference;
   if (field_count == 2) {
      return packet;
   }
   packet = write_context_reg(packet, R_0280E0_CB_COLOR0_FRAG + color_index * sizeof(uint32_t),
                              color->fmask);
   *packet++ = PKT3(PKT3_NOP, 0, 0);
   *packet++ = 4 * bo_reference;
   if (field_count == 3) {
      return packet;
   }
   packet = write_context_reg(packet, R_0280C0_CB_COLOR0_TILE + color_index * sizeof(uint32_t),
                              color->cmask);
   *packet++ = PKT3(PKT3_NOP, 0, 0);
   *packet++ = 4 * bo_reference;
   if (field_count == 4) {
      return packet;
   }
   packet = write_context_reg(packet, R_028060_CB_COLOR0_SIZE + color_index * sizeof(uint32_t),
                              color->size);
   if (field_count == 5) {
      return packet;
   }
   packet = write_context_reg(packet, R_028080_CB_COLOR0_VIEW + color_index * sizeof(uint32_t),
                              color->view);
   if (field_count == 6) {
      return packet;
   }
   return write_context_reg(packet, R_028100_CB_COLOR0_MASK + color_index * sizeof(uint32_t),
                            color->mask);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_cb_color_drm_relocations(
   uint32_t * const packet, uint32_t const color_index,
   struct terakan_hw_config_draw_terascale_1_cb_color const * const color,
   uint32_t const bo_reference)
{
   return terakan_hw_config_draw_terascale_1_write_cb_color_drm_relocations_prefix(
      packet, color_index, color, bo_reference, 7);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_cb_color_unbound(uint32_t * const packet,
                                                          uint32_t const color_index,
                                                          uint32_t const source_format)
{
   assert(color_index < 8);
   assert(source_format <= 1);
   return write_context_reg(packet, R_0280A0_CB_COLOR0_INFO + color_index * sizeof(uint32_t),
                            S_0280A0_SOURCE_FORMAT(source_format));
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_cb_shader_control(
   uint32_t * const packet, uint32_t const bound_color_count)
{
   assert(bound_color_count <= 8);
   return write_context_reg(packet, R_0287A0_CB_SHADER_CONTROL,
                            bound_color_count ? ((UINT32_C(1) << bound_color_count) - 1) : 1);
}

uint32_t
terakan_hw_config_draw_terascale_1_cb_color_control_encode(
   enum terakan_hw_config_draw_terascale_1_cb_color_operation const operation,
   uint32_t const rop3, bool const degamma_enable, bool const multiwrite_enable,
   uint32_t const target_blend_enable)
{
   assert(rop3 <= 0xFF);
   assert(target_blend_enable <= 0xFF);

   uint32_t special_op;
   switch (operation) {
   case TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_CB_COLOR_DISABLE:
      special_op = V_028808_SPECIAL_DISABLE;
      break;
   case TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_CB_COLOR_NORMAL:
      special_op = V_028808_SPECIAL_NORMAL;
      break;
   case TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_CB_COLOR_RESOLVE_BOX:
      special_op = V_028808_SPECIAL_RESOLVE_BOX;
      break;
   default:
      assert(!"invalid TeraScale 1 CB color operation");
      special_op = V_028808_SPECIAL_DISABLE;
      break;
   }

   /* RV710 is newer than the original R600 and therefore supports per-MRT blend control; this is
    * the same family check used by r600_create_blend_state_mode().
    */
   return S_028808_MULTIWRITE_ENABLE(multiwrite_enable) |
          S_028808_DEGAMMA_ENABLE(degamma_enable) | S_028808_SPECIAL_OP(special_op) |
          S_028808_PER_MRT_BLEND(1) | S_028808_TARGET_BLEND_ENABLE(target_blend_enable) |
          S_028808_ROP3(rop3);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_cb_color_control(uint32_t * const packet,
                                                          uint32_t const value)
{
   return write_context_reg(packet, R_028808_CB_COLOR_CONTROL, value);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_cb_blend_control(
   uint32_t * packet, uint32_t const first_color, uint32_t const color_count,
   uint32_t const * const blend_control)
{
   assert(first_color <= 8);
   assert(color_count <= 8 - first_color);
   if (!color_count) {
      return packet;
   }

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, color_count, 0);
   *packet++ = (R_028780_CB_BLEND0_CONTROL + first_color * sizeof(uint32_t) -
                R600_CONTEXT_REG_OFFSET) >>
               2;
   for (uint32_t color = 0; color < color_count; ++color) {
      /* Bit 30 is Evergreen BLEND_CONTROL_ENABLE, but reserved in the R700 register. */
      *packet++ = blend_control[color] & ~(UINT32_C(1) << 30);
   }
   return packet;
}
