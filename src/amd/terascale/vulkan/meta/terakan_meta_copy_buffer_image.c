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

#include "terakan_meta_impl.h"

#include "terakan_buffer.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_physical_device.h"

#include "util/macros.h"
#include "vk_format.h"

#include <assert.h>
#include <string.h>

/* Buffer to image copying by drawing to an RTV.
 * Not using an image UAV because "Tex2D UAV on cypress will fail/hang if tile mode is linear"
 * according to the R800 AddrLib.
 *
 * Image to buffer copying by drawing and writing to a UAV.
 *
 * Note that currently buffer memory footprints larger than 4 GB are not supported (that would
 * require splitting copy regions along Z and Y in case of overflow due to large buffer row lengths
 * and image heights). However, buffers larger than 4 GB are currently not supported by Terakan at
 * all for other reasons (including kernel driver allocation size limits).
 */

/* TODO(Triang3l): Cacheless UAV writes. */

enum {
   /* All values are in blocks. */

   /* The offsets are signed. */
   TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET,
   TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y,

   /* The pitches are unsigned. */
   TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH,
   TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH,

   TERAKAN_META_COPY_BUFFER_IMAGE_CONSTS_COUNT,
};

/* clang-format off */

#define TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_R8XX                                         \
   /* +0-2: Convert from image XY to buffer XY, and apply the layer pitch to the layer index.      \
    *                                                                                              \
    *     PV.X = SUB_INT R0.X, CB[push].image_offset_x_minus_buffer_offset                         \
    *     PV.Y = SUB_INT R0.Y, CB[push].image_offset_y                                             \
    * (T) PS (Z) = MULLO_UINT CB[push].buffer_z_pitch, R0.Z                                        \
    * Cycle 0: X = R0, Y = R0, T constant.                                                         \
    * Cycle 1: Z = R0.                                                                             \
    */                                                                                             \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(                                                                \
      0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET) |                \
      TERAKAN_SHADER_OP2_NW(false, 'X', SUB_INT, EG, 0, 'X', 0, 0, VEC_012),                       \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y) |       \
      TERAKAN_SHADER_OP2_NW(false, 'Y', SUB_INT, EG, 0, 'Y', 0, 0, VEC_012),                       \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(true, 'Z', MULLO_UINT, EG, 0, 0, 0, 'Z', SCL_210),                     \
                                                                                                   \
   /* +3-4: Apply the layer offset to the address, and apply the row pitch to the row index.       \
    *                                                                                              \
    *     PV.X = ADD_INT PV.X, PS                                                                  \
    * (T) PS (Y) = MULLO_UINT CB[push].buffer_y_pitch, PV.Y                                        \
    * Cycle 0: T constant.                                                                         \
    */                                                                                             \
   TERAKAN_SHADER_OP2_NW(false, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_PS, 0,        \
                         VEC_012),                                                                 \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(true, 'Y', MULLO_UINT, EG, 0, 0, V_SQ_ALU_SRC_PV, 'Y', SCL_210),       \
                                                                                                   \
   /* +5: Apply the row offset to the address written to R0.X.                                     \
    *                                                                                              \
    * (v) R0.X = ADD_INT PV.X, PS                                                                  \
    */                                                                                             \
   TERAKAN_SHADER_OP2(true, 0, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_PS, 0, VEC_012)

/* clang-format on */

#define TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_COUNT_R8XX 5

static uint32_t const terakan_meta_copy_buffer_to_image_ps_r8xx[] = {
   /* 0: Address calculation. */
   S_SQ_CF_WORD0_ADDR(6) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(
      TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET)) |
      S_SQ_CF_ALU_WORD1_COUNT(TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_COUNT_R8XX) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source buffer. */
   S_SQ_CF_WORD0_ADDR(4),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export the color and end the program. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3 (alignment padding), 4-5: Vertex-fetch from the source buffer to R0. */
   0,
   0,
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD0_MEGA_FETCH_COUNT(16 - 1),
   S_SQ_VTX_WORD1_GPR_DST_GPR(0) | S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W) | S_SQ_VTX_WORD1_USE_CONST_FIELDS(true),
   S_SQ_VTX_WORD2_MEGA_FETCH(true),
   0,

   /* 6: Address calculation ALU clause. */
   TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_R8XX,
};

static uint32_t const terakan_meta_copy_image_to_buffer_ps_r8xx[] = {
   /* 0: Loading the array layer index (may exceed the maximum number of attachment layers for
    * images without attachment usage enabled).
    */
   S_SQ_CF_WORD0_ADDR(5),
   S_SQ_CF_ALU_WORD1_COUNT(0) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source texture. */
   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Address calculation. */
   S_SQ_CF_WORD0_ADDR(8) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(
      TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET)) |
      S_SQ_CF_ALU_WORD1_COUNT(TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_COUNT_R8XX) |
      S_SQ_CF_ALU_WORD1_BARRIER(true) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 3: Write to the UAV. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 0, 1, 0xF, true, true),

   /* 4: Perform a dummy export and end the program. */
   TERAKAN_SHADER_CF_PS_DUMMY_EXPORT_DONE_AND_END_R8XX,

   /* 5: ALU clause. */

   /* +0: Load the array layer index.
    *
    * (V) INTERP_LOAD_P0 R0.Z, Param0.X
    */
   TERAKAN_SHADER_OP1(true, 0, 'Z', INTERP_LOAD_P0, EG, V_SQ_ALU_SRC_PARAM_BASE, 'X', VEC_012),

   /* 6-7: Fetch from the source texture to R1. */
   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(1) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,

   /* 8: Address calculation ALU clause. */
   TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_R8XX,
};

/* clang-format off */

#define TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_R9XX                                         \
   /* +0-1: Convert from image XY to buffer XY, and apply the layer pitch to the layer index.      \
    *                                                                                              \
    * R0.X = SUB_INT R0.X, CB[push].image_offset_x_minus_buffer_offset                             \
    * PV.Y = SUB_INT R0.Y, CB[push].image_offset_y                                                 \
    * Cycle 0: X = R0, Y = R0.                                                                     \
    */                                                                                             \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(                                                                \
      0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET) |                \
      TERAKAN_SHADER_OP2(false, 0, 'X', SUB_INT, EG, 0, 'X', 0, 0, VEC_012),                       \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y) |       \
      TERAKAN_SHADER_OP2_NW(true, 'Y', SUB_INT, EG, 0, 'Y', 0, 0, VEC_012),                        \
                                                                                                   \
   /* +2-5: Apply the row pitch to the row index.                                                  \
    *                                                                                              \
    * MULLO_UINT uses 4 slots.                                                                     \
    * PV.X = MULLO_UINT CB[push].buffer_y_pitch, PV.Y                                              \
    * R0.Y = MULLO_UINT CB[push].buffer_y_pitch, PV.Y                                              \
    * PV.Z = MULLO_UINT CB[push].buffer_y_pitch, PV.Y                                              \
    * PV.W = MULLO_UINT CB[push].buffer_y_pitch, PV.Y                                              \
    */                                                                                             \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(false, 'X', MULLO_UINT, EG, 0, 0, V_SQ_ALU_SRC_PV, 'Y', VEC_012),      \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH) |       \
      TERAKAN_SHADER_OP2(false, 0, 'Y', MULLO_UINT, EG, 0, 0, V_SQ_ALU_SRC_PV, 'Y', VEC_012),      \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(false, 'Z', MULLO_UINT, EG, 0, 0, V_SQ_ALU_SRC_PV, 'Y', VEC_012),      \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(true, 'W', MULLO_UINT, EG, 0, 0, V_SQ_ALU_SRC_PV, 'Y', VEC_012),       \
                                                                                                   \
   /* +6-9: Apply the layer pitch to the layer index.                                              \
    *                                                                                              \
    * MULLO_UINT uses 4 slots.                                                                     \
    * PV.X = MULLO_UINT CB[push].buffer_z_pitch, R0.Z                                              \
    * PV.Y = MULLO_UINT CB[push].buffer_z_pitch, R0.Z                                              \
    * PV.Z = MULLO_UINT CB[push].buffer_z_pitch, R0.Z                                              \
    * PV.W = MULLO_UINT CB[push].buffer_z_pitch, R0.Z                                              \
    */                                                                                             \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(false, 'X', MULLO_UINT, EG, 0, 0, 0, 'Z', VEC_012),                    \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(false, 'Y', MULLO_UINT, EG, 0, 0, 0, 'Z', VEC_012),                    \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(false, 'Z', MULLO_UINT, EG, 0, 0, 0, 'Z', VEC_012),                    \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(true, 'W', MULLO_UINT, EG, 0, 0, 0, 'Z', VEC_012),                     \
                                                                                                   \
   /* +10: Apply the layer offset to the address.                                                  \
    *                                                                                              \
    * PV.X = ADD_INT R0.X, PV.Z                                                                    \
    * Cycle 0: X = R0.                                                                             \
    */                                                                                             \
   TERAKAN_SHADER_OP2_NW(true, 'X', ADD_INT, EG, 0, 'X', V_SQ_ALU_SRC_PV, 'Z', VEC_012),           \
                                                                                                   \
   /* +11: Apply the row offset to the address written to R0.X.                                    \
    *                                                                                              \
    * R0.X = ADD_INT PV.X, R0.Y                                                                    \
    * Cycle 1: Y = R0.                                                                             \
    */                                                                                             \
   TERAKAN_SHADER_OP2(true, 0, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', 0, 'Y', VEC_012)

/* clang-format off */

#define TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_COUNT_R9XX 11

static uint32_t const terakan_meta_copy_buffer_to_image_ps_r9xx[] = {
   /* 0: Address calculation. */
   S_SQ_CF_WORD0_ADDR(6) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(
      TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET)) |
      S_SQ_CF_ALU_WORD1_COUNT(TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_COUNT_R9XX) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source buffer. */
   S_SQ_CF_WORD0_ADDR(4),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export the color. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: End the program. */
   TERAKAN_SHADER_CF_END_R9XX,

   /* 4-5: Vertex-fetch from the source buffer to R0. */
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X),
   S_SQ_VTX_WORD1_GPR_DST_GPR(0) | S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W) | S_SQ_VTX_WORD1_USE_CONST_FIELDS(true),
   0,
   0,

   /* 6: Address calculation ALU clause. */
   TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_R9XX,
};

static uint32_t const terakan_meta_copy_image_to_buffer_ps_r9xx[] = {
   /* 0: Loading the array layer index (may exceed the maximum number of attachment layers for
    * images without attachment usage enabled).
    */
   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_ALU_WORD1_COUNT(0) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source texture. */
   S_SQ_CF_WORD0_ADDR(8),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Address calculation. */
   S_SQ_CF_WORD0_ADDR(10) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(
      TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET)) |
      S_SQ_CF_ALU_WORD1_COUNT(TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_COUNT_R9XX) |
      S_SQ_CF_ALU_WORD1_BARRIER(true) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 3: Write to the UAV. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 0, 1, 0xF, true, true),

   /* 4: Perform a dummy export. */
   TERAKAN_SHADER_CF_PS_DUMMY_EXPORT_DONE_R9XX,

   /* 5: End the program. */
   TERAKAN_SHADER_CF_END_R9XX,

   /* 6: ALU clause. */

   /* +0: Load the array layer index.
    *
    * INTERP_LOAD_P0 R0.Z, Param0.X, unused 0
    */
   TERAKAN_SHADER_OP1(true, 0, 'Z', INTERP_LOAD_P0, EG, V_SQ_ALU_SRC_PARAM_BASE, 'X', VEC_012),

   /* 7 (alignment padding), 8-9: Fetch from the source texture to R1. */
   0,
   0,
   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(1) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,

   /* 10: Address calculation ALU clause. */
   TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_R9XX,
};

struct terakan_meta_shader const terakan_meta_copy_buffer_to_image_ps = {
   .r8xx =
      {
         .program = terakan_meta_copy_buffer_to_image_ps_r8xx,
         .program_size_bytes = sizeof(terakan_meta_copy_buffer_to_image_ps_r8xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_in_control =
                              {
                                 S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                 S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                    S_0286D0_FIXED_PT_POSITION_ADDR(0),
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .r9xx =
      {
         .program = terakan_meta_copy_buffer_to_image_ps_r9xx,
         .program_size_bytes = sizeof(terakan_meta_copy_buffer_to_image_ps_r9xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_in_control =
                              {
                                 S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                 S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                    S_0286D0_FIXED_PT_POSITION_ADDR(0),
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .kcache_used = BITFIELD_BIT(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS),
   .primary_meta_resource_used = true,
};

struct terakan_meta_shader const terakan_meta_copy_image_to_buffer_ps = {
   .r8xx =
      {
         .program = terakan_meta_copy_image_to_buffer_ps_r8xx,
         .program_size_bytes = sizeof(terakan_meta_copy_image_to_buffer_ps_r8xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(2) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_input_cntl =
                              {
                                 [0] = S_028644_FLAT_SHADE(1),
                              },
                           .spi_ps_in_control =
                              {
                                 S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                 S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                    S_0286D0_FIXED_PT_POSITION_ADDR(0),
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .r9xx =
      {
         .program = terakan_meta_copy_image_to_buffer_ps_r9xx,
         .program_size_bytes = sizeof(terakan_meta_copy_image_to_buffer_ps_r9xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(2) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_input_cntl =
                              {
                                 [0] = S_028644_FLAT_SHADE(1),
                              },
                           .spi_ps_in_control =
                              {
                                 S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                 S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                    S_0286D0_FIXED_PT_POSITION_ADDR(0),
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .kcache_used = BITFIELD_BIT(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS),
   .primary_meta_resource_used = true,
};

bool
terakan_meta_copy_buffer_image_translate_region_image(
   struct terakan_image const * const image, VkBufferImageCopy2 const * const region,
   struct terakan_meta_copy_buffer_image_region_image * const region_image_out)
{
   /* Image descriptor create info, including the Z axis. */

   unsigned const aspect_index = terakan_format_aspect_index(
      image->format_info.aspect_map, region->imageSubresource.aspectMask, 0);
   bool const image_is_3d = image->vk.image_type == VK_IMAGE_TYPE_3D;
   region_image_out->image_descriptor_create_info =
      (struct terakan_image_descriptor_create_info){
         .image = image,
         .view_format = terakan_meta_transfer_image_block_format_info(
            terascale_format_bytes_per_block[image->format_info.aspect_formats[aspect_index]
                                                                   .format]),
         .image_aspect_index = aspect_index,
         .subresource_range = {
            .base_mip_level = region->imageSubresource.mipLevel,
            .max_level_count = 1,
            /* #MemoryIntegrity: If the Z offset is negative (invalid usage), the subresource range
             * sanitization will fail as it will always exceed the depth as `uint32_t`.
             */
            .base_z_or_array_layer = image_is_3d ? (uint32_t)region->imageOffset.z
                                                 : region->imageSubresource.baseArrayLayer,
            .max_depth_or_layer_count = image_is_3d ? region->imageExtent.depth
                                                    : region->imageSubresource.layerCount,
         },
      };
   if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
                    image, &region_image_out->image_descriptor_create_info.subresource_range,
                    false))) {
      return false;
   }

   /* Sanitized rectangle, for #MemoryIntegrity, in particular to avoid incorrect addressing
    * calculations.
    * According to the valid usage rules, the region rectangle must be contained within the image
    * subresource. It's trivial to clip an invalid rectangle on the right and bottom. However,
    * clipping to 0 on the left and top would require addressing adjustment, so reject the region if
    * its offset is negative (the conversion to `uint32_t` will make it always exceed the width or
    * height).
    */

   uint32_t const subresource_width_texels = u_minify(
      image->vk.extent.width,
      region_image_out->image_descriptor_create_info.subresource_range.base_mip_level);
   uint32_t const subresource_height_texels = u_minify(
      image->vk.extent.height,
      region_image_out->image_descriptor_create_info.subresource_range.base_mip_level);
   if (unlikely((uint32_t)region->imageOffset.x >= subresource_width_texels ||
                (uint32_t)region->imageOffset.y >= subresource_height_texels)) {
      return false;
   }

   uint8_t const * const block_texels_log2 = terascale_format_block_texels_log2
      [image->format_info.aspect_formats[aspect_index].format];
   unsigned const block_mask_x = (1u << block_texels_log2[0]) - 1u;
   unsigned const block_mask_y = (1u << block_texels_log2[1]) - 1u;
   uint32_t const subresource_width_blocks =
      (subresource_width_texels >> block_texels_log2[0]) +
      (uint32_t)((subresource_width_texels & block_mask_x) != 0);
   uint32_t const subresource_height_blocks =
      (subresource_height_texels >> block_texels_log2[1]) +
      (uint32_t)((subresource_height_texels & block_mask_y) != 0);

   /* The `offset` must be block-aligned, but `offset + extent` is limited to the extent of the
    * subresource, which is not block-aligned, so round the extent (which is also used as the buffer
    * pitch if a 0 pitch is specified explicitly) up to the block size.
    */
   region_image_out->rect_blocks.bounds[0][0] =
      (uint16_t)region->imageOffset.x >> block_texels_log2[0];
   region_image_out->rect_blocks.bounds[0][1] =
      (uint16_t)region->imageOffset.y >> block_texels_log2[1];
   uint32_t const rect_width_blocks_unclamped =
      (region->imageExtent.width >> block_texels_log2[0]) +
      (uint32_t)((region->imageExtent.width & block_mask_x) != 0);
   uint32_t const rect_height_blocks_unclamped =
      (region->imageExtent.height >> block_texels_log2[1]) +
      (uint32_t)((region->imageExtent.height & block_mask_y) != 0);
   region_image_out->rect_blocks.bounds[1][0] =
      region_image_out->rect_blocks.bounds[0][0] +
      MIN2(rect_width_blocks_unclamped,
           subresource_width_blocks - region_image_out->rect_blocks.bounds[0][0]);
   region_image_out->rect_blocks.bounds[1][1] =
      region_image_out->rect_blocks.bounds[0][1] +
      MIN2(rect_height_blocks_unclamped,
           subresource_height_blocks - region_image_out->rect_blocks.bounds[0][1]);
   if (unlikely(terakan_screen_rect_is_empty(region_image_out->rect_blocks))) {
      return false;
   }

   /* Buffer pitches. */

   region_image_out->buffer_y_pitch_blocks =
      region->bufferRowLength != 0
         ? (region->bufferRowLength >> block_texels_log2[0]) +
           (uint32_t)((region->bufferRowLength & block_mask_x) != 0)
         : rect_width_blocks_unclamped;
   region_image_out->buffer_z_pitch_blocks =
      (uint64_t)region_image_out->buffer_y_pitch_blocks *
      (region->bufferImageHeight != 0
          ? (region->bufferImageHeight >> block_texels_log2[1]) +
            (uint32_t)((region->bufferImageHeight & block_mask_y) != 0)
          : rect_height_blocks_unclamped);

   return true;
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdCopyBufferToImage2(VkCommandBuffer const commandBuffer,
                               VkCopyBufferToImageInfo2 const * const pCopyBufferToImageInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_image const * const image =
      terakan_image_from_handle(pCopyBufferToImageInfo->dstImage);

   if (terakan_format_is_expand_3x(image->surface.aspects[0].bytes_per_block)) {
      terakan_meta_copy_expand_3x_buffer_to_image(command_writer, pCopyBufferToImageInfo);
      return;
   }

   struct terakan_buffer const * const buffer =
      terakan_buffer_from_handle(pCopyBufferToImageInfo->srcBuffer);
   struct terakan_resource_descriptor buffer_descriptor = {
      .resource = {
         [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
         [TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_VERTEX_BUFFER,
      }};

   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   struct terakan_meta_config_draw_begin_options const meta_begin_options = {
      .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
      .cb_and_db_shader_control_mode =
         TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_WITH_RTV_AND_DYNAMIC_DB_SHADER_CONTROL,
      .rasterization = {.enable = true},
   };
   terakan_meta_config_draw_begin(command_writer, &meta_begin_options);
   terakan_meta_config_draw_set_sq_pgm_vs(command_writer,
                                          TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_config_draw_set_sq_pgm_ps(command_writer,
                                          TERAKAN_META_SHADER_COPY_BUFFER_TO_IMAGE_PS);

   uint32_t constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONSTS_COUNT] = {};
   bool constants_set = false;

   for (uint32_t region_index = 0; region_index < pCopyBufferToImageInfo->regionCount;
        ++region_index) {
      VkBufferImageCopy2 const * const region = &pCopyBufferToImageInfo->pRegions[region_index];

      struct terakan_meta_copy_buffer_image_region_image region_image;
      if (unlikely(!terakan_meta_copy_buffer_image_translate_region_image(image, region,
                                                                          &region_image))) {
         continue;
      }

      buffer_descriptor.resource[3] =
         S_03000C_DST_SEL_X(region_image.image_descriptor_create_info.view_format.swizzle_r) |
         S_03000C_DST_SEL_Y(region_image.image_descriptor_create_info.view_format.swizzle_g) |
         S_03000C_DST_SEL_Z(region_image.image_descriptor_create_info.view_format.swizzle_b) |
         S_03000C_DST_SEL_W(region_image.image_descriptor_create_info.view_format.swizzle_a);

      uint8_t const region_bytes_per_block =
         terascale_format_bytes_per_block[region_image.image_descriptor_create_info.view_format
                                                                                       .format];
      uint64_t const buffer_z_pitch_bytes =
         region_bytes_per_block * region_image.buffer_z_pitch_blocks;
      uint64_t const buffer_slice_extent_bytes_minus_1 =
         region_bytes_per_block *
            terakan_meta_copy_buffer_image_region_image_buffer_slice_extent_blocks(&region_image) -
            1u;
      uint64_t buffer_offset = region->bufferOffset;

      if (constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET] !=
             region_image.rect_blocks.bounds[0][0] ||
          constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y] !=
             region_image.rect_blocks.bounds[0][1] ||
          constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH] !=
             region_image.buffer_y_pitch_blocks ||
          constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH] !=
             (uint32_t)region_image.buffer_z_pitch_blocks) {
         constants_set = false;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET] =
            region_image.rect_blocks.bounds[0][0];
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y] =
            region_image.rect_blocks.bounds[0][1];
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH] =
            region_image.buffer_y_pitch_blocks;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH] =
            (uint32_t)region_image.buffer_z_pitch_blocks;
      }
      if (!constants_set) {
         terakan_meta_config_draw_set_kcache_push_constants(command_writer, sizeof(constants),
                                                            constants, false, true);
         constants_set = true;
      }

      do {
         if (unlikely(buffer_offset >= buffer->vk.size)) {
            /* #MemoryIntegrity. */
            break;
         }

         struct terakan_color_descriptor image_descriptor;
         uint32_t const image_descriptor_slices = terakan_image_create_color_descriptor(
            &region_image.image_descriptor_create_info, V_028C70_TEXTURE2DARRAY, &image_descriptor,
            NULL);
         if (unlikely(image_descriptor_slices == 0)) {
            break;
         }
         terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control(
            command_writer, 0xF, &image->bo, &image_descriptor, NULL,
            TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);

         uint64_t const buffer_descriptor_base = buffer->va + buffer_offset;
         buffer_descriptor.resource[0] = (uint32_t)buffer_descriptor_base;
         buffer_descriptor.resource[1] = (uint32_t)MIN3(
            buffer_z_pitch_bytes * (image_descriptor_slices - 1u) +
               buffer_slice_extent_bytes_minus_1,
            buffer->vk.size - buffer_offset - 1u, UINT32_MAX);
         buffer_descriptor.resource[2] =
            S_030008_BASE_ADDRESS_HI(buffer_descriptor_base >> 32) |
            S_030008_STRIDE(region_bytes_per_block) |
            S_030008_DATA_FORMAT(region_image.image_descriptor_create_info.view_format.format) |
            S_030008_NUM_FORMAT_ALL(terascale_format_get_sq_num_format(
               (enum terascale_format_number_type)
                  region_image.image_descriptor_create_info.view_format.number_type)) |
            S_030008_ENDIAN_SWAP(G_028C70_ENDIAN(image_descriptor.info));
         terakan_hw_config_sqk_set_resource_fs(
            &command_writer->hw_config_sqk, TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
            buffer->bo, &buffer_descriptor);
         /* Meta shaders use VTX fetch (BUFFER_ID=TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META)
          * to read from the source buffer. VTX fetch reads from resources_vi_[], but set_resource_fs
          * only writes to resources_fs_[]. We must also set the descriptor in the VTX resource array.
          */
         terakan_hw_config_sqk_set_resource_vi(
            &command_writer->hw_config_sqk, TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
            buffer->bo, &buffer_descriptor);
         /* Emit the VTX resource descriptor to hardware before the meta draw.
          * The meta draw path (terakan_meta_before_draw → before_hw_draw) only emits
          * barriers, not config state. Without this, the VTX resource is never emitted
          * and the meta shader reads garbage from an unset VTX resource slot.
          */
         command_writer->hw_config_sqk.vi_.resources_used |=
            BITFIELD_BIT(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META);
         terakan_hw_config_sqk_emit_modified_for_draw(command_writer);

         terakan_meta_draw_rect(command_writer, region_image.rect_blocks, image_descriptor_slices);

         region_image.image_descriptor_create_info.subresource_range.base_z_or_array_layer +=
            image_descriptor_slices;
         region_image.image_descriptor_create_info.subresource_range.max_depth_or_layer_count -=
            image_descriptor_slices;
         buffer_offset += buffer_z_pitch_bytes * image_descriptor_slices;
      } while (region_image.image_descriptor_create_info.subresource_range
                  .max_depth_or_layer_count != 0);
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdCopyImageToBuffer2(VkCommandBuffer const commandBuffer,
                              VkCopyImageToBufferInfo2 const * const pCopyImageToBufferInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_image const * const image =
      terakan_image_from_handle(pCopyImageToBufferInfo->srcImage);

   if (terakan_format_is_expand_3x(image->surface.aspects[0].bytes_per_block)) {
      terakan_meta_copy_expand_3x_image_to_buffer(command_writer, pCopyImageToBufferInfo);
      return;
   }

   struct terakan_buffer const * const buffer =
      terakan_buffer_from_handle(pCopyImageToBufferInfo->dstBuffer);
   struct terakan_color_descriptor buffer_descriptor = {
      .attrib = TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_ATTRIB,
   };

   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV | TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   struct terakan_meta_config_draw_begin_options const meta_begin_options = {
      .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
      .cb_and_db_shader_control_mode = TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_UAV_ONLY,
      .rasterization = {.enable = true},
   };
   terakan_meta_config_draw_begin(command_writer, &meta_begin_options);
   terakan_meta_config_draw_set_sq_pgm_vs(command_writer,
                                          TERAKAN_META_SHADER_POSITION_FROM_INDEX_VS);
   terakan_meta_config_draw_set_sq_pgm_ps(command_writer,
                                          TERAKAN_META_SHADER_COPY_IMAGE_TO_BUFFER_PS);

   uint32_t constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONSTS_COUNT] = {};
   bool constants_set = false;

   uint8_t const pipe_interleave_bytes_log2 =
      terakan_gfx_command_writer_physical_device(command_writer)->tiling_info
         .pipe_interleave_bytes_log2;

   for (uint32_t region_index = 0; region_index < pCopyImageToBufferInfo->regionCount;
        ++region_index) {
      VkBufferImageCopy2 const * const region = &pCopyImageToBufferInfo->pRegions[region_index];

      struct terakan_meta_copy_buffer_image_region_image region_image;
      if (unlikely(!terakan_meta_copy_buffer_image_translate_region_image(image, region,
                                                                          &region_image))) {
         continue;
      }

      struct terakan_resource_descriptor image_descriptor;
      if (unlikely(!terakan_image_create_resource_descriptor(
                       &region_image.image_descriptor_create_info, V_030000_SQ_TEX_DIM_2D_ARRAY,
                       NULL, &image_descriptor))) {
         continue;
      }

      if (unlikely(region->bufferOffset >= buffer->vk.size)) {
         /* #MemoryIntegrity: Prevent a large offset from causing an integer wraparound. */
         continue;
      }
      uint8_t const region_bytes_per_block =
         terascale_format_bytes_per_block[region_image.image_descriptor_create_info.view_format
                                                                                       .format];
      uint64_t const buffer_va_block_aligned_non_uav_aligned =
         (buffer->va + region->bufferOffset) & ~(uint64_t)(region_bytes_per_block - 1);
      /* #MemoryIntegrity. */
      uint64_t const buffer_object_end_va_block_aligned =
         (buffer->va + buffer->vk.size) & ~(uint64_t)(region_bytes_per_block - 1);
      if (unlikely(buffer_va_block_aligned_non_uav_aligned >= buffer_object_end_va_block_aligned)) {
         /* Don't subtract 1 from 0. */
         continue;
      }
      uint64_t const buffer_va_aligned =
         buffer_va_block_aligned_non_uav_aligned >> pipe_interleave_bytes_log2 <<
         pipe_interleave_bytes_log2;
      uint32_t const buffer_uav_alignment_offset_blocks =
         (uint32_t)(buffer_va_block_aligned_non_uav_aligned - buffer_va_aligned) /
         region_bytes_per_block;
      uint32_t const buffer_max_blocks_minus_1 = (uint32_t)MIN2(
         (buffer_object_end_va_block_aligned - buffer_va_aligned) / region_bytes_per_block - 1u,
         UINT32_MAX);
      uint64_t const buffer_extent_blocks_minus_1 =
         buffer_uav_alignment_offset_blocks +
         (terakan_meta_copy_buffer_image_region_image_buffer_slice_extent_blocks(&region_image) -
          1u) +
         region_image.buffer_z_pitch_blocks *
            (region_image.image_descriptor_create_info.subresource_range.max_depth_or_layer_count -
             1u);
      buffer_descriptor.base = (uint32_t)(buffer_va_aligned >> 8);
      buffer_descriptor.info =
         S_028C70_ENDIAN(G_030008_ENDIAN_SWAP(image_descriptor.resource[2])) |
         S_028C70_FORMAT(region_image.image_descriptor_create_info.view_format.format) |
         S_028C70_NUMBER_TYPE(region_image.image_descriptor_create_info.view_format.number_type) |
         S_028C70_COMP_SWAP(region_image.image_descriptor_create_info.view_format.cb_color_swap) |
         TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_INFO_CONST_FIELDS;
      buffer_descriptor.dim =
         (uint32_t)MIN2(buffer_extent_blocks_minus_1, buffer_max_blocks_minus_1);
      terakan_meta_config_draw_set_cb_uav(command_writer, 0, buffer->bo, &buffer_descriptor);

      /* Set the image if the region wasn't rejected due to being invalid. */
      terakan_hw_config_sqk_set_resource_fs(
         &command_writer->hw_config_sqk, TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
         image->bo, &image_descriptor);

      int32_t const image_offset_x_minus_buffer_offset =
         (int32_t)region_image.rect_blocks.bounds[0][0] -
         (int32_t)buffer_uav_alignment_offset_blocks;
      if (constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET] !=
             (uint32_t)image_offset_x_minus_buffer_offset ||
          constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y] !=
             (uint32_t)region_image.rect_blocks.bounds[0][1] ||
          constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH] !=
             region_image.buffer_y_pitch_blocks ||
          constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH] !=
             (uint32_t)region_image.buffer_z_pitch_blocks) {
         constants_set = false;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET] =
            (uint32_t)image_offset_x_minus_buffer_offset;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y] =
            (uint32_t)region_image.rect_blocks.bounds[0][1];
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH] =
            region_image.buffer_y_pitch_blocks;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH] =
            (uint32_t)region_image.buffer_z_pitch_blocks;
      }
      if (!constants_set) {
         terakan_meta_config_draw_set_kcache_push_constants(command_writer, sizeof(constants),
                                                            constants, false, true);
         constants_set = true;
      }

      terakan_meta_draw_rect(
         command_writer, region_image.rect_blocks,
         region_image.image_descriptor_create_info.subresource_range.max_depth_or_layer_count);
   }
}
