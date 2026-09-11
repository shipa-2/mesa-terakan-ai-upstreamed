/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_hw_config_draw_terascale_1.h"

#include "gallium/drivers/r600/r600d.h"
#include "gallium/drivers/r600/r600d_common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                          \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         fprintf(stderr, "CHECK failed at %s:%u: %s\n", __FILE__, __LINE__, #condition);           \
         abort();                                                                                  \
      }                                                                                            \
   } while (0)

static void
test_db_depth_view(void)
{
   /* An arbitrary but representative value exercising both fields, matching the field layout
    * confirmed identical to R8xx/R9xx's own DB_DEPTH_VIEW (SLICE_START/SLICE_MAX, both 11 bits).
    */
   uint32_t const value = S_028004_SLICE_START(3) | S_028004_SLICE_MAX(5);

   uint32_t packets[3];
   uint32_t * const end = terakan_hw_config_draw_terascale_1_write_db_depth_view(packets, value);
   CHECK(end == packets + 3);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[1] == (R_028004_DB_DEPTH_VIEW - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] == value);
}

static void
test_spi_ps_interpolation(void)
{
   struct terakan_hw_config_draw_terascale_1_spi_ps_input const inputs[] = {
      {.semantic = 0, .gpr = 4, .position = true, .centroid = true},
      {.semantic = 3, .gpr = 1, .centroid = true},
      {.semantic = 7, .gpr = 2, .linear = true, .point_sprite = true, .sample = true},
      {.semantic = 0, .gpr = 5, .front_face_or_sample_mask = true, .flat = true},
      {.semantic = 0, .gpr = 6, .sample_id = true, .flat = true},
   };
   struct terakan_hw_config_draw_terascale_1_spi_ps spi;
   CHECK(terakan_hw_config_draw_terascale_1_spi_ps_encode(inputs, 5, &spi));
   CHECK(spi.input_count == 5);
   CHECK(spi.input_control[0] ==
         (S_028644_SEMANTIC(0) | S_028644_FLAT_SHADE(1) | S_028644_SEL_CENTROID(1)));
   CHECK(spi.input_control[1] ==
         (S_028644_SEMANTIC(3) | S_028644_SEL_CENTROID(1)));
   CHECK(spi.input_control[2] ==
         (S_028644_SEMANTIC(7) | S_028644_SEL_LINEAR(1) |
          S_028644_PT_SPRITE_TEX(1) | S_028644_SEL_SAMPLE(1)));
   CHECK(spi.input_control[3] == S_028644_FLAT_SHADE(1));
   CHECK(spi.input_control[4] == S_028644_FLAT_SHADE(1));
   CHECK(spi.in_control_0 ==
         (S_0286CC_NUM_INTERP(5) | S_0286CC_POSITION_ENA(1) |
          S_0286CC_POSITION_CENTROID(1) | S_0286CC_POSITION_ADDR(4) |
          S_0286CC_BARYC_SAMPLE_CNTL(1) | S_0286CC_PERSP_GRADIENT_ENA(1) |
          S_0286CC_LINEAR_GRADIENT_ENA(1)));
   CHECK(spi.in_control_1 ==
         (S_0286D0_FRONT_FACE_ENA(1) | S_0286D0_FRONT_FACE_ADDR(5) |
          S_0286D0_FIXED_PT_POSITION_ENA(1) | S_0286D0_FIXED_PT_POSITION_ADDR(6)));
   CHECK(spi.input_z == S_0286D8_PROVIDE_Z_TO_SPI(1));

   uint32_t packet[14];
   CHECK(terakan_hw_config_draw_terascale_1_write_spi_ps(packet, &spi) == packet + 14);
   CHECK(packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 5, 0));
   CHECK(packet[1] == (R_028644_SPI_PS_INPUT_CNTL_0 - R600_CONTEXT_REG_OFFSET) >> 2);
   for (uint32_t input_index = 0; input_index < 5; ++input_index) {
      CHECK(packet[2 + input_index] == spi.input_control[input_index]);
   }
   CHECK(packet[7] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(packet[8] == (R_0286CC_SPI_PS_IN_CONTROL_0 - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[9] == spi.in_control_0);
   CHECK(packet[10] == spi.in_control_1);
   CHECK(packet[11] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[12] == (R_0286D8_SPI_INPUT_Z - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[13] == spi.input_z);

   /* 0x0286E0 is SPI_FOG_FUNC_SCALE on R600/R700. The exact packet oracle above ends at
    * SPI_INPUT_Z and therefore proves that no Evergreen SPI_BARYC_CNTL write was appended.
    */
}

static void
test_spi_ps_rejects_invalid_allocations(void)
{
   struct terakan_hw_config_draw_terascale_1_spi_ps spi;
   struct terakan_hw_config_draw_terascale_1_spi_ps_input inputs[2] = {
      {.gpr = 1, .position = true},
      {.gpr = 2, .position = true},
   };
   CHECK(!terakan_hw_config_draw_terascale_1_spi_ps_encode(inputs, 2, &spi));
   inputs[1].position = false;
   inputs[0].gpr = 32;
   CHECK(!terakan_hw_config_draw_terascale_1_spi_ps_encode(inputs, 2, &spi));
   CHECK(!terakan_hw_config_draw_terascale_1_spi_ps_encode(
      inputs, TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_SPI_PS_INPUT_COUNT + 1, &spi));

   CHECK(terakan_hw_config_draw_terascale_1_spi_ps_encode(NULL, 0, &spi));
   CHECK(spi.in_control_0 == S_0286CC_PERSP_GRADIENT_ENA(1));
   uint32_t packet[7];
   CHECK(terakan_hw_config_draw_terascale_1_write_spi_ps(packet, &spi) == packet + 7);
   CHECK(packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(packet[1] == (R_0286CC_SPI_PS_IN_CONTROL_0 - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[2] == spi.in_control_0);
   CHECK(packet[3] == 0);
   CHECK(packet[4] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[5] == (R_0286D8_SPI_INPUT_Z - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[6] == 0);
}

static void
test_db_render_control_override(void)
{
   uint32_t const db_render_control = S_028D0C_ZPASS_INCREMENT_DISABLE(1);
   uint32_t const db_render_override =
      S_028D10_FORCE_HIS_ENABLE0(V_028D10_FORCE_DISABLE) |
      S_028D10_FORCE_HIS_ENABLE1(V_028D10_FORCE_DISABLE);

   uint32_t packets[4];
   uint32_t * const end = terakan_hw_config_draw_terascale_1_write_db_render_control_override(
      packets, db_render_control, db_render_override);
   CHECK(end == packets + 4);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(packets[1] == (R_028D0C_DB_RENDER_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] == db_render_control);
   CHECK(packets[3] == db_render_override);
}

static void
test_db_render_control_override_classic_r700_baseline(void)
{
   uint32_t db_render_control, db_render_override;
   CHECK(terakan_hw_config_draw_terascale_1_db_render_control_override_encode(
      0, 0, true, V_028D0C_EXPORT_ANY_Z, &db_render_control, &db_render_override));
   CHECK(db_render_control == S_028D0C_ZPASS_INCREMENT_DISABLE(1));
   CHECK(db_render_override ==
         (S_028D10_FORCE_HIZ_ENABLE(V_028D10_FORCE_DISABLE) |
          S_028D10_FORCE_HIS_ENABLE0(V_028D10_FORCE_DISABLE) |
          S_028D10_FORCE_HIS_ENABLE1(V_028D10_FORCE_DISABLE)));

   /* Nonzero inputs are Evergreen register payloads, not abstract state. Reject both independently
    * so a future caller cannot accidentally pass colliding fields through to R700.
    */
   CHECK(!terakan_hw_config_draw_terascale_1_db_render_control_override_encode(
      1, 0, true, V_028D0C_EXPORT_ANY_Z, &db_render_control, &db_render_override));
   CHECK(!terakan_hw_config_draw_terascale_1_db_render_control_override_encode(
      0, 1, true, V_028D0C_EXPORT_ANY_Z, &db_render_control, &db_render_override));

   uint32_t packets[4];
   uint32_t * const end = terakan_hw_config_draw_terascale_1_write_db_render_control_override(
      packets, S_028D0C_ZPASS_INCREMENT_DISABLE(1), db_render_override);
   CHECK(end == packets + 4);
   CHECK(packets[2] == S_028D0C_ZPASS_INCREMENT_DISABLE(1));
   CHECK(packets[3] == db_render_override);
}

static void
test_db_render_control_conservative_z(void)
{
   uint32_t db_render_control, db_render_override;
   CHECK(terakan_hw_config_draw_terascale_1_db_render_control_override_encode(
      0, 0, true, V_028D0C_EXPORT_LESS_THAN_Z, &db_render_control, &db_render_override));
   CHECK(db_render_control ==
         (S_028D0C_ZPASS_INCREMENT_DISABLE(1) |
          S_028D0C_CONSERVATIVE_Z_EXPORT(V_028D0C_EXPORT_LESS_THAN_Z)));
   CHECK(terakan_hw_config_draw_terascale_1_db_render_control_override_encode(
      0, 0, true, V_028D0C_EXPORT_GREATER_THAN_Z, &db_render_control, &db_render_override));
   CHECK(db_render_control ==
         (S_028D0C_ZPASS_INCREMENT_DISABLE(1) |
          S_028D0C_CONSERVATIVE_Z_EXPORT(V_028D0C_EXPORT_GREATER_THAN_Z)));

   /* r600_emit_db_misc_state() gates this field on gfx_level >= R700. This is the negative
    * control against accidentally emitting the R700 field on R600. */
   CHECK(!terakan_hw_config_draw_terascale_1_db_render_control_override_encode(
      0, 0, false, V_028D0C_EXPORT_LESS_THAN_Z, &db_render_control, &db_render_override));
   CHECK(!terakan_hw_config_draw_terascale_1_db_render_control_override_encode(
      0, 0, true, V_028D0C_EXPORT_RESERVED, &db_render_control, &db_render_override));
}

static void
test_db_render_control_occlusion_query(void)
{
   uint32_t db_render_control, db_render_override;
   CHECK(terakan_hw_config_draw_terascale_1_db_render_control_override_encode_query(
      0, 0, true, V_028D0C_EXPORT_ANY_Z, true, &db_render_control, &db_render_override));
   CHECK(db_render_control == S_028D0C_R700_PERFECT_ZPASS_COUNTS(1));
   CHECK(db_render_override ==
         (S_028D10_FORCE_HIZ_ENABLE(V_028D10_FORCE_DISABLE) |
          S_028D10_FORCE_HIS_ENABLE0(V_028D10_FORCE_DISABLE) |
          S_028D10_FORCE_HIS_ENABLE1(V_028D10_FORCE_DISABLE) |
          S_028D10_NOOP_CULL_DISABLE(1)));

   /* R600 has no R700 perfect-count bit, but the classic path still disables cull suppression
    * while a ZPASS query is active. This guards against blindly copying the R700 bit to R600. */
   CHECK(terakan_hw_config_draw_terascale_1_db_render_control_override_encode_query(
      0, 0, false, V_028D0C_EXPORT_ANY_Z, true, &db_render_control, &db_render_override));
   CHECK(db_render_control == 0);
   CHECK((db_render_override & S_028D10_NOOP_CULL_DISABLE(1)) != 0);

   /* Negative controls: the colliding Evergreen DB_COUNT_CONTROL payload and
    * R600 conservative-Z encodings must not be silently reinterpreted. */
   CHECK(!terakan_hw_config_draw_terascale_1_db_render_control_override_encode_query(
      (UINT32_C(1) << 1), 0, true, V_028D0C_EXPORT_ANY_Z, true,
      &db_render_control, &db_render_override));
   CHECK(!terakan_hw_config_draw_terascale_1_db_render_control_override_encode_query(
      0, 0, false, V_028D0C_EXPORT_GREATER_THAN_Z, true,
      &db_render_control, &db_render_override));
}

static struct terakan_hw_config_draw_terascale_1_db_shader_control_input
representative_db_shader_control_input(void)
{
   return (struct terakan_hw_config_draw_terascale_1_db_shader_control_input){
      .z_export_enable = true,
      .stencil_ref_export_enable = true,
      .z_order = V_02880C_EARLY_Z_THEN_LATE_Z,
      .kill_enable = true,
      .mask_export_enable = true,
      .dual_export_enable = true,
      .source_format = 2,
   };
}

static void
test_db_shader_control(void)
{
   struct terakan_hw_config_draw_terascale_1_db_shader_control_input input =
      representative_db_shader_control_input();
   uint32_t value;
   CHECK(terakan_hw_config_draw_terascale_1_db_shader_control_encode(&input, &value));
   CHECK(value ==
         (S_02880C_Z_EXPORT_ENABLE(1) | S_02880C_STENCIL_REF_EXPORT_ENABLE(1) |
          S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z) | S_02880C_KILL_ENABLE(1) |
          S_02880C_MASK_EXPORT_ENABLE(1) | S_02880C_DUAL_EXPORT_ENABLE(1)));

   /* DB_SOURCE_FORMAT=2 is deliberately absent from the R700 value. It is a valid Evergreen
    * software-side export description, but r600d.h has no corresponding register field.
    */
   CHECK((value & (UINT32_C(3) << 13)) == 0);

   uint32_t packet[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_db_shader_control(packet, value) == packet + 3);
   CHECK(packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[1] == (R_02880C_DB_SHADER_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[2] == value);
}

static void
test_db_shader_control_rejects_evergreen_only_fields(void)
{
   struct terakan_hw_config_draw_terascale_1_db_shader_control_input input =
      representative_db_shader_control_input();
   uint32_t value;

   input.exec_on_hier_fail = true;
   CHECK(!terakan_hw_config_draw_terascale_1_db_shader_control_encode(&input, &value));
   input.exec_on_hier_fail = false;
   input.exec_on_noop = true;
   CHECK(!terakan_hw_config_draw_terascale_1_db_shader_control_encode(&input, &value));
   input.exec_on_noop = false;
   input.alpha_to_mask_disable = true;
   CHECK(!terakan_hw_config_draw_terascale_1_db_shader_control_encode(&input, &value));
   input.alpha_to_mask_disable = false;
   input.depth_before_shader = true;
   CHECK(!terakan_hw_config_draw_terascale_1_db_shader_control_encode(&input, &value));
   input.depth_before_shader = false;
   /* A valid conservative-Z mode is consumed by the paired DB_RENDER_CONTROL encoder rather
    * than this register. The reserved fourth encoding remains rejected here. */
   input.conservative_z_export = V_028D0C_EXPORT_LESS_THAN_Z;
   CHECK(terakan_hw_config_draw_terascale_1_db_shader_control_encode(&input, &value));
   input.conservative_z_export = V_028D0C_EXPORT_RESERVED;
   CHECK(!terakan_hw_config_draw_terascale_1_db_shader_control_encode(&input, &value));
   input.conservative_z_export = 0;
   input.source_format = 3;
   CHECK(!terakan_hw_config_draw_terascale_1_db_shader_control_encode(&input, &value));
   input.source_format = 2;
   input.z_order = 4;
   CHECK(!terakan_hw_config_draw_terascale_1_db_shader_control_encode(&input, &value));
   input.z_order = V_02880C_EARLY_Z_THEN_LATE_Z;
   input.unknown_bits = UINT32_C(1) << 31;
   CHECK(!terakan_hw_config_draw_terascale_1_db_shader_control_encode(&input, &value));
}

static void
test_db_depth_size(void)
{
   uint32_t const pitch_tile_max = 47, slice_tile_max = 12345;
   uint32_t packets[3];
   uint32_t * const end =
      terakan_hw_config_draw_terascale_1_write_db_depth_size(packets, pitch_tile_max,
                                                              slice_tile_max);
   CHECK(end == packets + 3);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[1] == (R_028000_DB_DEPTH_SIZE - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] ==
        (S_028000_PITCH_TILE_MAX(pitch_tile_max) | S_028000_SLICE_TILE_MAX(slice_tile_max)));
}

static void
test_db_depth_base_info(void)
{
   uint32_t const db_depth_base = 0x12345u;
   uint32_t const db_depth_info =
      S_028010_FORMAT(V_028010_DEPTH_8_24) | S_028010_ARRAY_MODE(2) /* 1D tiled thin1 */;

   uint32_t packets[4];
   uint32_t * const end = terakan_hw_config_draw_terascale_1_write_db_depth_base_info(
      packets, db_depth_base, db_depth_info);
   CHECK(end == packets + 4);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(packets[1] == (R_02800C_DB_DEPTH_BASE - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] == db_depth_base);
   CHECK(packets[3] == db_depth_info);
}

static struct terakan_hw_config_draw_terascale_1_db_depth_input
representative_db_depth_input(void)
{
   return (struct terakan_hw_config_draw_terascale_1_db_depth_input){
      .base = 0x12345,
      .pitch_tile_max = 63,
      .height_tile_max = 31,
      .slice_tile_max = 2047,
      .slice_start = 2,
      .slice_max = 5,
      .format = TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_24,
      .array_mode = V_0280A0_ARRAY_2D_TILED_THIN1,
      .zrange_precision = true,
   };
}

static void
test_db_depth_encode(void)
{
   struct terakan_hw_config_draw_terascale_1_db_depth_input input =
      representative_db_depth_input();
   struct terakan_hw_config_draw_terascale_1_db_depth depth;
   CHECK(terakan_hw_config_draw_terascale_1_db_depth_encode(&input, &depth));
   CHECK(depth.base == 0x12345);
   CHECK(depth.size ==
         (S_028000_PITCH_TILE_MAX(63) | S_028000_SLICE_TILE_MAX(2047)));
   CHECK(depth.view == (S_028004_SLICE_START(2) | S_028004_SLICE_MAX(5)));
   CHECK(depth.info ==
         (S_028010_FORMAT(V_028010_DEPTH_X8_24) |
          S_028010_ARRAY_MODE(V_0280A0_ARRAY_2D_TILED_THIN1) |
          S_028010_ZRANGE_PRECISION(1)));
   CHECK(depth.prefetch_limit == S_028D34_DEPTH_HEIGHT_TILE_MAX(31));

   input.format = TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_16;
   CHECK(terakan_hw_config_draw_terascale_1_db_depth_encode(&input, &depth));
   CHECK(G_028010_FORMAT(depth.info) == V_028010_DEPTH_16);
   input.format = TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_32_FLOAT;
   CHECK(terakan_hw_config_draw_terascale_1_db_depth_encode(&input, &depth));
   CHECK(G_028010_FORMAT(depth.info) == V_028010_DEPTH_32_FLOAT);
}

static void
test_db_depth_encode_multisample_and_rejects_unported_surfaces(void)
{
   struct terakan_hw_config_draw_terascale_1_db_depth_input input =
      representative_db_depth_input();
   struct terakan_hw_config_draw_terascale_1_db_depth depth;

   /* MSAA sample count belongs to PA_SC_AA_CONFIG, not DB_DEPTH_INFO. The DB packet therefore is
    * byte-for-byte the classic r600_init_depth_surface() encoding for the same tiled dimensions. */
   input.samples_log2 = 2;
   CHECK(terakan_hw_config_draw_terascale_1_db_depth_encode(&input, &depth));
   CHECK(depth.info ==
         (S_028010_FORMAT(V_028010_DEPTH_X8_24) |
          S_028010_ARRAY_MODE(V_0280A0_ARRAY_2D_TILED_THIN1) | S_028010_ZRANGE_PRECISION(1)));
   uint32_t packets[13];
   CHECK(terakan_hw_config_draw_terascale_1_write_db_depth(packets, &depth) == packets + 13);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[1] == (R_028000_DB_DEPTH_SIZE - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[8] == input.base);
   CHECK(packets[9] == depth.info);
   input.samples_log2 = 4;
   CHECK(!terakan_hw_config_draw_terascale_1_db_depth_encode(&input, &depth));
   input.samples_log2 = 0;
   input.array_mode = V_0280A0_ARRAY_LINEAR_ALIGNED;
   CHECK(!terakan_hw_config_draw_terascale_1_db_depth_encode(&input, &depth));
   input.array_mode = V_0280A0_ARRAY_2D_TILED_THIN1;
   input.format = TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_INVALID;
   CHECK(!terakan_hw_config_draw_terascale_1_db_depth_encode(&input, &depth));
   input.format = TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_24;
   input.pitch_tile_max = 0x400;
   CHECK(!terakan_hw_config_draw_terascale_1_db_depth_encode(&input, &depth));
}

static void
test_db_depth_remaining_packets(void)
{
   struct terakan_hw_config_draw_terascale_1_db_depth_input const input =
      representative_db_depth_input();
   struct terakan_hw_config_draw_terascale_1_db_depth depth;
   CHECK(terakan_hw_config_draw_terascale_1_db_depth_encode(&input, &depth));
   uint32_t packets[13];
   CHECK(terakan_hw_config_draw_terascale_1_write_db_depth(packets, &depth) == packets + 13);
   uint32_t const registers[4] = {
      R_028000_DB_DEPTH_SIZE, R_028004_DB_DEPTH_VIEW, R_02800C_DB_DEPTH_BASE,
      R_028D34_DB_PREFETCH_LIMIT,
   };
   uint32_t const packet_offsets[4] = {0, 3, 6, 10};
   uint32_t const packet_counts[4] = {1, 1, 2, 1};
   for (uint32_t packet_index = 0; packet_index < 4; ++packet_index) {
      uint32_t const offset = packet_offsets[packet_index];
      CHECK(packets[offset] == PKT3(PKT3_SET_CONTEXT_REG, packet_counts[packet_index], 0));
      CHECK(packets[offset + 1] ==
            (registers[packet_index] - R600_CONTEXT_REG_OFFSET) >> 2);
   }
   CHECK(packets[2] == depth.size);
   CHECK(packets[5] == depth.view);
   CHECK(packets[8] == depth.base);
   CHECK(packets[9] == depth.info);
   CHECK(packets[12] == depth.prefetch_limit);

   uint32_t prefetch_packet[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_db_depth_prefetch_limit(
            prefetch_packet, 31) == prefetch_packet + 3);
   CHECK(prefetch_packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(prefetch_packet[1] == (R_028D34_DB_PREFETCH_LIMIT - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(prefetch_packet[2] == 31);

   uint32_t unbound_packet[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_db_depth_unbound(unbound_packet) ==
         unbound_packet + 3);
   CHECK(unbound_packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(unbound_packet[1] == (R_028010_DB_DEPTH_INFO - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(unbound_packet[2] == S_028010_FORMAT(V_028010_DEPTH_INVALID));
}

static void
test_db_alpha_to_mask(void)
{
   uint32_t const offsets = S_028D44_ALPHA_TO_MASK_OFFSET0(2) |
                            S_028D44_ALPHA_TO_MASK_OFFSET1(2) |
                            S_028D44_ALPHA_TO_MASK_OFFSET2(2) |
                            S_028D44_ALPHA_TO_MASK_OFFSET3(2);
   CHECK(terakan_hw_config_draw_terascale_1_db_alpha_to_mask_encode(false) == offsets);
   uint32_t const enabled = offsets | S_028D44_ALPHA_TO_MASK_ENABLE(1);
   CHECK(terakan_hw_config_draw_terascale_1_db_alpha_to_mask_encode(true) == enabled);

   uint32_t packets[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_db_alpha_to_mask(packets, enabled) ==
         packets + 3);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[1] == (R_028D44_DB_ALPHA_TO_MASK - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] == enabled);
}

static void
test_pa_sc_aa_mask(void)
{
   uint32_t const encoded =
      terakan_hw_config_draw_terascale_1_pa_sc_aa_mask_encode(UINT32_C(0x123456A5));
   CHECK(encoded == UINT32_C(0xA5A5A5A5));

   uint32_t packet[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_pa_sc_aa_mask(packet, encoded) == packet + 3);
   CHECK(packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[1] == (R_028C48_PA_SC_AA_MASK - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[2] == encoded);
}

static void
test_pa_sc_aa_config_sample_locs(void)
{
   uint8_t sample_locs[16][4] = {0};
   uint8_t const eight_sample_pattern[8] = {0x11, 0xDF, 0x2D, 0xF4,
                                             0xEB, 0x52, 0x35, 0xB3};
   for (uint32_t sample_index = 0; sample_index < 8; ++sample_index) {
      for (uint32_t pixel_index = 0; pixel_index < 4; ++pixel_index) {
         sample_locs[sample_index][pixel_index] = eight_sample_pattern[sample_index];
      }
   }

   struct terakan_hw_config_draw_terascale_1_pa_sc_aa aa;
   CHECK(terakan_hw_config_draw_terascale_1_pa_sc_aa_encode(3, 7, true, sample_locs, &aa));
   CHECK(aa.config == (S_028C04_MSAA_NUM_SAMPLES(3) |
                       S_028C04_AA_MASK_CENTROID_DTMN(1) |
                       S_028C04_MAX_SAMPLE_DIST(7)));
   CHECK(aa.sample_locs[0] == UINT32_C(0xF42DDF11));
   CHECK(aa.sample_locs[1] == UINT32_C(0xB33552EB));

   uint32_t packet[7];
   CHECK(terakan_hw_config_draw_terascale_1_pa_sc_aa_packet_dwords(false, &aa) == 7);
   CHECK(terakan_hw_config_draw_terascale_1_write_pa_sc_aa(packet, false, &aa) == packet + 7);
   CHECK(packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(packet[1] ==
         (R_028C1C_PA_SC_AA_SAMPLE_LOCS_MCTX - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[2] == aa.sample_locs[0]);
   CHECK(packet[3] == aa.sample_locs[1]);
   CHECK(packet[4] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[5] == (R_028C04_PA_SC_AA_CONFIG - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[6] == aa.config);

   CHECK(terakan_hw_config_draw_terascale_1_pa_sc_aa_packet_dwords(true, &aa) == 7);
   CHECK(terakan_hw_config_draw_terascale_1_write_pa_sc_aa(packet, true, &aa) == packet + 7);
   CHECK(packet[0] == PKT3(PKT3_SET_CONFIG_REG, 2, 0));
   CHECK(packet[1] ==
         (R_008B48_PA_SC_AA_SAMPLE_LOCS_8S_WD0 - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packet[2] == aa.sample_locs[0]);
   CHECK(packet[3] == aa.sample_locs[1]);
   CHECK(packet[4] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[5] == (R_028C04_PA_SC_AA_CONFIG - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[6] == aa.config);

   aa.config = 0;
   CHECK(terakan_hw_config_draw_terascale_1_pa_sc_aa_packet_dwords(true, &aa) == 3);
   CHECK(terakan_hw_config_draw_terascale_1_write_pa_sc_aa(packet, true, &aa) == packet + 3);
   CHECK(packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[1] == (R_028C04_PA_SC_AA_CONFIG - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[2] == 0);

   aa.config = S_028C04_MSAA_NUM_SAMPLES(1) | S_028C04_MAX_SAMPLE_DIST(4);
   CHECK(terakan_hw_config_draw_terascale_1_pa_sc_aa_packet_dwords(true, &aa) == 6);
   CHECK(terakan_hw_config_draw_terascale_1_write_pa_sc_aa(packet, true, &aa) == packet + 6);
   CHECK(packet[0] == PKT3(PKT3_SET_CONFIG_REG, 1, 0));
   CHECK(packet[1] == (R_008B40_PA_SC_AA_SAMPLE_LOCS_2S - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packet[2] == aa.sample_locs[0]);
   CHECK(packet[3] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[4] == (R_028C04_PA_SC_AA_CONFIG - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[5] == aa.config);

   aa.config = S_028C04_MSAA_NUM_SAMPLES(2) | S_028C04_MAX_SAMPLE_DIST(6);
   CHECK(terakan_hw_config_draw_terascale_1_pa_sc_aa_packet_dwords(true, &aa) == 6);
   CHECK(terakan_hw_config_draw_terascale_1_write_pa_sc_aa(packet, true, &aa) == packet + 6);
   CHECK(packet[0] == PKT3(PKT3_SET_CONFIG_REG, 1, 0));
   CHECK(packet[1] == (R_008B44_PA_SC_AA_SAMPLE_LOCS_4S - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packet[2] == aa.sample_locs[0]);
   CHECK(packet[3] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[4] == (R_028C04_PA_SC_AA_CONFIG - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[5] == aa.config);

   sample_locs[3][2] ^= 1;
   CHECK(!terakan_hw_config_draw_terascale_1_pa_sc_aa_encode(3, 7, true, sample_locs, &aa));
   sample_locs[3][2] ^= 1;
   CHECK(!terakan_hw_config_draw_terascale_1_pa_sc_aa_encode(4, 7, true, sample_locs, &aa));
   CHECK(!terakan_hw_config_draw_terascale_1_pa_sc_aa_encode(3, 16, true, sample_locs, &aa));
}

static void
test_pa_sc_mode(void)
{
   struct terakan_hw_config_draw_terascale_1_pa_sc_mode_input input = {
      .msaa_enable = true,
      .line_stipple_enable = true,
      .viewport_scissor_enable = true,
      .ps_iter_sample = true,
      .is_r700 = true,
   };
   uint32_t mode;
   CHECK(terakan_hw_config_draw_terascale_1_pa_sc_mode_encode(&input, &mode));
   uint32_t const expected = S_028A4C_MSAA_ENABLE(1) | S_028A4C_LINE_STIPPLE_ENABLE(1) |
                             S_028A4C_FORCE_EOV_CNTDWN_ENABLE(1) |
                             S_028A4C_FORCE_EOV_REZ_ENABLE(1) | S_028A4C_PS_ITER_SAMPLE(1) |
                             S_028A4C_R700_ZMM_LINE_OFFSET(1) |
                             S_028A4C_R700_VPORT_SCISSOR_ENABLE(1);
   CHECK(mode == expected);

   input.is_rv770 = true;
   CHECK(terakan_hw_config_draw_terascale_1_pa_sc_mode_encode(&input, &mode));
   CHECK(mode == (expected | S_028A4C_TILE_COVER_DISABLE(1)));
   input.msaa_enable = false;
   CHECK(terakan_hw_config_draw_terascale_1_pa_sc_mode_encode(&input, &mode));
   CHECK((mode & S_028A4C_TILE_COVER_DISABLE(1)) == 0);

   input.is_r700 = false;
   input.is_rv770 = false;
   input.msaa_enable = true;
   CHECK(terakan_hw_config_draw_terascale_1_pa_sc_mode_encode(&input, &mode));
   uint32_t const expected_r600 =
      S_028A4C_MSAA_ENABLE(1) | S_028A4C_LINE_STIPPLE_ENABLE(1) |
      S_028A4C_FORCE_EOV_CNTDWN_ENABLE(1) | S_028A4C_PS_ITER_SAMPLE(1) |
      S_028A4C_WALK_ALIGN8_PRIM_FITS_ST(1);
   CHECK(mode == expected_r600);

   input.unknown_mode_0_bits = UINT32_C(1) << 31;
   CHECK(!terakan_hw_config_draw_terascale_1_pa_sc_mode_encode(&input, &mode));
   input.unknown_mode_0_bits = 0;
   input.unknown_mode_1_bits = UINT32_C(1) << 30;
   CHECK(!terakan_hw_config_draw_terascale_1_pa_sc_mode_encode(&input, &mode));

   uint32_t packet[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_pa_sc_mode(packet, expected) == packet + 3);
   CHECK(packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[1] == (R_028A4C_PA_SC_MODE_CNTL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[2] == expected);
}

static void
test_pa_su_poly_offset(void)
{
   uint32_t const db_fmt_evergreen = UINT32_C(0xE9) | (UINT32_C(1) << 8);
   uint32_t db_fmt_r700;
   CHECK(terakan_hw_config_draw_terascale_1_pa_su_poly_offset_db_fmt_encode(
      db_fmt_evergreen, &db_fmt_r700));
   CHECK(db_fmt_r700 == (S_028DF8_POLY_OFFSET_NEG_NUM_DB_BITS(0xE9) |
                         S_028DF8_POLY_OFFSET_DB_IS_FLOAT_FMT(1)));
   CHECK(!terakan_hw_config_draw_terascale_1_pa_su_poly_offset_db_fmt_encode(
      db_fmt_evergreen | (UINT32_C(1) << 9), &db_fmt_r700));

   uint32_t db_fmt_packet[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_pa_su_poly_offset_db_fmt(
            db_fmt_packet, db_fmt_r700) == db_fmt_packet + 3);
   CHECK(db_fmt_packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(db_fmt_packet[1] ==
         (R_028DF8_PA_SU_POLY_OFFSET_DB_FMT_CNTL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(db_fmt_packet[2] == db_fmt_r700);

   uint32_t const clamp = UINT32_C(0x3E800000);
   uint32_t const scale = UINT32_C(0x40A00000);
   uint32_t const offset = UINT32_C(0xC0600000);
   uint32_t packet[7];
   CHECK(terakan_hw_config_draw_terascale_1_write_pa_su_poly_offset(
            packet, clamp, scale, offset) == packet + 7);
   CHECK(packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 5, 0));
   CHECK(packet[1] == (R_028DFC_PA_SU_POLY_OFFSET_CLAMP - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[2] == clamp);
   CHECK(packet[3] == scale);
   CHECK(packet[4] == offset);
   CHECK(packet[5] == scale);
   CHECK(packet[6] == offset);
}

static void
test_pa_cl_clip_cntl(void)
{
   uint32_t const common = S_028810_DX_CLIP_SPACE_DEF(1) |
                           S_028810_DX_LINEAR_ATTR_CLIP_ENA(1) |
                           S_028810_ZCLIP_NEAR_DISABLE(1);
   struct terakan_hw_config_draw_terascale_1_pa_cl_clip clip;

   CHECK(terakan_hw_config_draw_terascale_1_pa_cl_clip_encode(
      common | S_028810_DX_RASTERIZATION_KILL(1), true, &clip));
   CHECK(clip.clip_cntl == (common | S_028810_DX_RASTERIZATION_KILL(1)));
   CHECK(clip.sx_misc == 0);
   CHECK(terakan_hw_config_draw_terascale_1_pa_cl_clip_packet_dwords(true) == 3);
   uint32_t packet[6];
   CHECK(terakan_hw_config_draw_terascale_1_write_pa_cl_clip(packet, true, &clip) == packet + 3);
   CHECK(packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[1] == (R_028810_PA_CL_CLIP_CNTL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[2] == (common | S_028810_DX_RASTERIZATION_KILL(1)));

   CHECK(terakan_hw_config_draw_terascale_1_pa_cl_clip_encode(
      common | S_028810_DX_RASTERIZATION_KILL(1), false, &clip));
   CHECK(clip.clip_cntl == common);
   CHECK(clip.sx_misc == S_028350_MULTIPASS(1));
   CHECK(terakan_hw_config_draw_terascale_1_pa_cl_clip_packet_dwords(false) == 6);
   CHECK(terakan_hw_config_draw_terascale_1_write_pa_cl_clip(packet, false, &clip) == packet + 6);
   CHECK(packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[1] == (R_028810_PA_CL_CLIP_CNTL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[2] == common);
   CHECK(packet[3] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[4] == (R_028350_SX_MISC - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[5] == S_028350_MULTIPASS(1));

   CHECK(terakan_hw_config_draw_terascale_1_pa_cl_clip_encode(common, false, &clip));
   CHECK(clip.clip_cntl == common);
   CHECK(clip.sx_misc == 0);
   CHECK(!terakan_hw_config_draw_terascale_1_pa_cl_clip_encode(
      common | S_028810_UCP_ENA_0(1), false, &clip));
}

static void
test_sq_pgm_bindings(void)
{
   uint32_t const evergreen_resources =
      UINT32_C(37) | (UINT32_C(11) << 8) | (UINT32_C(1) << 21);
   uint32_t resources;
   CHECK(terakan_hw_config_draw_terascale_1_sq_pgm_resources_encode(
      evergreen_resources, false, &resources));
   CHECK(resources ==
         (S_028850_NUM_GPRS(37) | S_028850_STACK_SIZE(11) | S_028850_DX10_CLAMP(1)));

   CHECK(terakan_hw_config_draw_terascale_1_sq_pgm_resources_encode(
      evergreen_resources, true, &resources));
   CHECK(resources == (S_028850_NUM_GPRS(37) | S_028850_STACK_SIZE(11) |
                       S_028850_DX10_CLAMP(1) | S_028850_UNCACHED_FIRST_INST(1)));
   CHECK(!terakan_hw_config_draw_terascale_1_sq_pgm_resources_encode(
      evergreen_resources | (UINT32_C(1) << 23), false, &resources));

   uint32_t fs_packet[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_sq_pgm_fs(fs_packet, 0x12345) ==
         fs_packet + 3);
   CHECK(fs_packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(fs_packet[1] == (R_028894_SQ_PGM_START_FS - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(fs_packet[2] == 0x12345);

   uint32_t vs_packet[6];
   CHECK(terakan_hw_config_draw_terascale_1_write_sq_pgm_vs(vs_packet, 0x23456, resources) ==
         vs_packet + 6);
   CHECK(vs_packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(vs_packet[1] == (R_028858_SQ_PGM_START_VS - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(vs_packet[2] == 0x23456);
   CHECK(vs_packet[3] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(vs_packet[4] == (R_028868_SQ_PGM_RESOURCES_VS - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(vs_packet[5] == resources);

   uint32_t const exports = S_028854_EXPORT_COLORS(3) | S_028854_EXPORT_Z(1);
   uint32_t ps_packet[7];
   CHECK(terakan_hw_config_draw_terascale_1_write_sq_pgm_ps(
            ps_packet, 0x34567, resources, exports) == ps_packet + 7);
   CHECK(ps_packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(ps_packet[1] == (R_028840_SQ_PGM_START_PS - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(ps_packet[2] == 0x34567);
   CHECK(ps_packet[3] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(ps_packet[4] == (R_028850_SQ_PGM_RESOURCES_PS - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(ps_packet[5] == resources);
   CHECK(ps_packet[6] == exports);

   uint32_t es_packet[6];
   CHECK(terakan_hw_config_draw_terascale_1_write_sq_pgm_es(
            es_packet, 0x45678, resources) == es_packet + 6);
   CHECK(es_packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(es_packet[1] == (R_028880_SQ_PGM_START_ES - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(es_packet[2] == 0x45678);
   CHECK(es_packet[3] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(es_packet[4] == (R_028890_SQ_PGM_RESOURCES_ES - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(es_packet[5] == resources);

   uint32_t gs_packet[6];
   CHECK(terakan_hw_config_draw_terascale_1_write_sq_pgm_gs(
            gs_packet, 0x56789, resources) == gs_packet + 6);
   CHECK(gs_packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(gs_packet[1] == (R_02886C_SQ_PGM_START_GS - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(gs_packet[2] == 0x56789);
   CHECK(gs_packet[3] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(gs_packet[4] == (R_02887C_SQ_PGM_RESOURCES_GS - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(gs_packet[5] == resources);

   uint32_t const out_ids[2] = {UINT32_C(0x03020100), UINT32_C(0x07060504)};
   uint32_t out_id_packet[4];
   CHECK(terakan_hw_config_draw_terascale_1_write_spi_vs_out_id(out_id_packet, 2, out_ids) ==
         out_id_packet + 4);
   CHECK(out_id_packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(out_id_packet[1] == (R_028614_SPI_VS_OUT_ID_0 - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(out_id_packet[2] == out_ids[0]);
   CHECK(out_id_packet[3] == out_ids[1]);
}

static void
test_gpr_baseline_admission(void)
{
   /* RV710's exact begin-atom partition from r600_init_atom_start_cs(). The packet carrying these
    * values is checked independently by terakan_hw_config_shared_terascale_1_test; this oracle
    * checks the draw admission boundary against that packet rather than reproducing repartitioning
    * that Terakan does not emit yet.
    */
   struct terakan_hw_config_draw_terascale_1_gpr_counts const rv710_baseline = {
      .ps = 192,
      .vs = 56,
      .gs = 0,
      .es = 0,
   };
   struct terakan_hw_config_draw_terascale_1_gpr_counts required = rv710_baseline;
   CHECK(terakan_hw_config_draw_terascale_1_gprs_fit_baseline(&required, &rv710_baseline));

   ++required.ps;
   CHECK(!terakan_hw_config_draw_terascale_1_gprs_fit_baseline(&required, &rv710_baseline));
   required = rv710_baseline;
   ++required.vs;
   CHECK(!terakan_hw_config_draw_terascale_1_gprs_fit_baseline(&required, &rv710_baseline));
   required = rv710_baseline;
   required.gs = 1;
   CHECK(!terakan_hw_config_draw_terascale_1_gprs_fit_baseline(&required, &rv710_baseline));
   required = rv710_baseline;
   required.es = 1;
   CHECK(!terakan_hw_config_draw_terascale_1_gprs_fit_baseline(&required, &rv710_baseline));

   CHECK(!terakan_hw_config_draw_terascale_1_gprs_fit_baseline(NULL, &rv710_baseline));
   CHECK(!terakan_hw_config_draw_terascale_1_gprs_fit_baseline(&required, NULL));
}

static void
test_draw_constant_packet_is_empty(void)
{
   /* terakan_hw_config_shared_terascale_1_test checks the exact non-empty replacement packet.
    * Here the exact per-draw packet oracle is zero: allowing the Evergreen constant array after
    * that packet would overwrite several unrelated R700 registers at colliding addresses.
    */
   CHECK(terakan_hw_config_draw_terascale_1_constant_packet_dwords() == 0);
}

static void
test_absent_vgt_controls(void)
{
   uint32_t packet_dwords = UINT32_MAX;
   CHECK(terakan_hw_config_draw_terascale_1_absent_vgt_control_encode(0, &packet_dwords));
   CHECK(packet_dwords == 0);
   CHECK(!terakan_hw_config_draw_terascale_1_absent_vgt_control_encode(1, &packet_dwords));
   CHECK(!terakan_hw_config_draw_terascale_1_absent_vgt_control_encode(0, NULL));
}

static void
test_ring_itemsize_is_begin_atom_only(void)
{
   uint32_t itemsize_dwords[6] = {};
   uint32_t packet_dwords = UINT32_MAX;
   CHECK(terakan_hw_config_draw_terascale_1_ring_itemsize_encode(
      itemsize_dwords, 6, &packet_dwords));
   CHECK(packet_dwords == 0);
   itemsize_dwords[2] = 1;
   CHECK(!terakan_hw_config_draw_terascale_1_ring_itemsize_encode(
      itemsize_dwords, 6, &packet_dwords));
   CHECK(!terakan_hw_config_draw_terascale_1_ring_itemsize_encode(NULL, 6, &packet_dwords));
   CHECK(!terakan_hw_config_draw_terascale_1_ring_itemsize_encode(itemsize_dwords, 6, NULL));
}

static void
test_absent_ls_bool_const(void)
{
   uint32_t packet_dwords = UINT32_MAX;
   CHECK(terakan_hw_config_draw_terascale_1_absent_ls_bool_const_encode(0, &packet_dwords));
   CHECK(packet_dwords == 0);
   CHECK(!terakan_hw_config_draw_terascale_1_absent_ls_bool_const_encode(1, &packet_dwords));
   CHECK(!terakan_hw_config_draw_terascale_1_absent_ls_bool_const_encode(0, NULL));
}

static void
test_absent_cb_immed(void)
{
   CHECK(terakan_hw_config_draw_terascale_1_cb_immed_packet_dwords() == 0);
}

static void
test_absent_index_buffer_unbind(void)
{
   CHECK(terakan_hw_config_draw_terascale_1_index_buffer_unbind_packet_dwords() == 0);
}

static void
test_zero_count_draw(void)
{
   uint32_t packet[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_zero_count_draw(packet) == packet + 3);
   CHECK(packet[0] == PKT3(PKT3_DRAW_INDEX_AUTO, 1, 0));
   CHECK(packet[1] == 0);
   CHECK(packet[2] == S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX));
}

static void
test_draw_index(void)
{
   uint32_t packet[5];
   uint64_t const va = UINT64_C(0x000000ab12345678);
   CHECK(terakan_hw_config_draw_terascale_1_write_draw_index(packet, va, 37) == packet + 5);
   CHECK(packet[0] == PKT3(PKT3_DRAW_INDEX, 3, 0));
   CHECK(packet[1] == UINT32_C(0x12345678));
   CHECK(packet[2] == UINT32_C(0xAB));
   CHECK(packet[3] == 37);
   CHECK(packet[4] == S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_DMA));
}

static struct terakan_hw_config_draw_terascale_1_cb_color_input
representative_cb_color_input(void)
{
   return (struct terakan_hw_config_draw_terascale_1_cb_color_input){
      .base = 0x12345,
      .pitch_tile_max = 63,
      .slice_tile_max = 511,
      .slice_start = 3,
      .slice_max = 7,
      .endian = 0,
      .format = V_0280A0_COLOR_8_8_8_8,
      .array_mode = V_0280A0_ARRAY_2D_TILED_THIN1,
      .number_type = V_0280A0_NUMBER_UNORM,
      .comp_swap = V_0280A0_SWAP_ALT,
      .blend_clamp = true,
      .simple_float = true,
      .source_format = 1,
   };
}

static void
test_cb_color_encode(void)
{
   struct terakan_hw_config_draw_terascale_1_cb_color_input const input =
      representative_cb_color_input();
   struct terakan_hw_config_draw_terascale_1_cb_color color;
   CHECK(terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
   CHECK(color.base == input.base);
   CHECK(color.size == (S_028060_PITCH_TILE_MAX(63) | S_028060_SLICE_TILE_MAX(511)));
   CHECK(color.view == (S_028080_SLICE_START(3) | S_028080_SLICE_MAX(7)));
   CHECK(color.info ==
         (S_0280A0_FORMAT(V_0280A0_COLOR_8_8_8_8) |
          S_0280A0_ARRAY_MODE(V_0280A0_ARRAY_2D_TILED_THIN1) |
          S_0280A0_NUMBER_TYPE(V_0280A0_NUMBER_UNORM) |
          S_0280A0_COMP_SWAP(V_0280A0_SWAP_ALT) | S_0280A0_BLEND_CLAMP(1) |
          S_0280A0_SIMPLE_FLOAT(1) | S_0280A0_SOURCE_FORMAT(V_0280A0_EXPORT_NORM)));
   CHECK(color.fmask == input.base);
   CHECK(color.cmask == input.base);
   CHECK(color.mask == 0);
}

static void
test_cb_color_encode_multisample_metadata(void)
{
   struct terakan_hw_config_draw_terascale_1_cb_color_input input =
      representative_cb_color_input();
   struct terakan_hw_config_draw_terascale_1_cb_color color;

   /* Exact CB_COLORn metadata fields used by r600_init_color_surface() for a 4x target. The
    * addresses and tile maxima are deliberately distinct from BASE and each other: a test with
    * aliases would not prove that FRAG/TILE/MASK survived the translation. */
   input.is_multisampled = true;
   input.metadata_enabled = true;
   input.fmask = 0x22345;
   input.cmask = 0x32345;
   input.cmask_tile_max = 5;
   input.fmask_tile_max = 0x345;
   CHECK(terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
   CHECK(color.info ==
         (S_0280A0_FORMAT(V_0280A0_COLOR_8_8_8_8) |
          S_0280A0_ARRAY_MODE(V_0280A0_ARRAY_2D_TILED_THIN1) |
          S_0280A0_NUMBER_TYPE(V_0280A0_NUMBER_UNORM) |
          S_0280A0_COMP_SWAP(V_0280A0_SWAP_ALT) | S_0280A0_BLEND_CLAMP(1) |
          S_0280A0_SIMPLE_FLOAT(1) | S_0280A0_SOURCE_FORMAT(V_0280A0_EXPORT_NORM) |
          S_0280A0_TILE_MODE(V_0280A0_FRAG_ENABLE)));
   CHECK(color.fmask == input.fmask);
   CHECK(color.cmask == input.cmask);
   CHECK(color.mask ==
         (S_028100_CMASK_BLOCK_MAX(input.cmask_tile_max) |
          S_028100_FMASK_TILE_MAX(input.fmask_tile_max)));

   /* Negative controls: R6xx accepts the pair as a unit only. Dropping either the metadata state
    * or MASK must reject the descriptor rather than emit a surface whose CB accesses disagree. */
   input.metadata_enabled = false;
   CHECK(!terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
   input.metadata_enabled = true;
   input.cmask_tile_max = 0x1000;
   CHECK(!terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
}

static void
test_cb_color_encode_rejects_unported_surfaces(void)
{
   struct terakan_hw_config_draw_terascale_1_cb_color_input input =
      representative_cb_color_input();
   struct terakan_hw_config_draw_terascale_1_cb_color color;

   input.is_uav = true;
   CHECK(!terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
   input.is_uav = false;
   input.is_multisampled = true;
   CHECK(!terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
   input.is_multisampled = false;
   input.metadata_enabled = true;
   CHECK(!terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
   input.metadata_enabled = false;
   input.source_format = 2;
   CHECK(!terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
   input.source_format = 1;
   input.pitch_tile_max = 0x400;
   CHECK(!terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
}

static void
test_cb_color_packets(void)
{
   struct terakan_hw_config_draw_terascale_1_cb_color_input const input =
      representative_cb_color_input();
   struct terakan_hw_config_draw_terascale_1_cb_color color;
   CHECK(terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));

   uint32_t packets[21];
   uint32_t * const end =
      terakan_hw_config_draw_terascale_1_write_cb_color(packets, 2, &color);
   CHECK(end == packets + 21);
   uint32_t const registers[7] = {
      R_0280A0_CB_COLOR0_INFO + 2 * 4, R_028040_CB_COLOR0_BASE + 2 * 4,
      R_0280E0_CB_COLOR0_FRAG + 2 * 4, R_0280C0_CB_COLOR0_TILE + 2 * 4,
      R_028060_CB_COLOR0_SIZE + 2 * 4, R_028080_CB_COLOR0_VIEW + 2 * 4,
      R_028100_CB_COLOR0_MASK + 2 * 4,
   };
   uint32_t const values[7] = {
      color.info, color.base, color.fmask, color.cmask, color.size, color.view, color.mask,
   };
   for (uint32_t register_index = 0; register_index < 7; ++register_index) {
      uint32_t const packet_index = register_index * 3;
      CHECK(packets[packet_index] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
      CHECK(packets[packet_index + 1] ==
            (registers[register_index] - R600_CONTEXT_REG_OFFSET) >> 2);
      CHECK(packets[packet_index + 2] == values[register_index]);
   }

   uint32_t drm_packets[27];
   CHECK(terakan_hw_config_draw_terascale_1_write_cb_color_drm_relocations(
            drm_packets, 2, &color, 7) == drm_packets + 27);
   /* Negative control: moving these NOPs to the end makes r600_cs_packet_next_reloc reject
    * CB_COLOR0_BASE before it can validate a draw. Keep the exact parser-required positions. */
   uint32_t const relocation_packet_indices[3] = {6, 11, 16};
   for (uint32_t relocation_index = 0; relocation_index < 3; ++relocation_index) {
      uint32_t const packet_index = relocation_packet_indices[relocation_index];
      CHECK(drm_packets[packet_index] == PKT3(PKT3_NOP, 0, 0));
      CHECK(drm_packets[packet_index + 1] == 4 * 7);
   }
   CHECK(drm_packets[3] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(drm_packets[8] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(drm_packets[13] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(drm_packets[18] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   uint32_t const prefix_dwords[7] = {3, 8, 13, 18, 21, 24, 27};
   for (uint32_t field_count = 1; field_count <= 7; ++field_count) {
      uint32_t prefix_packets[27];
      CHECK(terakan_hw_config_draw_terascale_1_write_cb_color_drm_relocations_prefix(
               prefix_packets, 2, &color, 7, field_count) ==
            prefix_packets + prefix_dwords[field_count - 1]);
   }

   uint32_t unbound_packet[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_cb_color_unbound(unbound_packet, 7, 1) ==
         unbound_packet + 3);
   CHECK(unbound_packet[1] ==
         (R_0280A0_CB_COLOR0_INFO + 7 * 4 - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(unbound_packet[2] == S_0280A0_SOURCE_FORMAT(V_0280A0_EXPORT_NORM));
}

static void
test_cb_color_control_and_blend_packets(void)
{
   uint32_t const control = terakan_hw_config_draw_terascale_1_cb_color_control_encode(
      TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_CB_COLOR_NORMAL, 0xCC, true, true, 0xA5);
   CHECK(control ==
         (S_028808_MULTIWRITE_ENABLE(1) | S_028808_DEGAMMA_ENABLE(1) |
          S_028808_SPECIAL_OP(V_028808_SPECIAL_NORMAL) | S_028808_PER_MRT_BLEND(1) |
          S_028808_TARGET_BLEND_ENABLE(0xA5) | S_028808_ROP3(0xCC)));
   CHECK(terakan_hw_config_draw_terascale_1_cb_color_control_encode(
            TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_CB_COLOR_DISABLE, 0xCC, false, false, 0) ==
         (S_028808_SPECIAL_OP(V_028808_SPECIAL_DISABLE) | S_028808_PER_MRT_BLEND(1) |
          S_028808_ROP3(0xCC)));
   CHECK(terakan_hw_config_draw_terascale_1_cb_color_control_encode(
            TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_CB_COLOR_RESOLVE_BOX, 0xCC, false, false, 0) ==
         (S_028808_SPECIAL_OP(V_028808_SPECIAL_RESOLVE_BOX) | S_028808_PER_MRT_BLEND(1) |
          S_028808_ROP3(0xCC)));

   uint32_t control_packet[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_cb_color_control(control_packet, control) ==
         control_packet + 3);
   CHECK(control_packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(control_packet[1] == (R_028808_CB_COLOR_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(control_packet[2] == control);

   uint32_t const blend_control[2] = {
      S_028804_COLOR_SRCBLEND(V_028804_BLEND_SRC_ALPHA) |
         S_028804_COLOR_DESTBLEND(V_028804_BLEND_ONE_MINUS_SRC_ALPHA) | (UINT32_C(1) << 30),
      S_028804_COLOR_SRCBLEND(V_028804_BLEND_ONE) |
         S_028804_COLOR_DESTBLEND(V_028804_BLEND_ZERO),
   };
   uint32_t blend_packet[4];
   CHECK(terakan_hw_config_draw_terascale_1_write_cb_blend_control(blend_packet, 3, 2,
                                                                   blend_control) ==
         blend_packet + 4);
   CHECK(blend_packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(blend_packet[1] ==
         (R_02878C_CB_BLEND3_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(blend_packet[2] == (blend_control[0] & ~(UINT32_C(1) << 30)));
   CHECK(blend_packet[3] == blend_control[1]);
}

static void
test_cb_shader_control_packet(void)
{
   uint32_t packet[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_cb_shader_control(packet, 0) == packet + 3);
   CHECK(packet[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packet[1] == (R_0287A0_CB_SHADER_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packet[2] == 0x1);

   CHECK(terakan_hw_config_draw_terascale_1_write_cb_shader_control(packet, 3) == packet + 3);
   CHECK(packet[2] == 0x7);
   CHECK(terakan_hw_config_draw_terascale_1_write_cb_shader_control(packet, 8) == packet + 3);
   CHECK(packet[2] == 0xff);
}

int
main(void)
{
   test_spi_ps_interpolation();
   test_spi_ps_rejects_invalid_allocations();
   test_db_depth_view();
   test_db_render_control_override();
   test_db_render_control_override_classic_r700_baseline();
   test_db_render_control_conservative_z();
   test_db_render_control_occlusion_query();
   test_db_shader_control();
   test_db_shader_control_rejects_evergreen_only_fields();
   test_db_depth_size();
   test_db_depth_base_info();
   test_db_depth_encode();
   test_db_depth_encode_multisample_and_rejects_unported_surfaces();
   test_db_depth_remaining_packets();
   test_db_alpha_to_mask();
   test_pa_sc_aa_mask();
   test_pa_sc_aa_config_sample_locs();
   test_pa_sc_mode();
   test_pa_cl_clip_cntl();
   test_pa_su_poly_offset();
   test_sq_pgm_bindings();
   test_gpr_baseline_admission();
   test_draw_constant_packet_is_empty();
   test_absent_vgt_controls();
   test_ring_itemsize_is_begin_atom_only();
   test_absent_ls_bool_const();
   test_absent_cb_immed();
   test_absent_index_buffer_unbind();
   test_zero_count_draw();
   test_draw_index();
   test_cb_color_encode();
   test_cb_color_encode_multisample_metadata();
   test_cb_color_encode_rejects_unported_surfaces();
   test_cb_color_packets();
   test_cb_color_control_and_blend_packets();
   test_cb_shader_control_packet();
   return 0;
}
