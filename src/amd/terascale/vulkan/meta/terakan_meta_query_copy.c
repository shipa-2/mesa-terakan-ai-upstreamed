/*
 * Copyright © 2025 Vitaliy Triang3l Kuzmin
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

#include "terakan_barrier.h"
#include "terakan_buffer.h"
#include "terakan_entrypoints.h"
#include "terakan_physical_device.h"
#include "terakan_query.h"

#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_endian.h"

#include <assert.h>

/* The query copy shaders are optimized with the assumption that the probability of the results
 * being unavailable is near zero. Any potential optimizations for the unavailable case are
 * deliberately ignored if they involve doing more work for queries with available results.
 */

#define TERAKAN_META_QUERY_COPY_SHADER(name, num_gprs)                                             \
   struct terakan_meta_shader const terakan_meta_query_copy_##name##_vs = {                        \
      .r8xx =                                                                                      \
         {                                                                                         \
            .program = terakan_meta_query_copy_##name##_vs_r8xx,                                   \
            .program_size_bytes = sizeof(terakan_meta_query_copy_##name##_vs_r8xx),                \
            .static_registers =                                                                    \
               {                                                                                   \
                  .sq_pgm_resources =                                                              \
                     {                                                                             \
                        S_028860_NUM_GPRS(num_gprs) | S_028860_STACK_SIZE(1) |                     \
                           TERAKAN_META_SQ_PGM_RESOURCES_COMMON,                                   \
                        TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,                                    \
                     },                                                                            \
               },                                                                                  \
         },                                                                                        \
      .r9xx =                                                                                      \
         {                                                                                         \
            .program = terakan_meta_query_copy_##name##_vs_r9xx,                                   \
            .program_size_bytes = sizeof(terakan_meta_query_copy_##name##_vs_r9xx),                \
            .static_registers =                                                                    \
               {                                                                                   \
                  .sq_pgm_resources =                                                              \
                     {                                                                             \
                        S_028860_NUM_GPRS(num_gprs) | S_028860_STACK_SIZE(1) |                     \
                           TERAKAN_META_SQ_PGM_RESOURCES_COMMON,                                   \
                        TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,                                    \
                     },                                                                            \
               },                                                                                  \
         },                                                                                        \
      .kcache_used = BITFIELD_BIT(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS),                           \
      .primary_meta_resource_used = true,                                                          \
      .non_pixel_stage_specific_resource_used = true,                                              \
   };

enum {
   TERAKAN_META_QUERY_COPY_BOOL_INDEX_WITH_AVAILABILITY,

   TERAKAN_META_QUERY_COPY_BOOL_INDEX_PIPELINESTAT_0,
   TERAKAN_META_QUERY_COPY_BOOL_INDEX_PIPELINESTAT_10 =
      TERAKAN_META_QUERY_COPY_BOOL_INDEX_PIPELINESTAT_0 + 10,
};

enum {
   /* Values used for address calculation are in dwords. */

   TERAKAN_META_QUERY_COPY_CONST_PARTIAL,
   TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE,
   /* Address pointing to the end of the result for the first query in a query copy draw command in
    * the destination UAV. If VK_QUERY_RESULT_WITH_AVAILABILITY_BIT is set, this is where the
    * availability for the first query is written.
    */
   TERAKAN_META_QUERY_COPY_CONST_DST_RESULT_END_OFFSET,
   TERAKAN_META_QUERY_COPY_CONSTS_COUNT_COMMON,

   /* The pipeline statistics counter offset enum values must be contiguous. */

   TERAKAN_META_QUERY_COPY_CONSTS_PIPELINESTAT_DST_COUNTER_OFFSETS =
      TERAKAN_META_QUERY_COPY_CONSTS_COUNT_COMMON,

   /* For 32-bit pipeline statistics, all relative to the availability so that multiple addresses
    * can be calculated in one instruction group.
    * Result end offset - counter N offset.
    */
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_32_BIT_DST_0_TO_RESULT_END =
      TERAKAN_META_QUERY_COPY_CONSTS_PIPELINESTAT_DST_COUNTER_OFFSETS,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_32_BIT_DST_1_TO_RESULT_END,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_32_BIT_DST_2_TO_RESULT_END,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_32_BIT_DST_3_TO_RESULT_END,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_32_BIT_DST_4_TO_RESULT_END,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_32_BIT_DST_5_TO_RESULT_END,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_32_BIT_DST_6_TO_RESULT_END,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_32_BIT_DST_7_TO_RESULT_END,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_32_BIT_DST_8_TO_RESULT_END,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_32_BIT_DST_9_TO_RESULT_END,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_32_BIT_DST_10_TO_RESULT_END,

   /* For 64-bit pipeline statistics, relative to the previous, so PV/PS can be used to free a GPR
    * read port.
    */
   /* Result end offset - counter 10 offset. */
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_10_TO_RESULT_END =
      TERAKAN_META_QUERY_COPY_CONSTS_PIPELINESTAT_DST_COUNTER_OFFSETS,
   /* Counter 0 offset - counter 10 offset. */
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_10_TO_0,
   /* Counter N offset - counter N-1 offset. */
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_0_TO_1,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_1_TO_2,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_2_TO_3,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_3_TO_4,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_4_TO_5,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_5_TO_6,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_6_TO_7,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_7_TO_8,
   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_8_TO_9,

   TERAKAN_META_QUERY_COPY_CONSTS_COUNT_PIPELINESTAT,
};

/* clang-format off */

/* The location of these in the instructions in the shader must be aligned to 2 qwords.
 *
 * R[sample_fetch_address_gpr].Y is where the sample fetch source address will be written.
 * For 64-bit, `sample_fetch_address_gpr` should be the last register overwritten by the sample
 * fetches.
 * For 32-bit, R0.Y can be used (by passing 0 as `sample_fetch_address_gpr`) for the sample fetch
 * source address because the availability write will use only R0.X.
 *
 * +0: Fetch clause. Load the availability to R0.Y.
 *
 * +2: ALU clause.
 *
 * Group 0: Apply the UAV destination address stride, writing to PS or PV.
 * MULLO_UINT is scalar-only on R8xx, 4-slot on R9xx.
 *
 * Group 1:
 * - X: Copy the availability to R0.X for storing.
 * - Y: Make the sample fetch address in R[sample_fetch_address_gpr].Y out of bounds so fetches load
 *      0 if the result is unavailable for writing as a partial result.
 * - Z: Obtain the condition for writing the result to R0.Z.
 * - W: Write the result end address in the UAV, which is also where the availability will be
 *      written if needed, to R0.W.
 * Cycle 0: XY = R0.
 * Cycle 1: X = R0.
 *
 * Section "Query Operation" of the Vulkan 1.0.28 specification says:
 *
 *     "If no bits are set in flags, results for all requested queries in the available state are
 *     written as 32-bit unsigned integer values, and nothing is written for queries in the
 *     unavailable state.
 *
 *     If VK_QUERY_RESULT_64_BIT is set, the results are written as an array of 64-bit unsigned
 *     integer values as described for vkGetQueryPoolResults."
 */

#define TERAKAN_META_QUERY_COPY_AVAILABILITY_FETCH_AND_ALU_R8XX_7_QWORDS(sample_fetch_address_gpr) \
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |                                       \
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_NON_PIXEL_STAGE_SPECIFIC) |                  \
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X) |                  \
      S_SQ_VTX_WORD0_MEGA_FETCH_COUNT(sizeof(uint32_t) - 1),                                       \
   S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_MASK) |                                              \
      S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_X) |                                              \
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_MASK) |                                           \
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_MASK) |                                           \
      S_SQ_VTX_WORD1_DATA_FORMAT(TERASCALE_FORMAT_INDEX_32) |                                      \
      S_SQ_VTX_WORD1_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) |                          \
      S_SQ_VTX_WORD1_GPR_DST_GPR(0),                                                               \
   S_SQ_VTX_WORD2_ENDIAN_SWAP(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32                   \
                                                   : TERASCALE_ENDIAN_SWAP_NONE) |                 \
      S_SQ_VTX_WORD2_MEGA_FETCH(true),                                                             \
   0,                                                                                              \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |                  \
      TERAKAN_SHADER_OP2_NW(true, 0, MULLO_UINT, EG, 0, 'X', 0, 0, SCL_210),                       \
   TERAKAN_SHADER_OP1(false, 0, 'X', MOV, EG, 0, 'Y', VEC_012),                                    \
   TERAKAN_SHADER_OP3(false, sample_fetch_address_gpr, 'Y', CNDE_INT, EG, 0, 'Y',                  \
                      V_SQ_ALU_SRC_M_1_INT, 0, 0, 'X', VEC_021),                                   \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_QUERY_COPY_CONST_PARTIAL) |                     \
      TERAKAN_SHADER_OP3(false, 0, 'Z', CNDE_INT, EG, 0, 0, 0, 'Y', V_SQ_ALU_SRC_M_1_INT, 0,       \
                         VEC_102),                                                                 \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_RESULT_END_OFFSET) |       \
      TERAKAN_SHADER_OP2(true, 0, 'W', ADD_INT, EG, V_SQ_ALU_SRC_PS, 0, 0, 0, VEC_012)

#define TERAKAN_META_QUERY_COPY_AVAILABILITY_FETCH_AND_ALU_R9XX_10_QWORDS(                         \
   sample_fetch_address_gpr)                                                                       \
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |                                       \
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_NON_PIXEL_STAGE_SPECIFIC) |                  \
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X),                   \
   S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_MASK) |                                              \
      S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_X) |                                              \
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_MASK) |                                           \
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_MASK) |                                           \
      S_SQ_VTX_WORD1_DATA_FORMAT(TERASCALE_FORMAT_INDEX_32) |                                      \
      S_SQ_VTX_WORD1_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) |                          \
      S_SQ_VTX_WORD1_GPR_DST_GPR(0),                                                               \
   S_SQ_VTX_WORD2_ENDIAN_SWAP(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32                   \
                                                   : TERASCALE_ENDIAN_SWAP_NONE),                  \
   0,                                                                                              \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |                  \
      TERAKAN_SHADER_OP2_NW(false, 'X', MULLO_UINT, EG, 0, 'X', 0, 0, VEC_012),                    \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |                  \
      TERAKAN_SHADER_OP2_NW(false, 'Y', MULLO_UINT, EG, 0, 'X', 0, 0, VEC_012),                    \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |                  \
      TERAKAN_SHADER_OP2_NW(false, 'Z', MULLO_UINT, EG, 0, 'X', 0, 0, VEC_012),                    \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |                  \
      TERAKAN_SHADER_OP2_NW(true, 'W', MULLO_UINT, EG, 0, 'X', 0, 0, VEC_012),                     \
   TERAKAN_SHADER_OP1(false, 0, 'X', MOV, EG, 0, 'Y', VEC_012),                                    \
   TERAKAN_SHADER_OP3(false, sample_fetch_address_gpr, 'Y', CNDE_INT, EG, 0, 'Y',                  \
                      V_SQ_ALU_SRC_M_1_INT, 0, 0, 'X', VEC_021),                                   \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_QUERY_COPY_CONST_PARTIAL) |                     \
      TERAKAN_SHADER_OP3(false, 0, 'Z', CNDE_INT, EG, 0, 0, 0, 'Y', V_SQ_ALU_SRC_M_1_INT, 0,       \
                         VEC_102),                                                                 \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_RESULT_END_OFFSET) |       \
      TERAKAN_SHADER_OP2(true, 0, 'W', ADD_INT, EG, V_SQ_ALU_SRC_PV, 0, 0, 0, VEC_012)

/* 0-1: Invoke AVAILABILITY_FETCH_AND_ALU.
 * 2: Skip writing the availability if not requested by jumping over the write.
 * 3: Write the availability.
 * 4: Fetch the samples, using the fetch clause after the AVAILABILITY_FETCH_AND_ALU.
 * 5: Calculate the result, result destination addresses, and disable partial result writes if not
 *    needed, using the ALU clause after the sample fetch clause.
 */
#define TERAKAN_META_QUERY_COPY_CF_BEFORE_STORE_6_QWORDS(is_r9xx, is_64_bit,                       \
                                                         availability_fetch_and_alu_cf_address,    \
                                                         sample_fetch_count,                       \
                                                         result_and_pred_alu_count)                \
   S_SQ_CF_WORD0_ADDR(availability_fetch_and_alu_cf_address),                                      \
   S_SQ_CF_WORD1_COUNT(1 - 1) |                                                                    \
      ((is_r9xx) ? EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX : EG_V_SQ_CF_WORD1_SQ_CF_INST_VTX),             \
   S_SQ_CF_WORD0_ADDR((availability_fetch_and_alu_cf_address) + 2) |                               \
      S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |                       \
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),                                       \
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(0) | S_SQ_CF_ALU_WORD1_COUNT(((is_r9xx) ? 8 : 5) - 1) |          \
      S_SQ_CF_ALU_WORD1_BARRIER(true) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,                       \
   S_SQ_CF_WORD0_ADDR(4),                                                                          \
   S_SQ_CF_WORD1_COND(V_SQ_CF_COND_BOOL) |                                                         \
      S_SQ_CF_WORD1_CF_CONST(TERAKAN_META_QUERY_COPY_BOOL_INDEX_WITH_AVAILABILITY) |               \
      EG_V_SQ_CF_WORD1_SQ_CF_INST_JUMP,                                                            \
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(is_r9xx, 0, 0, is_64_bit, false, true),                    \
   S_SQ_CF_WORD0_ADDR(                                                                             \
      ALIGN_POT((availability_fetch_and_alu_cf_address) + ((is_r9xx) ? 10 : 7), 2)),               \
   S_SQ_CF_WORD1_COUNT((sample_fetch_count) - 1) |                                                 \
      ((is_r9xx) ? EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX : EG_V_SQ_CF_WORD1_SQ_CF_INST_VTX),             \
   S_SQ_CF_WORD0_ADDR(                                                                             \
      ALIGN_POT((availability_fetch_and_alu_cf_address) + ((is_r9xx) ? 10 : 7), 2) +               \
      2 * (sample_fetch_count)) |                                                                  \
      S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |                       \
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),                                       \
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(0) | S_SQ_CF_ALU_WORD1_COUNT((result_and_pred_alu_count) - 1) |  \
      S_SQ_CF_ALU_WORD1_BARRIER(true) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_PUSH_BEFORE

/* +0: Restore the active mask without partial result writing disabling for the ending export.
 * +1-2: End.
 */
#define TERAKAN_META_QUERY_COPY_CF_AFTER_STORE_R8XX_3_QWORDS(this_cf_address)                      \
   S_SQ_CF_WORD0_ADDR((this_cf_address) + 1),                                                      \
   S_SQ_CF_WORD1_POP_COUNT(1) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_POP,        \
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_POS_PARAM_DONE_AND_END_R8XX_2_QWORDS

/* +0: Restore the active mask without partial result writing disabling for the ending export.
 * +1-3: End.
 */
#define TERAKAN_META_QUERY_COPY_CF_AFTER_STORE_R9XX_4_QWORDS(this_cf_address)                      \
   S_SQ_CF_WORD0_ADDR((this_cf_address) + 1),                                                      \
   S_SQ_CF_WORD1_POP_COUNT(1) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_POP,        \
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_POS_PARAM_DONE_R9XX_2_QWORDS,                                 \
   TERAKAN_SHADER_CF_END_R9XX

/* The maximum mega fetch counter count is 8.
 *
 * Using INDEX##format instead of INDEX_##format because text editors may highlight a numeric
 * literal parsing error for tokens like 32_32_32.
 */

#define TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(format, dst_gpr, dst_sel_x, dst_sel_y,          \
                                                   dst_sel_z, dst_sel_w, src_y_gpr,                \
                                                   offset_counter_pairs, mega_fetch_dwords,        \
                                                   is_mega_fetch)                                  \
   (S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |                                      \
    S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |              \
    S_SQ_VTX_WORD0_SRC_GPR(src_y_gpr) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_Y) |            \
    S_SQ_VTX_WORD0_MEGA_FETCH_COUNT(sizeof(uint32_t) * (mega_fetch_dwords) - 1)),                  \
   (S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_##dst_sel_x) |                                      \
    S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_##dst_sel_y) |                                      \
    S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_##dst_sel_z) |                                      \
    S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_##dst_sel_w) |                                      \
    S_SQ_VTX_WORD1_DATA_FORMAT(TERASCALE_FORMAT_INDEX##format) |                                   \
    S_SQ_VTX_WORD1_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) |                            \
    S_SQ_VTX_WORD1_GPR_DST_GPR(dst_gpr)),                                                          \
   (S_SQ_VTX_WORD2_OFFSET(sizeof(uint64_t) * 2 * (offset_counter_pairs)) |                         \
    S_SQ_VTX_WORD2_ENDIAN_SWAP(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32                  \
                                                    : TERASCALE_ENDIAN_SWAP_NONE) |                \
    S_SQ_VTX_WORD2_MEGA_FETCH(is_mega_fetch)),                                                     \
   0

#define TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(format, dst_gpr, dst_sel_x, dst_sel_y,          \
                                                   dst_sel_z, dst_sel_w, src_y_gpr,                \
                                                   offset_counter_pairs)                           \
   (S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |                                      \
    S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |              \
    S_SQ_VTX_WORD0_SRC_GPR(src_y_gpr) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_Y)),            \
   (S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_##dst_sel_x) |                                      \
    S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_##dst_sel_y) |                                      \
    S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_##dst_sel_z) |                                      \
    S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_##dst_sel_w) |                                      \
    S_SQ_VTX_WORD1_DATA_FORMAT(TERASCALE_FORMAT_INDEX##format) |                                   \
    S_SQ_VTX_WORD1_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) |                            \
    S_SQ_VTX_WORD1_GPR_DST_GPR(dst_gpr)),                                                          \
   (S_SQ_VTX_WORD2_OFFSET(sizeof(uint64_t) * 2 * (offset_counter_pairs)) |                         \
    S_SQ_VTX_WORD2_ENDIAN_SWAP(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32                  \
                                                    : TERASCALE_ENDIAN_SWAP_NONE)),                \
   0

/* For the last ALU clause (that calculates the result). */

/* 64-bit subtraction or addition is 3 independent operations:
 * - lower half (`LO` in the macro names here),
 * - borrow or carry (`BC`),
 * - upper half without borrowing or carrying (`HI32`);
 * and one operation that depends on the result of the previous ones:
 * - upper half (`HIWBC`).
 * Therefore, instructions for a single counter need to be spread over 2 groups, so accumulation of
 * query results needs to be done by interleaving at least 2 chains of operations to use all VLIW4
 * lanes.
 */

#define TERAKAN_META_QUERY_COPY_64_BIT_LO_Z_BC_W_HI32(close, dst_gpr, op, op_bc, src0_gpr,         \
                                                      src0_chan_lo, src1_gpr, src1_chan_lo)        \
   TERAKAN_SHADER_OP2(false, dst_gpr, UTIL_ARCH_BIG_ENDIAN ? 'Y' : 'X', op##_INT, EG, src0_gpr,    \
                      src0_chan_lo, src1_gpr, src1_chan_lo, VEC_012),                              \
   TERAKAN_SHADER_OP2_NW(false, 'Z', op##op_bc##_UINT, EG, src0_gpr, src0_chan_lo, src1_gpr,       \
                         src1_chan_lo, VEC_012),                                                   \
   TERAKAN_SHADER_OP2_NW(close, 'W', op##_INT, EG, src0_gpr, (src0_chan_lo) ^ 1, src1_gpr,         \
                         (src1_chan_lo) ^ 1, VEC_012)

#define TERAKAN_META_QUERY_COPY_64_BIT_HIWBC(close, dst_gpr, op)                                   \
   TERAKAN_SHADER_OP2(close, dst_gpr, UTIL_ARCH_BIG_ENDIAN ? 'X' : 'Y', op##_INT, EG,              \
                      V_SQ_ALU_SRC_PV, 'W', V_SQ_ALU_SRC_PV, 'Z', VEC_012)

#if UTIL_ARCH_BIG_ENDIAN
#define TERAKAN_META_QUERY_COPY_64_BIT_LO_HIWBC_Z_BC_W_HI32(close, dst_gpr, op, op_bc, src0_gpr,   \
                                                            src0_chan_lo, src1_gpr, src1_chan_lo,  \
                                                            hiwbc_dst_gpr, hiwbc_op)               \
   TERAKAN_META_QUERY_COPY_64_BIT_HIWBC(false, hiwbc_dst_gpr, hiwbc_op),                           \
   TERAKAN_SHADER_OP2(false, dst_gpr, 'Y', op##_INT, EG, src0_gpr, src0_chan_lo, src1_gpr,         \
                      src1_chan_lo, VEC_012),                                                      \
   TERAKAN_SHADER_OP2_NW(false, 'Z', op##op_bc##_UINT, EG, src0_gpr, src0_chan_lo, src1_gpr,       \
                         src1_chan_lo, VEC_012),                                                   \
   TERAKAN_SHADER_OP2_NW(close, 'W', op##_INT, EG, src0_gpr, (src0_chan_lo) ^ 1, src1_gpr,         \
                         (src1_chan_lo) ^ 1, VEC_012)
#else
#define TERAKAN_META_QUERY_COPY_64_BIT_LO_HIWBC_Z_BC_W_HI32(close, dst_gpr, op, op_bc, src0_gpr,   \
                                                            src0_chan_lo, src1_gpr, src1_chan_lo,  \
                                                            hiwbc_dst_gpr, hiwbc_op)               \
   TERAKAN_SHADER_OP2(false, dst_gpr, 'X', op##_INT, EG, src0_gpr, src0_chan_lo, src1_gpr,         \
                      src1_chan_lo, VEC_012),                                                      \
   TERAKAN_META_QUERY_COPY_64_BIT_HIWBC(false, hiwbc_dst_gpr, hiwbc_op),                           \
   TERAKAN_SHADER_OP2_NW(false, 'Z', op##op_bc##_UINT, EG, src0_gpr, src0_chan_lo, src1_gpr,       \
                         src1_chan_lo, VEC_012),                                                   \
   TERAKAN_SHADER_OP2_NW(close, 'W', op##_INT, EG, src0_gpr, (src0_chan_lo) ^ 1, src1_gpr,         \
                         (src1_chan_lo) ^ 1, VEC_012)
#endif

/* Disable writing an unavailable result if partial result writes aren't enabled, as checked in the
 * ALU_PROCESS_AVAILABILITY clause, after writing the availability, but before attempting to write
 * the result, by disabling the lane in the execute mask if needed.
 *
 * Can be coissued with UAV address calculation reading R0.W on the same cycle as the first operand
 * in this instruction.
 *
 * Cycle bank_swizzle[0]: Z = R0.
 */
#define TERAKAN_META_QUERY_COPY_ALU_PRED(close, dst_chan, bank_swizzle)                            \
   TERAKAN_SHADER_OP2_NW(close, dst_chan, PRED_SETNE_INT, EG, 0, 'Z', V_SQ_ALU_SRC_0, 0,           \
                         bank_swizzle) |                                                           \
      S_SQ_ALU_WORD1_OP2_UPDATE_EXECUTE_MASK(1)

/* clang-format on */

/* Z pass.
 *
 * Need to subtract the beginning sample from the end sample for each render backend, and sum the
 * results.
 *
 * For disabled render backends, all samples are zero-initialized and never modified in the driver,
 * so no need to check the upper bit.
 *
 * For 64-bit, R[1+N].XY = beginning, R[1+N].ZW = end, for render backend N.
 * For 32-bit, R[1+N/2] {X, Y} for even RB or {Z, W} for odd RB = {beginning, end}.
 *
 * ALU operations done:
 * - 32-bit (1 operation) or 64-bit (4 operations) subtractions and additions:
 *   - `rb_count` subtractions.
 *   - `rb_count - 1` additions.
 * - Result write predicate setting.
 * - Address calculation (end minus inline 1 for 32-bit, end minus literal 2 for 64-bit).
 */

/* clang-format off */

/* 0-5: Control flow prolog.
 * 6: Store the result.
 * 7-9: Control flow epilog.
 * 10-16: Fetch and process the availability.
 * 17: Alignment padding for sample fetching.
 * The samples must be fetched at the address R0 for 32-bit, R[rb_count] for 64-bit.
 */
#define TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R8XX(is_64_bit, rb_count)                             \
   TERAKAN_META_QUERY_COPY_CF_BEFORE_STORE_6_QWORDS(                                               \
      false, true, 10, rb_count,                                                                   \
      ((is_64_bit) ? 4 : 1) * ((rb_count) + ((rb_count) - 1)) + ((is_64_bit) ? 3 : 2)),            \
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(false, 0, 1, is_64_bit, false, true),                      \
   TERAKAN_META_QUERY_COPY_CF_AFTER_STORE_R8XX_3_QWORDS(7),                                        \
   TERAKAN_META_QUERY_COPY_AVAILABILITY_FETCH_AND_ALU_R8XX_7_QWORDS((is_64_bit) ? (rb_count) : 0), \
   0,                                                                                              \
   0

/* 0-5: Control flow prolog.
 * 6: Store the result.
 * 7-10: Control flow epilog.
 * 11 (alignment padding), 12-21: Fetch and process the availability.
 * The samples must be fetched at the address R0 for 32-bit, R[rb_count] for 64-bit.
 */
#define TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R9XX(is_64_bit, rb_count)                             \
   TERAKAN_META_QUERY_COPY_CF_BEFORE_STORE_6_QWORDS(                                               \
      true, true, 12, rb_count,                                                                    \
      ((is_64_bit) ? 4 : 1) * ((rb_count) + ((rb_count) - 1)) + ((is_64_bit) ? 3 : 2)),            \
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(true, 0, 1, is_64_bit, false, true),                       \
   TERAKAN_META_QUERY_COPY_CF_AFTER_STORE_R9XX_4_QWORDS(7),                                        \
   0,                                                                                              \
   0,                                                                                              \
   TERAKAN_META_QUERY_COPY_AVAILABILITY_FETCH_AND_ALU_R9XX_10_QWORDS((is_64_bit) ? (rb_count) : 0)

/* Complete the sum reduction from PV.Z and PV.W, and set the result write predicate and address.
 * Cycle 0: ZW = R0.
 */
#define TERAKAN_META_QUERY_COPY_ZPASS_32_BIT_X_SUM_Z_PRED_W_ADDRESS(close)                         \
   TERAKAN_SHADER_OP2(false, 1, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_PV, 'Y',      \
                      VEC_012),                                                                    \
   TERAKAN_META_QUERY_COPY_ALU_PRED(false, 'Z', VEC_012),                                          \
   TERAKAN_SHADER_OP2(close, 1, 'W', SUB_INT, EG, 0, 'W', V_SQ_ALU_SRC_1_INT, 0, VEC_012)

/* e0-b0 */
#define TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_FIRST(close)                              \
   TERAKAN_META_QUERY_COPY_64_BIT_LO_Z_BC_W_HI32(close, 0x7F, SUB, B, 1, 'Z', 1, 'X')
#define TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_OTHER(close, interleave, rb)              \
   TERAKAN_META_QUERY_COPY_64_BIT_LO_HIWBC_Z_BC_W_HI32(close, 0x7F - (rb) % (interleave), SUB, B,  \
                                                       1 + (rb), 'Z', 1 + (rb), 'X', 0x7F, SUB)
/* +eN */
#define TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD(close, interleave, rb, hiwbc_op)                  \
   TERAKAN_META_QUERY_COPY_64_BIT_LO_HIWBC_Z_BC_W_HI32(                                            \
      close, 0x7F - (rb) % (interleave), ADD, C, 0x7F - (rb) % (interleave),                       \
      UTIL_ARCH_BIG_ENDIAN ? 'Y' : 'X', 1 + (rb), 'Z',                                             \
      0x7F - ((rb) + ((interleave) - 1)) % (interleave), hiwbc_op)
/* -bN */
#define TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_SUB(close, interleave, rb, hiwbc_op)                  \
   TERAKAN_META_QUERY_COPY_64_BIT_LO_HIWBC_Z_BC_W_HI32(                                            \
      close, 0x7F - (rb) % (interleave), SUB, B, 0x7F - (rb) % (interleave),                       \
      UTIL_ARCH_BIG_ENDIAN ? 'Y' : 'X', 1 + (rb), 'X',                                             \
      0x7F - ((rb) + ((interleave) - 1)) % (interleave), hiwbc_op)

/* Finalization. */

/* R1.LO <- T0+T1 lower
 * PV.Z <- T0+T1 carry
 * PV.W <- T0+T1 upper without carry
 */
#define TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD_T0_T1_R1_LO_Z_C_W_HI32(close)                     \
   TERAKAN_SHADER_OP2(false, 1, UTIL_ARCH_BIG_ENDIAN ? 'Y' : 'X', ADD_INT, EG, 0x7F, 'X', 0x7E,    \
                      'X', VEC_012),                                                               \
   TERAKAN_SHADER_OP2_NW(false, 'Z', ADDC_UINT, EG, 0x7F, 'X', 0x7E, 'X', VEC_012),                \
   TERAKAN_SHADER_OP2_NW(close, 'W', ADD_INT, EG, 0x7F, 'Y', 0x7E, 'Y', VEC_012)

/* R1.HI <- total upper with borrow or carry
 * Z <- result write predicate
 * R1.W <- UAV address
 */
#define TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_R1_HIWBC_Z_PRED_W_ADDRESS_4_QWORDS(hiwbc_op)          \
   TERAKAN_META_QUERY_COPY_64_BIT_HIWBC(false, 1, hiwbc_op),                                       \
   TERAKAN_META_QUERY_COPY_ALU_PRED(false, 'Z', VEC_210),                                          \
   TERAKAN_SHADER_OP2(true, 1, 'W', SUB_INT, EG, 0, 'W', V_SQ_ALU_SRC_LITERAL, 'X', VEC_210),      \
   2,                                                                                              \
   0

/* clang-format on */

static uint32_t const terakan_meta_query_copy_zpass_32_bit_1_rb_vs_r8xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R8XX(false, 1),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 1, X, Z, MASK, MASK, 0, 0, 3, true),

   TERAKAN_SHADER_OP2(false, 1, 'X', SUB_INT, EG, 1, 'Y', 1, 'X', VEC_012),
   TERAKAN_META_QUERY_COPY_ALU_PRED(false, 'Z', VEC_210),
   TERAKAN_SHADER_OP2(true, 1, 'W', SUB_INT, EG, 0, 'W', V_SQ_ALU_SRC_1_INT, 0, VEC_210),
};

static uint32_t const terakan_meta_query_copy_zpass_32_bit_1_rb_vs_r9xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R9XX(false, 1),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 1, X, Z, MASK, MASK, 0, 0),

   TERAKAN_SHADER_OP2(false, 1, 'X', SUB_INT, EG, 1, 'Y', 1, 'X', VEC_012),
   TERAKAN_META_QUERY_COPY_ALU_PRED(false, 'Z', VEC_210),
   TERAKAN_SHADER_OP2(true, 1, 'W', SUB_INT, EG, 0, 'W', V_SQ_ALU_SRC_1_INT, 0, VEC_210),
};

TERAKAN_META_QUERY_COPY_SHADER(zpass_32_bit_1_rb, 2)

static uint32_t const terakan_meta_query_copy_zpass_32_bit_2_rb_vs_r8xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R8XX(false, 2),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 1, X, Z, MASK, MASK, 0, 0, 7, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 1, MASK, MASK, X, Z, 0, 1, 3, false),

   /* e0-b0 */
   TERAKAN_SHADER_OP2_NW(0, 'X', SUB_INT, EG, 1, 'Y', 1, 'X', VEC_012),
   /* e1-b1 */
   TERAKAN_SHADER_OP2_NW(1, 'Y', SUB_INT, EG, 1, 'W', 1, 'Z', VEC_012),

   TERAKAN_META_QUERY_COPY_ZPASS_32_BIT_X_SUM_Z_PRED_W_ADDRESS(true),
};

static uint32_t const terakan_meta_query_copy_zpass_32_bit_2_rb_vs_r9xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R9XX(false, 2),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 1, X, Z, MASK, MASK, 0, 0),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 1, MASK, MASK, X, Z, 0, 1),

   /* e0-b0 */
   TERAKAN_SHADER_OP2_NW(0, 'X', SUB_INT, EG, 1, 'Y', 1, 'X', VEC_012),
   /* e1-b1 */
   TERAKAN_SHADER_OP2_NW(1, 'Y', SUB_INT, EG, 1, 'W', 1, 'Z', VEC_012),

   TERAKAN_META_QUERY_COPY_ZPASS_32_BIT_X_SUM_Z_PRED_W_ADDRESS(true),
};

TERAKAN_META_QUERY_COPY_SHADER(zpass_32_bit_2_rb, 2)

static uint32_t const terakan_meta_query_copy_zpass_32_bit_4_rb_vs_r8xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R8XX(false, 4),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 1, X, Z, MASK, MASK, 0, 0, 15, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 1, MASK, MASK, X, Z, 0, 1, 11, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 2, X, Z, MASK, MASK, 0, 2, 7, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 2, MASK, MASK, X, Z, 0, 3, 3, false),

   /* e0-b0 */
   TERAKAN_SHADER_OP2_NW(0, 'X', SUB_INT, EG, 1, 'Y', 1, 'X', VEC_012),
   /* e1-b1 */
   TERAKAN_SHADER_OP2_NW(0, 'Y', SUB_INT, EG, 1, 'W', 1, 'Z', VEC_012),
   /* e2-b2 */
   TERAKAN_SHADER_OP2_NW(0, 'Z', SUB_INT, EG, 2, 'Y', 2, 'X', VEC_102),
   /* e3-b3 */
   TERAKAN_SHADER_OP2_NW(1, 'W', SUB_INT, EG, 2, 'W', 2, 'Z', VEC_102),

   /* e0-b0+e1-b1 */
   TERAKAN_SHADER_OP2_NW(0, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_PV, 'Y', VEC_012),
   /* e2-b2+e3-b3 */
   TERAKAN_SHADER_OP2_NW(1, 'Y', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'Z', V_SQ_ALU_SRC_PV, 'W', VEC_012),

   TERAKAN_META_QUERY_COPY_ZPASS_32_BIT_X_SUM_Z_PRED_W_ADDRESS(true),
};

/* No R9xx chips with a maximum of 4 render backends, provide an aligned dummy. */
static uint32_t const terakan_meta_query_copy_zpass_32_bit_4_rb_vs_r9xx[] = {0, 0};

TERAKAN_META_QUERY_COPY_SHADER(zpass_32_bit_4_rb, 3)

static uint32_t const terakan_meta_query_copy_zpass_32_bit_8_rb_vs_r8xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R8XX(false, 8),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 1, X, Z, MASK, MASK, 0, 0, 15, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 1, MASK, MASK, X, Z, 0, 1, 11, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 2, X, Z, MASK, MASK, 0, 2, 7, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 2, MASK, MASK, X, Z, 0, 3, 3, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 3, X, Z, MASK, MASK, 0, 4, 15, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 3, MASK, MASK, X, Z, 0, 5, 11, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 4, X, Z, MASK, MASK, 0, 6, 7, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 4, MASK, MASK, X, Z, 0, 7, 3, false),

   /* e0-b0 */
   TERAKAN_SHADER_OP2(0, 0x7F, 'X', SUB_INT, EG, 1, 'Y', 1, 'X', VEC_012),
   /* e1-b1 */
   TERAKAN_SHADER_OP2(0, 0x7F, 'Y', SUB_INT, EG, 1, 'W', 1, 'Z', VEC_012),
   /* e2-b2 */
   TERAKAN_SHADER_OP2(0, 0x7F, 'Z', SUB_INT, EG, 2, 'Y', 2, 'X', VEC_102),
   /* e3-b3 */
   TERAKAN_SHADER_OP2(1, 0x7F, 'W', SUB_INT, EG, 2, 'W', 2, 'Z', VEC_102),

   /* e4-b4 */
   TERAKAN_SHADER_OP2_NW(0, 'X', SUB_INT, EG, 3, 'Y', 3, 'X', VEC_012),
   /* e5-b5 */
   TERAKAN_SHADER_OP2_NW(0, 'Y', SUB_INT, EG, 3, 'W', 3, 'Z', VEC_012),
   /* e6-b6 */
   TERAKAN_SHADER_OP2_NW(0, 'Z', SUB_INT, EG, 4, 'Y', 4, 'X', VEC_102),
   /* e7-b7 */
   TERAKAN_SHADER_OP2_NW(1, 'W', SUB_INT, EG, 4, 'W', 4, 'Z', VEC_102),

   /* e0-b0+e1-b1 */
   TERAKAN_SHADER_OP2_NW(0, 'X', ADD_INT, EG, 0x7F, 'X', 0x7F, 'Y', VEC_012),
   /* e2-b2+e3-b3 */
   TERAKAN_SHADER_OP2_NW(0, 'Y', ADD_INT, EG, 0x7F, 'Z', 0x7F, 'W', VEC_012),
   /* e4-b4+e5-b5 */
   TERAKAN_SHADER_OP2_NW(0, 'Z', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_PV, 'Y', VEC_012),
   /* e6-b6+e7-b7 */
   TERAKAN_SHADER_OP2_NW(1, 'W', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'Z', V_SQ_ALU_SRC_PV, 'W', VEC_012),

   /* e0-b0+e1-b1+e2-b2+e3-b3 */
   TERAKAN_SHADER_OP2_NW(0, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_PV, 'Y', VEC_012),
   /* e4-b4+e5-b5+e6-b6+e7-b7 */
   TERAKAN_SHADER_OP2_NW(1, 'Y', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'Z', V_SQ_ALU_SRC_PV, 'W', VEC_012),

   TERAKAN_META_QUERY_COPY_ZPASS_32_BIT_X_SUM_Z_PRED_W_ADDRESS(true),
};

static uint32_t const terakan_meta_query_copy_zpass_32_bit_8_rb_vs_r9xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R9XX(false, 8),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 1, X, Z, MASK, MASK, 0, 0),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 1, MASK, MASK, X, Z, 0, 1),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 2, X, Z, MASK, MASK, 0, 2),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 2, MASK, MASK, X, Z, 0, 3),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 3, X, Z, MASK, MASK, 0, 4),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 3, MASK, MASK, X, Z, 0, 5),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 4, X, Z, MASK, MASK, 0, 6),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 4, MASK, MASK, X, Z, 0, 7),

   /* e0-b0 */
   TERAKAN_SHADER_OP2(0, 0x7F, 'X', SUB_INT, EG, 1, 'Y', 1, 'X', VEC_012),
   /* e1-b1 */
   TERAKAN_SHADER_OP2(0, 0x7F, 'Y', SUB_INT, EG, 1, 'W', 1, 'Z', VEC_012),
   /* e2-b2 */
   TERAKAN_SHADER_OP2(0, 0x7F, 'Z', SUB_INT, EG, 2, 'Y', 2, 'X', VEC_102),
   /* e3-b3 */
   TERAKAN_SHADER_OP2(1, 0x7F, 'W', SUB_INT, EG, 2, 'W', 2, 'Z', VEC_102),

   /* e4-b4 */
   TERAKAN_SHADER_OP2_NW(0, 'X', SUB_INT, EG, 3, 'Y', 3, 'X', VEC_012),
   /* e5-b5 */
   TERAKAN_SHADER_OP2_NW(0, 'Y', SUB_INT, EG, 3, 'W', 3, 'Z', VEC_012),
   /* e6-b6 */
   TERAKAN_SHADER_OP2_NW(0, 'Z', SUB_INT, EG, 4, 'Y', 4, 'X', VEC_102),
   /* e7-b7 */
   TERAKAN_SHADER_OP2_NW(1, 'W', SUB_INT, EG, 4, 'W', 4, 'Z', VEC_102),

   /* e0-b0+e1-b1 */
   TERAKAN_SHADER_OP2_NW(0, 'X', ADD_INT, EG, 0x7F, 'X', 0x7F, 'Y', VEC_012),
   /* e2-b2+e3-b3 */
   TERAKAN_SHADER_OP2_NW(0, 'Y', ADD_INT, EG, 0x7F, 'Z', 0x7F, 'W', VEC_012),
   /* e4-b4+e5-b5 */
   TERAKAN_SHADER_OP2_NW(0, 'Z', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_PV, 'Y', VEC_012),
   /* e6-b6+e7-b7 */
   TERAKAN_SHADER_OP2_NW(1, 'W', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'Z', V_SQ_ALU_SRC_PV, 'W', VEC_012),

   /* e0-b0+e1-b1+e2-b2+e3-b3 */
   TERAKAN_SHADER_OP2_NW(0, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_PV, 'Y', VEC_012),
   /* e4-b4+e5-b5+e6-b6+e7-b7 */
   TERAKAN_SHADER_OP2_NW(1, 'Y', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'Z', V_SQ_ALU_SRC_PV, 'W', VEC_012),

   TERAKAN_META_QUERY_COPY_ZPASS_32_BIT_X_SUM_Z_PRED_W_ADDRESS(true),
};

TERAKAN_META_QUERY_COPY_SHADER(zpass_32_bit_8_rb, 5)

static uint32_t const terakan_meta_query_copy_zpass_64_bit_1_rb_vs_r8xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R8XX(true, 1),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 1, X, Y, Z, W, 1, 0, 4, true),

   TERAKAN_SHADER_OP2(false, 1, UTIL_ARCH_BIG_ENDIAN ? 'Y' : 'X', SUB_INT, EG, 1, 'Z', 1, 'X',
                      VEC_012),
   TERAKAN_SHADER_OP2_NW(false, 'Z', SUBB_UINT, EG, 1, 'Z', 1, 'X', VEC_012),
   TERAKAN_SHADER_OP2_NW(true, 'W', SUB_INT, EG, 1, 'W', 1, 'Y', VEC_012),

   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_R1_HIWBC_Z_PRED_W_ADDRESS_4_QWORDS(SUB),
};

static uint32_t const terakan_meta_query_copy_zpass_64_bit_1_rb_vs_r9xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R9XX(true, 1),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 1, X, Y, Z, W, 1, 0),

   TERAKAN_SHADER_OP2(false, 1, UTIL_ARCH_BIG_ENDIAN ? 'Y' : 'X', SUB_INT, EG, 1, 'Z', 1, 'X',
                      VEC_012),
   TERAKAN_SHADER_OP2_NW(false, 'Z', SUBB_UINT, EG, 1, 'Z', 1, 'X', VEC_012),
   TERAKAN_SHADER_OP2_NW(true, 'W', SUB_INT, EG, 1, 'W', 1, 'Y', VEC_012),

   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_R1_HIWBC_Z_PRED_W_ADDRESS_4_QWORDS(SUB),
};

TERAKAN_META_QUERY_COPY_SHADER(zpass_64_bit_1_rb, 1 + 1)

static uint32_t const terakan_meta_query_copy_zpass_64_bit_2_rb_vs_r8xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R8XX(true, 2),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 1, X, Y, Z, W, 2, 0, 8, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 2, X, Y, Z, W, 2, 1, 4, false),

   /* e0-b0 */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_FIRST(true),
   /* e1-b1 */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_OTHER(true, 2, 1),
   /* e1-b1 upper with carry */
   TERAKAN_META_QUERY_COPY_64_BIT_HIWBC(true, 0x7E, SUB),
   /* Finalization. */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD_T0_T1_R1_LO_Z_C_W_HI32(true),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_R1_HIWBC_Z_PRED_W_ADDRESS_4_QWORDS(ADD),
};

static uint32_t const terakan_meta_query_copy_zpass_64_bit_2_rb_vs_r9xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R9XX(true, 2),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 1, X, Y, Z, W, 2, 0),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 2, X, Y, Z, W, 2, 1),

   /* e0-b0 */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_FIRST(true),
   /* e1-b1 */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_OTHER(true, 2, 1),
   /* e1-b1 upper with carry */
   TERAKAN_META_QUERY_COPY_64_BIT_HIWBC(true, 0x7E, SUB),
   /* Finalization. */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD_T0_T1_R1_LO_Z_C_W_HI32(true),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_R1_HIWBC_Z_PRED_W_ADDRESS_4_QWORDS(ADD),
};

TERAKAN_META_QUERY_COPY_SHADER(zpass_64_bit_2_rb, 1 + 2)

static uint32_t const terakan_meta_query_copy_zpass_64_bit_4_rb_vs_r8xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R8XX(true, 4),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 1, X, Y, Z, W, 4, 0, 16, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 2, X, Y, Z, W, 4, 1, 12, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 3, X, Y, Z, W, 4, 2, 8, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 4, X, Y, Z, W, 4, 3, 4, false),

   /* T0.XY <- e0-b0 (XY @ C1) | T0.Z <- e3-b3 lower (Z @ C1) */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_FIRST(false),
   TERAKAN_SHADER_OP2(true, 0x7F, 'Z', SUB_INT, EG, 4, 'Z', 4, 'X', SCL_122),
   /* T1.XY <- e1-b1 (XY @ C1) | T0.W <- e3-b3 upper without borrow (W @ C1) */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_OTHER(false, 3, 1),
   TERAKAN_SHADER_OP2(true, 0x7F, 'W', SUB_INT, EG, 4, 'W', 4, 'Y', SCL_122),
   /* T2.XY <- e2-b2 (XY @ C1) | PS <- e3-b3 borrow (Z @ C1) */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_OTHER(false, 3, 2),
   TERAKAN_SHADER_OP2_NW(true, 0, SUBB_UINT, EG, 4, 'Z', 4, 'X', SCL_122),
   /* T0.XY <- e0-b0+e1-b1 (XY @ C1) | T0.W <- e3-b3 upper with borrow */
   TERAKAN_META_QUERY_COPY_64_BIT_LO_HIWBC_Z_BC_W_HI32(false, 0x7F, ADD, C, 0x7F,
                                                       UTIL_ARCH_BIG_ENDIAN ? 'Y' : 'X', 0x7E,
                                                       UTIL_ARCH_BIG_ENDIAN ? 'Y' : 'X', 0x7D, SUB),
   TERAKAN_SHADER_OP2(true, 0x7F, 'W', SUB_INT, EG, 0x7F, 'W', V_SQ_ALU_SRC_PS, 0, SCL_210),
   /* T1.XY <- e2-b2+e3-b3 */
   TERAKAN_META_QUERY_COPY_64_BIT_LO_HIWBC_Z_BC_W_HI32(
      true, 0x7E, ADD, C, 0x7D, UTIL_ARCH_BIG_ENDIAN ? 'Y' : 'X', 0x7F, 'Z', 0x7F, ADD),
   /* e2-b2+e3-b3 upper with carry */
   TERAKAN_META_QUERY_COPY_64_BIT_HIWBC(true, 0x7E, ADD),
   /* Finalization. */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD_T0_T1_R1_LO_Z_C_W_HI32(true),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_R1_HIWBC_Z_PRED_W_ADDRESS_4_QWORDS(ADD),
};

/* No R9xx chips with a maximum of 4 render backends, provide an aligned dummy. */
static uint32_t const terakan_meta_query_copy_zpass_64_bit_4_rb_vs_r9xx[] = {0, 0};

TERAKAN_META_QUERY_COPY_SHADER(zpass_64_bit_4_rb, 1 + 4)

static uint32_t const terakan_meta_query_copy_zpass_64_bit_8_rb_vs_r8xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R8XX(true, 8),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 1, X, Y, Z, W, 8, 0, 16, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 2, X, Y, Z, W, 8, 1, 12, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 3, X, Y, Z, W, 8, 2, 8, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 4, X, Y, Z, W, 8, 3, 4, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 5, X, Y, Z, W, 8, 4, 16, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 6, X, Y, Z, W, 8, 5, 12, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 7, X, Y, Z, W, 8, 6, 8, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 8, X, Y, Z, W, 8, 7, 4, false),

   /* Even and odd render backend chains interleaved, the vector units processing RBs bottom-up, the
    * scalar unit going top-down.
    */

   /* T0.XY <- e0-b0 (XY @ C1) | T0.Z <- e6-b6 lower (Z @ C1) */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_FIRST(false),
   TERAKAN_SHADER_OP2(true, 0x7F, 'Z', SUB_INT, EG, 7, 'Z', 7, 'X', SCL_122),
   /* T1.XY <- e1-b1 (XY @ C1) | T0.W <- e6-b6 upper without borrow (W @ C1) */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_OTHER(false, 2, 1),
   TERAKAN_SHADER_OP2(true, 0x7F, 'W', SUB_INT, EG, 7, 'W', 7, 'Y', SCL_122),
   /* T0.XY <- e0-b0+e2 (ZW @ C1) | PS <- e6-b6 borrow (X @ C1) */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD(false, 2, 2, SUB),
   TERAKAN_SHADER_OP2_NW(true, 0, SUBB_UINT, EG, 7, 'Z', 7, 'X', SCL_210),
   /* T1.XY <- e1-b1+e3 (ZW @ C1) | T0.W <- e6-b6 upper with borrow */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD(false, 2, 3, ADD),
   TERAKAN_SHADER_OP2(true, 0x7F, 'W', SUB_INT, EG, 0x7F, 'W', V_SQ_ALU_SRC_PS, 0, SCL_210),
   /* T0.XY <- e0-b0+e2-b2 (XY @ C1) | T1.Z <- e7-b7 lower (Z @ C1) */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_SUB(false, 2, 2, ADD),
   TERAKAN_SHADER_OP2(true, 0x7E, 'Z', SUB_INT, EG, 8, 'Z', 8, 'X', SCL_122),
   /* T1.XY <- e1-b1+e3-b3 (XY @ C1) | T1.W <- e7-b7 upper without borrow (W @ C1) */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_SUB(false, 2, 3, SUB),
   TERAKAN_SHADER_OP2(true, 0x7E, 'W', SUB_INT, EG, 8, 'W', 8, 'Y', SCL_122),
   /* T0.XY <- e0-b0+e2-b2+e4 (ZW @ C1) | PS <- e7-b7 borrow (X @ C1) */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD(false, 2, 4, SUB),
   TERAKAN_SHADER_OP2_NW(true, 0, SUBB_UINT, EG, 8, 'Z', 8, 'X', SCL_210),
   /* T1.XY <- e1-b1+e3-b3+e5 (ZW @ C1) | T1.W <- e7-b7 upper with borrow */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD(false, 2, 5, ADD),
   TERAKAN_SHADER_OP2(true, 0x7E, 'W', SUB_INT, EG, 0x7E, 'W', V_SQ_ALU_SRC_PS, 0, SCL_210),
   /* T0.XY <- e0-b0+e2-b2+e4-b4 */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_SUB(true, 2, 4, ADD),
   /* T1.XY <- e1-b1+e3-b3+e5-b5 */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_SUB(true, 2, 5, SUB),
   /* T0.XY <- e0-b0+e2-b2+e4-b4+e6-b6 */
   TERAKAN_META_QUERY_COPY_64_BIT_LO_HIWBC_Z_BC_W_HI32(
      true, 0x7F, ADD, C, 0x7F, UTIL_ARCH_BIG_ENDIAN ? 'Y' : 'X', 0x7F, 'Z', 0x7E, SUB),
   /* T1.XY <- e1-b1+e3-b3+e5-b5+e7-b7 (except for upper with carry) */
   TERAKAN_META_QUERY_COPY_64_BIT_LO_HIWBC_Z_BC_W_HI32(
      true, 0x7E, ADD, C, 0x7E, UTIL_ARCH_BIG_ENDIAN ? 'Y' : 'X', 0x7E, 'Z', 0x7F, ADD),
   /* e1-b1+e3-b3+e5-b5+e7-b7 upper with carry */
   TERAKAN_META_QUERY_COPY_64_BIT_HIWBC(true, 0x7E, ADD),
   /* Finalization. */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD_T0_T1_R1_LO_Z_C_W_HI32(true),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_R1_HIWBC_Z_PRED_W_ADDRESS_4_QWORDS(ADD),
};

static uint32_t const terakan_meta_query_copy_zpass_64_bit_8_rb_vs_r9xx[] = {
   TERAKAN_META_QUERY_COPY_ZPASS_PROLOG_R9XX(true, 8),

   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 1, X, Y, Z, W, 8, 0),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 2, X, Y, Z, W, 8, 1),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 3, X, Y, Z, W, 8, 2),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 4, X, Y, Z, W, 8, 3),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 5, X, Y, Z, W, 8, 4),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 6, X, Y, Z, W, 8, 5),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 7, X, Y, Z, W, 8, 6),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 8, X, Y, Z, W, 8, 7),

   /* e0-b0+e2-b2+... and e1-b1+e3-b3+... interleaved. */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_FIRST(true),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_INITIAL_SUB_OTHER(true, 2, 1),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD(true, 2, 2, SUB),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD(true, 2, 3, ADD),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_SUB(true, 2, 2, ADD),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_SUB(true, 2, 3, SUB),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD(true, 2, 4, SUB),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD(true, 2, 5, ADD),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_SUB(true, 2, 4, ADD),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_SUB(true, 2, 5, SUB),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD(true, 2, 6, SUB),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD(true, 2, 7, ADD),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_SUB(true, 2, 6, ADD),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_SUB(true, 2, 7, SUB),
   /* e1-b1+e3-b3+e5-b5+e7-b7 upper with carry */
   TERAKAN_META_QUERY_COPY_64_BIT_HIWBC(true, 0x7E, SUB),
   /* Finalization. */
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_ADD_T0_T1_R1_LO_Z_C_W_HI32(true),
   TERAKAN_META_QUERY_COPY_ZPASS_64_BIT_R1_HIWBC_Z_PRED_W_ADDRESS_4_QWORDS(ADD),
};

TERAKAN_META_QUERY_COPY_SHADER(zpass_64_bit_8_rb, 1 + 8)

/* Pipeline statistics.
 *
 * For each 64-bit pipeline statistics counter, the result is written to XY of the GPR that contains
 * either its beginning sample or its end sample, so that other samples are never overwritten too
 * early. The per-counter UAV addresses are written to the W component of GPRs in a similar manner
 * too.
 *
 * Also, on R9xx, the W lane is needed for UAV address writing, so the "upper half without borrow"
 * part is calculated separately.
 *
 * This is why XYZW and YXWZ swizzled have to be alternated on R9xx, so when working with 4 counters
 * at once, unique GPR XYZW can be read on both cycles 0 and 1.
 *
 * The 64-bit sample loads thus are done as follows:
 * -  R1.XY =  0 beginning,  R1.ZW =  1 beginning.
 * -  R2.XY =  1 end,        R2.ZW =  2 end.
 * -  R3.YX =  2 beginning,  R3.WZ =  3 beginning.
 * -  R4.YX =  3 end,        R4.WZ =  4 end.
 * -  R5.XY =  4 beginning,  R5.ZW =  5 beginning.
 * -  R6.XY =  5 end,        R6.ZW =  6 end.
 * -  R7.YX =  6 beginning,  R7.WZ =  7 beginning.
 * -  R8.YX =  7 end,        R8.WZ =  8 end.
 * -  R9.XY =  8 beginning,  R9.ZW =  9 beginning.
 * - R10.XY =  9 end,       R10.ZW = 10 end.
 * - R11.YX = 10 beginning, R11.WZ =  0 end.
 *
 * For 32-bit copying, fetching to YZ so that XW that contain the UAV store arguments can be written
 * at any point. Beginning and end for one counter are in different lanes, so SCL_221 bank swizzle
 * can be used for subtraction.
 * -  R1.Y =  0 beginning,  R1.Z =  1 beginning.
 * -  R2.Y =  1 end,        R2.Z =  2 end.
 * -  R3.Y =  2 beginning,  R3.Z =  3 beginning.
 * -  R4.Y =  3 end,        R4.Z =  4 end.
 * -  R5.Y =  4 beginning,  R5.Z =  5 beginning.
 * -  R6.Y =  5 end,        R6.Z =  6 end.
 * -  R7.Y =  6 beginning,  R7.Z =  7 beginning.
 * -  R8.Y =  7 end,        R8.Z =  8 end.
 * -  R9.Y =  8 beginning,  R9.Z =  9 beginning.
 * - R10.Y =  9 end,       R10.Z = 10 end.
 * - R11.Y = 10 beginning, R11.Z =  0 end.
 */

#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(counter) (1 + (counter) / 2 * 2)
#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(counter)                                      \
   ((counter) ? 2 + ((counter) - 1) / 2 * 2 : 11)

#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_BEGIN_CHAN(counter)                            \
   (((counter) & 1) ? 'Z' : 'Y')
#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_END_CHAN(counter) (((counter) & 1) ? 'Y' : 'Z')

#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(counter)                              \
   ((counter) % 2 * 2 + (counter) / 2 % 2)
#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO(counter)                                \
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(11 + (counter))

/* clang-format off */

#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB(close, counter, is_scalar)               \
   TERAKAN_SHADER_OP2(close, 1 + (counter), 'X', SUB_INT, EG,                                      \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(counter),                       \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_END_CHAN(counter),               \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(counter),                     \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_BEGIN_CHAN(counter),             \
                      VEC_012) |                                                                   \
      S_SQ_ALU_WORD1_BANK_SWIZZLE((is_scalar) ? SQ_ALU_SCL_221 : SQ_ALU_VEC_012)
/* VEC_012 or SCL_210 (have the same encoding). */
#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_W_ADDRESS(close, counter)                      \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(                                                                \
      0, TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_32_BIT_DST_0_TO_RESULT_END + (counter)) |      \
      TERAKAN_SHADER_OP2(close, 1 + (counter), 'W', SUB_INT, EG, 0, 'W', 0, 0, VEC_012)
#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(close, counter)                \
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB(false, counter, false),                       \
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_W_ADDRESS(close, counter)

#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_HI32_TO_TEMP(close, dst_chan, counter)         \
   TERAKAN_SHADER_OP2(close, 0x7F - (counter) / 4, dst_chan, SUB_INT, EG,                          \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(counter),                       \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO(counter) ^ 1,             \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(counter),                     \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(counter) ^ 1, VEC_012)

/*       R[1 + counter].LO = N lower
 * R[1 + (counter - 1)].HI = N-1 upper with borrow
 *                   PV.Z  = N borrow
 *                   PV.W  = N upper without borrow
 * R[1 + (counter - 1)].W  = N-1 address
 */
#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_HI32_ADDRESS_R8XX(counter)           \
   TERAKAN_META_QUERY_COPY_64_BIT_LO_HIWBC_Z_BC_W_HI32(                                            \
      false, 1 + (counter), SUB, B, TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(counter),         \
      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO(counter),                                 \
      TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(counter),                                     \
      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(counter), 1 + ((counter) - 1), SUB),    \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(                                                                \
      0, TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_10_TO_0 + ((counter) - 1)) |        \
      TERAKAN_SHADER_OP2(true, 1 + ((counter) - 1), 'W', ADD_INT, EG, V_SQ_ALU_SRC_PS, 'W', 0, 0,  \
                         SCL_210)

/*       R[1 + counter].LO = N lower
 *       R[1 + counter].HI = N upper with borrow
 *                   PV.Z  = N+1 borrow
 * R[1 + (counter - 1)].W  = N-1 address
 */
#if UTIL_ARCH_BIG_ENDIAN
#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_ADDRESS_R9XX(counter)                \
   TERAKAN_SHADER_OP2(false, 1 + (counter), 'X', SUB_INT, EG, 0x7F - (counter) / 4, (counter) % 4, \
                      V_SQ_ALU_SRC_PV, 'Z', VEC_210),                                              \
   TERAKAN_SHADER_OP2(false, 1 + (counter), 'Y', SUB_INT, EG,                                      \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(counter),                       \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO(counter),                 \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(counter),                     \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(counter), VEC_012),     \
   TERAKAN_SHADER_OP2_NW(false, 'Z', SUBB_UINT, EG,                                                \
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR((counter) + 1),              \
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO((counter) + 1),        \
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR((counter) + 1),            \
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO((counter) + 1),      \
                         VEC_012),                                                                 \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(                                                                \
      0, TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_10_TO_0 + ((counter) - 1)) |        \
      TERAKAN_SHADER_OP2(true, 1 + ((counter) - 1), 'W', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'W', 0, 0,  \
                         VEC_210)
#else
#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_ADDRESS_R9XX(counter)                \
   TERAKAN_SHADER_OP2(false, 1 + (counter), 'X', SUB_INT, EG,                                      \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(counter),                       \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO(counter),                 \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(counter),                     \
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(counter), VEC_012),     \
   TERAKAN_SHADER_OP2(false, 1 + (counter), 'Y', SUB_INT, EG, 0x7F - (counter) / 4, (counter) % 4, \
                      V_SQ_ALU_SRC_PV, 'Z', VEC_210),                                              \
   TERAKAN_SHADER_OP2_NW(false, 'Z', SUBB_UINT, EG,                                                \
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR((counter) + 1),              \
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO((counter) + 1),        \
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR((counter) + 1),            \
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO((counter) + 1),      \
                         VEC_012),                                                                 \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(                                                                \
      0, TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_10_TO_0 + ((counter) - 1)) |        \
      TERAKAN_SHADER_OP2(true, 1 + ((counter) - 1), 'W', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'W', 0, 0,  \
                         VEC_210)
#endif

#define TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(                          \
   is_r9xx, skip_address, counter, two_component, barrier)                                         \
   S_SQ_CF_WORD0_ADDR(skip_address),                                                               \
   S_SQ_CF_WORD1_COND(V_SQ_CF_COND_BOOL) |                                                         \
      S_SQ_CF_WORD1_CF_CONST(TERAKAN_META_QUERY_COPY_BOOL_INDEX_PIPELINESTAT_0 + (counter)) |      \
      S_SQ_CF_WORD1_BARRIER(barrier) | EG_V_SQ_CF_WORD1_SQ_CF_INST_JUMP,                           \
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(is_r9xx, 0, 1 + (counter), two_component, false, true)

/* clang-format on */

static uint32_t const terakan_meta_query_copy_pipelinestat_32_bit_vs_r8xx[] = {
   /* 0-5: Control flow prolog. */
   TERAKAN_META_QUERY_COPY_CF_BEFORE_STORE_6_QWORDS(false, false, 32, 11, 2 * 11 + 1),

   /* 6-27: Store the result. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 8, 0, false, true),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 10, 1, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 12, 2, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 14, 3, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 16, 4, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 18, 5, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 20, 6, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 22, 7, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 24, 8, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 26, 9, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 28, 10, false, false),

   /* 28-30: Control flow epilog. */
   TERAKAN_META_QUERY_COPY_CF_AFTER_STORE_R8XX_3_QWORDS(28),

   /* 31 (alignment padding), 32-38: Fetch and process the availability. */
   0,
   0,
   TERAKAN_META_QUERY_COPY_AVAILABILITY_FETCH_AND_ALU_R8XX_7_QWORDS(0),

   /* 39 (alignment padding), 40-61: Fetch the samples. */
   0,
   0,
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 1, MASK, X, Z, MASK, 0, 0, 15, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 3, MASK, X, Z, MASK, 0, 1, 11, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 5, MASK, X, Z, MASK, 0, 2, 7, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 7, MASK, X, Z, MASK, 0, 3, 3, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 9, MASK, X, Z, MASK, 0, 4, 15, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 11, MASK, X, Z, MASK, 0, 5, 11, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 2, MASK, X, Z, MASK, 0, 6, 7, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 4, MASK, X, Z, MASK, 0, 7, 3, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 6, MASK, X, Z, MASK, 0, 8, 11, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 8, MASK, X, Z, MASK, 0, 9, 7, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 10, MASK, X, Z, MASK, 0, 10, 3, false),

   /* 62: Sample processing ALU clause. */

   /* Counter 0, counter 2 subtraction. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(false, 0),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB(true, 2, true),
   /* Counter 1, counter 2 address. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(false, 1),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_W_ADDRESS(true, 2),
   /* Counter 3, counter 5 subtraction. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(false, 3),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB(true, 5, true),
   /* Counter 4, counter 5 address. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(false, 4),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_W_ADDRESS(true, 5),
   /* Counter 6, counter 8 subtraction. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(false, 6),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB(true, 8, true),
   /* Counter 7, counter 8 address. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(false, 7),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_W_ADDRESS(true, 8),
   /* Counter 9. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(true, 9),
   /* Counter 10, result write predicate. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB(false, 10, false),
   TERAKAN_META_QUERY_COPY_ALU_PRED(false, 'Z', VEC_210),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_W_ADDRESS(true, 10),
};

static uint32_t const terakan_meta_query_copy_pipelinestat_32_bit_vs_r9xx[] = {
   /* 0-5: Control flow prolog. */
   TERAKAN_META_QUERY_COPY_CF_BEFORE_STORE_6_QWORDS(true, false, 32, 11, 2 * 11 + 1),

   /* 6-27: Store the result. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 8, 0, false, true),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 10, 1, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 12, 2, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 14, 3, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 16, 4, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 18, 5, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 20, 6, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 22, 7, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 24, 8, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 26, 9, false, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 28, 10, false, false),

   /* 28-31: Control flow epilog. */
   TERAKAN_META_QUERY_COPY_CF_AFTER_STORE_R9XX_4_QWORDS(28),

   /* 32-41: Fetch and process the availability. */
   TERAKAN_META_QUERY_COPY_AVAILABILITY_FETCH_AND_ALU_R9XX_10_QWORDS(0),

   /* 42-63: Fetch the samples. */
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 1, MASK, X, Z, MASK, 0, 0),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 3, MASK, X, Z, MASK, 0, 1),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 5, MASK, X, Z, MASK, 0, 2),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 7, MASK, X, Z, MASK, 0, 3),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 9, MASK, X, Z, MASK, 0, 4),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 11, MASK, X, Z, MASK, 0, 5),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 2, MASK, X, Z, MASK, 0, 6),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 4, MASK, X, Z, MASK, 0, 7),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 6, MASK, X, Z, MASK, 0, 8),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 8, MASK, X, Z, MASK, 0, 9),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 10, MASK, X, Z, MASK, 0, 10),

   /* 64: Sample processing ALU clause. */

   /* Counters 0...9. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(true, 0),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(true, 1),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(true, 2),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(true, 3),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(true, 4),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(true, 5),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(true, 6),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(true, 7),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(true, 8),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB_W_ADDRESS(true, 9),
   /* Counter 10, result write predicate. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_X_SUB(false, 10, false),
   TERAKAN_META_QUERY_COPY_ALU_PRED(false, 'Z', VEC_210),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_32_BIT_W_ADDRESS(true, 10),
};

TERAKAN_META_QUERY_COPY_SHADER(pipelinestat_32_bit, 12)

static uint32_t const terakan_meta_query_copy_pipelinestat_64_bit_vs_r8xx[] = {
   /* 0-5: Control flow prolog. */
   TERAKAN_META_QUERY_COPY_CF_BEFORE_STORE_6_QWORDS(false, true, 32, 11, 5 * 11 + 1),

   /* 6-27: Store the result. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 8, 0, true, true),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 10, 1, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 12, 2, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 14, 3, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 16, 4, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 18, 5, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 20, 6, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 22, 7, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 24, 8, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 26, 9, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(false, 28, 10, true, false),

   /* 28-30: Control flow epilog. */
   TERAKAN_META_QUERY_COPY_CF_AFTER_STORE_R8XX_3_QWORDS(28),

   /* 31 (alignment padding), 32-38: Fetch and process the availability. */
   0,
   0,
   TERAKAN_META_QUERY_COPY_AVAILABILITY_FETCH_AND_ALU_R8XX_7_QWORDS(10),

   /* 39 (alignment padding), 40-61: Fetch the samples. */
   0,
   0,
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 1, X, Y, Z, W, 10, 0, 16, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 3, Y, X, W, Z, 10, 1, 12, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 5, X, Y, Z, W, 10, 2, 8, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 7, Y, X, W, Z, 10, 3, 4, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 9, X, Y, Z, W, 10, 4, 16, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 11, Y, X, W, Z, 10, 5, 12, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 2, X, Y, Z, W, 10, 6, 8, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 4, Y, X, W, Z, 10, 7, 4, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 6, X, Y, Z, W, 10, 8, 12, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 8, Y, X, W, Z, 10, 9, 8, false),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 10, X, Y, Z, W, 10, 10, 4, false),

   /* 62: Sample processing ALU clause. */

   /* 0 lower, borrow, upper without borrow, 10 address (overwriting 0 end). */
   TERAKAN_META_QUERY_COPY_64_BIT_LO_Z_BC_W_HI32(
      false, 1, SUB, B, TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(0),
      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO(0),
      TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(0),
      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(0)),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(
      0, TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_10_TO_RESULT_END) |
      TERAKAN_SHADER_OP2(true, 1 + 10, 'W', SUB_INT, EG, 0, 'W', 0, 0, SCL_210),

   /* N lower, borrow, upper without borrow, N-1 upper with borrow and address (overwriting N
    * beginning or end).
    */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_HI32_ADDRESS_R8XX(1),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_HI32_ADDRESS_R8XX(2),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_HI32_ADDRESS_R8XX(3),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_HI32_ADDRESS_R8XX(4),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_HI32_ADDRESS_R8XX(5),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_HI32_ADDRESS_R8XX(6),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_HI32_ADDRESS_R8XX(7),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_HI32_ADDRESS_R8XX(8),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_HI32_ADDRESS_R8XX(9),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_HI32_ADDRESS_R8XX(10),

   /* 10 upper with borrow, result write predicate. */
   TERAKAN_META_QUERY_COPY_64_BIT_HIWBC(false, 1 + 10, SUB),
   TERAKAN_META_QUERY_COPY_ALU_PRED(true, 'Z', VEC_210),
};

static uint32_t const terakan_meta_query_copy_pipelinestat_64_bit_vs_r9xx[] = {
   /* 0-5: Control flow prolog. */
   TERAKAN_META_QUERY_COPY_CF_BEFORE_STORE_6_QWORDS(true, true, 32, 11, 5 * 11 + 1),

   /* 6-27: Store the result. */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 8, 0, true, true),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 10, 1, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 12, 2, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 14, 3, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 16, 4, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 18, 5, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 20, 6, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 22, 7, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 24, 8, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 26, 9, true, false),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_STORE_COUNTER_CONDITIONALLY(true, 28, 10, true, false),

   /* 28-31: Control flow epilog. */
   TERAKAN_META_QUERY_COPY_CF_AFTER_STORE_R9XX_4_QWORDS(28),

   /* 32-41: Fetch and process the availability. */
   TERAKAN_META_QUERY_COPY_AVAILABILITY_FETCH_AND_ALU_R9XX_10_QWORDS(10),

   /* 42-63: Fetch the samples. */
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 1, X, Y, Z, W, 10, 0),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 3, Y, X, W, Z, 10, 1),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 5, X, Y, Z, W, 10, 2),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 7, Y, X, W, Z, 10, 3),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 9, X, Y, Z, W, 10, 4),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 11, Y, X, W, Z, 10, 5),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 2, X, Y, Z, W, 10, 6),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 4, Y, X, W, Z, 10, 7),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 6, X, Y, Z, W, 10, 8),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 8, Y, X, W, Z, 10, 9),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 10, X, Y, Z, W, 10, 10),

   /* 64: Sample processing ALU clause. */

   /* Upper halves without borrow. */

   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_HI32_TO_TEMP(0, 'X', 0),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_HI32_TO_TEMP(0, 'Y', 1),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_HI32_TO_TEMP(0, 'Z', 2),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_HI32_TO_TEMP(1, 'W', 3),

   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_HI32_TO_TEMP(0, 'X', 4),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_HI32_TO_TEMP(0, 'Y', 5),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_HI32_TO_TEMP(0, 'Z', 6),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_HI32_TO_TEMP(1, 'W', 7),

   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_HI32_TO_TEMP(0, 'X', 8),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_HI32_TO_TEMP(0, 'Y', 9),
   /* Calculate the first counter's borrow early, so instead of:
    * {N lower, N-1 upper with borrow, N borrow, N-1 address}
    * instruction groups can calculate:
    * {N lower, N upper with borrow, N+1 borrow, N-1 address}
    * and one excess instruction group for the last counter's upper half without borrow can be
    * eliminated.
    * The other instructions in this group read GPR ZYW on cycle 0 and YWX on cycle 1, so read W on
    * cycle 2 and X on cycle 0.
    */
   TERAKAN_SHADER_OP2_NW(true, 'Z', SUBB_UINT, EG, TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(0),
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO(0),
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(0),
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(0), VEC_201),
   /* Write the counter 10 upper without borrow to W, not to Z, so predicate setting can read GPR Z
    * on cycle 2 too.
    */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_HI32_TO_TEMP(1, 'W', 10),

/* 0 result, 1 borrow, 10 address (overwriting 0 end). */
#if UTIL_ARCH_BIG_ENDIAN
   TERAKAN_SHADER_OP2(false, 1, 'X', SUB_INT, EG, 0x7F, 'X', V_SQ_ALU_SRC_PV, 'Z', VEC_210),
   TERAKAN_SHADER_OP2(false, 1, 'Y', SUB_INT, EG, TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(0),
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO(0),
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(0),
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(0), VEC_012),
#else
   TERAKAN_SHADER_OP2(false, 1, 'X', SUB_INT, EG, TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(0),
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO(0),
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(0),
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(0), VEC_012),
   TERAKAN_SHADER_OP2(false, 1, 'Y', SUB_INT, EG, 0x7F, 'X', V_SQ_ALU_SRC_PV, 'Z', VEC_210),
#endif
   TERAKAN_SHADER_OP2_NW(false, 'Z', SUB_INT, EG, TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(1),
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO(1),
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(1),
                         TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(1), VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(
      0, TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_10_TO_RESULT_END) |
      TERAKAN_SHADER_OP2(true, 1 + 10, 'W', SUB_INT, EG, 0, 'W', 0, 0, VEC_210),

   /* N result, N+1 borrow, N-1 address (overwriting N beginning or end). */
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_ADDRESS_R9XX(1),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_ADDRESS_R9XX(2),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_ADDRESS_R9XX(3),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_ADDRESS_R9XX(4),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_ADDRESS_R9XX(5),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_ADDRESS_R9XX(6),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_ADDRESS_R9XX(7),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_ADDRESS_R9XX(8),
   TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_LO_HIWB_B_ADDRESS_R9XX(9),

/* 10 result, result write predicate, 9 address. */
#if UTIL_ARCH_BIG_ENDIAN
   TERAKAN_SHADER_OP2(false, 1 + 10, 'X', SUB_INT, EG, 0x7F - 10 / 4, 'W', V_SQ_ALU_SRC_PV, 'Z',
                      VEC_210),
   TERAKAN_SHADER_OP2(false, 1 + 10, 'Y', SUB_INT, EG,
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(10),
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO(10),
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(10),
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(10), VEC_012),
#else
   TERAKAN_SHADER_OP2(false, 1 + 10, 'X', SUB_INT, EG,
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_END_GPR(10),
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_END_LO(10),
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_BEGIN_GPR(10),
                      TERAKAN_META_QUERY_COPY_PIPELINESTAT_64_BIT_BEGIN_LO(10), VEC_012),
   TERAKAN_SHADER_OP2(false, 1 + 10, 'Y', SUB_INT, EG, 0x7F - 10 / 4, 'W', V_SQ_ALU_SRC_PV, 'Z',
                      VEC_210),
#endif
   TERAKAN_META_QUERY_COPY_ALU_PRED(false, 'Z', VEC_210),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0,
                                   TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_8_TO_9) |
      TERAKAN_SHADER_OP2(true, 1 + 9, 'W', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'W', 0, 0, VEC_210),
};

TERAKAN_META_QUERY_COPY_SHADER(pipelinestat_64_bit, 12)

/* Timestamp.
 *
 * VUID-vkCmdCopyQueryPoolResults-queryType-09439:
 *
 *     "If the queryType used to create queryPool was VK_QUERY_TYPE_TIMESTAMP, flags must not
 *     contain VK_QUERY_RESULT_PARTIAL_BIT"
 *
 * Setting the result write predicate directly based on the fetched availability, disregarding
 * VK_QUERY_RESULT_PARTIAL_BIT.
 *
 * Though COPY_DW or CP DMA can be used for timestamp queries because no subtraction is needed,
 * COND_EXEC is not supported by DRM Radeon 2.50.0.
 */

static uint32_t const terakan_meta_query_copy_timestamp_32_bit_vs_r8xx[] = {
   /* 0: Calculate the availability address in the UAV. */
   S_SQ_CF_WORD0_ADDR(9) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(0) | S_SQ_CF_ALU_WORD1_COUNT(1) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch the availability and timestamp. */
   S_SQ_CF_WORD0_ADDR(12),
   S_SQ_CF_WORD1_COUNT(2 - 1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_VTX,

   /* 2: Skip writing the availability if not requested by jumping over the write. */
   S_SQ_CF_WORD0_ADDR(4),
   S_SQ_CF_WORD1_COND(V_SQ_CF_COND_BOOL) |
      S_SQ_CF_WORD1_CF_CONST(TERAKAN_META_QUERY_COPY_BOOL_INDEX_WITH_AVAILABILITY) |
      EG_V_SQ_CF_WORD1_SQ_CF_INST_JUMP,

   /* 3: Write the availability. */
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(false, 0, 1, false, false, true),

   /* 4: Disable the result write if not available, and calculate the result destination addresses.
    */
   S_SQ_CF_WORD0_ADDR(16),
   S_SQ_CF_ALU_WORD1_COUNT(1) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_PUSH_BEFORE,

   /* 5: Write the result. */
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(false, 0, 0, false, false, true),

   /* 6: Restore the active mask without unavailable result writing disabling for the ending export.
    */
   S_SQ_CF_WORD0_ADDR(7),
   S_SQ_CF_WORD1_POP_COUNT(1) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_POP,

   /* 7-8: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_POS_PARAM_DONE_AND_END_R8XX_2_QWORDS,

   /* 9: ALU clause. */

   /* +0: Apply the UAV destination address stride, writing to PS. MULLO_UINT is scalar-only. */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |
      TERAKAN_SHADER_OP2_NW(true, 0, MULLO_UINT, EG, 0, 'X', 0, 0, SCL_210),
   /* +1: Write the result end address in the UAV, which is also where the availability will be
    *     written if needed, to R1.W.
    */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_RESULT_END_OFFSET) |
      TERAKAN_SHADER_OP2(true, 1, 'W', ADD_INT, EG, V_SQ_ALU_SRC_PS, 0, 0, 0, VEC_012),

   /* 11 (alignment padding), 12-13: Fetch the availability to R1.X. */
   0,
   0,
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_NON_PIXEL_STAGE_SPECIFIC) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD0_MEGA_FETCH_COUNT(sizeof(uint32_t) - 1),
   S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DATA_FORMAT(TERASCALE_FORMAT_INDEX_32) |
      S_SQ_VTX_WORD1_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) |
      S_SQ_VTX_WORD1_GPR_DST_GPR(1),
   S_SQ_VTX_WORD2_ENDIAN_SWAP(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                   : TERASCALE_ENDIAN_SWAP_NONE) |
      S_SQ_VTX_WORD2_MEGA_FETCH(true),
   0,

   /* 14-15: Fetch the timestamp to R0.X. */
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD0_MEGA_FETCH_COUNT(sizeof(uint32_t) - 1),
   S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DATA_FORMAT(TERASCALE_FORMAT_INDEX_32) |
      S_SQ_VTX_WORD1_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) |
      S_SQ_VTX_WORD1_GPR_DST_GPR(0),
   S_SQ_VTX_WORD2_ENDIAN_SWAP(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                   : TERASCALE_ENDIAN_SWAP_NONE) |
      S_SQ_VTX_WORD2_MEGA_FETCH(true),
   0,

   /* 16: ALU clause. */

   /* +0: Set the result write predicate.
    * +1: Calculate the result write address to R0.W.
    */
   TERAKAN_SHADER_OP2_NW(false, 'X', PRED_SETNE_INT, EG, 1, 'X', V_SQ_ALU_SRC_0, 0, VEC_012) |
      S_SQ_ALU_WORD1_OP2_UPDATE_EXECUTE_MASK(1),
   TERAKAN_SHADER_OP2(true, 0, 'W', SUB_INT, EG, 1, 'W', V_SQ_ALU_SRC_1_INT, 0, VEC_012),
};

static uint32_t const terakan_meta_query_copy_timestamp_32_bit_vs_r9xx[] = {
   /* 0: Calculate the availability address in the UAV. */
   S_SQ_CF_WORD0_ADDR(10) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(0) | S_SQ_CF_ALU_WORD1_COUNT(4) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch the availability and timestamp. */
   S_SQ_CF_WORD0_ADDR(16),
   S_SQ_CF_WORD1_COUNT(2 - 1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Skip writing the availability if not requested by jumping over the write. */
   S_SQ_CF_WORD0_ADDR(4),
   S_SQ_CF_WORD1_COND(V_SQ_CF_COND_BOOL) |
      S_SQ_CF_WORD1_CF_CONST(TERAKAN_META_QUERY_COPY_BOOL_INDEX_WITH_AVAILABILITY) |
      EG_V_SQ_CF_WORD1_SQ_CF_INST_JUMP,

   /* 3: Write the availability. */
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(true, 0, 1, false, false, true),

   /* 4: Disable the result write if not available, and calculate the result destination addresses.
    */
   S_SQ_CF_WORD0_ADDR(20),
   S_SQ_CF_ALU_WORD1_COUNT(1) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_PUSH_BEFORE,

   /* 5: Write the result. */
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(true, 0, 0, false, false, true),

   /* 6: Restore the active mask without unavailable result writing disabling for the ending export.
    */
   S_SQ_CF_WORD0_ADDR(7),
   S_SQ_CF_WORD1_POP_COUNT(1) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_POP,

   /* 7-9: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_POS_PARAM_DONE_R9XX_2_QWORDS,
   TERAKAN_SHADER_CF_END_R9XX,

   /* 10: ALU clause. */

   /* +0-3: Apply the UAV destination address stride, writing to PV. MULLO_UINT is 4-slot. */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |
      TERAKAN_SHADER_OP2_NW(false, 'X', MULLO_UINT, EG, 0, 'X', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |
      TERAKAN_SHADER_OP2_NW(false, 'Y', MULLO_UINT, EG, 0, 'X', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |
      TERAKAN_SHADER_OP2_NW(false, 'Z', MULLO_UINT, EG, 0, 'X', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |
      TERAKAN_SHADER_OP2_NW(true, 'W', MULLO_UINT, EG, 0, 'X', 0, 0, VEC_012),
   /* +4: Write the result end address in the UAV, which is also where the availability will be
    *     written if needed, to R1.W.
    */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_RESULT_END_OFFSET) |
      TERAKAN_SHADER_OP2(true, 1, 'W', ADD_INT, EG, V_SQ_ALU_SRC_PV, 0, 0, 0, VEC_012),

   /* 15 (alignment padding), 16-17: Fetch the availability to R1.X. */
   0,
   0,
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_NON_PIXEL_STAGE_SPECIFIC) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X),
   S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DATA_FORMAT(TERASCALE_FORMAT_INDEX_32) |
      S_SQ_VTX_WORD1_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) |
      S_SQ_VTX_WORD1_GPR_DST_GPR(1),
   S_SQ_VTX_WORD2_ENDIAN_SWAP(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                   : TERASCALE_ENDIAN_SWAP_NONE),
   0,

   /* 18-19: Fetch the timestamp to R0.X. */
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X),
   S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DATA_FORMAT(TERASCALE_FORMAT_INDEX_32) |
      S_SQ_VTX_WORD1_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) |
      S_SQ_VTX_WORD1_GPR_DST_GPR(0),
   S_SQ_VTX_WORD2_ENDIAN_SWAP(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                   : TERASCALE_ENDIAN_SWAP_NONE),
   0,

   /* 20: ALU clause. */

   /* +0: Set the result write predicate.
    * +1: Calculate the result write address to R0.W.
    */
   TERAKAN_SHADER_OP2_NW(false, 'X', PRED_SETNE_INT, EG, 1, 'X', V_SQ_ALU_SRC_0, 0, VEC_012) |
      S_SQ_ALU_WORD1_OP2_UPDATE_EXECUTE_MASK(1),
   TERAKAN_SHADER_OP2(true, 0, 'W', SUB_INT, EG, 1, 'W', V_SQ_ALU_SRC_1_INT, 0, VEC_012),
};

TERAKAN_META_QUERY_COPY_SHADER(timestamp_32_bit, 2)

static uint32_t const terakan_meta_query_copy_timestamp_64_bit_vs_r8xx[] = {
   /* 0: Calculate the availability address in the UAV. */
   S_SQ_CF_WORD0_ADDR(9) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(0) | S_SQ_CF_ALU_WORD1_COUNT(1) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch the availability and timestamp. */
   S_SQ_CF_WORD0_ADDR(12),
   S_SQ_CF_WORD1_COUNT(2 - 1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_VTX,

   /* 2: Skip writing the availability if not requested by jumping over the write. */
   S_SQ_CF_WORD0_ADDR(4),
   S_SQ_CF_WORD1_COND(V_SQ_CF_COND_BOOL) |
      S_SQ_CF_WORD1_CF_CONST(TERAKAN_META_QUERY_COPY_BOOL_INDEX_WITH_AVAILABILITY) |
      EG_V_SQ_CF_WORD1_SQ_CF_INST_JUMP,

   /* 3: Write the availability. */
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(false, 0, 1, true, false, true),

   /* 4: Disable the result write if not available, and calculate the result destination addresses.
    */
   S_SQ_CF_WORD0_ADDR(16),
   S_SQ_CF_ALU_WORD1_COUNT(2) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_PUSH_BEFORE,

   /* 5: Write the result. */
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(false, 0, 0, true, false, true),

   /* 6: Restore the active mask without unavailable result writing disabling for the ending export.
    */
   S_SQ_CF_WORD0_ADDR(7),
   S_SQ_CF_WORD1_POP_COUNT(1) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_POP,

   /* 7-8: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_POS_PARAM_DONE_AND_END_R8XX_2_QWORDS,

   /* 9: ALU clause. */

   /* +0: Apply the UAV destination address stride, writing to PS. MULLO_UINT is scalar-only. */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |
      TERAKAN_SHADER_OP2_NW(true, 0, MULLO_UINT, EG, 0, 'X', 0, 0, SCL_210),
   /* +1: Write the result end address in the UAV, which is also where the availability will be
    *     written if needed, to R1.W.
    */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_RESULT_END_OFFSET) |
      TERAKAN_SHADER_OP2(true, 1, 'W', ADD_INT, EG, V_SQ_ALU_SRC_PS, 0, 0, 0, VEC_012),

   /* 11 (alignment padding), 12-13: Fetch the availability to R1.X. */
   0,
   0,
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_NON_PIXEL_STAGE_SPECIFIC) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD0_MEGA_FETCH_COUNT(sizeof(uint32_t) - 1),
   S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DATA_FORMAT(TERASCALE_FORMAT_INDEX_32) |
      S_SQ_VTX_WORD1_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) |
      S_SQ_VTX_WORD1_GPR_DST_GPR(1),
   S_SQ_VTX_WORD2_ENDIAN_SWAP(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                   : TERASCALE_ENDIAN_SWAP_NONE) |
      S_SQ_VTX_WORD2_MEGA_FETCH(true),
   0,

   /* 14-15: Fetch the timestamp to R0.X. */
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD0_MEGA_FETCH_COUNT(sizeof(uint32_t) * 2 - 1),
   S_SQ_VTX_WORD1_DST_SEL_X(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_SWIZZLE_Y : TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Y(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_SWIZZLE_X : TERASCALE_SWIZZLE_Y) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DATA_FORMAT(TERASCALE_FORMAT_INDEX_32_32) |
      S_SQ_VTX_WORD1_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) |
      S_SQ_VTX_WORD1_GPR_DST_GPR(0),
   S_SQ_VTX_WORD2_ENDIAN_SWAP(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                   : TERASCALE_ENDIAN_SWAP_NONE) |
      S_SQ_VTX_WORD2_MEGA_FETCH(true),
   0,

   /* 16: ALU clause. */

   /* +0: Set the result write predicate.
    * +1: Calculate the result write address to R0.W.
    * +2: Literal pair.
    */
   TERAKAN_SHADER_OP2_NW(false, 'X', PRED_SETNE_INT, EG, 1, 'X', V_SQ_ALU_SRC_0, 0, VEC_012) |
      S_SQ_ALU_WORD1_OP2_UPDATE_EXECUTE_MASK(1),
   TERAKAN_SHADER_OP2(true, 0, 'W', SUB_INT, EG, 1, 'W', V_SQ_ALU_SRC_LITERAL, 'X', VEC_012),
   2,
   0,
};

static uint32_t const terakan_meta_query_copy_timestamp_64_bit_vs_r9xx[] = {
   /* 0: Calculate the availability address in the UAV. */
   S_SQ_CF_WORD0_ADDR(10) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(0) | S_SQ_CF_ALU_WORD1_COUNT(4) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch the availability and timestamp. */
   S_SQ_CF_WORD0_ADDR(16),
   S_SQ_CF_WORD1_COUNT(2 - 1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Skip writing the availability if not requested by jumping over the write. */
   S_SQ_CF_WORD0_ADDR(4),
   S_SQ_CF_WORD1_COND(V_SQ_CF_COND_BOOL) |
      S_SQ_CF_WORD1_CF_CONST(TERAKAN_META_QUERY_COPY_BOOL_INDEX_WITH_AVAILABILITY) |
      EG_V_SQ_CF_WORD1_SQ_CF_INST_JUMP,

   /* 3: Write the availability. */
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(true, 0, 1, true, false, true),

   /* 4: Disable the result write if not available, and calculate the result destination addresses.
    */
   S_SQ_CF_WORD0_ADDR(20),
   S_SQ_CF_ALU_WORD1_COUNT(2) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_PUSH_BEFORE,

   /* 5: Write the result. */
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(true, 0, 0, true, false, true),

   /* 6: Restore the active mask without unavailable result writing disabling for the ending export.
    */
   S_SQ_CF_WORD0_ADDR(7),
   S_SQ_CF_WORD1_POP_COUNT(1) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_POP,

   /* 7-9: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_POS_PARAM_DONE_R9XX_2_QWORDS,
   TERAKAN_SHADER_CF_END_R9XX,

   /* 10: ALU clause. */

   /* +0-3: Apply the UAV destination address stride, writing to PV. MULLO_UINT is 4-slot. */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |
      TERAKAN_SHADER_OP2_NW(false, 'X', MULLO_UINT, EG, 0, 'X', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |
      TERAKAN_SHADER_OP2_NW(false, 'Y', MULLO_UINT, EG, 0, 'X', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |
      TERAKAN_SHADER_OP2_NW(false, 'Z', MULLO_UINT, EG, 0, 'X', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE) |
      TERAKAN_SHADER_OP2_NW(true, 'W', MULLO_UINT, EG, 0, 'X', 0, 0, VEC_012),
   /* +4: Write the result end address in the UAV, which is also where the availability will be
    *     written if needed, to R1.W.
    */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_QUERY_COPY_CONST_DST_RESULT_END_OFFSET) |
      TERAKAN_SHADER_OP2(true, 1, 'W', ADD_INT, EG, V_SQ_ALU_SRC_PV, 0, 0, 0, VEC_012),

   /* 15 (alignment padding), 16-17: Fetch the availability to R1.X. */
   0,
   0,
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_NON_PIXEL_STAGE_SPECIFIC) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X),
   S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DATA_FORMAT(TERASCALE_FORMAT_INDEX_32) |
      S_SQ_VTX_WORD1_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) |
      S_SQ_VTX_WORD1_GPR_DST_GPR(1),
   S_SQ_VTX_WORD2_ENDIAN_SWAP(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                   : TERASCALE_ENDIAN_SWAP_NONE),
   0,

   /* 18-19: Fetch the timestamp to R0.X. */
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X),
   S_SQ_VTX_WORD1_DST_SEL_X(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_SWIZZLE_Y : TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Y(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_SWIZZLE_X : TERASCALE_SWIZZLE_Y) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_MASK) |
      S_SQ_VTX_WORD1_DATA_FORMAT(TERASCALE_FORMAT_INDEX_32_32) |
      S_SQ_VTX_WORD1_NUM_FORMAT_ALL(TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) |
      S_SQ_VTX_WORD1_GPR_DST_GPR(0),
   S_SQ_VTX_WORD2_ENDIAN_SWAP(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                   : TERASCALE_ENDIAN_SWAP_NONE),
   0,

   /* 20: ALU clause. */

   /* +0: Set the result write predicate.
    * +1: Calculate the result write address to R0.W.
    * +2: Literal pair.
    */
   TERAKAN_SHADER_OP2_NW(false, 'X', PRED_SETNE_INT, EG, 1, 'X', V_SQ_ALU_SRC_0, 0, VEC_012) |
      S_SQ_ALU_WORD1_OP2_UPDATE_EXECUTE_MASK(1),
   TERAKAN_SHADER_OP2(true, 0, 'W', SUB_INT, EG, 1, 'W', V_SQ_ALU_SRC_LITERAL, 'X', VEC_012),
   2,
   0,
};

TERAKAN_META_QUERY_COPY_SHADER(timestamp_64_bit, 2)

/* Stream out statistics. */

static uint32_t const terakan_meta_query_copy_streamoutstats_32_bit_vs_r8xx[] = {
   /* 0-5: Control flow prolog. */
   TERAKAN_META_QUERY_COPY_CF_BEFORE_STORE_6_QWORDS(false, false, 10, 2, 2 + 3),

   /* 6: Store the result. */
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(false, 0, 1, true, false, true),

   /* 7-9: Control flow epilog. */
   TERAKAN_META_QUERY_COPY_CF_AFTER_STORE_R8XX_3_QWORDS(7),

   /* 10-16: Fetch and process the availability. */
   TERAKAN_META_QUERY_COPY_AVAILABILITY_FETCH_AND_ALU_R8XX_7_QWORDS(0),

   /* 17 (alignment padding), 18-21: Fetch the samples. */
   0,
   0,
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 1, X, Z, MASK, MASK, 0, 0, 7, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32, 1, MASK, MASK, X, Z, 0, 1, 3, false),

   /* 22: Sample processing ALU clause. */

   /* Result, result write predicate, address. */
   TERAKAN_SHADER_OP2(false, 1, 'X', SUB_INT, EG, 1, 'X', 1, 'Z', VEC_012),
   TERAKAN_SHADER_OP2(false, 1, 'Y', SUB_INT, EG, 1, 'Y', 1, 'W', VEC_012),
   TERAKAN_META_QUERY_COPY_ALU_PRED(false, 'Z', VEC_210),
   TERAKAN_SHADER_OP2(true, 1, 'W', SUB_INT, EG, 0, 'W', V_SQ_ALU_SRC_LITERAL, 'X', SCL_210),
   2,
   0,
};

static uint32_t const terakan_meta_query_copy_streamoutstats_32_bit_vs_r9xx[] = {
   /* 0-5: Control flow prolog. */
   TERAKAN_META_QUERY_COPY_CF_BEFORE_STORE_6_QWORDS(true, false, 12, 2, 2 + 3),

   /* 6: Store the result. */
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(true, 0, 1, true, false, true),

   /* 7-10: Control flow epilog. */
   TERAKAN_META_QUERY_COPY_CF_AFTER_STORE_R9XX_4_QWORDS(7),

   /* 11 (alignment padding), 12-21: Fetch and process the availability. */
   0,
   0,
   TERAKAN_META_QUERY_COPY_AVAILABILITY_FETCH_AND_ALU_R9XX_10_QWORDS(0),

   /* 22-25: Fetch the samples. */
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 1, X, Z, MASK, MASK, 0, 0),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32, 1, MASK, MASK, X, Z, 0, 1),

   /* 26: Sample processing ALU clause. */

   /* Result, result write predicate, address. */
   TERAKAN_SHADER_OP2(false, 1, 'X', SUB_INT, EG, 1, 'X', 1, 'Z', VEC_012),
   TERAKAN_SHADER_OP2(false, 1, 'Y', SUB_INT, EG, 1, 'Y', 1, 'W', VEC_012),
   TERAKAN_META_QUERY_COPY_ALU_PRED(false, 'Z', VEC_210),
   TERAKAN_SHADER_OP2(true, 1, 'W', SUB_INT, EG, 0, 'W', V_SQ_ALU_SRC_LITERAL, 'X', VEC_210),
   2,
   0,
};

TERAKAN_META_QUERY_COPY_SHADER(streamoutstats_32_bit, 2)

static uint32_t const terakan_meta_query_copy_streamoutstats_64_bit_vs_r8xx[] = {
   /* 0-5: Control flow prolog. */
   TERAKAN_META_QUERY_COPY_CF_BEFORE_STORE_6_QWORDS(false, true, 10, 2, 4 * 2 + 3),

   /* 6: Store the result. */
   TERAKAN_SHADER_CF_UAV(false, STORE_DWORD, 0, 0, 1, 0b1111, false, true),

   /* 7-9: Control flow epilog. */
   TERAKAN_META_QUERY_COPY_CF_AFTER_STORE_R8XX_3_QWORDS(7),

   /* 10-16: Fetch and process the availability. */
   TERAKAN_META_QUERY_COPY_AVAILABILITY_FETCH_AND_ALU_R8XX_7_QWORDS(2),

   /* 17 (alignment padding), 18-21: Fetch the samples. */
   0,
   0,
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 1, X, Y, Z, W, 2, 0, 8, true),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R8XX(_32_32_32_32, 2, X, Y, Z, W, 2, 1, 4, false),

   /* 22: Sample processing ALU clause. */

   /* Borrows, upper halves without borrow, address. */
   TERAKAN_SHADER_OP2_NW(false, 'X', SUBB_UINT, EG, 2, 'X', 1, 'X', VEC_012),
   TERAKAN_SHADER_OP2_NW(false, 'Y', SUB_INT, EG, 2, 'Y', 1, 'Y', VEC_012),
   TERAKAN_SHADER_OP2_NW(false, 'Z', SUBB_UINT, EG, 2, 'Z', 1, 'Z', VEC_012),
   TERAKAN_SHADER_OP2_NW(false, 'W', SUB_INT, EG, 2, 'W', 1, 'W', VEC_012),
   TERAKAN_SHADER_OP2(true, 0, 'X', SUB_INT, EG, 0, 'W', V_SQ_ALU_SRC_LITERAL, 'X', SCL_210),
   4,
   0,

/* Result, result write predicate. */
#if UTIL_ARCH_BIG_ENDIAN
   TERAKAN_SHADER_OP2(false, 1, 'X', SUB_INT, EG, V_SQ_ALU_SRC_PV, 'Y', V_SQ_ALU_SRC_PV, 'X',
                      VEC_012),
   TERAKAN_SHADER_OP2(false, 1, 'Y', SUB_INT, EG, 2, 'X', 1, 'X', VEC_012),
   TERAKAN_SHADER_OP2(false, 1, 'Z', SUB_INT, EG, V_SQ_ALU_SRC_PV, 'W', V_SQ_ALU_SRC_PV, 'Z',
                      VEC_012),
   TERAKAN_SHADER_OP2(false, 1, 'W', SUB_INT, EG, 2, 'Z', 1, 'Z', VEC_012),
#else
   TERAKAN_SHADER_OP2(false, 1, 'X', SUB_INT, EG, 2, 'X', 1, 'X', VEC_012),
   TERAKAN_SHADER_OP2(false, 1, 'Y', SUB_INT, EG, V_SQ_ALU_SRC_PV, 'Y', V_SQ_ALU_SRC_PV, 'X',
                      VEC_012),
   TERAKAN_SHADER_OP2(false, 1, 'Z', SUB_INT, EG, 2, 'Z', 1, 'Z', VEC_012),
   TERAKAN_SHADER_OP2(false, 1, 'W', SUB_INT, EG, V_SQ_ALU_SRC_PV, 'W', V_SQ_ALU_SRC_PV, 'Z',
                      VEC_012),
#endif
   TERAKAN_META_QUERY_COPY_ALU_PRED(true, 'Z', SCL_210),
};

static uint32_t const terakan_meta_query_copy_streamoutstats_64_bit_vs_r9xx[] = {
   /* 0-5: Control flow prolog. */
   TERAKAN_META_QUERY_COPY_CF_BEFORE_STORE_6_QWORDS(true, true, 12, 2, 4 * 2 + 3),

   /* 6: Store the result. */
   TERAKAN_SHADER_CF_UAV(true, STORE_DWORD, 0, 0, 1, 0b1111, false, true),

   /* 7-10: Control flow epilog. */
   TERAKAN_META_QUERY_COPY_CF_AFTER_STORE_R9XX_4_QWORDS(7),

   /* 11 (alignment padding), 12-21: Fetch and process the availability. */
   0,
   0,
   TERAKAN_META_QUERY_COPY_AVAILABILITY_FETCH_AND_ALU_R9XX_10_QWORDS(2),

   /* 22-25: Fetch the samples. */
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 1, X, Y, Z, W, 2, 0),
   TERAKAN_META_QUERY_COPY_FETCH_SAMPLES_R9XX(_32_32_32_32, 2, X, Y, Z, W, 2, 1),

   /* 26: Sample processing ALU clause. */

   /* Borrows, upper halves without borrow. */
   TERAKAN_SHADER_OP2_NW(0, 'X', SUBB_UINT, EG, 2, 'X', 1, 'X', VEC_012),
   TERAKAN_SHADER_OP2_NW(0, 'Y', SUB_INT, EG, 2, 'Y', 1, 'Y', VEC_012),
   TERAKAN_SHADER_OP2_NW(0, 'Z', SUBB_UINT, EG, 2, 'Z', 1, 'Z', VEC_012),
   TERAKAN_SHADER_OP2_NW(1, 'W', SUB_INT, EG, 2, 'W', 1, 'W', VEC_012),

/* Result. */
#if UTIL_ARCH_BIG_ENDIAN
   TERAKAN_SHADER_OP2(0, 1, 'X', SUB_INT, EG, V_SQ_ALU_SRC_PV, 'Y', V_SQ_ALU_SRC_PV, 'X', VEC_012),
   TERAKAN_SHADER_OP2(0, 1, 'Y', SUB_INT, EG, 2, 'X', 1, 'X', VEC_012),
   TERAKAN_SHADER_OP2(0, 1, 'Z', SUB_INT, EG, V_SQ_ALU_SRC_PV, 'W', V_SQ_ALU_SRC_PV, 'Z', VEC_012),
   TERAKAN_SHADER_OP2(1, 1, 'W', SUB_INT, EG, 2, 'Z', 1, 'Z', VEC_012),
#else
   TERAKAN_SHADER_OP2(0, 1, 'X', SUB_INT, EG, 2, 'X', 1, 'X', VEC_012),
   TERAKAN_SHADER_OP2(0, 1, 'Y', SUB_INT, EG, V_SQ_ALU_SRC_PV, 'Y', V_SQ_ALU_SRC_PV, 'X', VEC_012),
   TERAKAN_SHADER_OP2(0, 1, 'Z', SUB_INT, EG, 2, 'Z', 1, 'Z', VEC_012),
   TERAKAN_SHADER_OP2(1, 1, 'W', SUB_INT, EG, V_SQ_ALU_SRC_PV, 'W', V_SQ_ALU_SRC_PV, 'Z', VEC_012),
#endif

   /* Address, result write predicate. */
   TERAKAN_SHADER_OP2(false, 0, 'X', SUB_INT, EG, 0, 'W', V_SQ_ALU_SRC_LITERAL, 'X', VEC_210),
   TERAKAN_META_QUERY_COPY_ALU_PRED(true, 'Z', VEC_210),
   4,
   0,
};

TERAKAN_META_QUERY_COPY_SHADER(streamoutstats_64_bit, 3)

void
terakan_meta_query_copy_init_offsets(VkQueryPipelineStatisticFlags const flags,
                                     int8_t * const offsets_32_bit_out,
                                     int8_t * const offsets_64_bit_out)
{
   /* Initialize `32_BIT_DST_#_TO_RESULT_END`, which are subtracted from the end (availability)
    * address.
    *
    * These are indexed by hardware counter, not by VkQueryPipelineStatisticFlags bit: that is the
    * order SAMPLE_PIPELINESTAT writes the samples in, the order the copy shader walks them in, and
    * the order the 64-bit chaining below assumes. The two orders are not the same -- the hardware
    * writes the fragment-shader counter first and the input-assembly ones near the end -- so
    * indexing this by the flag bit put every counter at the offset belonging to some unrelated one.
    *
    * The destination order is still the flag-bit order the specification requires, because
    * u_foreach_bit walks the flags from the lowest bit up and hands out the offsets from the
    * furthest-from-the-end slot down.
    */
   for (unsigned counter_index = 0; counter_index < TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_COUNT;
        ++counter_index) {
      /* A counter that is not enabled needs any offset that points within the boundaries of the
       * result, so that calculations involving it stay safe.
       */
      offsets_32_bit_out[counter_index] = 1;
   }
   unsigned end_to_next_vk_counter = util_bitcount((uint32_t)flags);
   u_foreach_bit (pipelinestat_vk_index, (uint32_t)flags) {
      offsets_32_bit_out[terakan_query_pipelinestat_vk_hw_counters[pipelinestat_vk_index]] =
         (int8_t)end_to_next_vk_counter--;
   }

   /* Initialize the 64-bit result offsets in dwords, which start from the counter 10's address
    * subtracted from the end address, and then chained, each being relative to the address of the
    * previous counter.
    */
   offsets_64_bit_out[TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_10_TO_RESULT_END -
                      TERAKAN_META_QUERY_COPY_CONSTS_PIPELINESTAT_DST_COUNTER_OFFSETS] =
      2 * offsets_32_bit_out[TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_COUNT - 1];
   for (unsigned counter_index = 0; counter_index < TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_COUNT - 1;
        ++counter_index) {
      /* (N-1)%11 to N = (end - N to end) - (end - (N-1)%11 to end) = (N-1)%11 to end - N to end */
      offsets_64_bit_out[TERAKAN_META_QUERY_COPY_CONST_PIPELINESTAT_64_BIT_DST_10_TO_0 -
                         TERAKAN_META_QUERY_COPY_CONSTS_PIPELINESTAT_DST_COUNTER_OFFSETS +
                         counter_index] =
         2 *
         (offsets_32_bit_out[(counter_index + (TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_COUNT - 1)) %
                             TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_COUNT] -
          offsets_32_bit_out[counter_index]);
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdCopyQueryPoolResults(VkCommandBuffer const commandBuffer, VkQueryPool const queryPool,
                                uint32_t firstQuery, uint32_t queryCount, VkBuffer const dstBuffer,
                                VkDeviceSize const dstOffset, VkDeviceSize const stride,
                                VkQueryResultFlags const flags)
{
   struct terakan_command_buffer * const command_buffer =
      terakan_command_buffer_from_handle(commandBuffer);
   struct terakan_gfx_command_writer * const command_writer = command_buffer->command_writer.gfx;
   struct terakan_physical_device const * const physical_device =
      terakan_gfx_command_writer_physical_device(command_writer);

   /* The destination is an Evergreen-shaped UAV and the query-copy meta shaders have not been
    * validated on R600/R700. Keep the command-side query API behind the same explicit safety
    * boundary as the rest of TeraScale 1 submission; host-side query results remain supported. */
   if (physical_device->chip_info.is_terascale_1) {
      vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_FEATURE_NOT_PRESENT);
      return;
   }

   struct terakan_query_pool const * const query_pool = terakan_query_pool_from_handle(queryPool);

   bool const is_64_bit = (flags & VK_QUERY_RESULT_64_BIT) != 0;

   enum terakan_meta_shader_index vs_index;
   switch (query_pool->vk.query_type) {
   case VK_QUERY_TYPE_OCCLUSION:
      vs_index = (is_64_bit ? TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_1_RB_VS
                            : TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_1_RB_VS) +
                 physical_device->chip_info.max_render_backends_log2;
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      vs_index = is_64_bit ? TERAKAN_META_SHADER_QUERY_COPY_PIPELINESTAT_64_BIT_VS
                           : TERAKAN_META_SHADER_QUERY_COPY_PIPELINESTAT_32_BIT_VS;
      break;
   case VK_QUERY_TYPE_TIMESTAMP:
      vs_index = is_64_bit ? TERAKAN_META_SHADER_QUERY_COPY_TIMESTAMP_64_BIT_VS
                           : TERAKAN_META_SHADER_QUERY_COPY_TIMESTAMP_32_BIT_VS;
      break;
   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT:
      vs_index = is_64_bit ? TERAKAN_META_SHADER_QUERY_COPY_STREAMOUTSTATS_64_BIT_VS
                           : TERAKAN_META_SHADER_QUERY_COPY_STREAMOUTSTATS_32_BIT_VS;
      break;
   default:
      assert(!"Unsupported query type");
      return;
   }

   bool const with_availability = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;

   VkDeviceSize const dst_result_and_availability_dwords =
      (query_pool->copy_dst_result_size_counters + (unsigned)with_availability)
      << (unsigned)is_64_bit;

   struct terakan_buffer const * const dst_buffer = terakan_buffer_from_handle(dstBuffer);

   /* Prevent inconsistencies, such as mixing aligned and misaligned values or division by zero, in
    * calculations related to maintaining #MemoryIntegrity in case of invalid usage.
    *
    * VUID-vkCmdCopyQueryPoolResults-dstBuffer-00825:
    *     "dstBuffer must have been created with VK_BUFFER_USAGE_TRANSFER_DST_BIT usage flag"
    *
    * VUID-vkCmdCopyQueryPoolResults-flags-00822:
    *     "If VK_QUERY_RESULT_64_BIT is not set in flags then dstOffset and stride must be multiples
    *     of 4"
    *
    * VUID-vkCmdCopyQueryPoolResults-flags-00823:
    *     "If VK_QUERY_RESULT_64_BIT is set in flags then dstOffset and stride must be multiples
    *     of 8"
    * However, 4 if sufficient because a UAV with 4 bytes per element is used for the destination.
    *
    * VUID-vkCmdCopyQueryPoolResults-queryCount-09438:
    *     "If queryCount is greater than 1, stride must not be zero"
    */
   if (unlikely(((dst_buffer->va | dstOffset) & (sizeof(uint32_t) - 1)) != 0)) {
      vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_VALIDATION_FAILED_EXT);
      return;
   }
   VkDeviceSize dst_stride_dwords;
   if (queryCount > 1) {
      if (unlikely(stride == 0 || (stride & (sizeof(uint32_t) - 1)) != 0)) {
         vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_VALIDATION_FAILED_EXT);
         return;
      }
      dst_stride_dwords = stride / sizeof(uint32_t);
   } else {
      dst_stride_dwords = 0;
   }

   /* #MemoryIntegrity: prevent out-of-bounds access to the destination and the source.
    * Some of these cases may be valid if `queryCount` is 0 though, but this doesn't set
    * `VK_ERROR_VALIDATION_FAILED_EXT` for graceful degradation anyway.
    */
   VkDeviceSize const dst_offset_dwords = dstOffset / sizeof(uint32_t);
   VkDeviceSize const dst_buffer_size_dwords = dst_buffer->vk.size / sizeof(uint32_t);
   if (unlikely(dst_offset_dwords > dst_buffer_size_dwords ||
                dst_buffer_size_dwords - dst_offset_dwords < dst_result_and_availability_dwords)) {
      return;
   }
   if (queryCount > 1) {
      VkDeviceSize const dst_max_query_count =
         1u + (dst_buffer_size_dwords - dst_offset_dwords - dst_result_and_availability_dwords) /
                 dst_stride_dwords;
      queryCount = (uint32_t)MIN2(dst_max_query_count, queryCount);
   }
   terakan_query_pool_clamp_range(query_pool, &firstQuery, &queryCount);

   /* `queryCount == 0` is valid usage, so don't invalidate anything in the hardware state
    * configuration if there's nothing to do.
    */
   if (unlikely(queryCount == 0)) {
      return;
   }

   /* Meta shaders share the VS SQ resource registers with application vertex shaders. Preserve
    * the application bindings because the setters below also update the driver's tracked state;
    * merely making the application stage pending would otherwise re-emit the query buffers on the
    * next draw.
    */
   uint32_t const samples_resource_index =
      TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META;
   uint32_t const availability_resource_index =
      TERAKAN_RESOURCE_RANGE_NON_PIXEL_STAGE_SPECIFIC;
   struct terakan_hw_config_sqk_stage * const vs_sqk_stage =
      &command_writer->hw_config_sqk.stages_[MESA_SHADER_VERTEX];
   bool const saved_samples_resource_bound =
      BITSET_TEST(vs_sqk_stage->resources_bound, samples_resource_index);
   bool const saved_availability_resource_bound =
      BITSET_TEST(vs_sqk_stage->resources_bound, availability_resource_index);
   struct terakan_hw_config_sqk_resource saved_samples_resource;
   struct terakan_hw_config_sqk_resource saved_availability_resource;
   if (saved_samples_resource_bound) {
      saved_samples_resource = command_writer->hw_config_sqk.resources_vs_[samples_resource_index];
   }
   if (saved_availability_resource_bound) {
      saved_availability_resource =
         command_writer->hw_config_sqk.resources_vs_[availability_resource_index];
   }

   struct terakan_meta_config_draw_begin_options const meta_begin_options = {
      .vgt_primitive_type = V_008958_DI_PT_POINTLIST,
      .cb_and_db_shader_control_mode = TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_UAV_ONLY,
   };
   terakan_meta_config_draw_begin(command_writer, &meta_begin_options);
   terakan_meta_config_draw_set_sq_pgm_vs(command_writer, vs_index);

   uint32_t bool_const = 0b0;
   if (with_availability) {
      bool_const |= BITFIELD_BIT(TERAKAN_META_QUERY_COPY_BOOL_INDEX_WITH_AVAILABILITY);
   }
   if (query_pool->vk.query_type == VK_QUERY_TYPE_PIPELINE_STATISTICS) {
      bool_const |= (uint32_t)query_pool->pipelinestat_hw_counters
                    << TERAKAN_META_QUERY_COPY_BOOL_INDEX_PIPELINESTAT_0;
   }
   /* Bool constants are used only by meta shaders. */
   /* TODO(Triang3l): `USE_LS_CONSTS`. */
   terakan_hw_config_draw_set_sq_bool_const_vses(&command_writer->hw_config_draw, bool_const);

   uint32_t * packet;

   /* Always assuming `VK_QUERY_RESULT_WAIT_BIT` because there's no concept of a partial
    * `EVENT_WRITE`, and the buffer resource fetch cache needs to be flushed for the latest value to
    * be visible to the copy shader.
    */
   /* TODO(Triang3l): Only insert the barrier if a query was reset or ended in the indirect buffer
    * without a barrier so far.
    */
   /* TODO(Triang3l): Integrate into the barrier infrastructure. */
   terakan_barrier_emit_actions_unconditionally(command_writer, TERAKAN_BARRIER_ACTION_INV_VC);

   uint32_t const src_samples_size_bytes = sizeof(uint64_t) * query_pool->samples_size_counters;

   /* Buffer resource size limit is 2^32 bytes, thus 2^(32-3) counters.
    * Availability is also bound as a buffer resource, but it's smaller than samples.
    */
   uint32_t const src_resource_max_queries = ((uint32_t)1 << (32 - 3)) / src_samples_size_bytes;

   uint64_t src_samples_va =
      query_pool->bo->va + terakan_query_pool_samples_offset_bytes(query_pool, firstQuery);
   struct terakan_resource_descriptor src_samples_resource = {
      .resource = {
         [2] = S_030008_STRIDE(src_samples_size_bytes),
         [3] = S_03000C_DST_SEL_X(TERASCALE_SWIZZLE_X) | S_03000C_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
               S_03000C_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_03000C_DST_SEL_W(TERASCALE_SWIZZLE_W),
         [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
         [TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_QUERY,
      }};

   uint64_t src_availability_va =
      query_pool->bo->va + terakan_query_pool_availability_offset_bytes(query_pool, firstQuery);
   struct terakan_resource_descriptor src_availability_resource = {
      .resource = {
         [2] = S_030008_STRIDE(sizeof(uint32_t)),
         [3] = S_03000C_DST_SEL_X(TERASCALE_SWIZZLE_X) | S_03000C_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
               S_03000C_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_03000C_DST_SEL_W(TERASCALE_SWIZZLE_W),
         [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
         [TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_QUERY,
      }};

   uint64_t dst_va = dst_buffer->va + dstOffset;
   struct terakan_color_descriptor dst_uav = {
      .info = S_028C70_ENDIAN(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                   : TERASCALE_ENDIAN_SWAP_NONE) |
              S_028C70_FORMAT(TERASCALE_FORMAT_INDEX_32) |
              S_028C70_NUMBER_TYPE(TERASCALE_FORMAT_NUMBER_TYPE_UINT) |
              TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_INFO_CONST_FIELDS,
      .attrib = TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_ATTRIB,
   };
   /* PITCH_TILE_MAX and SLICE_TILE_MAX mean nothing to the hardware for a buffer UAV, but DRM
    * Radeon validates the colour surface from them whether or not it is one, and rejects the whole
    * submission when the pitch they describe is not a multiple of 64. Left at zero they describe a
    * pitch of 8, so every vkCmdCopyQueryPoolResults was rejected outright. Unlike base and dim
    * below, they do not depend on the batch, so they are filled in once here.
    */
   terakan_color_descriptor_calculate_buffer_pitch_slice(&dst_uav, sizeof(uint32_t),
                                                         physical_device);

   uint32_t const constants_size_bytes =
      sizeof(uint32_t) * (query_pool->vk.query_type == VK_QUERY_TYPE_PIPELINE_STATISTICS
                             ? TERAKAN_META_QUERY_COPY_CONSTS_COUNT_PIPELINESTAT
                             : TERAKAN_META_QUERY_COPY_CONSTS_COUNT_COMMON);

   struct terakan_bo const * constants_bo = NULL;

   uint32_t constants_uav_alignment_offset_dwords = 0;

   int8_t const * const constants_dst_counter_offsets =
      is_64_bit ? query_pool->pipelinestat_copy_offsets_64_bit
                : query_pool->pipelinestat_copy_offsets_32_bit;

   /* TODO(Triang3l): Batch multiple queries, carefully avoiding integer overflow.
    * Batch size limits:
    * - Actual remaining query count (already clamped).
    * - Source resource: 2^32 bytes (2^29 / sample counter count).
    * - Destination UAV: 2^32 elements.
    *   1 + (2^32 - UAV alignment offset dwords - result and availability dwords) / stride
    *   (if stride is not 0, which, with valid usage, may be the case for 1 query).
    */
   for (uint32_t query_index = 0; query_index < queryCount;) {
      /* Limit the batch size to prevent integer overflow in sizes and addresses on the GPU. */

      uint32_t batch_query_count = MIN2(queryCount - query_index, src_resource_max_queries);

      uint64_t const dst_va_aligned =
         dst_va >> physical_device->tiling_info.pipe_interleave_bytes_log2
                      << physical_device->tiling_info.pipe_interleave_bytes_log2;
      uint32_t const dst_uav_alignment_offset_dwords =
         (uint32_t)(dst_va - dst_va_aligned) / sizeof(uint32_t);
      if (batch_query_count > 1) {
         /* Buffer UAV size limit is 2^32 elements (32-bit in this case).
          * In the batch, include the last query that may be written within the UAV boundaries, plus
          * all the queries preceding it that can fit in the destination UAV given the stride.
          * `VkDeviceSize`, not `uint32_t`, because this limit may end up being 2^32 if the result
          * is a single dword (32-bit timestamp or one pipeline statistic counter).
          */
         VkDeviceSize const dst_uav_max_queries =
            (VkDeviceSize)1 +
            (uint32_t)(((uint64_t)1 << 32) -
                       (dst_uav_alignment_offset_dwords + dst_result_and_availability_dwords)) /
               dst_stride_dwords;
         batch_query_count = (uint32_t)MIN2((VkDeviceSize)batch_query_count, dst_uav_max_queries);
      }

      /* Source samples resource. */
      src_samples_resource.resource[0] = (uint32_t)src_samples_va;
      src_samples_resource.resource[2] =
         (src_samples_resource.resource[2] & C_030008_BASE_ADDRESS_HI) |
         S_030008_BASE_ADDRESS_HI(src_samples_va >> 32);
      src_samples_resource.resource[1] =
         (uint32_t)((uint64_t)src_samples_size_bytes * batch_query_count - 1);
      terakan_hw_config_sqk_set_resource_vs(&command_writer->hw_config_sqk,
                                            samples_resource_index,
                                            query_pool->bo, &src_samples_resource);

      /* Source availability resource. */
      src_availability_resource.resource[0] = (uint32_t)src_availability_va;
      src_availability_resource.resource[2] =
         (src_availability_resource.resource[2] & C_030008_BASE_ADDRESS_HI) |
         S_030008_BASE_ADDRESS_HI(src_availability_va >> 32);
      /* 32-bit multiplication by 4 is sufficient due to `batch_query_count` limiting to no more
       * than 2^(32-3).
       */
      src_availability_resource.resource[1] = (uint32_t)sizeof(uint32_t) * batch_query_count - 1;
      terakan_hw_config_sqk_set_resource_vs(&command_writer->hw_config_sqk,
                                            availability_resource_index,
                                            query_pool->bo, &src_availability_resource);

      /* Destination UAV. */
      dst_uav.base = (uint32_t)(dst_va_aligned >> 8);
      /* `DIM` is element count minus 1. */
      dst_uav.dim =
         (uint32_t)(dst_uav_alignment_offset_dwords + dst_result_and_availability_dwords - 1 +
                    dst_stride_dwords * (batch_query_count - 1));
      terakan_meta_config_draw_set_cb_uav(command_writer, 0, dst_buffer->bo, &dst_uav);

      /* Push constants. */
      if (constants_uav_alignment_offset_dwords != dst_uav_alignment_offset_dwords) {
         constants_bo = NULL;
         constants_uav_alignment_offset_dwords = dst_uav_alignment_offset_dwords;
      }
      if (constants_bo == NULL) {
         uint32_t constants_va_lines;
         uint32_t * const constants = terakan_push_buffer_allocate_kcache(
            command_writer->base.command_buffer, constants_size_bytes, &constants_bo,
            &constants_va_lines);
         if (unlikely(constants == NULL)) {
            goto restore_resources;
         }
         constants[TERAKAN_META_QUERY_COPY_CONST_PARTIAL] =
            (flags & VK_QUERY_RESULT_PARTIAL_BIT) ? UINT32_MAX : 0;
         constants[TERAKAN_META_QUERY_COPY_CONST_DST_STRIDE] = dst_stride_dwords;
         constants[TERAKAN_META_QUERY_COPY_CONST_DST_RESULT_END_OFFSET] =
            constants_uav_alignment_offset_dwords +
            (query_pool->copy_dst_result_size_counters << (unsigned)is_64_bit);
         if (query_pool->vk.query_type == VK_QUERY_TYPE_PIPELINE_STATISTICS) {
            for (unsigned counter_index = 0;
                 counter_index < TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_COUNT; ++counter_index) {
               constants[TERAKAN_META_QUERY_COPY_CONSTS_PIPELINESTAT_DST_COUNTER_OFFSETS +
                         counter_index] =
                  (uint32_t)(int32_t)constants_dst_counter_offsets[counter_index];
            }
         }
         terakan_meta_config_draw_set_kcache_push_constant_buffer_vs(
            command_writer, DIV_ROUND_UP(constants_size_bytes, TERAKAN_KCACHE_HW_LINE_BYTES),
            constants_bo, constants_va_lines);
      }

      terakan_meta_draw_auto(command_writer, batch_query_count, 1);

      query_index += batch_query_count;
      src_samples_va += (uint64_t)src_samples_resource.resource[1] + 1;
      src_availability_va += src_availability_resource.resource[1] + 1;
      dst_va = dst_va_aligned + sizeof(uint32_t) * ((VkDeviceSize)dst_uav.dim + 1);
   }

   /* The copy is a transfer to a buffer, so making its writes visible to whatever reads them next
    * is the application's barrier to insert. What is not the application's problem, and what this
    * SURFACE_SYNC does not cover, is the query pool on the source side: the samples and the
    * availability are read here by a vertex shader, and rewritten by vkCmdResetQueryPool,
    * vkCmdBeginQuery and vkCmdEndQuery from the ME, which runs far ahead of it. Raise a pending
    * VS partial flush so that the next of those waits for this copy to finish fetching -- see
    * terakan_query_sync_before_pool_write. Deferring it rather than waiting here keeps a run of
    * back-to-back copies down to a single wait, and costs nothing at all when the pool is never
    * reused.
    */
   command_writer->pending_barrier_actions |= TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_VS;
   {
      packet = terakan_gfx_command_writer_emit(command_writer,
                                               TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
      if (unlikely(packet == NULL)) {
         goto restore_resources;
      }
      *packet++ = PKT3(PKT3_SURFACE_SYNC, 4 - 1, 0);
      *packet++ = S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_SMX_ACTION_ENA(1) |
                  TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME;
      *packet++ = UINT32_MAX;
      *packet++ = 0;
      *packet++ = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }

restore_resources:
   terakan_hw_config_sqk_set_resource_vs(
      &command_writer->hw_config_sqk, samples_resource_index,
      saved_samples_resource_bound ? saved_samples_resource.bo : NULL,
      saved_samples_resource_bound ? &saved_samples_resource.descriptor : NULL);
   terakan_hw_config_sqk_set_resource_vs(
      &command_writer->hw_config_sqk, availability_resource_index,
      saved_availability_resource_bound ? saved_availability_resource.bo : NULL,
      saved_availability_resource_bound ? &saved_availability_resource.descriptor : NULL);
}
