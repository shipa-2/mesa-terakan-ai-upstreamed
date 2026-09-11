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

#ifndef TERAKAN_VERTEX_INPUT_H
#define TERAKAN_VERTEX_INPUT_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"

#include "amd/terascale/common/terascale_format.h"
#include "util/bitscan.h"
#include "util/macros.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Returns the format bits of the fetch instruction word 1 for the given format, or bits with
 * `DATA_FORMAT` set to `INVALID` if the provided format doesn't support vertex fetch.
 */
uint32_t terakan_vertex_input_format_fetch_word1(struct terascale_format_info const * format_info);

/* Per-vertex attribute fetching is currently only implemented for the base vertex being pre-applied
 * to R0.X (using `VGT_INDX_OFFSET` and `SQ_VTX_FETCH_NO_INDEX_OFFSET`, not `SQ_VTX_BASE_VTX_LOC`
 * and `SQ_VTX_FETCH_VERTEX_DATA`), like in Vulkan and OpenGL, but not in Direct3D.
 *
 * Attributes are fetches to R[1 + attribute index].
 *
 * Supporting only the Direct3D 10+ element alignment, `min(element size, 4 bytes)`, expected by the
 * hardware.
 * https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#4.4.6%20Element%20Alignment
 * While unaligned fetching, like in OpenGL and in Vulkan `VK_EXT_legacy_vertex_attributes`, is
 * theoretically implementable as vertex fetch is fully programmable, it's currently not supported
 * due to the massive complexity increase it would require, especially for elements larger than
 * 4 bytes, which, for loading of individual bytes, would use additional GPRs and thus make the GPR
 * count for the software vertex shader stage dynamic, and it also would need to handle swizzling.
 * The primary use case of unaligned attributes is OpenGL, but the OpenGL implementation may align
 * the attributes instead.
 *
 * Using the same limit for the number of attributes as for bindings in the hardware, because
 * different attributes may need multiple hardware resource bindings even if they use the same
 * application-side binding.
 */

struct terakan_vertex_input_fs_layout {
   /* Attributes used by the vertex shader. */
   uint32_t attributes_used;

   /* If an attribute is used, but its `DATA_FORMAT` is `INVALID`, the actual buffer fetch is
    * explictly not generated, but the destination GPR is still filled, excluding components where
    * `DSL_SEL` is `MASK`, with 1 for the specified `NUM_FORMAT_ALL` if `DST_SEL` is a constant 1,
    * or with 0 (similarly to XYZW data channels for a fetch from a null resource) otherwise. Other
    * parameters of the attribute specified in this structure not mentioned earlier in this comment
    * are ignored by fetch shader generation.
    *
    * If `attribute_format_fetch_word1` is zero, particularly, (0, 0, 0, 0) will be written to the
    * destination GPR, because it means `DATA_FORMAT = INVALID, DST_SEL = XXXX`, and the X data
    * channel is replaced with 0 in this case.
    *
    * For formats that don't support vertex fetch for any reason, unless `DATA_FORMAT` is set to
    * `INVALID` explicitly, the destination GPR value, and whether a fetch instruction is actually
    * emitted, is undefined.
    */
   uint32_t attribute_format_fetch_word1[TERAKAN_RESOURCE_HW_COUNT_FETCH];
   uint8_t attribute_bindings[TERAKAN_RESOURCE_HW_COUNT_FETCH];
   uint16_t attribute_offsets[TERAKAN_RESOURCE_HW_COUNT_FETCH];

   uint32_t instance_rate_attributes;
   /* Ignored if the attribute is per-vertex. For per-instance attributes, a divisor of 0 means that
    * the value is the same for all instances (like in `VK_EXT_vertex_attribute_divisor`).
    */
   uint32_t attribute_instance_divisors[TERAKAN_RESOURCE_HW_COUNT_FETCH];

   /* #2048StrideAs1024 #hashtag: Which bindings (see `attribute_bindings`) need the index to be
    * adjusted in the fetch shader so they can be fetched with a stride of 2048, which is the
    * minimum required by Direct3D 10+ era graphics APIs, via a fetch constant with a stride of
    * 1024, because before R9xx, strides only up to 2047 could be specified in fetch constants.
    *
    * If any instanced attribute uses this workaround, the vertex shader must load the base instance
    * index to R0.Z before calling the fetch shader.
    *
    * For per-vertex attributes, only pre-offset vertex index in R0.X (like in Vulkan and OpenGL) is
    * currently supported by this workaround. However, if later this is used with a 0-based vertex
    * index in R0.X, the same method as for the instance index would need to be implemented
    * (possibly expecting the base vertex index to be loaded to R0.Y before calling the fetch
    * shader, although it'd collide with `VGT_VTX_CNT_EN`).
    *
    * Note that the current implementation of the workaround doubles the index, so it should work
    * not only for exactly 2048, but also for even strides in [2048, 4094], however, support for
    * strides beyond 2048 isn't required by Vulkan or Direct3D, so it must not be advertised on
    * architecture generations prior to R9xx, and because the workaround may be enabled on R9xx for
    * regression testing as well, it should be used only when the stride is exactly 2048, not just
    * greater than or equal to it, because otherwise odd strides above 2048 will not work on R9xx.
    */
   uint32_t bindings_with_2048_stride_as_1024;
};

/* Whether the layouts will surely have identically behaving fetch shaders built for them, and with
 * the same resource descriptor usage.
 */
bool terakan_vertex_input_fs_layout_identical(struct terakan_vertex_input_fs_layout const * a,
                                              struct terakan_vertex_input_fs_layout const * b);

struct terakan_vertex_input_fs_resource_usage {
   /* Mask of which resource registers are used. */
   uint32_t resources_used;

   /* Pre-R9xx hardware performs bounds checking only for up to the first 32 bits of an element, so
    * the buffer size in the fetch constant needs to be shrunk depending on the attribute format for
    * precise robust access. On R9xx, bounds checking is done for the whole element.
    *
    * Bits [4:0] - application binding index.
    * Bits [:5] - number of dwords to subtract from the buffer size in the fetch constant.
    *
    * Only resources included in `resources_used` have their values in this array initialized.
    */
   uint8_t resource_bindings_and_truncation[TERAKAN_RESOURCE_HW_COUNT_FETCH];
};

/* Return the number of dwords that must be subtracted from a fetch descriptor for a
 * particular resource when its base address is VA.  On pre-R9xx hardware the
 * bounds check starts at the first naturally aligned chunk of each element.  The
 * old static value is correct only when the buffer binding itself is naturally
 * aligned; Vulkan permits arbitrary binding offsets, so the actual address must
 * participate in the calculation.  R9xx checks the whole element and therefore
 * always returns zero.
 */
uint8_t terakan_vertex_input_fs_resource_truncation(
   struct terakan_vertex_input_fs_layout const * layout,
   struct terakan_vertex_input_fs_resource_usage const * usage,
   unsigned resource_index, bool is_r9xx, uint64_t va);

static inline bool
terakan_vertex_input_fs_resource_usage_equal(
   struct terakan_vertex_input_fs_resource_usage const * const a,
   struct terakan_vertex_input_fs_resource_usage const * const b)
{
   if (a->resources_used != b->resources_used) {
      return false;
   }
   unsigned resources_remaining = a->resources_used;
   while (resources_remaining) {
      int range_start, range_length;
      u_bit_scan_consecutive_range(&resources_remaining, &range_start, &range_length);
      if (memcmp(&a->resource_bindings_and_truncation[range_start],
                 &b->resource_bindings_and_truncation[range_start],
                 sizeof(a->resource_bindings_and_truncation[0]) * range_length) != 0) {
         return false;
      }
   }
   return true;
}

/* Longest possible sequence for an attribute, also assuming parallelization with other attributes,
 * and thus treating each immediate as 1 qword, for simplicity of the upper bound calculation, and
 * supporting 2048 stride as 1024 on R9xx too even though it's not needed there for the possibility
 * of regression testing of the workaround on all architecture generations:
 * - 0: LSHR_INT index, fast_udiv_info.pre_shift (+2)
 * - 2: ADD_INT index, 1 (+1, adding the increment, using an inline constant)
 * - 3: 4-slot MULHI_INT index, fast_udiv_info.multiplier (+5)
 * - 8: LSHR_INT index, fast_udiv_info.post_shift (+2)
 * - 10: LSHL_INT index, 1 (+1, multiplying by 2 for the 2048 stride workaround, using an inline
 *       constant)
 * - 11: ADD_INT index, R0.Z (+1, adding the base for the 2048 stride workaround)
 *
 * For attributes that are used, but not bound, up to 4 instructions are emitted to write 0 or 1.
 */
#define TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_QWORDS_PER_ATTRIBUTE 12
#define TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_QWORDS                                           \
   (TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_QWORDS_PER_ATTRIBUTE *                               \
    TERAKAN_RESOURCE_HW_COUNT_FETCH)
/* An ALU clause can contain up to 0x80 qwords, but the last instruction group may overflow to the
 * next clause if there's less space available than needed for it (up to 5 instruction qwords and 2
 * literal constant qwords).
 */
#define TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_CLAUSES                                          \
   DIV_ROUND_UP(TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_QWORDS, 0x80 - (7 - 1))

/* #Signed2101010Alpha: the hardware decodes the two-bit field of `2_10_10_10` and `10_10_10_2` as
 * unsigned whatever `FORMAT_COMP_ALL` says, so a signed attribute of one of those formats needs its
 * alpha corrected after the fetch. The ten-bit fields are decoded correctly and are left alone.
 *
 * The correction runs in place on the fetched component, one dependent instruction per group:
 * - MULADD  v = v * s + 0.5    (s is 0.25 for SCALED, where v is 0..3, and 0.75 for NORM, where the
 *                               hardware produced v/3), giving 0.5, 0.75, 1.0, 1.25
 * - FRACT   v                  giving 0.5, 0.75, 0.0, 0.25
 * - MULADD  v = v * 4.0 - 2.0  giving 0, 1, -2, -1, which is the signed value of the two bits
 * - MAX     v = max(v, -1.0)   for NORM only, where -2 clamps to -1
 *
 * Longest per corrected component: two MULADDs of one qword each carrying one and two literals (one
 * literal qword each), a FRACT and a MAX of one qword each, so 6 qwords. A format has at most one
 * two-bit field, but `DST_SEL` may route it to more than one destination component.
 */
#define TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_QWORDS_PER_ATTRIBUTE 24
#define TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_QWORDS                                          \
   (TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_QWORDS_PER_ATTRIBUTE *                              \
    TERAKAN_RESOURCE_HW_COUNT_FETCH)
#define TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_CLAUSES                                         \
   DIV_ROUND_UP(TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_QWORDS, 0x80 - (7 - 1))

/* Pre-fetch ALU clauses, one fetch clause (up to 64 fetches per clause, but currently always doing
 * one fetch per attribute), post-fetch ALU clauses, return.
 */
#define TERAKAN_VERTEX_INPUT_FS_MAX_CONTROL_FLOW_QWORDS                                            \
   (TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_CLAUSES +                                            \
    TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_CLAUSES + 2)

/* Control flow, pre-fetch ALU instruction, 2-qwords-aligned fetches, post-fetch ALU. */
#define TERAKAN_VERTEX_INPUT_FS_MAX_QWORDS                                                         \
   (DIV_ROUND_UP(TERAKAN_VERTEX_INPUT_FS_MAX_CONTROL_FLOW_QWORDS +                                 \
                    TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_QWORDS,                              \
                 2) +                                                                              \
    2 * TERAKAN_RESOURCE_HW_COUNT_FETCH + TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_QWORDS)

struct terakan_vertex_input_fs_code {
   uint32_t control_flow_qwords;
   uint32_t pre_fetch_alu_qwords;
   uint32_t fetch_count;
   uint32_t post_fetch_alu_qwords;

   /* The 32-bit instruction words have the host endianness, must be copied to the device shader
    * program as little-endian.
    */
   uint32_t control_flow[2 * TERAKAN_VERTEX_INPUT_FS_MAX_CONTROL_FLOW_QWORDS];
   uint32_t pre_fetch_alu[2 * TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_QWORDS];
   uint32_t fetch[4 * TERAKAN_RESOURCE_HW_COUNT_FETCH];
   uint32_t post_fetch_alu[2 * TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_QWORDS];
};

void terakan_vertex_input_create_fs_code(
   struct terakan_vertex_input_fs_layout const * layout, bool is_r9xx,
   struct terakan_vertex_input_fs_resource_usage * resource_usage_out,
   struct terakan_vertex_input_fs_code * code_out);

static inline uint32_t
terakan_vertex_input_fs_code_qwords(struct terakan_vertex_input_fs_code const * const code)
{
   uint32_t fs_code_qwords = code->control_flow_qwords + code->pre_fetch_alu_qwords;
   if (code->fetch_count != 0) {
      /* Fetch instructions must be aligned to 2 qwords. */
      fs_code_qwords += (fs_code_qwords & 1) + 2 * code->fetch_count;
   }
   fs_code_qwords += code->post_fetch_alu_qwords;
   return fs_code_qwords;
}

static inline bool
terakan_vertex_input_fs_code_is_no_operation(struct terakan_vertex_input_fs_code const * const code)
{
   /* Does nothing if the only instruction is `RETURN`. */
   return code->control_flow_qwords <= 1;
}

/* `terakan_vertex_input_fs_code_qwords` will be written to `program_out` as little-endian. */
void terakan_vertex_input_combine_fs(struct terakan_vertex_input_fs_code const * code,
                                     uint32_t * program_out);

struct terakan_vertex_input_fs {
   struct terakan_bo const * bo;
   uint32_t va_shr8;

   struct terakan_vertex_input_fs_resource_usage resource_usage;

   struct terakan_vertex_input_fs_layout layout;
};

#endif /* TERAKAN_VERTEX_INPUT_H */
