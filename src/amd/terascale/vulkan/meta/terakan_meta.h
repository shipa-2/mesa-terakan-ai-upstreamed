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

#ifndef TERAKAN_META_H
#define TERAKAN_META_H

/* Most meta operations, not only terakan_meta.h itself, need these Terakan headers, as well as
 * evergreend.h and the ISA headers (plus stdint.h and stdbool.h for shader code, and stddef.h for
 * NULL checks when emitting packets), so by including terakan_meta.h, meta operation code can
 * assume that these headers are also included.
 */

#include "terakan_bo.h"
#include "terakan_shader.h"

#include "gallium/drivers/r600/evergreend.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_meta_shader_description {
   uint32_t const * program;
   size_t program_size_bytes;
   struct terakan_shader_static static_registers;
};

struct terakan_meta_shader {
   struct terakan_meta_shader_description r8xx;
   struct terakan_meta_shader_description r9xx;

   uint16_t kcache_used;
   /* Whether `TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META` is used. */
   bool primary_meta_resource_used;
   /* Whether `TERAKAN_RESOURCE_RANGE_NON_PIXEL_STAGE_SPECIFIC` is used.
    * Must not be used in pixel shaders.
    */
   bool non_pixel_stage_specific_resource_used;
};

enum terakan_meta_shader_index {
   /* Exports NaN position, expecting the primitive to be discarded.
    * Used by `terakan_hw_config_draw` as the replacement for a NULL vertex shader.
    */
   TERAKAN_META_SHADER_DUMMY_NAN_VS,
   /* Exports (0, 0, 0, 1) to RTV 0 (all pixel shaders must perform at least one export).
    * For alpha to coverage, fragments are considered fully opaque.
    * Used by `terakan_hw_config_draw` as the replacement for a NULL pixel shader.
    */
   TERAKAN_META_SHADER_DUMMY_OPAQUE_PS,

   /* Vertex index unpacked as X16Y16 into the position, Z = 0, W = 1.
    * Exports the instance ID as an integer in all components of the first parameter.
    */
   TERAKAN_META_SHADER_POSITION_FROM_INDEX_VS,
   /* Vertex index unpacked as X16Y16 into the position, Z = 0, W = 1, instance ID into the array
    * layer.
    * Exports the instance ID as an integer in all components of the first parameter.
    */
   TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS,

   TERAKAN_META_SHADER_CLEAR_DEPTH_VS,
   TERAKAN_META_SHADER_CLEAR_COLOR_PS,

   TERAKAN_META_SHADER_COPY_BUFFER_TO_IMAGE_PS,
   TERAKAN_META_SHADER_COPY_IMAGE_TO_BUFFER_PS,
   TERAKAN_META_SHADER_COPY_IMAGE_PS,
   TERAKAN_META_SHADER_BLIT_IMAGE_PS,
   TERAKAN_META_SHADER_COPY_EXPAND_3X_PS,

   /* Z pass query accumulation shader indices for each render backend count must be consecutive. */
   TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_1_RB_VS,
   TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_2_RB_VS,
   TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_4_RB_VS,
   TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_8_RB_VS,
   TERAKAN_META_SHADER_QUERY_ACCUM_PIPELINESTAT_VS,
   TERAKAN_META_SHADER_QUERY_ACCUM_STREAMOUTSTATS_VS,

   /* Z pass query copy shader indices for each render backend count must be consecutive. */
   TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_1_RB_VS,
   TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_2_RB_VS,
   TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_4_RB_VS,
   TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_8_RB_VS,
   TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_1_RB_VS,
   TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_2_RB_VS,
   TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_4_RB_VS,
   TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_8_RB_VS,
   TERAKAN_META_SHADER_QUERY_COPY_PIPELINESTAT_32_BIT_VS,
   TERAKAN_META_SHADER_QUERY_COPY_PIPELINESTAT_64_BIT_VS,
   TERAKAN_META_SHADER_QUERY_COPY_TIMESTAMP_32_BIT_VS,
   TERAKAN_META_SHADER_QUERY_COPY_TIMESTAMP_64_BIT_VS,
   TERAKAN_META_SHADER_QUERY_COPY_STREAMOUTSTATS_32_BIT_VS,
   TERAKAN_META_SHADER_QUERY_COPY_STREAMOUTSTATS_64_BIT_VS,

   TERAKAN_META_SHADER_COUNT,
};

extern struct terakan_meta_shader const * const terakan_meta_shaders[TERAKAN_META_SHADER_COUNT];

struct terakan_gfx_command_writer;

#define TERAKAN_META_QUERY_ACCUM_UAV_CP_COHER_CNTL                                                 \
   (S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_SMX_ACTION_ENA(1))

/* Returns `dst_uav_dwords` for `terakan_meta_query_accum`. */
unsigned terakan_meta_query_accum_begin(struct terakan_gfx_command_writer * command_writer,
                                        VkQueryType query_type);

struct terakan_command_buffer_indirect_buffer_query_sample;

/* `ib_end_sample` and `ib_begin_sample` will be read via the kcache.
 * `query_accumulator_bo` of the device will be used as the accumulator source via the kcache.
 */
void terakan_meta_query_accum(
   struct terakan_gfx_command_writer * command_writer,
   struct terakan_command_buffer_indirect_buffer_query_sample const * ib_end_sample,
   struct terakan_command_buffer_indirect_buffer_query_sample const * ib_begin_sample,
   struct terakan_bo const * dst_uav_bo, uint64_t dst_uav_va, unsigned dst_uav_dwords);

/* Initializes the offset constants for the pipeline statistics query result copy shaders. */
void terakan_meta_query_copy_init_offsets(VkQueryPipelineStatisticFlags flags,
                                          int8_t * offsets_32_bit_out, int8_t * offsets_64_bit_out);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_META_H */
