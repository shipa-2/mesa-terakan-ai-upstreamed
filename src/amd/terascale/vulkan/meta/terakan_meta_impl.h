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

#ifndef TERAKAN_META_IMPL_H
#define TERAKAN_META_IMPL_H

/* Most meta operations, not only `terakan_meta_impl.h` itself, need these Terakan headers, as well
 * as `evergreend.h` and the ISA headers (plus `stdint.h` and `stdbool.h` for shader code, and
 * `stddef.h` for NULL checks when emitting packets), so by including `terakan_meta_impl.h`, meta
 * operation code can assume that these headers are also included.
 */

#include "terakan_bo.h"
#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_format.h"
#include "terakan_image.h"
#include "terakan_meta.h"
#include "terakan_screen_rect.h"
#include "terakan_shader.h"

#include "gallium/drivers/r600/eg_sq.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600_opcodes.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/format/u_formats.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

/* Note that there are various differences from the other Vulkan drivers in Mesa in how Terakan
 * implements meta passes.
 *
 * The common Mesa Vulkan meta passes aren't used, primarily because of the more limited compute
 * capabilities of TeraScale. Most importantly, according to the R800 AddrLib, "Tex2D UAV on cypress
 * will fail/hang if tile mode is linear", so transfers to images must be done via an RTV rather
 * than a UAV.
 *
 * Also, shaders are written directly in the hardware microcode rather than NIR. There's no need for
 * compiling shaders from a single representation for many ISA versions as the driver targets only 2
 * (R8xx VLIW5 and R9xx VLIW4), and the shaders perform mostly simple operations, so using NIR
 * largely would not offer significant advantages. On the other hand, using NIR and the backend
 * shader compiler would require very careful selection of instructions and lowerings (some may be
 * mandatory, while some may break compilation), as well as continuously making sure that the meta
 * shaders (and any special case logic in the compilation infrastructure potentially added for them,
 * such as using special binding registers to avoid interfering with the bindings set by the
 * application) don't end up broken when changes are made to the backend compiler or NIR itself
 * (including for the needs of other, unrelated drivers). So the meta shaders are kept isolated and
 * stable with respect to the compiler infrastructure changes, and any work related to NIR usage in
 * Terakan can focus purely on supporting application shaders.
 */

#define TERAKAN_META_SQ_PGM_RESOURCES_COMMON S_028844_DX10_CLAMP(1)
#define TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON                                                     \
   (S_028848_SINGLE_ROUND(V_SQ_ROUND_NEAREST_EVEN) | S_028848_DOUBLE_ROUND(V_SQ_ROUND_NEAREST_EVEN))

extern struct terakan_meta_shader const terakan_meta_dummy_nan_vs;
extern struct terakan_meta_shader const terakan_meta_dummy_opaque_ps;
extern struct terakan_meta_shader const terakan_meta_position_from_index_vs;
extern struct terakan_meta_shader const terakan_meta_position_and_layer_from_index_vs;
extern struct terakan_meta_shader const terakan_meta_clear_depth_vs;
extern struct terakan_meta_shader const terakan_meta_clear_color_ps;
extern struct terakan_meta_shader const terakan_meta_copy_buffer_to_image_ps;
extern struct terakan_meta_shader const terakan_meta_copy_image_to_buffer_ps;
extern struct terakan_meta_shader const terakan_meta_copy_image_ps;
extern struct terakan_meta_shader const terakan_meta_blit_image_ps;
extern struct terakan_meta_shader const terakan_meta_copy_expand_3x_ps;
extern struct terakan_meta_shader const terakan_meta_query_accum_zpass_1_rb_vs;
extern struct terakan_meta_shader const terakan_meta_query_accum_zpass_2_rb_vs;
extern struct terakan_meta_shader const terakan_meta_query_accum_zpass_4_rb_vs;
extern struct terakan_meta_shader const terakan_meta_query_accum_zpass_8_rb_vs;
extern struct terakan_meta_shader const terakan_meta_query_accum_pipelinestat_vs;
extern struct terakan_meta_shader const terakan_meta_query_accum_streamoutstats_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_zpass_32_bit_1_rb_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_zpass_32_bit_2_rb_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_zpass_32_bit_4_rb_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_zpass_32_bit_8_rb_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_zpass_64_bit_1_rb_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_zpass_64_bit_2_rb_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_zpass_64_bit_4_rb_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_zpass_64_bit_8_rb_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_pipelinestat_32_bit_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_pipelinestat_64_bit_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_timestamp_32_bit_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_timestamp_64_bit_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_streamoutstats_32_bit_vs;
extern struct terakan_meta_shader const terakan_meta_query_copy_streamoutstats_64_bit_vs;

/* `DB_SHADER_CONTROL` for use when the pixel shader exports to memory, with otherwise identity
 * parameters.
 */
#define TERAKAN_META_CONFIG_DRAW_DB_SHADER_CONTROL_PS_MEMORY_EXPORT                                \
   ((TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY & C_02880C_Z_ORDER) |                               \
    S_02880C_Z_ORDER(V_02880C_LATE_Z) | S_02880C_EXEC_ON_HIER_FAIL(true) |                         \
    S_02880C_EXEC_ON_NOOP(true))

static inline void
terakan_meta_config_draw_set_vgt_num_instances(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const value)
{
   assert(value != 0 && "The hardware interprets 0 instances as 1, check if the 0 is intentional");
   terakan_hw_config_shared_draw_set_vgt_num_instances(&command_writer->hw_config_shared, value);
}

static inline void
terakan_meta_config_draw_set_vgt_index_offset(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const value)
{
   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_VGT_INDEX_OFFSET);
   terakan_hw_config_draw_set_vgt_index_offset(&command_writer->hw_config_draw, value);
}

void terakan_meta_config_draw_set_sq_pgm_vs(struct terakan_gfx_command_writer * command_writer,
                                            enum terakan_meta_shader_index shader_index);
void terakan_meta_config_draw_set_sq_pgm_ps(struct terakan_gfx_command_writer * command_writer,
                                            enum terakan_meta_shader_index shader_index);

static inline void
terakan_meta_config_draw_set_kcache_push_constant_buffer_vs(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const size_lines,
   struct terakan_bo const * const bo, uint32_t const va_lines)
{
   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_VERTEX_BIT;
   terakan_hw_config_sqk_set_kcache_vs(&command_writer->hw_config_sqk,
                                       TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS, size_lines, bo,
                                       va_lines);
}

static inline void
terakan_meta_config_draw_set_kcache_push_constant_buffer_ps(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const size_lines,
   struct terakan_bo const * const bo, uint32_t const va_lines)
{
   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_FRAGMENT_BIT;
   terakan_hw_config_sqk_set_kcache_fs(&command_writer->hw_config_sqk,
                                       TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS, size_lines, bo,
                                       va_lines);
}

/* Helper function. */
static inline bool
terakan_meta_config_draw_set_kcache_push_constants(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const constants_size,
   void const * const constants, bool const set_for_vs, bool const set_for_ps)
{
   struct terakan_bo const * constants_bo = NULL;
   uint32_t constants_va_lines;
   void * const constants_mapping = terakan_push_buffer_allocate_kcache(
      command_writer->base.command_buffer, constants_size, &constants_bo, &constants_va_lines);
   if (unlikely(constants_mapping == NULL)) {
      return false;
   }
   memcpy(constants_mapping, constants, constants_size);
   uint32_t const constants_size_lines = DIV_ROUND_UP(constants_size, TERAKAN_KCACHE_HW_LINE_BYTES);
   if (set_for_vs) {
      terakan_meta_config_draw_set_kcache_push_constant_buffer_vs(
         command_writer, constants_size_lines, constants_bo, constants_va_lines);
   }
   if (set_for_ps) {
      terakan_meta_config_draw_set_kcache_push_constant_buffer_ps(
         command_writer, constants_size_lines, constants_bo, constants_va_lines);
   }
   return true;
}

static inline void
terakan_meta_config_draw_set_db_depth_stencil_buffer(
   struct terakan_gfx_command_writer * const command_writer, struct terakan_bo const * const bo,
   struct terakan_depth_stencil_descriptor const * const descriptor)
{
   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_BUFFER);
   terakan_hw_config_draw_set_db_depth_stencil_buffer(&command_writer->hw_config_draw, bo,
                                                      descriptor);
}

static inline void
terakan_meta_config_draw_set_db_stencilrefmask(
   struct terakan_gfx_command_writer * const command_writer, bool const back, uint32_t const value)
{
   terakan_app_config_draw_set_pending(
      &command_writer->app_config_draw,
      TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_CONTROL_REF_MASK);
   terakan_hw_config_draw_set_db_stencilrefmask(&command_writer->hw_config_draw, back, value);
}

static inline void
terakan_meta_config_draw_set_db_depth_control(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const value)
{
   terakan_app_config_draw_set_pending(
      &command_writer->app_config_draw,
      TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_CONTROL_REF_MASK);
   terakan_hw_config_draw_set_db_depth_control(&command_writer->hw_config_draw, value);
}

/* Note that `DUAL_EXPORT_ENABLE` in `DB_SHADER_CONTROL` depends on RTV export formats, so use
 * `terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control` rather than this function directly
 * when writing to RTVs.
 */
static inline void
terakan_meta_config_draw_set_db_shader_control(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const value)
{
   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_DB_SHADER_CONTROL);
   terakan_hw_config_draw_set_db_shader_control(&command_writer->hw_config_draw, value);
}

static inline void
terakan_meta_config_draw_set_cb_color_control_for_mode(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const mode)
{
   terakan_app_config_draw_set_pending(&command_writer->app_config_draw,
                                       TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_CONTROL);
   terakan_hw_config_draw_set_cb_color_control(
      &command_writer->hw_config_draw,
      S_028808_MODE(mode) | S_028808_ROP3(TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_COPY));
}

/* Sets (even if null, for #MemoryIntegrity reasons) all the used RTVs enabled in `CB_TARGET_MASK`,
 * the `CB_TARGET_MASK` itself, and `DB_SHADER_CONTROL`, because `DUAL_EXPORT_ENABLE` in
 * `DB_SHADER_CONTROL` depends on RTV formats.
 * Also disables blending for the bound targets.
 * The RTV indices in the arrays are `CB_TARGET_MASK` bit indices divided by 4.
 * The BO and color target descriptor arrays must be provided if any RTV is enabled in
 * `CB_TARGET_MASK`, but the meta array may be NULL.
 */
void terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control(
   struct terakan_gfx_command_writer * command_writer, uint32_t cb_target_mask,
   struct terakan_bo const * const * rtvs_bo, struct terakan_color_descriptor const * rtvs_color,
   struct terakan_color_meta_descriptor const * rtvs_meta_opt, uint32_t db_shader_control);

void terakan_meta_config_draw_set_cb_uav(struct terakan_gfx_command_writer * command_writer,
                                         unsigned uav_index, struct terakan_bo const * bo,
                                         struct terakan_color_descriptor const * color);

enum terakan_meta_config_draw_begin_cb_mode {
   /* `CB_DISABLE` with `TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY`.
    * Must be 0 for the zero initialization fallback.
    */
   TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_DISABLE = 0,
   /* Don't configure `DB_SHADER_CONTROL`, `CB_TARGET_MASK` and `CB_COLOR_CONTROL` when beginning
    * meta drawing.
    */
   TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_DYNAMIC,
   /* `CB_NORMAL` with `TERAKAN_META_CONFIG_DRAW_DB_SHADER_CONTROL_PS_MEMORY_EXPORT`.
    * Also, if rasterization is used, `CB_TARGET_MASK` will be set to 0.
    * Needed even for vertex shader UAVs, even with rasterization disabled.
    */
   TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_UAV_ONLY,
   /* `CB_NORMAL`, with dynamic `DB_SHADER_CONTROL` since it depends on the RTV export formats. */
   TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_WITH_RTV_AND_DYNAMIC_DB_SHADER_CONTROL,
};

/* The structure is expected to be zero-initialized, with needed fields set explicitly. */
struct terakan_meta_config_draw_begin_options {
   uint8_t vgt_primitive_type;

   /* If enabled, `VGT_INDEX_OFFSET` must be configured by the caller.
    * Otherwise, vertex index offset will be set to 0.
    */
   bool vgt_index_offset_explicit : 1;

   enum terakan_meta_config_draw_begin_cb_mode cb_and_db_shader_control_mode;

   struct {
      /* The scissor of the first viewport and the pixel shader must be set by the caller if
       * rasterization is used (primitives are rasterized rather than discarded after the vertex
       * shader). The viewport transform isn't used by meta draws.
       * Disabling rasterization enables `DX_RASTERIZATION_KILL` in `PA_CL_CLIP_CNTL`.
       */
      bool enable : 1;

      /* The rest of the `rasterization` structure options are used only when `enable` is true,
       * otherwise these are ignored.
       */

      /* TODO(Triang3l): Multisampling (not needed when copying to a buffer, for instance). */

      /* If enabled, `DB_DEPTH_CONTROL` and, if needed, `DB_DEPTH_STENCIL_BUFFER` must be configured
       * by the caller.
       */
      bool db_explicit : 1;

      unsigned char msaa_num_samples_log2 : 3;
      unsigned char msaa_num_anchor_samples_log2 : 3;
   } rasterization;
};

/* Note that if both compute and graphics meta operations need to be done,
 * `terakan_meta_config_draw_begin` must be called when switching from compute to graphics.
 */
void
terakan_meta_config_draw_begin(struct terakan_gfx_command_writer * command_writer,
                               struct terakan_meta_config_draw_begin_options const * begin_options);

static inline void
terakan_meta_before_draw(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_gfx_command_writer_before_hw_draw(command_writer);
}

static inline void
terakan_meta_draw_auto(struct terakan_gfx_command_writer * const command_writer,
                       uint32_t const vertex_count, uint32_t const instance_count)
{
   assert(vertex_count != 0);

   terakan_meta_config_draw_set_vgt_num_instances(command_writer, instance_count);

   terakan_meta_before_draw(command_writer);
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 3);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_DRAW_INDEX_AUTO, 3 - 2, 0);
   *packet++ = vertex_count;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX);
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

/* Returns a pointer to the index buffer memory (with host endianness), which may be write-combined,
 * or NULL if failed to allocate the index buffer or to emit the draw for any other reason.
 * Use only for primitive counts small enough for a command emission (around 64, being the maximum
 * wave size, is recommended).
 */
uint32_t *
terakan_meta_draw_immediate_32_bit_indexed(struct terakan_gfx_command_writer * command_writer,
                                           uint32_t index_count, uint32_t instance_count);

/* Packs the rectangle coordinates into 32-bit vertex indices for a 3-vertex
 * `V_008958_DI_PT_RECTLIST` primitive for use with vertex shaders like
 * `TERAKAN_META_SHADER_POSITION_FROM_INDEX_VS`.
 */
static inline void
terakan_meta_screen_rect_to_3_vertex_rect_indices(struct terakan_screen_rect const rect,
                                                  uint32_t * const indices_out)
{
   indices_out[0] = rect.bounds[0][0] | ((uint32_t)rect.bounds[0][1] << 16);
   indices_out[1] = rect.bounds[0][0] | ((uint32_t)rect.bounds[1][1] << 16);
   indices_out[2] = rect.bounds[1][0] | ((uint32_t)rect.bounds[0][1] << 16);
}

static inline bool
terakan_meta_draw_rect(struct terakan_gfx_command_writer * command_writer,
                       struct terakan_screen_rect const rect, uint32_t instance_count)
{
   uint32_t * const vertices =
      terakan_meta_draw_immediate_32_bit_indexed(command_writer, 3, instance_count);
   if (unlikely(vertices == NULL)) {
      return false;
   }
   terakan_meta_screen_rect_to_3_vertex_rect_indices(rect, vertices);
   return true;
}

/* Callers are allowed, for simplicity, to make assumptions that the returned format is one of the
 * exact formats listed here, and hardcode certain descriptor fields based on that.
 */
static inline struct terascale_format_info
terakan_meta_transfer_image_block_format_info(unsigned const bytes_per_block)
{
   enum pipe_format format = PIPE_FORMAT_R8_UNORM;
   /* Note that writing to 3x-expanded images must be done via a buffer UAV rather than an RTV or an
    * image UAV, but an image resource may still be used for reading whole texels, and also this
    * function may be called by common helpers even for 3x-expanded format writes.
    */
   switch (bytes_per_block) {
   /* 64bpp `4C_16BPC` export. */
   case 1:
      format = PIPE_FORMAT_R8_UNORM;
      break;
   case 2:
      format = PIPE_FORMAT_R8G8_UNORM;
      break;
   case 4:
      format = PIPE_FORMAT_R8G8B8A8_UNORM;
      break;
   /* 64bpp `2C_32BPC_GR` export on R9xx, 128bpp `4C_32BPC` prior to it. */
   case 8:
      format = PIPE_FORMAT_R32G32_UINT;
      break;
   /* 128bpp `4C_32BPC` export. */
   case 16:
      format = PIPE_FORMAT_R32G32B32A32_UINT;
      break;
   /* Color export is not applicable. */
   case 3:
      format = PIPE_FORMAT_R8G8B8_UNORM;
      break;
   case 6:
      format = PIPE_FORMAT_R16G16B16_UINT;
      break;
   case 12:
      format = PIPE_FORMAT_R32G32B32_UINT;
      break;
   default:
      assert(!"Unsupported number of bytes per block");
   }
   return terascale_format_info_r8xx[format];
}

struct terakan_meta_copy_buffer_image_region_image {
   uint32_t buffer_y_pitch_blocks;
   uint64_t buffer_z_pitch_blocks;
   struct terakan_image_descriptor_create_info image_descriptor_create_info;
   struct terakan_screen_rect rect_blocks;
};

static inline uint64_t
terakan_meta_copy_buffer_image_region_image_buffer_slice_extent_blocks(
   struct terakan_meta_copy_buffer_image_region_image const * const region_image)
{
   return (region_image->rect_blocks.bounds[1][0] - region_image->rect_blocks.bounds[0][0]) +
          (region_image->rect_blocks.bounds[1][1] - region_image->rect_blocks.bounds[0][1] - 1u) *
             (uint64_t)region_image->buffer_y_pitch_blocks;
}

/* Returns whether the output structure is valid as a result, and the rectangle is non-empty.
 * If not, its contents are undefined.
 */
bool terakan_meta_copy_buffer_image_translate_region_image(
   struct terakan_image const * image, VkBufferImageCopy2 const * region,
   struct terakan_meta_copy_buffer_image_region_image * region_image_out);

/* Note that an image slice is always within `2 ^ (TERAKAN_IMAGE_MAX_WIDTH_HEIGHT_LOG2 * 2)` texels,
 * or 2^30 surfels, or 3/4 * 2^32 bytes, for 3x-expanded formats, so it's always fine to create a
 * buffer descriptor for the entire slice. This may, in particular, simplify configuring an UAV
 * descriptor for the destination image by making its base address point to the beginning of slice,
 * as the base address of an image slice is already aligned to the pipe interleave.
 */

/* Creates a buffer resource descriptor (without the actual memory range) for reading the full texel
 * of a 3x-expanded image accepting the index in surfels.
 */
static inline struct terakan_resource_descriptor
terakan_meta_transfer_expand_3x_resource(unsigned const bytes_per_surfel)
{
   enum terascale_format_index data_format;
   switch (bytes_per_surfel) {
   case 1:
      data_format = TERASCALE_FORMAT_INDEX_8_8_8;
      break;
   case 2:
      data_format = TERASCALE_FORMAT_INDEX_16_16_16;
      break;
   case 4:
      data_format = TERASCALE_FORMAT_INDEX_32_32_32;
      break;
   default:
      assert(!"Unsupported 3x-expanded format surfel byte count");
      data_format = TERASCALE_FORMAT_INDEX_INVALID;
   }

   return (struct terakan_resource_descriptor){
      .resource = {
         [2] = S_030008_STRIDE(bytes_per_surfel) | S_030008_DATA_FORMAT(data_format) |
               S_030008_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT),
         [3] = S_03000C_DST_SEL_X(TERASCALE_SWIZZLE_X) | S_03000C_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
               S_03000C_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_03000C_DST_SEL_W(TERASCALE_SWIZZLE_1),
         [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
         [TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_SHADER_READ_BUFFER,
      }};
}

/* Creates a buffer resource descriptor (without the actual memory range) for accessing one surfel
 * of a 3x-expanded image.
 */
static inline struct terakan_color_descriptor
terakan_meta_transfer_expand_3x_uav(unsigned const bytes_per_surfel)
{
   enum terascale_format_index format;
   switch (bytes_per_surfel) {
   case 1:
      format = TERASCALE_FORMAT_INDEX_8;
      break;
   case 2:
      format = TERASCALE_FORMAT_INDEX_16;
      break;
   case 4:
      format = TERASCALE_FORMAT_INDEX_32;
      break;
   default:
      assert(!"Unsupported 3x-expanded format surfel byte count");
      format = TERASCALE_FORMAT_INDEX_INVALID;
   }
   struct terakan_color_descriptor descriptor = {};
   descriptor.info = S_028C70_FORMAT(format) |
                     S_028C70_NUMBER_TYPE(TERASCALE_FORMAT_NUMBER_TYPE_UINT) |
                     TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_INFO_CONST_FIELDS;
   descriptor.attrib = TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_ATTRIB;
   return descriptor;
}

void terakan_meta_copy_expand_3x_buffer_to_image(
   struct terakan_gfx_command_writer * command_writer,
   VkCopyBufferToImageInfo2 const * copy_buffer_to_image_info);
void terakan_meta_copy_expand_3x_image_to_buffer(
   struct terakan_gfx_command_writer * command_writer,
   VkCopyImageToBufferInfo2 const * copy_image_to_buffer_info);
void terakan_meta_copy_expand_3x_image(struct terakan_gfx_command_writer * command_writer,
                                       VkCopyImageInfo2 const * copy_image_info);

#endif /* TERAKAN_META_IMPL_H */
