/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_H
#define TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_SPI_PS_INPUT_COUNT 32

/* r600_adjust_gprs() may repartition this fixed-size pool when a shader exceeds the family
 * baseline. Terakan doesn't perform that live repartition yet because it also requires the
 * associated WAIT_3D_IDLE ordering to be validated. Until then, reject a draw unless every bound
 * hardware stage fits the exact baseline already written by the begin atom. This is intentionally
 * more conservative than the classic driver, but prevents SQ_PGM_RESOURCES_*.NUM_GPRS from
 * exceeding SQ_GPR_RESOURCE_MGMT*.NUM_*_GPRS, which r600_adjust_gprs() documents as a GPU lockup.
 */
struct terakan_hw_config_draw_terascale_1_gpr_counts {
   uint32_t ps, vs, gs, es;
};

bool terakan_hw_config_draw_terascale_1_gprs_fit_baseline(
   struct terakan_hw_config_draw_terascale_1_gpr_counts const * required,
   struct terakan_hw_config_draw_terascale_1_gpr_counts const * baseline);

/* R600/R700 selects perspective/linear and center/centroid/sample interpolation in every
 * SPI_PS_INPUT_CNTL_n entry. Evergreen removed those selections from the per-input words and
 * introduced SPI_BARYC_CNTL at 0x0286E0 instead. On R600/R700 that address is
 * SPI_FOG_FUNC_SCALE, so an Evergreen payload must never be emitted there.
 *
 * Keep this input generation-neutral. The shader compiler fills it from r600_shader_io, while
 * this translation unit (which includes only r600d.h) owns the exact R700 field packing.
 */
struct terakan_hw_config_draw_terascale_1_spi_ps_input {
   uint32_t semantic;
   uint32_t gpr;
   bool position;
   bool front_face_or_sample_mask;
   bool sample_id;
   bool flat;
   bool centroid;
   bool linear;
   bool point_sprite;
   bool sample;
};

struct terakan_hw_config_draw_terascale_1_spi_ps {
   uint32_t input_count;
   uint32_t input_control[TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_SPI_PS_INPUT_COUNT];
   uint32_t in_control_0;
   uint32_t in_control_1;
   uint32_t input_z;
};

bool terakan_hw_config_draw_terascale_1_spi_ps_encode(
   struct terakan_hw_config_draw_terascale_1_spi_ps_input const * inputs,
   uint32_t input_count, struct terakan_hw_config_draw_terascale_1_spi_ps * spi_out);

/* Write SPI_PS_INPUT_CNTL_n, SPI_PS_IN_CONTROL_0/1 and SPI_INPUT_Z. Deliberately does not write
 * 0x0286E0: it is SPI_FOG_FUNC_SCALE on R600/R700, not SPI_BARYC_CNTL.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_spi_ps(
   uint32_t * packet, struct terakan_hw_config_draw_terascale_1_spi_ps const * spi);

/* R600/R700 has one 8-bit sample mask per pixel in R_028C48_PA_SC_AA_MASK. Classic
 * r600_emit_sample_mask() repeats the API-visible low byte for all four pixels. Evergreen uses
 * R_028C3C for its one-dword form, but that address is R_028C3C_CB_CLRCMP_MSK on R600/R700, so the
 * shared emitter must not be used even though the payload itself has the same repeated shape.
 */
uint32_t terakan_hw_config_draw_terascale_1_pa_sc_aa_mask_encode(uint32_t sample_mask);

uint32_t * terakan_hw_config_draw_terascale_1_write_pa_sc_aa_mask(uint32_t * packet,
                                                                   uint32_t value);

/* RV6xx/RV7xx has one sample-location pattern shared by the four pixels of a 2x2 quad. Original
 * R600 stores the 2x/4x/8x patterns in separate configuration registers; all later TeraScale 1
 * chips use two context registers. The physical device therefore advertises a 1x1 programmable
 * grid on TeraScale 1. The encoder still verifies that its internal [sample][pixel] input was
 * replicated, so a future caller can't silently drop per-pixel differences.
 */
struct terakan_hw_config_draw_terascale_1_pa_sc_aa {
   uint32_t config;
   uint32_t sample_locs[2];
};

bool terakan_hw_config_draw_terascale_1_pa_sc_aa_encode(
   uint32_t sample_count_log2, uint32_t max_sample_dist, bool aa_mask_centroid_determine,
   uint8_t const sample_locs[16][4],
   struct terakan_hw_config_draw_terascale_1_pa_sc_aa * aa_out);

/* Exact packet size and writer for r600_emit_msaa_state()'s split: CHIP_R600 uses one of the
 * PA_SC_AA_SAMPLE_LOCS_{2S,4S,8S_*} configuration-register forms, while RV610 and every newer
 * TeraScale 1 family use the two PA_SC_AA_SAMPLE_LOCS_MCTX context dwords. `is_original_r600`
 * means CHIP_R600 specifically, not the whole R600 gfx-level family.
 */
uint32_t terakan_hw_config_draw_terascale_1_pa_sc_aa_packet_dwords(
   bool is_original_r600, struct terakan_hw_config_draw_terascale_1_pa_sc_aa const * aa);

uint32_t * terakan_hw_config_draw_terascale_1_write_pa_sc_aa(
   uint32_t * packet, bool is_original_r600,
   struct terakan_hw_config_draw_terascale_1_pa_sc_aa const * aa);

/* Evergreen splits rasterizer mode state between PA_SC_MODE_CNTL_0 at 0x028A48 and
 * PA_SC_MODE_CNTL_1 at 0x028A4C. R600/R700 has one differently-shaped PA_SC_MODE_CNTL at 0x028A4C;
 * 0x028A48 is PA_SC_MPASS_PS_CNTL there. The R600 and R700 baselines within that register differ
 * too, so the runtime gfx-level is an explicit semantic input rather than a build-time choice.
 */
struct terakan_hw_config_draw_terascale_1_pa_sc_mode_input {
   bool msaa_enable;
   bool line_stipple_enable;
   bool viewport_scissor_enable;
   bool ps_iter_sample;
   bool is_r700;
   bool is_rv770;
   uint32_t unknown_mode_0_bits;
   uint32_t unknown_mode_1_bits;
};

bool terakan_hw_config_draw_terascale_1_pa_sc_mode_encode(
   struct terakan_hw_config_draw_terascale_1_pa_sc_mode_input const * input,
   uint32_t * mode_out);

uint32_t * terakan_hw_config_draw_terascale_1_write_pa_sc_mode(uint32_t * packet,
                                                                uint32_t value);

/* PA_CL_CLIP_CNTL's DX_RASTERIZATION_KILL bit is R700-only even though r600d.h exposes its
 * position with that caveat. Classic r600_create_rs_state() uses SX_MISC.MULTIPASS for the same
 * Vulkan rasterizer-discard meaning on the R600 gfx level. All other fields Terakan currently
 * produces below are bit-compatible and are validated before being passed through.
 */
struct terakan_hw_config_draw_terascale_1_pa_cl_clip {
   uint32_t clip_cntl;
   uint32_t sx_misc;
};

bool terakan_hw_config_draw_terascale_1_pa_cl_clip_encode(
   uint32_t evergreen_value, bool is_r700,
   struct terakan_hw_config_draw_terascale_1_pa_cl_clip * clip_out);

uint32_t terakan_hw_config_draw_terascale_1_pa_cl_clip_packet_dwords(bool is_r700);

uint32_t * terakan_hw_config_draw_terascale_1_write_pa_cl_clip(
   uint32_t * packet, bool is_r700,
   struct terakan_hw_config_draw_terascale_1_pa_cl_clip const * clip);

/* R700 moves the complete polygon-offset block from Evergreen's 0x028B78..0x028B8C to
 * 0x028DF8..0x028E0C. DB_FMT_CNTL's two fields have identical positions in both headers, while
 * clamp/scale/offset are raw IEEE-754 dwords. Validate the former and isolate all address changes
 * here so the shared Evergreen path stays untouched.
 */
bool terakan_hw_config_draw_terascale_1_pa_su_poly_offset_db_fmt_encode(
   uint32_t evergreen_value, uint32_t * r700_value_out);

uint32_t * terakan_hw_config_draw_terascale_1_write_pa_su_poly_offset_db_fmt(
   uint32_t * packet, uint32_t value);

uint32_t * terakan_hw_config_draw_terascale_1_write_pa_su_poly_offset(
   uint32_t * packet, uint32_t clamp, uint32_t scale, uint32_t offset);

/* The first SQ_PGM_RESOURCES word has compatible NUM_GPRS/STACK_SIZE/DX10_CLAMP/
 * UNCACHED_FIRST_INST/CLAMP_CONSTS positions, but nearby fields and all register addresses differ.
 * Accept only that proven subset; Evergreen RESOURCES_2 rounding/denorm state has no port here and
 * must be zero before these writers are used. `force_uncached_first_inst` is the original-CHIP_R600
 * pixel-shader workaround from r600_update_ps_state(), not an R600-gfx-level default.
 */
bool terakan_hw_config_draw_terascale_1_sq_pgm_resources_encode(
   uint32_t evergreen_resources, bool force_uncached_first_inst,
   uint32_t * r700_resources_out);

/* The classic R600/R700 baseline is emitted by the per-indirect-buffer begin atom. The separate
 * Evergreen draw-constant packet must therefore contain exactly zero dwords on TeraScale 1.
 */
uint32_t terakan_hw_config_draw_terascale_1_constant_packet_dwords(void);

/* Evergreen tessellation-stage controls have no R600/R700 registers. A disabled tracked value is
 * represented by an empty packet; anything else is unsupported rather than reinterpreted.
 */
bool terakan_hw_config_draw_terascale_1_absent_vgt_control_encode(
   uint32_t value, uint32_t * packet_dwords_out);

/* The begin atom clears the real R600/R700 ring-item-size block. Evergreen ring indices and
 * addresses are not reusable; until their users are ported, accept only an all-zero state.
 */
bool terakan_hw_config_draw_terascale_1_ring_itemsize_encode(
   uint32_t const * itemsize_dwords, uint32_t itemsize_count, uint32_t * packet_dwords_out);

/* R600/R700 has PS/VS/GS/ES boolean-constant stages 0..3, but no Evergreen LS stage 4. */
bool terakan_hw_config_draw_terascale_1_absent_ls_bool_const_encode(
   uint32_t value, uint32_t * packet_dwords_out);

/* R600/R700 has no Evergreen CB_IMMEDn_BASE UAV immediate-address block. */
uint32_t terakan_hw_config_draw_terascale_1_cb_immed_packet_dwords(void);

/* R600/R700 direct indexed draws carry their address in DRAW_INDEX. The Evergreen-only
 * INDEX_BUFFER_SIZE packet must therefore not be emitted merely to represent an unbound buffer. */
uint32_t terakan_hw_config_draw_terascale_1_index_buffer_unbind_packet_dwords(void);

/* Diagnostic draw used to distinguish packet/state validation from shader execution. */
uint32_t * terakan_hw_config_draw_terascale_1_write_zero_count_draw(uint32_t * packet);

/* Direct indexed draws on R600/R700 carry the absolute index address in the draw packet. */
uint32_t * terakan_hw_config_draw_terascale_1_write_draw_index(
   uint32_t * packet, uint64_t index_va, uint32_t index_count);

uint32_t * terakan_hw_config_draw_terascale_1_write_sq_pgm_fs(uint32_t * packet,
                                                               uint32_t program_va_shr8);
uint32_t * terakan_hw_config_draw_terascale_1_write_sq_pgm_vs(
   uint32_t * packet, uint32_t program_va_shr8, uint32_t resources);
uint32_t * terakan_hw_config_draw_terascale_1_write_sq_pgm_vs_start(uint32_t * packet,
                                                                      uint32_t program_va_shr8);
uint32_t * terakan_hw_config_draw_terascale_1_write_sq_pgm_vs_resources(uint32_t * packet,
                                                                          uint32_t resources);
uint32_t * terakan_hw_config_draw_terascale_1_write_sq_pgm_ps(
   uint32_t * packet, uint32_t program_va_shr8, uint32_t resources, uint32_t exports);
uint32_t * terakan_hw_config_draw_terascale_1_write_sq_pgm_ps_start(uint32_t * packet,
                                                                      uint32_t program_va_shr8);
uint32_t * terakan_hw_config_draw_terascale_1_write_sq_pgm_ps_resources(
   uint32_t * packet, uint32_t resources, uint32_t exports);
uint32_t * terakan_hw_config_draw_terascale_1_write_sq_pgm_es(
   uint32_t * packet, uint32_t program_va_shr8, uint32_t resources);
uint32_t * terakan_hw_config_draw_terascale_1_write_sq_pgm_gs(
   uint32_t * packet, uint32_t program_va_shr8, uint32_t resources);
uint32_t * terakan_hw_config_draw_terascale_1_write_spi_vs_out_id(
   uint32_t * packet, uint32_t count, uint32_t const * values);

/* Per-draw (as opposed to once-per-command-buffer) TeraScale 1 register emission that genuinely
 * needs its own code, as opposed to the R8xx/R9xx code already working unchanged (see
 * DB_DEPTH_CONTROL/CB_TARGET_MASK in TODO.md) -- either because the register lives at a different
 * offset with the same field layout, or because R600/R700 has no equivalent of an R8xx/R9xx
 * register at all and the driver's own existing behavior for it is trivial enough to port directly.
 *
 * DB_DEPTH_SIZE/DB_DEPTH_BASE/DB_DEPTH_INFO emission (see below) exist here, since the underlying
 * tiling math (ARRAY_MODE, pitch/height in tiles) is now ported and tested
 * (terakan_image_tiling_terascale_1.c, wired into terakan_image_surface_compute_terascale_1() --
 * see TODO.md), but the VALUE COMPUTATION side -- deriving a DB_DEPTH_INFO FORMAT/DB_DEPTH_BASE
 * pair from a Vulkan depth/stencil attachment -- is not written yet, and is a genuinely open
 * research question, not just unstarted work: R600/R700 binds depth and stencil through a single
 * combined surface and base address (R_02800C_DB_DEPTH_BASE, no separate stencil base at all,
 * unlike R8xx/R9xx's four independent read/write Z/stencil base registers), which doesn't map onto
 * Terakan's existing terakan_depth_stencil_descriptor (built around separate z_base/stencil_base/
 * z_info/stencil_info) without figuring out how the packed combined-surface model actually stores
 * stencil data alongside depth on this hardware -- guessing at DB_DEPTH_INFO's FORMAT encoding or
 * the combined surface layout without that research done first is exactly the kind of
 * silent-memory-corruption risk this driver's porting conventions exist to avoid. The two functions
 * below take fully caller-computed values for exactly this reason: they exist so the register SHAPE
 * is ready once that research is done, not because the value-computation problem is solved.
 */

/* PKT3_SET_CONTEXT_REG(DB_DEPTH_VIEW). Field-for-field identical to R8xx/R9xx's own DB_DEPTH_VIEW
 * (SLICE_START/SLICE_MAX, both 11 bits) -- confirmed against evergreend.h directly -- just at a
 * different register offset (R_028004_DB_DEPTH_VIEW here vs R_028008_DB_DEPTH_VIEW there), so the
 * caller's existing R8xx/R9xx value-computation logic for `descriptor->view` needs no TeraScale 1
 * equivalent, only this offset-isolating wrapper.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth_view(uint32_t * packet,
                                                                    uint32_t value);

/* Convert the currently all-zero Evergreen software state to the actual no-query, no-HTILE R700
 * baseline from r600_emit_db_misc_state(). The only extra state accepted is R700 conservative-Z:
 * it originates in the Evergreen-shaped DB_SHADER_CONTROL value, but classic emits it here. R600
 * only admits EXPORT_ANY_Z because the classic driver gates this transition on gfx_level >= R700.
 * Nonzero Evergreen render-control state is rejected because query/HTILE/copy paths are unported.
 * There is no DB_RENDER_OVERRIDE2 equivalent on R600/R700.
 */
bool terakan_hw_config_draw_terascale_1_db_render_control_override_encode(
   uint32_t evergreen_db_render_control, uint32_t evergreen_db_render_override, bool is_r700,
   uint32_t conservative_z_export,
   uint32_t * db_render_control_out, uint32_t * db_render_override_out);

/* Query state is separate from the Evergreen DB_COUNT_CONTROL payload. R700 counts Z passes by
 * setting R700_PERFECT_ZPASS_COUNTS in DB_RENDER_CONTROL and disabling cull suppression in the
 * paired override; R600 has the latter but no R700 perfect-count bit. */
bool terakan_hw_config_draw_terascale_1_db_render_control_override_encode_query(
   uint32_t evergreen_db_render_control, uint32_t evergreen_db_render_override, bool is_r700,
   uint32_t conservative_z_export, bool zpass_query_active,
   uint32_t * db_render_control_out, uint32_t * db_render_override_out);

/* PKT3_SET_CONTEXT_REG_SEQ(DB_RENDER_CONTROL, DB_RENDER_OVERRIDE), 2 dwords. Values must already
 * be R700-shaped, normally produced by the encoder above. This two-register sequence is confirmed
 * against r600_emit_db_misc_state(), not inferred from the adjacent register addresses.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_db_render_control_override(
   uint32_t * packet, uint32_t db_render_control, uint32_t db_render_override);

/* DB_SHADER_CONTROL has a shared address and six shared field positions, but the remainder of the
 * Evergreen payload is not R700 state. Keep the input field-based so this translation unit can
 * encode the R700 register without including evergreend.h. `source_format` is validated but not
 * emitted: classic r600_update_db_shader_control() selects only DUAL_EXPORT_ENABLE on R700, while
 * DB_SOURCE_FORMAT exists only on Evergreen. The other trailing fields are rejected until their
 * semantics are ported. `conservative_z_export` is validated here but emitted in R700
 * DB_RENDER_CONTROL, its actual classic location.
 */
struct terakan_hw_config_draw_terascale_1_db_shader_control_input {
   bool z_export_enable;
   bool stencil_ref_export_enable;
   uint32_t z_order;
   bool kill_enable;
   bool mask_export_enable;
   bool dual_export_enable;
   uint32_t source_format;
   bool exec_on_hier_fail;
   bool exec_on_noop;
   bool alpha_to_mask_disable;
   bool depth_before_shader;
   uint32_t conservative_z_export;
   uint32_t unknown_bits;
};

bool terakan_hw_config_draw_terascale_1_db_shader_control_encode(
   struct terakan_hw_config_draw_terascale_1_db_shader_control_input const * input,
   uint32_t * db_shader_control_out);

uint32_t * terakan_hw_config_draw_terascale_1_write_db_shader_control(uint32_t * packet,
                                                                       uint32_t value);

/* PKT3_SET_CONTEXT_REG(DB_DEPTH_SIZE), 1 dword. R_028000_DB_DEPTH_SIZE has no R8xx/R9xx equivalent
 * at this offset or in this shape: it packs PITCH_TILE_MAX (10 bits) and SLICE_TILE_MAX (20 bits,
 * the whole slice's tile count, not a separate height field) into one register, where R8xx/R9xx
 * splits pitch and height into DB_DEPTH_SIZE and a separate DB_DEPTH_SLICE register -- R600/R700 has
 * no DB_DEPTH_SLICE at all. pitch_tile_max and slice_tile_max are taken as fully caller-computed
 * values (already including the "-1" MAX convention both fields use), not derived here, since this
 * is emission only -- see the file comment above for why the value side isn't written yet.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth_size(uint32_t * packet,
                                                                   uint32_t pitch_tile_max,
                                                                   uint32_t slice_tile_max);

/* PKT3_SET_CONTEXT_REG_SEQ(DB_DEPTH_BASE, DB_DEPTH_INFO), 2 dwords, matching
 * r600_state.c's own `radeon_set_context_reg_seq(cs, R_02800C_DB_DEPTH_BASE, 2)` sequencing (see
 * r600_db_state in the reference). db_depth_base is a plain shifted GPU address with no named fields
 * (unlike R8xx/R9xx's four separate Z/stencil read/write base registers -- see the file comment
 * above), and db_depth_info is the full DB_DEPTH_INFO value (FORMAT/READ_SIZE/ARRAY_MODE/
 * TILE_SURFACE_ENABLE/TILE_COMPACT/ZRANGE_PRECISION), both fully caller-computed.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth_base_info(uint32_t * packet,
                                                                        uint32_t db_depth_base,
                                                                        uint32_t db_depth_info);

enum terakan_hw_config_draw_terascale_1_db_depth_format {
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_INVALID,
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_16,
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_24,
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_32_FLOAT,
};

struct terakan_hw_config_draw_terascale_1_db_depth_input {
   uint32_t base;
   uint32_t pitch_tile_max;
   uint32_t height_tile_max;
   uint32_t slice_tile_max;
   uint32_t slice_start;
   uint32_t slice_max;
   enum terakan_hw_config_draw_terascale_1_db_depth_format format;
   uint32_t array_mode;
   bool zrange_precision;
   uint32_t samples_log2;
};

struct terakan_hw_config_draw_terascale_1_db_depth {
   uint32_t base;
   uint32_t size;
   uint32_t view;
   uint32_t info;
   uint32_t prefetch_limit;
};

/* Encode the depth-only R600/R700 subset of r600_init_depth_surface(). MSAA count is consumed by
 * PA_SC_AA_CONFIG, not DB_DEPTH_INFO, so 2x/4x/8x use the same DB register encoding as 1x once
 * the surface layout supplies the larger pitch/slice allocation. Packed depth/stencil formats and
 * HTILE remain deliberately rejected; the caller must unbind DB when this returns false.
 */
bool terakan_hw_config_draw_terascale_1_db_depth_encode(
   struct terakan_hw_config_draw_terascale_1_db_depth_input const * input,
   struct terakan_hw_config_draw_terascale_1_db_depth * depth_out);

/* Writes SIZE, VIEW, BASE/INFO and PREFETCH_LIMIT in the order used by
 * r600_emit_framebuffer_state(). The relocated BASE payload is dword 8 of the 13-dword stream.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth(
   uint32_t * packet, struct terakan_hw_config_draw_terascale_1_db_depth const * depth);

uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth_prefetch_limit(uint32_t * packet,
                                                                             uint32_t value);

uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth_unbound(uint32_t * packet);

/* R700 DB_ALPHA_TO_MASK has the same ENABLE/OFFSET field positions as Evergreen, but lives at
 * R_028D44 rather than R_028B70. Classic r600_create_blend_state_mode() always uses the regular
 * 2,2,2,2 offsets, so take only the API-visible enable here rather than carrying an
 * Evergreen-shaped register value across generations.
 */
uint32_t terakan_hw_config_draw_terascale_1_db_alpha_to_mask_encode(bool enable);

uint32_t * terakan_hw_config_draw_terascale_1_write_db_alpha_to_mask(uint32_t * packet,
                                                                      uint32_t value);

/* R600/R700 CB_COLOR is not the shorter form of the R8xx/R9xx descriptor. Even though the
 * FORMAT/ARRAY_MODE/NUMBER_TYPE values are numerically shared, INFO diverges after bit 14, and the
 * other fields live in seven independently-strided register arrays rather than one per-target
 * register block. Keep the input field-based so this translation unit never needs evergreend.h.
 *
 * `r600_init_color_surface()` uses an FMASK/CMASK pair for a multisampled target, encoded through
 * CB_COLORn_FRAG, CB_COLORn_TILE and CB_COLORn_MASK. Keep the three translated values explicit:
 * their Evergreen source registers have different names and positions, but the R600/R700 values
 * are not optional aliases of BASE. UAV/compute use is still not ready on TeraScale 1.
 */
struct terakan_hw_config_draw_terascale_1_cb_color_input {
   uint32_t base;
   uint32_t pitch_tile_max;
   uint32_t slice_tile_max;
   uint32_t slice_start;
   uint32_t slice_max;
   uint32_t endian;
   uint32_t format;
   uint32_t array_mode;
   uint32_t number_type;
   uint32_t comp_swap;
   bool blend_clamp;
   bool blend_bypass;
   bool simple_float;
   uint32_t source_format;
   bool is_uav;
   bool is_multisampled;
   bool metadata_enabled;
   uint32_t fmask;
   uint32_t cmask;
   uint32_t cmask_tile_max;
   uint32_t fmask_tile_max;
};

struct terakan_hw_config_draw_terascale_1_cb_color {
   uint32_t base;
   uint32_t size;
   uint32_t view;
   uint32_t info;
   uint32_t fmask;
   uint32_t cmask;
   uint32_t mask;
};

bool terakan_hw_config_draw_terascale_1_cb_color_encode(
   struct terakan_hw_config_draw_terascale_1_cb_color_input const * input,
   struct terakan_hw_config_draw_terascale_1_cb_color * color_out);

/* Writes INFO, BASE, FRAG (FMASK), TILE (CMASK), SIZE, VIEW and MASK, in that order, as seven
 * one-register SET_CONTEXT_REG packets. The base-bearing payload dwords are at indices 5, 8 and 11
 * in the returned 21-dword stream, which the caller uses for BO relocations.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_cb_color(
   uint32_t * packet, uint32_t color_index,
   struct terakan_hw_config_draw_terascale_1_cb_color const * color);

/* Radeon DRM consumes a relocation NOP directly after each CB address register. The ordinary
 * writer deliberately does not include those NOPs because Radeon Software WDDM carries the same
 * relocations out of band. `bo_reference` is converted to the dword offset expected by DRM. */
uint32_t * terakan_hw_config_draw_terascale_1_write_cb_color_drm_relocations(
   uint32_t * packet, uint32_t color_index,
   struct terakan_hw_config_draw_terascale_1_cb_color const * color, uint32_t bo_reference);

/* Diagnostic prefix writer. `field_count` follows INFO, BASE, FRAG, TILE, SIZE, VIEW, MASK and is
 * deliberately exposed to the CPU packet oracle used during RV710 bring-up. */
uint32_t * terakan_hw_config_draw_terascale_1_write_cb_color_drm_relocations_prefix(
   uint32_t * packet, uint32_t color_index,
   struct terakan_hw_config_draw_terascale_1_cb_color const * color, uint32_t bo_reference,
   uint32_t field_count);

uint32_t * terakan_hw_config_draw_terascale_1_write_cb_color_unbound(uint32_t * packet,
                                                                      uint32_t color_index,
                                                                      uint32_t source_format);

/* R600/R700 selects enabled render targets in CB_SHADER_CONTROL, a register absent from the
 * Evergreen framebuffer path. Classic r600_emit_framebuffer_state() enables all slots through the
 * highest bound color slot, and keeps RT0 enabled even with no color target for alpha testing. */
uint32_t * terakan_hw_config_draw_terascale_1_write_cb_shader_control(
   uint32_t * packet, uint32_t bound_color_count);

enum terakan_hw_config_draw_terascale_1_cb_color_operation {
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_CB_COLOR_DISABLE,
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_CB_COLOR_NORMAL,
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_CB_COLOR_RESOLVE_BOX,
};

/* R700 moves blend enables out of CB_BLENDn_CONTROL bit 30 and into
 * CB_COLOR_CONTROL.TARGET_BLEND_ENABLE. Its bits 4-6 are SPECIAL_OP, not Evergreen MODE, despite
 * the shared register address. Keep the operation neutral at this interface so evergreend.h values
 * can never accidentally reach the R700 packet.
 */
uint32_t terakan_hw_config_draw_terascale_1_cb_color_control_encode(
   enum terakan_hw_config_draw_terascale_1_cb_color_operation operation, uint32_t rop3,
   bool degamma_enable, bool multiwrite_enable, uint32_t target_blend_enable);

uint32_t * terakan_hw_config_draw_terascale_1_write_cb_color_control(uint32_t * packet,
                                                                      uint32_t value);

/* The coefficient/equation fields are identical between R700's CB_BLENDn_CONTROL and Evergreen's
 * CB_BLENDn_CONTROL, but R700 has no BLEND_CONTROL_ENABLE bit 30. The caller supplies that bit
 * separately through CB_COLOR_CONTROL.TARGET_BLEND_ENABLE; this writer strips it from every
 * register payload.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_cb_blend_control(
   uint32_t * packet, uint32_t first_color, uint32_t color_count,
   uint32_t const * blend_control);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_H */
