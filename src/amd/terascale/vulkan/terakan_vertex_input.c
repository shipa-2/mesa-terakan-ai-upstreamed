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

#include "terakan_vertex_input.h"

#include "amd/terascale/common/terascale_format.h"
#include "gallium/drivers/r600/eg_sq.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600_asm.h"
#include "gallium/drivers/r600/r600_opcodes.h"
#include "util/bitscan.h"
#include "util/fast_idiv_by_const.h"
#include "util/macros.h"
#include "util/u_endian.h"
#include "util/u_math.h"
#include "util/u_qsort.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TERAKAN_VERTEX_INPUT_FORMAT_FETCH_WORD1_BITS                                               \
   (~(uint32_t)(C_SQ_VTX_WORD1_DST_SEL_X & C_SQ_VTX_WORD1_DST_SEL_Y & C_SQ_VTX_WORD1_DST_SEL_Z &   \
                C_SQ_VTX_WORD1_DST_SEL_W & C_SQ_VTX_WORD1_DATA_FORMAT &                            \
                C_SQ_VTX_WORD1_NUM_FORMAT_ALL & C_SQ_VTX_WORD1_FORMAT_COMP_ALL &                   \
                C_SQ_VTX_WORD1_SRF_MODE_ALL))

/* Bits that have effect on the replacement value for an attribute with `DATA_FORMAT` set to
 * `INVALID`.
 */
#define TERAKAN_VERTEX_INPUT_FORMAT_FETCH_WORD1_NULL_REPLACEMENT_BITS                              \
   (~(uint32_t)(C_SQ_VTX_WORD1_DST_SEL_X & C_SQ_VTX_WORD1_DST_SEL_Y & C_SQ_VTX_WORD1_DST_SEL_Z &   \
                C_SQ_VTX_WORD1_DST_SEL_W & C_SQ_VTX_WORD1_NUM_FORMAT_ALL))

uint32_t
terakan_vertex_input_format_fetch_word1(struct terascale_format_info const * const format_info)
{
   if (!format_info->supports_sq_vertex_fetch) {
      /* Zero means `DATA_FORMAT = INVALID, DST_SEL = XXXX`, which will be replaced with
       * (0, 0, 0, 0).
       */
      return 0;
   }
   return S_SQ_VTX_WORD1_DST_SEL_X(format_info->swizzle_r) |
          S_SQ_VTX_WORD1_DST_SEL_Y(format_info->swizzle_g) |
          S_SQ_VTX_WORD1_DST_SEL_Z(format_info->swizzle_b) |
          S_SQ_VTX_WORD1_DST_SEL_W(format_info->swizzle_a) |
          S_SQ_VTX_WORD1_DATA_FORMAT(format_info->format) |
          S_SQ_VTX_WORD1_NUM_FORMAT_ALL(
             terascale_format_get_sq_num_format(format_info->number_type)) |
          S_SQ_VTX_WORD1_FORMAT_COMP_ALL(format_info->channels_signed != 0b0);
}

bool
terakan_vertex_input_fs_layout_identical(struct terakan_vertex_input_fs_layout const * const a,
                                         struct terakan_vertex_input_fs_layout const * const b)
{
   if (a->attributes_used != b->attributes_used) {
      return false;
   }
   uint32_t const instance_rate_attributes_different =
      a->instance_rate_attributes ^ b->instance_rate_attributes;
   uint32_t bindings_used = 0b0;
   u_foreach_bit (attribute_index, a->attributes_used) {
      uint32_t const a_format_fetch_word1 = a->attribute_format_fetch_word1[attribute_index];
      uint32_t const b_format_fetch_word1 = b->attribute_format_fetch_word1[attribute_index];
      if (G_SQ_VTX_WORD1_DATA_FORMAT(a_format_fetch_word1) == TERASCALE_FORMAT_INDEX_INVALID) {
         if ((a_format_fetch_word1 ^ b_format_fetch_word1) &
             TERAKAN_VERTEX_INPUT_FORMAT_FETCH_WORD1_NULL_REPLACEMENT_BITS) {
            return false;
         }
         continue;
      }
      if ((a_format_fetch_word1 ^ b_format_fetch_word1) &
          TERAKAN_VERTEX_INPUT_FORMAT_FETCH_WORD1_BITS) {
         return false;
      }
      uint8_t const binding_index = a->attribute_bindings[attribute_index];
      bindings_used |= BITFIELD_BIT(binding_index);
      if (binding_index != b->attribute_bindings[attribute_index] ||
          a->attribute_offsets[attribute_index] != b->attribute_offsets[attribute_index] ||
          (instance_rate_attributes_different & BITFIELD_BIT(attribute_index)) ||
          ((a->instance_rate_attributes & BITFIELD_BIT(attribute_index)) &&
           a->attribute_instance_divisors[attribute_index] !=
              b->attribute_instance_divisors[attribute_index])) {
         return false;
      }
   }
   if ((a->bindings_with_2048_stride_as_1024 ^ b->bindings_with_2048_stride_as_1024) &
       bindings_used) {
      return false;
   }
   return true;
}

uint8_t
terakan_vertex_input_fs_resource_truncation(
   struct terakan_vertex_input_fs_layout const * const layout,
   struct terakan_vertex_input_fs_resource_usage const * const usage,
   unsigned const resource_index, bool const is_r9xx, uint64_t const va)
{
   assert(resource_index < TERAKAN_RESOURCE_HW_COUNT_FETCH);
   if (is_r9xx || !(usage->resources_used & BITFIELD_BIT(resource_index))) {
      return 0;
   }

   uint8_t const binding = usage->resource_bindings_and_truncation[resource_index] & 0x1Fu;
   uint8_t truncation = 0;
   u_foreach_bit (attribute_index, layout->attributes_used) {
      if (layout->attribute_bindings[attribute_index] != binding) {
         continue;
      }
      uint32_t const word1 = layout->attribute_format_fetch_word1[attribute_index];
      uint32_t const bytes = terascale_format_bytes_per_block[G_SQ_VTX_WORD1_DATA_FORMAT(word1)];
      if (bytes == 0) {
         continue;
      }

      uint64_t const address = va + layout->attribute_offsets[attribute_index];
      unsigned const address_alignment_log2 = address != 0 ? __builtin_ctzll(address) : 64;
      uint32_t const alignment = address_alignment_log2 < 31 ? 1u << address_alignment_log2
                                                              : UINT32_MAX;
      uint32_t const checked_bytes = MIN2(bytes, MAX2(4u, alignment));
      truncation = MAX2(truncation, (uint8_t)((bytes - checked_bytes) / sizeof(uint32_t)));
   }
   return truncation;
}

/* In the beginning of the software vertex stage shader:
 * R0.X = 0-based vertex index + `VGT_INDX_OFFSET`
 * R0.W = 0-based instance index
 *
 * With `SQ_VTX_FETCH_VERTEX_DATA` or `SQ_VTX_FETCH_INSTANCE_DATA`, vertex fetch also adds the value
 * of the `SQ_VTX_BASE_VTX_LOC` or `SQ_VTX_START_INST_LOC` register respectively to the provided
 * index.
 * However, if the base is pre-applied to the index passed to the fetch instruction,
 * `SQ_VTX_FETCH_NO_INDEX_OFFSET` should be used instead.
 * These `SQ_VTX_*` base registers are also automatically loaded by the command processor indirect
 * draw commands, while `VGT_INDX_OFFSET` needs to be copied to explicitly for indirect draws.
 *
 * In Vulkan and OpenGL, `gl_VertexIndex` or `gl_VertexID` includes the base, so it's more
 * straightforward to use `VGT_INDX_OFFSET` than `SQ_VTX_FETCH_VERTEX_DATA`, fetching per-vertex
 * data with `SQ_VTX_FETCH_NO_INDEX_OFFSET`. In Direct3D, `SV_VertexID` is 0-based, so
 * `SQ_VTX_FETCH_VERTEX_DATA` should be used there.
 *
 * For per-instance data, however, not only the hardware doesn't have a register whose value would
 * be pre-added to R0.W, but also, the divisor needs to apply to the 0-based instance index without
 * the base - the fetch instance index in graphics APIs is:
 * - `0-based instance index / divisor + base instance`, if the divisor is not 0, or
 * - Base instance, if the divisor is 0.
 * Per-instance attributes therefore need to be fetched with `SQ_VTX_FETCH_INSTANCE_DATA`. For
 * Vulkan `gl_InstanceIndex` calculation, the base instance index needs to be loaded and added
 * explicitly by the vertex shader (but OpenGL `gl_InstanceID` and Direct3D `SV_InstanceID` are
 * 0-based).
 *
 * See `util_fast_udiv32` for the instance index division algorithm.
 * Addition of the `increment` is performed via saturating arithmetic - the value is clamped to
 * `~increment`, and then the `increment` is added. It's known during fetch shader creation whether
 * the divisor is greater than 1 and what the increment amount is, so this can be done.
 *
 * #2048StrideAs1024 #hashtag: Pre-R9xx GPUs don't support strides greater than 2047 due to the bit
 * field width oversight, but Direct3D 10+ era graphics APIs require 2048 to be supported.
 * 2048 stride is therefore implemented on those GPUs using a stride of 1024 and doubling the index
 * in the fetch shader - by multiplying the index normally passed to the fetch instruction by 2, and
 * for `SQ_VTX_FETCH_VERTEX/INSTANCE_DATA`, also adding the base index (expected to be loaded by the
 * vertex shader before calling the fetch shader, in particular, to R0.Z for the base instance
 * index) in the fetch shader, so that it's added twice, and thus doubled too - first added
 * explicitly in the fetch shader, and then inside vertex fetching.
 */

static int
terakan_vertex_input_compare_attribute_indices(void const * const a_ptr, void const * const b_ptr,
                                               void * const layout_ptr)
{
   uint8_t const a = *(uint8_t const *)a_ptr;
   uint8_t const b = *(uint8_t const *)b_ptr;

   struct terakan_vertex_input_fs_layout const * const layout = layout_ptr;

   /* For more consistent access patterns between consecutive fetches, make access frequency the
    * most important sorting criterion, per-vertex at one end, constant for all instances at the
    * other (here specifically, high-frequency first, low-frequency last).
    */
   bool const a_is_instance_rate = (layout->instance_rate_attributes & BITFIELD_BIT(a)) != 0;
   bool const b_is_instance_rate = (layout->instance_rate_attributes & BITFIELD_BIT(b)) != 0;
   if (a_is_instance_rate != b_is_instance_rate) {
      return b_is_instance_rate ? -1 : 1;
   }
   if (b_is_instance_rate) {
      /* The frequency decreases as the divisor increases, but a divisor of 0 is the special case
       * for zero frequency. Treat a divisor of 0 as the lowest frequency using subtraction
       * overflow, which is defined to be wrapping for unsigned integers in C.
       */
      uint32_t const a_frequency_inverse = layout->attribute_instance_divisors[a] - (uint32_t)1;
      uint32_t const b_frequency_inverse = layout->attribute_instance_divisors[b] - (uint32_t)1;
      if (a_frequency_inverse != b_frequency_inverse) {
         return a_frequency_inverse < b_frequency_inverse ? -1 : 1;
      }
   }

   /* Within one step rate, order by the binding index, for consecutive fetches to point to close
    * memory locations, and to break iteration in mega fetch construction when the binding or the
    * step rate changes.
    */
   uint8_t const a_binding = layout->attribute_bindings[a];
   uint8_t const b_binding = layout->attribute_bindings[b];
   if (a_binding != b_binding) {
      return a_binding < b_binding ? -1 : 1;
   }

   /* Finally, order by the offset, for consecutive fetches to use increasing addresses, and so mega
    * fetch construction iteration can go in just one direction.
    */
   uint32_t const a_offset = layout->attribute_offsets[a];
   uint32_t const b_offset = layout->attribute_offsets[b];
   if (a_offset != b_offset) {
      return a_offset < b_offset ? -1 : 1;
   }

   /* If the fetches statically use the same address, order by the provided index for stable
    * sorting.
    */
   assert(a != b);
   if (likely(a != b)) {
      return a < b ? -1 : 1;
   }

   return 0;
}

enum terakan_vertex_input_indexing_alu_op {
   /* The operations in this enumeration are specified in the order they must be done in.
    *
    * All operations here can be coissued independently for different index calculations, assuming
    * that there are no read port conflicts for the index accumulator operand itself, but they can
    * be avoided by using a single component exclusively for one pre-multiplication or
    * post-multiplication chain until there are no more unscheduled operations remaining in it.
    * The GPR read port allocation is:
    * - Cycle 0: Vector operation index accumulators.
    * - Cycle 1: R0.
    * - Cycle 2: Scalar operation index accumulator.
    *
    * Additionally, it must not be possible to read the result of an instruction in multiple other
    * instructions - particularly, don't add any operations working with the non-2048 stride
    * accumulator that may end up being done after the #2048StrideAs1024 workaround operations. The
    * reason is that in the "scalar unit write, then cycle 0 read" case, the scalar instruction's
    * destination component needs to be patched depending on which component the result will be read
    * from afterwards, and that can't be done if the scalar result is potentially going to be read
    * via different components - so only one read is currently supported for each result.
    */

   /* Chain of any-unit operations that may need to be done before the `MULHI_UINT` by the fast
    * integer division multiplier.
    *
    * This should include only operations that must be done specifically to prepare for the
    * multiplication, because pre-multiplication chains are scheduled earlier to unblock
    * multiplications that depend on them as quickly as possible.
    *
    * Also, pre-multiplication and post-multiplication chains are intended to be scheduled
    * separately, with the multiplication unpinning the chain from the VLIW component it was pinned
    * to, so the post-multiplication chain can start from loading the result via any cycle 0 GPR
    * read port, as multiplication is either scalar-only or 4-slot, thus being able to write to any
    * GPR component. Without the multiplication between the two chains, the post-multiplication
    * chain would have to be scheduled immediately after the pre-multiplication one in the same VLIW
    * component, as the last write in the pre-multiplication chain would generally be done on a
    * vector unit, thus only to a specific component.
    */

   /* index = LSHR_INT(index, fast_udiv_info.pre_shift); */
   TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_PRE_SHIFT,
   /* index = ADD_INT(index, fast_udiv_info.increment);
    * The 0-based instance index never exceeds `UINT32_MAX - 1` because the instance count is
    * 32-bit, so no unsigned wrapping handling is needed.
    */
   TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_ADD_INCREMENT,

   /* Scalar-only (VLIW5) or 4-slot (VLIW4) operation between the two chains. */

   /* index = MULHI_UINT(index, fast_udiv_info.multiplier); */
   TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH,

   /* Chain of any-unit operations that may need to be done after the `MULHI_UINT` by the fast
    * integer division multiplier, or if there's no multiplication instruction at all.
    */

   /* index = MOV(0);
    * For per-instance data with a divisor of 0.
    */
   TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_MOVE_ZERO,
   /* index = LSHR_INT(index, fast_udiv_info.post_shift); */
   TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_POST_SHIFT,
   /* index_2048_stride = LSHL_INT(index, 1); */
   TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_2048_STRIDE_DOUBLE_INDEX,
   /* index_2048_stride = ADD_INT(index_2048_stride, R0.Z);
    * Always preceded by 2048_STRIDE_DOUBLE_INDEX, so the cycle 0/2 GPR will be the index
    * accumulator (not the VGT-provided index in R0).
    */
   TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_2048_STRIDE_ADD_BASE,

   TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_COUNT,
};

static_assert(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_COUNT <= 32,
              "Using vertex input index calculation ALU operation indices in 32-bit bitfields.");

/* This fetch shader generation is designed for simplicity, especially considering that it can be
 * used when the vertex input layout is dynamic rather than precompiled, so it currently uses
 * completely separate calculations for every unique index formula.
 *
 * Common subexpression elimination is not performed as it may cause GPR read port conflicts that
 * are currently avoided by pinning the entire operation chain to a single component. Layouts with
 * many different instance index divisors are likely to be extremely rare anyway.
 *
 * The exception is when the frequency for different attributes is the same, but some attributes use
 * the #2048StrideAs1024 workaround, some don't. This case is handled by one indexing function, but
 * writing the results without the workaround and with it to separate GPRs, since in both cases the
 * division by the instance divisor is the same, just the 2048 stride workaround performs additional
 * transformations of the result.
 */

struct terakan_vertex_input_indexing {
   /* Destination GPR without and with the #2048StrideAs1024 workaround (that is the last
    * overwritten by a fetch that uses this indexing).
    * 0 if not used for the given stride.
    */
   uint8_t dest_gpr;
   uint8_t dest_gpr_2048_stride;

   /* Scheduling input: GPR ([2:], normally 0) and component ([1:0]) containing the initial value of
    * the index.
    * Scheduling temporary: Current index accumulator GPR and component.
    * Scheduling output: GPR and component that writing of the final index was scheduled to.
    */
   uint8_t current_gpr_and_component;
   uint8_t current_gpr_and_component_2048_stride;

   /* The vector component if the chain is currently pinned to one in the scheduling, or `UINT8_MAX`
    * if it's not.
    */
   uint8_t current_vector_component_pinned_to;

   /* Offset of the last write to the accumulator done on the scalar unit on VLIW5, or of the 4-slot
    * instruction group last writing to the accumulator on VLIW4, in qwords, within the pre-fetch
    * ALU instructions, or `UINT32_MAX` if the last accumulator write was not done on the scalar
    * unit or by a 4-slot instruction.
    */
   uint32_t last_scalar_or_4_slot_write_pre_fetch_alu_qword_offset;

   /* Mask of ALU operations that haven't been scheduled yet. */
   uint32_t alu_ops_remaining;

   /* Ignored for vertex-rate attributes. */
   uint32_t instance_divisor;
   struct util_fast_udiv_info instance_division_info;
};

/* Note that what this sorting does exactly is assumed by scheduling, including, for example, that
 * the indexing functions for which the next operation is multiplication are in the beginning of the
 * resulting array.
 */
/* TODO(Triang3l): With the categorization pass, precise grouping isn't needed, just place indexings
 * with pre-multiplication operations first (primarily for pinned chains), and then sort by the
 * number of operations in the pre-multiplication and post-multiplication chains.
 */
static int
terakan_vertex_input_compare_indexing_scheduling_indices(void const * const a_index_ptr,
                                                         void const * const b_index_ptr,
                                                         void * const indexings_ptr)
{
   uint8_t const a_index = *(uint8_t const *)a_index_ptr;
   uint8_t const b_index = *(uint8_t const *)b_index_ptr;

   struct terakan_vertex_input_indexing const * const indexings =
      (struct terakan_vertex_input_indexing const *)indexings_ptr;
   struct terakan_vertex_input_indexing const * const a = &indexings[a_index];
   struct terakan_vertex_input_indexing const * const b = &indexings[b_index];

   /* Multiplications scheduled first, only supported on the scalar unit (or 4-slot on R9xx), and
    * potentially unblocking chains of multiple operations.
    */
   bool const a_next_is_multiply =
      (a->alu_ops_remaining &
       BITFIELD_MASK(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH + 1)) ==
      BITFIELD_BIT(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH);
   bool const b_next_is_multiply =
      (b->alu_ops_remaining &
       BITFIELD_MASK(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH + 1)) ==
      BITFIELD_BIT(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH);
   if (a_next_is_multiply != b_next_is_multiply) {
      return a_next_is_multiply ? -1 : 1;
   }

   /* Not using vector components for new chains until already started ones are finished, for GPR
    * read port allocation simplicity. Therefore, scheduling already started chains before potential
    * new chains.
    */
   bool const a_pinned = a->current_vector_component_pinned_to != UINT8_MAX;
   bool const b_pinned = b->current_vector_component_pinned_to != UINT8_MAX;
   if (a_pinned != b_pinned) {
      return a_pinned ? -1 : 1;
   }

   /* Schedule chains leading to multiplications and possibly other (post-multiplication) chains
    * before chains that nothing depends on in indexing calculations anymore.
    */
   bool const a_has_multiply =
      (a->alu_ops_remaining &
       BITFIELD_BIT(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH)) != 0;
   bool const b_has_multiply =
      (b->alu_ops_remaining &
       BITFIELD_BIT(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH)) != 0;
   if (a_has_multiply != b_has_multiply) {
      return a_has_multiply ? -1 : 1;
   }

   /* Start longer chains earlier to complete them earlier as well. */

   if (a_has_multiply) {
      /* Schedule shorter pre-multiplication chains earlier. */
      unsigned const a_remaining_pre_multiply_op_count = util_bitcount(
         a->alu_ops_remaining &
         BITFIELD_MASK(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH));
      unsigned const b_remaining_pre_multiply_op_count = util_bitcount(
         b->alu_ops_remaining &
         BITFIELD_MASK(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH));
      if (a_remaining_pre_multiply_op_count != b_remaining_pre_multiply_op_count) {
         return a_remaining_pre_multiply_op_count < b_remaining_pre_multiply_op_count ? -1 : 1;
      }
   }

   /* Within the same pre-multiplication chain length, or if only independent operations are
    * remaining, prioritize the indexing function with more operations remaining.
    *
    * In the former case, the contribution of the pre-multiplication chain and the multiplication
    * would be the same for both sides (otherwise that would've been handled by other conditionals
    * above), so it won't affect the comparison, and this logic can be used for both cases.
    */
   unsigned const a_remaining_op_count = util_bitcount(a->alu_ops_remaining);
   unsigned const b_remaining_op_count = util_bitcount(b->alu_ops_remaining);
   if (a_remaining_op_count != b_remaining_op_count) {
      return a_remaining_op_count < b_remaining_op_count ? -1 : 1;
   }

   /* If the remaining parts of the indexing functions are similar, order by the provided index for
    * stable sorting and for consistent scheduling iteration order across instruction groups.
    */
   assert(a_index != b_index);
   if (likely(a_index != b_index)) {
      return a_index < b_index ? -1 : 1;
   }

   return 0;
}

/* Returns the value of the constant operand of the operation, which may end up requiring a literal
 * constant or being an inline constant depending on its value, or if the operation doesn't use a
 * constant, returns 0 (available as an inline constant).
 */
static uint32_t
terakan_vertex_input_indexing_alu_op_constant(
   enum terakan_vertex_input_indexing_alu_op const op,
   struct util_fast_udiv_info const * const instance_division_info)
{
   switch (op) {
   case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_PRE_SHIFT:
      return instance_division_info->pre_shift;
   case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_ADD_INCREMENT:
      return instance_division_info->increment;
   case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH:
      return (uint32_t)instance_division_info->multiplier;
   case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_MOVE_ZERO:
      return 0;
   case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_POST_SHIFT:
      return instance_division_info->post_shift;
   case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_2048_STRIDE_DOUBLE_INDEX:
      /* Left shift amount. */
      return 1;
   default:
      return 0;
   }
}

/* Returns whether it's possible to specify the constant in the instruction group. If it's not, the
 * outputs are not modified.
 */
static bool
terakan_vertex_input_try_add_alu_constant_for_next_op(
   struct terakan_vertex_input_indexing const * const indexing, uint8_t * const group_literal_count,
   uint32_t * const group_literals, uint32_t * const op_word0_accumulator_for_src1)
{
   assert(indexing->alu_ops_remaining);
   uint32_t const constant = terakan_vertex_input_indexing_alu_op_constant(
      (enum terakan_vertex_input_indexing_alu_op)(ffs(indexing->alu_ops_remaining) - 1),
      &indexing->instance_division_info);
   unsigned src_sel;
   r600_bytecode_special_constants(constant, &src_sel);
   if (src_sel == V_SQ_ALU_SRC_LITERAL) {
      /* Try to reuse an existing literal first. */
      uint8_t group_literal_index = 0;
      for (; group_literal_index < *group_literal_count; ++group_literal_index) {
         if (group_literals[group_literal_index] == constant) {
            break;
         }
      }
      if (group_literal_index >= *group_literal_count) {
         if (group_literal_index >= 4) {
            return false;
         }
         group_literals[(*group_literal_count)++] = constant;
      }
      *op_word0_accumulator_for_src1 |= S_SQ_ALU_WORD0_SRC1_CHAN(group_literal_index);
   }
   *op_word0_accumulator_for_src1 |= S_SQ_ALU_WORD0_SRC1_SEL(src_sel);
   return true;
}

/* Returns the index of the component of an unbound attribute to fill on the scalar unit next, or -1
 * if there are none remaining.
 */
static int
terakan_vertex_input_next_unbound_attribute_scalar_component(
   uint32_t const unbound_attributes_remaining_per_component[4],
   uint8_t const tie_break_priority_components)
{
   assert(tie_break_priority_components <= 0xF);
   int best_component_attribute_count = -1;
   int best_component_index = -1;
   for (unsigned priority = 0; priority <= 1; ++priority) {
      u_foreach_bit (component_index, priority ? tie_break_priority_components ^ 0xF
                                               : tie_break_priority_components) {
         uint32_t const component_attributes =
            unbound_attributes_remaining_per_component[component_index];
         if (!component_attributes) {
            continue;
         }
         int const component_attribute_count = (int)util_bitcount(component_attributes);
         if (component_attribute_count > best_component_attribute_count) {
            best_component_attribute_count = component_attribute_count;
            best_component_index = component_index;
         }
      }
   }
   return best_component_index;
}

/* Returns the attribute index for which the constant was moved to specified component, or -1 if the
 * move wasn't emitted. The bit corresponding to the written component of the attribute is excluded
 * from `unbound_attributes_remaining_per_component`.
 */
static int
terakan_vertex_input_try_emit_unbound_attribute_mov(
   unsigned const component_index, uint32_t * const unbound_attributes_remaining_per_component,
   uint32_t const * const attributes_format_fetch_word1,
   struct terakan_vertex_input_fs_code * const code_out)
{
   int const attribute_index = ffs(unbound_attributes_remaining_per_component[component_index]) - 1;
   if (attribute_index < 0) {
      return -1;
   }
   assert(code_out->pre_fetch_alu_qwords < TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_QWORDS);
   uint32_t * const alu_op2 = &code_out->pre_fetch_alu[2 * code_out->pre_fetch_alu_qwords++];
   uint32_t const format_fetch_word1 = attributes_format_fetch_word1[attribute_index];
   uint32_t const src =
      ((format_fetch_word1 >> (9 + 3 * component_index)) & 0b111) == TERASCALE_SWIZZLE_1
         ? (G_SQ_VTX_WORD1_NUM_FORMAT_ALL(format_fetch_word1) == TERASCALE_FORMAT_SQ_NUM_FORMAT_INT
               ? V_SQ_ALU_SRC_1_INT
               : V_SQ_ALU_SRC_1)
         : V_SQ_ALU_SRC_0;
   /* Set both sources to the same constant, even though the result of this 2-operand instruction
    * depends only on the first source, to unambiguously not use a GPR read port for the second
    * source.
    */
   alu_op2[0] = S_SQ_ALU_WORD0_SRC0_SEL(src) | S_SQ_ALU_WORD0_SRC1_SEL(src);
   /* The bank swizzle doesn't matter when moving a constant. */
   alu_op2[1] = S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV) |
                S_SQ_ALU_WORD1_OP2_WRITE_MASK(true) | S_SQ_ALU_WORD1_DST_GPR(1 + attribute_index) |
                S_SQ_ALU_WORD1_DST_CHAN(component_index);
   unbound_attributes_remaining_per_component[component_index] &= ~BITFIELD_BIT(attribute_index);
   return attribute_index;
}

/* #Signed2101010Alpha: whether this attribute's fetch produces a two-bit field the hardware
 * decoded as unsigned but the format calls signed, and if so, which destination components carry
 * it. The ten-bit fields of the same formats come back correct and are not touched.
 *
 * Returns the mask of destination components to correct, zero when there is nothing to do, and
 * writes the number format, which decides which correction the component needs.
 */
static unsigned
terakan_vertex_input_signed_2_bit_alpha_components(uint32_t const format_fetch_word1,
                                                   unsigned * const number_format_out)
{
   *number_format_out = TERASCALE_FORMAT_SQ_NUM_FORMAT_NORM;
   if (!G_SQ_VTX_WORD1_FORMAT_COMP_ALL(format_fetch_word1)) {
      return 0;
   }
   unsigned const num_format = G_SQ_VTX_WORD1_NUM_FORMAT_ALL(format_fetch_word1);
   unsigned two_bit_data_channel;
   switch (G_SQ_VTX_WORD1_DATA_FORMAT(format_fetch_word1)) {
   case TERASCALE_FORMAT_INDEX_2_10_10_10:
      /* The two-bit field is the most significant one, which is data channel W. */
      two_bit_data_channel = TERASCALE_SWIZZLE_W;
      break;
   case TERASCALE_FORMAT_INDEX_10_10_10_2:
      two_bit_data_channel = TERASCALE_SWIZZLE_X;
      break;
   default:
      return 0;
   }
   *number_format_out = num_format;
   unsigned components = 0;
   for (unsigned component_index = 0; component_index < 4; ++component_index) {
      if (((format_fetch_word1 >> (9 + 3 * component_index)) & 0b111) == two_bit_data_channel) {
         components |= BITFIELD_BIT(component_index);
      }
   }
   return components;
}

/* Emits one post-fetch ALU instruction group holding a single instruction, plus its literals. */
static void
terakan_vertex_input_emit_post_fetch_alu(struct terakan_vertex_input_fs_code * const code_out,
                                         uint32_t const word0, uint32_t const word1,
                                         unsigned const literal_count,
                                         uint32_t const * const literals)
{
   assert(literal_count <= 2);
   assert(code_out->post_fetch_alu_qwords < TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_QWORDS);
   uint32_t * const alu = &code_out->post_fetch_alu[2 * code_out->post_fetch_alu_qwords++];
   /* Each instruction depends on the one before it, so every group holds exactly one. */
   alu[0] = word0 | S_SQ_ALU_WORD0_LAST(true);
   alu[1] = word1;
   if (literal_count != 0) {
      assert(code_out->post_fetch_alu_qwords < TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_QWORDS);
      uint32_t * const literal_qword =
         &code_out->post_fetch_alu[2 * code_out->post_fetch_alu_qwords++];
      literal_qword[0] = literals[0];
      literal_qword[1] = literal_count > 1 ? literals[1] : 0;
   }
}

void
terakan_vertex_input_create_fs_code(
   struct terakan_vertex_input_fs_layout const * const layout, bool const is_r9xx,
   struct terakan_vertex_input_fs_resource_usage * const resource_usage_out,
   struct terakan_vertex_input_fs_code * const code_out)
{
   /* Check which attributes are bound and unbound, and for the bound ones, gather the counts of
    * bytes per element, resource usage, and reorder attributes for more optimal memory access
    * patterns and for mega fetches.
    *
    * As a result of the sorting, in particular, within one step rate and binding, fetches will be
    * consecutive and ordered by the offset, making mega fetch construction only need to look at the
    * neighborhood of an attribute in one direction, not to iterate all attributes.
    */

   uint8_t attributes_bytes_per_element[TERAKAN_RESOURCE_HW_COUNT_FETCH];

   uint8_t bound_used_attribute_count = 0;
   /* `sorted_attribute_indices` includes only bound used attributes. */
   uint8_t sorted_attribute_indices[TERAKAN_RESOURCE_HW_COUNT_FETCH];

   /* Constructed only from bound used attributes. */
   uint8_t attribute_truncations[TERAKAN_RESOURCE_HW_COUNT_FETCH];
   uint32_t bindings_used = 0b0;
   uint8_t bindings_truncations_used[TERAKAN_RESOURCE_HW_COUNT_FETCH] = {};

   /* Which unbound used attributes require an instruction for setting them to 0 or 1 (depending on
    * `DST_SEL`) to be emitted.
    */
   uint32_t unbound_attributes_remaining_per_component[4] = {};

   u_foreach_bit (attribute_index, layout->attributes_used) {
      uint32_t const attribute_format_fetch_word1 =
         layout->attribute_format_fetch_word1[attribute_index];
      uint8_t const attribute_bytes_per_element =
         terascale_format_bytes_per_block[G_SQ_VTX_WORD1_DATA_FORMAT(attribute_format_fetch_word1)];
      attributes_bytes_per_element[attribute_index] = attribute_bytes_per_element;
      if (attribute_bytes_per_element != 0) {
         sorted_attribute_indices[bound_used_attribute_count++] = attribute_index;
         /* The hardware checks only the *first naturally aligned chunk* of the fetch, not the
          * whole element. `terakan_vertex_fetch_bounds_probe` measured the thresholds and then
          * confirmed the rule against eight predictions it had not been fitted to: the chunk is
          * `min(element_bytes, max(4, 2^ctz(attribute_offset)))`, and the fetch is accepted
          * whenever `attribute_offset + chunk <= SIZE`. That is why the thresholds looked
          * unruly and non-monotonic -- offset 0 is checked in full because its chunk is the whole
          * element, offset 2 only to its first dword.
          *
          * So shrink `SIZE` by exactly what the check does not cover, `element_bytes - chunk`.
          * The hardware's `attribute_offset + chunk <= SIZE - (element_bytes - chunk)` is then
          * `attribute_offset + element_bytes <= SIZE`, which is the bound the specification asks
          * for.
          *
          * The earlier `element_bytes - 4` assumed the chunk was always one dword. It is not, and
          * that over-truncation is what made `dEQP-VK.rasterization.depth_bias_control.*` fail
          * with `32_32_32_32_FLOAT` at offset 0, whose chunk is the full sixteen bytes.
          *
          * The chunk is derived from the attribute offset alone, while the address the hardware
          * sees also carries the buffer's bound offset. A bound offset less aligned than the
          * attribute offset makes the real chunk smaller than assumed and leaves part of the hole
          * open; buffer objects are allocated well aligned, so this covers the ordinary case
          * rather than every case.
          */
         uint8_t attribute_truncation = 0;
         if (!is_r9xx) {
            uint32_t const attribute_offset = layout->attribute_offsets[attribute_index];
            uint32_t const offset_alignment =
               attribute_offset != 0 ? (attribute_offset & -attribute_offset) : UINT32_MAX;
            uint32_t const checked_bytes =
               MIN2((uint32_t)attribute_bytes_per_element, MAX2(4u, offset_alignment));
            attribute_truncation =
               (uint8_t)((attribute_bytes_per_element - checked_bytes) / sizeof(uint32_t));
         }
         attribute_truncations[attribute_index] = attribute_truncation;
         uint8_t const binding_index = layout->attribute_bindings[attribute_index];
         bindings_used |= BITFIELD_BIT(binding_index);
         bindings_truncations_used[binding_index] |= BITFIELD_BIT(attribute_truncation);
      } else {
         uint32_t const attribute_bit = BITFIELD_BIT(attribute_index);
         for (unsigned component_index = 0; component_index < 4; ++component_index) {
            if (((attribute_format_fetch_word1 >> (9 + 3 * component_index)) & 0b111) !=
                TERASCALE_SWIZZLE_MASK) {
               unbound_attributes_remaining_per_component[component_index] |= attribute_bit;
            }
         }
      }
   }

   util_qsort_r(sorted_attribute_indices, bound_used_attribute_count,
                sizeof(*sorted_attribute_indices), terakan_vertex_input_compare_attribute_indices,
                (void *)layout);

   /* For each binding, place the resource for the attributes with the largest number of bytes per
    * element at the same index as the binding, since it's likely to be the least changing resource
    * for the binding (positions are usually the largest, and they may be shared between depth-only
    * and color render passes). Then place other truncation for each binding starting from the upper
    * resource indices, where they're less likely to collide with bindings in other vertex input
    * layouts.
    */
   resource_usage_out->resources_used = bindings_used;
   /* [Binding index][buffer size truncation amount in dwords]. */
   uint8_t bindings_resource_indices[TERAKAN_RESOURCE_HW_COUNT_FETCH][(12 / 4) + 1] = {};
   u_foreach_bit (binding_index, bindings_used) {
      if (unlikely(binding_index >= ARRAY_SIZE(bindings_resource_indices))) {
         continue;
      }
      uint8_t * const binding_resource_indices = bindings_resource_indices[binding_index];

      unsigned binding_truncations_remaining = bindings_truncations_used[binding_index];

      assert(binding_truncations_remaining);
      if (unlikely(!binding_truncations_remaining)) {
         continue;
      }
      uint8_t const binding_truncation_largest = util_last_bit(binding_truncations_remaining) - 1;
      if (unlikely(binding_truncation_largest >= ARRAY_SIZE(bindings_resource_indices[0]))) {
         continue;
      }
      binding_truncations_remaining &= ~(1u << binding_truncation_largest);
      resource_usage_out->resource_bindings_and_truncation[binding_index] =
         binding_index | (binding_truncation_largest << 5);
      binding_resource_indices[binding_truncation_largest] = binding_index;

      while (binding_truncations_remaining) {
         unsigned const binding_truncation = u_bit_scan(&binding_truncations_remaining);
         uint32_t const resources_unallocated = ~resource_usage_out->resources_used;
         assert(resources_unallocated);
         if (unlikely(!resources_unallocated)) {
            break;
         }
         unsigned const resource_index = util_last_bit(resources_unallocated) - 1;
         if (unlikely(resource_index >=
                      ARRAY_SIZE(resource_usage_out->resource_bindings_and_truncation))) {
            break;
         }
         resource_usage_out->resources_used |= BITFIELD_BIT(resource_index);
         resource_usage_out->resource_bindings_and_truncation[resource_index] =
            binding_index | (binding_truncation << 5);
         binding_resource_indices[binding_truncation] = resource_index;
      }
   }

   /* Gather all unique indexing functions for all attributes. */

   uint8_t indexing_count = 0;
   struct terakan_vertex_input_indexing indexings[TERAKAN_RESOURCE_HW_COUNT_FETCH];
   uint8_t attribute_indexings[TERAKAN_RESOURCE_HW_COUNT_FETCH];
   /* Iterating in the fetch instruction order, so the last destination GPR set in this loop for an
    * indexing function is the last that won't be overwritten by fetches until all fetches reading
    * the index from it are done.
    */
   for (uint8_t sorted_attribute_index = 0; sorted_attribute_index < bound_used_attribute_count;
        ++sorted_attribute_index) {
      uint8_t const attribute_index = sorted_attribute_indices[sorted_attribute_index];

      bool const attribute_instance_rate =
         (layout->instance_rate_attributes & BITFIELD_BIT(attribute_index)) != 0;
      /* The source to the first operation is R0.X (vertex index) for vertex-rate and R0.W (instance
       * index) for instance-rate attributes.
       */
      uint8_t const attribute_vgt_index_gpr_and_component = attribute_instance_rate ? 3 : 0;
      uint32_t const attribute_divisor =
         attribute_instance_rate ? layout->attribute_instance_divisors[attribute_index] : 1;

      bool const attribute_with_2048_stride_as_1024 =
         (layout->bindings_with_2048_stride_as_1024 &
          BITFIELD_BIT(layout->attribute_bindings[attribute_index])) != 0;

      /* Try to locate the existing indexing function that can be used for this attribute.
       * Search in reverse, since consecutive sorted attributes are likely to have the same
       * indexing.
       */
      {
         uint8_t next_after_existing_indexing_index = indexing_count;
         for (; next_after_existing_indexing_index != 0; --next_after_existing_indexing_index) {
            uint8_t const existing_indexing_index = next_after_existing_indexing_index - 1;
            struct terakan_vertex_input_indexing * const existing_indexing =
               &indexings[existing_indexing_index];
            if (existing_indexing->current_gpr_and_component !=
                attribute_vgt_index_gpr_and_component) {
               /* One is vertex-rate, the other is instance-rate. There are no indexing functions
                * that can be reused in this case.
                */
               continue;
            }
            if (attribute_instance_rate &&
                existing_indexing->instance_divisor != attribute_divisor) {
               continue;
            }
            /* Reuse the indexing function, making it usable for the specified stride.
             * See new indexing function initialization below for additional info about the
             * #2048StrideAs1024 workaround cases.
             */
            attribute_indexings[attribute_index] = existing_indexing_index;
            if (attribute_with_2048_stride_as_1024) {
               existing_indexing->dest_gpr_2048_stride = 1 + attribute_index;
               if (attribute_instance_rate) {
                  if (attribute_divisor != 0) {
                     existing_indexing->alu_ops_remaining |=
                        BITFIELD_BIT(
                           TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_2048_STRIDE_DOUBLE_INDEX) |
                        BITFIELD_BIT(
                           TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_2048_STRIDE_ADD_BASE);
                  }
               } else {
                  existing_indexing->alu_ops_remaining |=
                     BITFIELD_BIT(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_2048_STRIDE_DOUBLE_INDEX);
               }
            } else {
               existing_indexing->dest_gpr = 1 + attribute_index;
               if (attribute_instance_rate && attribute_divisor == 0) {
                  existing_indexing->alu_ops_remaining |=
                     BITFIELD_BIT(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_MOVE_ZERO);
               }
            }
            break;
         }
         if (next_after_existing_indexing_index != 0) {
            /* Indexing was reused (the loop was broken). */
            continue;
         }
      }

      /* Build the new indexing function. */
      uint8_t const new_indexing_index = indexing_count++;
      attribute_indexings[attribute_index] = new_indexing_index;
      struct terakan_vertex_input_indexing * const new_indexing = &indexings[new_indexing_index];
      *new_indexing = (struct terakan_vertex_input_indexing){
         .dest_gpr = attribute_with_2048_stride_as_1024 ? 0 : 1 + attribute_index,
         .dest_gpr_2048_stride = attribute_with_2048_stride_as_1024 ? 1 + attribute_index : 0,
         .current_gpr_and_component = attribute_vgt_index_gpr_and_component,
         .current_gpr_and_component_2048_stride = attribute_vgt_index_gpr_and_component,
         .current_vector_component_pinned_to = UINT8_MAX,
         .last_scalar_or_4_slot_write_pre_fetch_alu_qword_offset = UINT32_MAX,
         .instance_divisor = attribute_divisor,
      };
      if (attribute_instance_rate) {
         if (attribute_divisor == 0) {
            /* Set the initial index value to the base instance index loaded by the vertex shader to
             * R0.Z for the #2048StrideAs1024 workaround so the base is multiplied by 2 by adding
             * the base twice (via both the explicit index and `SQ_VTX_FETCH_INSTANCE_DATA`). Doing
             * this regardless of whether this specific attribute requires the 2048 stride
             * workaround, because this indexing function may be reused for a different attribute
             * which may need the 2048 stride workaround even if the current one doesn't.
             */
            new_indexing->current_gpr_and_component_2048_stride = 2;
            if (!attribute_with_2048_stride_as_1024) {
               /* The base instance index is added implicitly by the fetch instruction, pass zero as
                * the instance index.
                */
               new_indexing->alu_ops_remaining |=
                  BITFIELD_BIT(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_MOVE_ZERO);
            }
         } else {
            if (attribute_divisor != 1) {
               if (IS_POT(attribute_divisor)) {
                  /* Divide by powers of 2 specifically using only the any-unit post-shift (not the
                   * pre-shift as pre-multiplication operations are intended to be used only if
                   * there actually is multiplication) without the multiplication itself, rather
                   * than using multiplication (that `util_compute_fast_udiv_info` produces for
                   * powers of 2 as of March 2026) which is scalar-only or 4-slot.
                   */
                  new_indexing->instance_division_info.post_shift = ffs(attribute_divisor) - 1;
                  new_indexing->alu_ops_remaining |= BITFIELD_BIT(
                     TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_POST_SHIFT);
               } else {
                  new_indexing->instance_division_info =
                     util_compute_fast_udiv_info(attribute_divisor, 32, 32);
                  /* The multiplication is the division: the shifts and the increment only
                   * condition its input and output. Requesting the surrounding operations without
                   * it left the index shifted rather than divided, so every divisor that is not a
                   * power of two fetched the wrong instance --
                   * dEQP-VK.draw.renderpass.instanced with divisor 20 failed all 96 of its cases
                   * while 0, 1, 2 and 4 passed all of theirs.
                   */
                  new_indexing->alu_ops_remaining |= BITFIELD_BIT(
                     TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH);
                  if (new_indexing->instance_division_info.pre_shift != 0) {
                     new_indexing->alu_ops_remaining |= BITFIELD_BIT(
                        TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_PRE_SHIFT);
                  }
                  if (new_indexing->instance_division_info.increment != 0) {
                     new_indexing->alu_ops_remaining |= BITFIELD_BIT(
                        TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_ADD_INCREMENT);
                  }
                  if (new_indexing->instance_division_info.post_shift != 0) {
                     new_indexing->alu_ops_remaining |= BITFIELD_BIT(
                        TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_POST_SHIFT);
                  }
               }
            }
            if (attribute_with_2048_stride_as_1024) {
               new_indexing->alu_ops_remaining |=
                  BITFIELD_BIT(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_2048_STRIDE_DOUBLE_INDEX) |
                  BITFIELD_BIT(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_2048_STRIDE_ADD_BASE);
            }
         }
      } else {
         if (attribute_with_2048_stride_as_1024) {
            new_indexing->alu_ops_remaining |=
               BITFIELD_BIT(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_2048_STRIDE_DOUBLE_INDEX);
         }
      }
   }

   /* Schedule the indexing ALU operations.
    *
    * Scheduling multiplications, and thus pre-multiplication chains, as soon as possible, because
    * multiplication can be done less frequently than other operations since they're scalar-only or
    * 4-slot-only, but a chain of multiple operations may depend on it.
    *
    * Beginning longer chains earlier, so they end earlier too instead of pushing the total boundary
    * of the chains further.
    */

   code_out->pre_fetch_alu_qwords = 0;
   code_out->post_fetch_alu_qwords = 0;
   uint32_t current_pre_fetch_alu_clause_address = code_out->pre_fetch_alu_qwords;
   unsigned pre_fetch_alu_clause_count = 0;
   uint8_t pre_fetch_alu_clauses_qwords[TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_CLAUSES];

   uint32_t remaining_indexings = 0b0;
   for (uint8_t indexing_index = 0; indexing_index < indexing_count; ++indexing_index) {
      if (indexings[indexing_index].alu_ops_remaining) {
         remaining_indexings |= BITFIELD_BIT(indexing_index);
      }
   }

   while (remaining_indexings) {
      uint32_t const pre_fetch_alu_qwords_before_group = code_out->pre_fetch_alu_qwords;

      /* Update the scheduling priority according to the current situation. */
      uint8_t sorted_indexing_count = 0;
      uint8_t sorted_indexings[TERAKAN_RESOURCE_HW_COUNT_FETCH];
      u_foreach_bit (indexing_index, remaining_indexings) {
         sorted_indexings[sorted_indexing_count++] = indexing_index;
      }
      assert(sorted_indexing_count > 0);
      util_qsort_r(sorted_indexings, sorted_indexing_count, sizeof(*sorted_indexings),
                   terakan_vertex_input_compare_indexing_scheduling_indices, indexings);

      /* Categorize sorted indexings to take both the kind of dependencies involved and the
       * scheduling priority within a single type into account.
       */
      uint32_t sorted_indexings_where_next_is_multiply = 0b0;
      uint32_t sorted_indexings_pinned_to_vector_components = 0b0;
      uint32_t unscheduled_sorted_indexings_unpinned_where_next_is_pre_multiply = 0b0;
      uint32_t unscheduled_sorted_indexings_unpinned_where_next_is_in_final_chain = 0b0;
      for (uint8_t sorted_indexing_index = 0; sorted_indexing_index < sorted_indexing_count;
           ++sorted_indexing_index) {
         uint32_t const sorted_indexing_bit = BITFIELD_BIT(sorted_indexing_index);
         struct terakan_vertex_input_indexing const * const indexing =
            &indexings[sorted_indexings[sorted_indexing_index]];
         if (indexing->current_vector_component_pinned_to != UINT8_MAX) {
            assert(
               (indexing->alu_ops_remaining &
                BITFIELD_MASK(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH +
                              1)) !=
                  BITFIELD_BIT(
                     TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH) &&
               "Pre-multiplication chains must be unpinned from a VLIW component when finished");
            sorted_indexings_pinned_to_vector_components |= sorted_indexing_bit;
         } else {
            if (indexing->alu_ops_remaining &
                BITFIELD_BIT(
                   TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH)) {
               if (indexing->alu_ops_remaining &
                   BITFIELD_MASK(
                      TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH)) {
                  unscheduled_sorted_indexings_unpinned_where_next_is_pre_multiply |=
                     sorted_indexing_bit;
               } else {
                  sorted_indexings_where_next_is_multiply |= sorted_indexing_bit;
               }
            } else {
               unscheduled_sorted_indexings_unpinned_where_next_is_in_final_chain |=
                  sorted_indexing_bit;
            }
         }
      }

      uint8_t group_literal_count = 0;
      uint32_t group_literals[4];

      uint8_t group_indexings[5] = {UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX};
      uint32_t group_indexing_constant_word0_src1[5] = {};

      /* Schedule multiplications as early as possible, because multiplication can be executed only
       * at a frequency lower than other operations (scalar-only on VLIW5 or 4-slot on VLIW4), yet
       * it may be blocking a chain of multiple operations than depend on it.
       */
      if (sorted_indexings_where_next_is_multiply) {
         uint8_t const multiply_indexing_index =
            sorted_indexings[ffs(sorted_indexings_where_next_is_multiply) - 1];
         group_indexings[4] = multiply_indexing_index;
         struct terakan_vertex_input_indexing const * const multiply_indexing =
            &indexings[multiply_indexing_index];
         terakan_vertex_input_try_add_alu_constant_for_next_op(
            multiply_indexing, &group_literal_count, group_literals,
            &group_indexing_constant_word0_src1[4]);
      }

      if (is_r9xx && group_indexings[4] != UINT8_MAX) {
         /* Emit the 4-slot multiplication.
          * The `LAST` field and the literal constant will be emitted by the logic shared with the
          * general scheduling code.
          */
         uint8_t const multiply_indexing_index = group_indexings[4];
         struct terakan_vertex_input_indexing * const multiply_indexing =
            &indexings[multiply_indexing_index];
         assert((enum terakan_vertex_input_indexing_alu_op)(
                   ffs(multiply_indexing->alu_ops_remaining) - 1) ==
                TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH);
         uint32_t const multiply_alu_op_word0 =
            S_SQ_ALU_WORD0_SRC0_SEL(multiply_indexing->current_gpr_and_component >> 2) |
            S_SQ_ALU_WORD0_SRC0_CHAN(multiply_indexing->current_gpr_and_component & 3) |
            group_indexing_constant_word0_src1[4];
         uint32_t const multiply_alu_op_word1 =
            S_SQ_ALU_WORD1_DST_GPR(multiply_indexing->dest_gpr) |
            S_SQ_ALU_WORD1_OP2_WRITE_MASK(true) |
            S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MULHI_UINT);
         assert(TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_QWORDS - code_out->pre_fetch_alu_qwords >=
                4);
         for (uint32_t component_index = 0; component_index < 4; ++component_index) {
            uint32_t * const multiply_alu_op =
               &code_out->pre_fetch_alu[2 * code_out->pre_fetch_alu_qwords++];
            multiply_alu_op[0] = multiply_alu_op_word0;
            multiply_alu_op[1] = multiply_alu_op_word1 | S_SQ_ALU_WORD1_DST_CHAN(component_index);
         }

         /* Update the indexing to match the state after the new instruction. */
         multiply_indexing->last_scalar_or_4_slot_write_pre_fetch_alu_qword_offset =
            pre_fetch_alu_qwords_before_group;
         multiply_indexing->current_gpr_and_component = multiply_indexing->dest_gpr << 2;
         multiply_indexing->alu_ops_remaining &=
            ~BITFIELD_BIT(TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH);
         if (!multiply_indexing->alu_ops_remaining) {
            remaining_indexings &= ~BITFIELD_BIT(multiply_indexing_index);
         }
      } else {
         /* Schedule new operations for chains currently pinned.
          * Note that on VLIW5, because only up to 4 literals can be used in an instruction group,
          * with the multiplication, in some cases, it may be possible to schedule operations for
          * only 3 rather than 4 pinned chains. In this case, with the use of sorting, the
          * lowest-priority chain (likely the shorter one) will be skipped in the current
          * instruction group.
          */
         uint8_t components_with_pinned_chains = 0b0;
         u_foreach_bit (pinned_sorted_indexing_index,
                        sorted_indexings_pinned_to_vector_components) {
            uint8_t const pinned_indexing_index = sorted_indexings[pinned_sorted_indexing_index];
            struct terakan_vertex_input_indexing const * const pinned_indexing =
               &indexings[pinned_indexing_index];
            components_with_pinned_chains |=
               BITFIELD_BIT(pinned_indexing->current_vector_component_pinned_to);
            if (terakan_vertex_input_try_add_alu_constant_for_next_op(
                   pinned_indexing, &group_literal_count, group_literals,
                   &group_indexing_constant_word0_src1[pinned_indexing
                                                          ->current_vector_component_pinned_to])) {
               group_indexings[pinned_indexing->current_vector_component_pinned_to] =
                  pinned_indexing_index;
            }
         }

         /* Schedule new chains on the vector units.
          * Note that in the current code, only one chain can be pinned to a single component at
          * once. However, if an operation of a currently pinned chain wasn't scheduled due to a
          * literal constant count overflow, it's still possible to schedule a new chain consisting
          * of only 1 operation on the vector component without interfering with the existing pinned
          * chain.
          */
         for (unsigned component_index = 0; component_index < 4; ++component_index) {
            if (group_indexings[component_index] != UINT8_MAX) {
               continue;
            }
            uint8_t const component_bit = BITFIELD_BIT(component_index);
            /* Try to schedule a pre-multiplication chain first, to unblock multiplication and
             * post-multiplication operations earlier.
             */
            u_foreach_bit (sorted_indexing_index,
                           unscheduled_sorted_indexings_unpinned_where_next_is_pre_multiply) {
               uint8_t const indexing_index = sorted_indexings[sorted_indexing_index];
               struct terakan_vertex_input_indexing const * const unpinned_indexing =
                  &indexings[indexing_index];
               if ((components_with_pinned_chains & component_bit) &&
                   !IS_POT(
                      unpinned_indexing->alu_ops_remaining &
                      BITFIELD_MASK(
                         TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH))) {
                  /* The component already has a chain pinned, but wasn't able to continue the
                   * pinned chain in this instruction group, so only schedule a single-operation
                   * chain that itself won't need to be pinned.
                   */
                  continue;
               }
               if (!terakan_vertex_input_try_add_alu_constant_for_next_op(
                      unpinned_indexing, &group_literal_count, group_literals,
                      &group_indexing_constant_word0_src1[component_index])) {
                  continue;
               }
               group_indexings[component_index] = indexing_index;
               unscheduled_sorted_indexings_unpinned_where_next_is_pre_multiply &=
                  ~BITFIELD_BIT(sorted_indexing_index);
            }
            if (group_indexings[component_index] != UINT8_MAX) {
               continue;
            }
            /* Try to schedule a post-multiplication chain or an indexing function that doesn't
             * include multiplication.
             */
            u_foreach_bit (sorted_indexing_index,
                           unscheduled_sorted_indexings_unpinned_where_next_is_in_final_chain) {
               uint8_t const indexing_index = sorted_indexings[sorted_indexing_index];
               struct terakan_vertex_input_indexing const * const unpinned_indexing =
                  &indexings[indexing_index];
               if ((components_with_pinned_chains & component_bit) &&
                   !IS_POT(unpinned_indexing->alu_ops_remaining)) {
                  /* The component already has a chain pinned, but wasn't able to continue the
                   * pinned chain in this instruction group, so only schedule a single-operation
                   * chain that itself won't need to be pinned.
                   */
                  continue;
               }
               if (!terakan_vertex_input_try_add_alu_constant_for_next_op(
                      unpinned_indexing, &group_literal_count, group_literals,
                      &group_indexing_constant_word0_src1[component_index])) {
                  continue;
               }
               group_indexings[component_index] = indexing_index;
               unscheduled_sorted_indexings_unpinned_where_next_is_in_final_chain &=
                  ~BITFIELD_BIT(sorted_indexing_index);
            }
         }

         /* Schedule any pending operation on the scalar unit, without the need to pin the chain to
          * any component, because the scalar unit can write to any component, and the destination
          * component in the instruction can be patched depending on the GPR read port allocation
          * when its result is later read.
          * Only do this if all vector units are occupied, however, so the destination component can
          * be set freely later with the instruction unambiguously being assigned to the scalar
          * unit, rather than a vector unit, by the hardware, without adjusting the bank swizzle if
          * changing the destination component ends up moving the instruction between vector and
          * scalar units.
          */
         if (!is_r9xx && group_indexings[4] == UINT8_MAX && group_indexings[0] != UINT8_MAX &&
             group_indexings[1] != UINT8_MAX && group_indexings[2] != UINT8_MAX &&
             group_indexings[3] != UINT8_MAX) {
            u_foreach_bit (sorted_indexing_index,
                           unscheduled_sorted_indexings_unpinned_where_next_is_pre_multiply) {
               uint8_t const indexing_index = sorted_indexings[sorted_indexing_index];
               struct terakan_vertex_input_indexing const * const unpinned_indexing =
                  &indexings[indexing_index];
               if (!terakan_vertex_input_try_add_alu_constant_for_next_op(
                      unpinned_indexing, &group_literal_count, group_literals,
                      &group_indexing_constant_word0_src1[4])) {
                  continue;
               }
               group_indexings[4] = indexing_index;
               unscheduled_sorted_indexings_unpinned_where_next_is_pre_multiply &=
                  ~BITFIELD_BIT(sorted_indexing_index);
            }
            if (group_indexings[4] == UINT8_MAX) {
               u_foreach_bit (sorted_indexing_index,
                              unscheduled_sorted_indexings_unpinned_where_next_is_in_final_chain) {
                  uint8_t const indexing_index = sorted_indexings[sorted_indexing_index];
                  struct terakan_vertex_input_indexing const * const unpinned_indexing =
                     &indexings[indexing_index];
                  if (!terakan_vertex_input_try_add_alu_constant_for_next_op(
                         unpinned_indexing, &group_literal_count, group_literals,
                         &group_indexing_constant_word0_src1[4])) {
                     continue;
                  }
                  group_indexings[4] = indexing_index;
                  unscheduled_sorted_indexings_unpinned_where_next_is_in_final_chain &=
                     ~BITFIELD_BIT(sorted_indexing_index);
               }
            }
         }

         /* Emit the current pre-fetch ALU instruction group. */

         for (unsigned component_index = 0; component_index < 5; ++component_index) {
            uint8_t const indexing_index = group_indexings[component_index];

            bool const component_is_scalar = component_index > 3;

            if (indexing_index == UINT8_MAX) {
               /* Try to fill the slot with a write of an unbound attribute's component if there's
                * nothing else to do in the current slot. On the scalar unit, when the choice is
                * ambiguous, prefer filling a component that's occupied by a pinned chain on the
                * vector unit corresponding to it.
                */
               int const unbound_component_index =
                  component_is_scalar
                     ? (is_r9xx ? -1
                                : terakan_vertex_input_next_unbound_attribute_scalar_component(
                                     unbound_attributes_remaining_per_component,
                                     components_with_pinned_chains))
                     : (int)component_index;
               if (unbound_component_index >= 0) {
                  terakan_vertex_input_try_emit_unbound_attribute_mov(
                     (unsigned)unbound_component_index, unbound_attributes_remaining_per_component,
                     layout->attribute_format_fetch_word1, code_out);
               }
               continue;
            }

            assert(!(is_r9xx && component_is_scalar));
            struct terakan_vertex_input_indexing * const indexing = &indexings[indexing_index];

            enum terakan_vertex_input_indexing_alu_op const alu_op_index =
               (enum terakan_vertex_input_indexing_alu_op)(ffs(indexing->alu_ops_remaining) - 1);
            bool const alu_op_writes_to_2048_stride_accumulator =
               alu_op_index == TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_2048_STRIDE_DOUBLE_INDEX ||
               alu_op_index == TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_2048_STRIDE_ADD_BASE;

            assert(code_out->pre_fetch_alu_qwords <
                   TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_QWORDS);
            uint32_t * const alu_op2 = &code_out->pre_fetch_alu[2 * code_out->pre_fetch_alu_qwords];

            alu_op2[0] = 0;
            alu_op2[1] = S_SQ_ALU_WORD1_OP2_WRITE_MASK(true) |
                         S_SQ_ALU_WORD1_DST_GPR(alu_op_writes_to_2048_stride_accumulator
                                                   ? indexing->dest_gpr_2048_stride
                                                   : indexing->dest_gpr) |
                         S_SQ_ALU_WORD1_DST_CHAN(component_index & 3);

            uint32_t alu_op2_inst = EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP;

            /* If the first source is the index, the bank swizzle will also be configured so that
             * it's read on the correct cycle depending on whether it's the accumulator or R0.
             */
            bool src0_is_index = true;
            bool src1_is_constant = true;

            switch (alu_op_index) {
            case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_PRE_SHIFT:
            case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_POST_SHIFT:
               alu_op2_inst = EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_LSHR_INT;
               break;
            case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_ADD_INCREMENT:
               alu_op2_inst = EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT;
               break;
            case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH:
               alu_op2_inst = EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MULHI_UINT;
               break;
            case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_MOVE_ZERO:
               alu_op2_inst = EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV;
               src0_is_index = false;
               src1_is_constant = false;
               /* Set both sources to the same constant, even though the result of this 2-operand
                * instruction depends only on the first source, to unambiguously not use a GPR read
                * port for the second source.
                */
               alu_op2[0] |=
                  S_SQ_ALU_WORD0_SRC0_SEL(V_SQ_ALU_SRC_0) | S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0);
               break;
            case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_2048_STRIDE_DOUBLE_INDEX:
               alu_op2_inst = EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_LSHL_INT;
               break;
            case TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_2048_STRIDE_ADD_BASE:
               alu_op2_inst = EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT;
               src1_is_constant = false;
               alu_op2[0] |= S_SQ_ALU_WORD0_SRC1_SEL(0) | S_SQ_ALU_WORD0_SRC1_CHAN(2);
               /* The bank swizzle is `VEC_012` or `SCL_210` (both are encoded as 0), the first
                * source is always the accumulator (this instruction is always preceded by
                * `2048_STRIDE_DOUBLE_INDEX`), the second is always R0.
                */
               break;
            default:
               assert(!"Invalid pre-fetch ALU operation");
            }

            alu_op2[1] |= S_SQ_ALU_WORD1_OP2_ALU_INST(alu_op2_inst);

            if (src0_is_index) {
               uint8_t index_src_gpr_and_component =
                  alu_op_index == TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_2048_STRIDE_ADD_BASE
                     ? indexing->current_gpr_and_component_2048_stride
                     : indexing->current_gpr_and_component;
               uint8_t const index_src_gpr = index_src_gpr_and_component >> 2;
               if (index_src_gpr == 0) {
                  /* The first source is R0, loaded on cycle 1 to prevent read port conflicts with
                   * the accumulator.
                   */
                  alu_op2[1] |= S_SQ_ALU_WORD1_BANK_SWIZZLE(component_is_scalar ? SQ_ALU_SCL_122
                                                                                : SQ_ALU_VEC_102);
               } else {
                  /* The first source is the accumulator.
                   * The bank swizzle is `VEC_012` or `SCL_210` (both are encoded as 0).
                   */
                  if (!component_is_scalar) {
                     if (indexing->last_scalar_or_4_slot_write_pre_fetch_alu_qword_offset !=
                         UINT32_MAX) {
                        /* On VLIW5, make the last instruction writing to the index accumulator,
                         * which is executed on the scalar unit in this case, write to the component
                         * this vector instruction will be reading from.
                         * On VLIW4, expecting 4-slot instructions to write to all components.
                         */
                        if (!is_r9xx) {
                           uint32_t * const last_scalar_write_alu_word1 =
                              &code_out->pre_fetch_alu
                                  [2 * indexing
                                          ->last_scalar_or_4_slot_write_pre_fetch_alu_qword_offset +
                                   1];
                           *last_scalar_write_alu_word1 =
                              (*last_scalar_write_alu_word1 & C_SQ_ALU_WORD1_DST_CHAN) |
                              S_SQ_ALU_WORD1_DST_CHAN(component_index);
                        }
                        index_src_gpr_and_component =
                           (index_src_gpr_and_component & ~(uint8_t)3) | component_index;
                     }
                     assert(
                        (index_src_gpr_and_component & 3) == component_index &&
                        "Using the same accumulator vector write and read components in one "
                        "instruction group for GPR read port allocation simplicity, particularly, "
                        "exclusively pinning a chain of operations to a single component");
                  }
               }
               alu_op2[0] |= S_SQ_ALU_WORD0_SRC0_SEL(index_src_gpr) |
                             S_SQ_ALU_WORD0_SRC0_CHAN(index_src_gpr_and_component & 3);
            }

            if (src1_is_constant) {
               /* The constant must be set by scheduling if the operation needs it.
                * The array is zero-initialized, which means R0.X rather than a constant.
                */
               assert(group_indexing_constant_word0_src1[component_index] != 0);
               alu_op2[0] |= group_indexing_constant_word0_src1[component_index];
            }

            /* Update the last scalar write offset to the new instruction before advancing. */
            indexing->last_scalar_or_4_slot_write_pre_fetch_alu_qword_offset =
               component_is_scalar ? code_out->pre_fetch_alu_qwords : UINT32_MAX;

            /* Advance to the next instruction. */
            ++code_out->pre_fetch_alu_qwords;

            /* Update the indexing to match the state after the new instruction. */
            *(alu_op_writes_to_2048_stride_accumulator
                 ? &indexing->current_gpr_and_component_2048_stride
                 : &indexing->current_gpr_and_component) =
               (G_SQ_ALU_WORD1_DST_GPR(alu_op2[1]) << 2) | G_SQ_ALU_WORD1_DST_CHAN(alu_op2[1]);
            indexing->alu_ops_remaining &= ~BITFIELD_BIT(alu_op_index);
            if (!indexing->alu_ops_remaining ||
                (enum terakan_vertex_input_indexing_alu_op)(ffs(indexing->alu_ops_remaining) - 1) ==
                   TERAKAN_VERTEX_INPUT_INDEXING_ALU_OP_INSTANCE_DIVISION_MULTIPLY_HIGH) {
               /* End the chain, either the pre-multiplication or the final one.
                * Operations after the multiplication will be in their own chain, later pinned to a
                * new component if needed.
                */
               indexing->current_vector_component_pinned_to = UINT8_MAX;
               if (!indexing->alu_ops_remaining) {
                  remaining_indexings &= ~BITFIELD_BIT(indexing_index);
               }
            } else if (!component_is_scalar) {
               indexing->current_vector_component_pinned_to = component_index;
            }
         }
      }

      /* Set the `LAST` field for the last operation in the instruction group. */
      assert(
         code_out->pre_fetch_alu_qwords != pre_fetch_alu_qwords_before_group &&
         "At least one pre-fetch ALU instruction is expected to have been scheduled in each group, "
         "otherwise scheduling wouldn't be able to make any progress");
      code_out->pre_fetch_alu[2 * (code_out->pre_fetch_alu_qwords - 1)] |=
         S_SQ_ALU_WORD0_LAST(true);

      /* Emit the literals. */
      if (group_literal_count & 1) {
         group_literals[group_literal_count++] = 0;
      }
      assert(TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_QWORDS - code_out->pre_fetch_alu_qwords >=
             group_literal_count >> 1);
      memcpy(&code_out->pre_fetch_alu[2 * code_out->pre_fetch_alu_qwords], group_literals,
             sizeof(uint32_t) * group_literal_count);
      code_out->pre_fetch_alu_qwords += group_literal_count >> 1;

      /* If the new instruction group doesn't fit into the current clause, split the clause before
       * the new group.
       */
      if (code_out->pre_fetch_alu_qwords - current_pre_fetch_alu_clause_address > 0x80) {
         assert(pre_fetch_alu_clause_count < TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_CLAUSES);
         pre_fetch_alu_clauses_qwords[pre_fetch_alu_clause_count++] =
            (uint8_t)(pre_fetch_alu_qwords_before_group - current_pre_fetch_alu_clause_address);
         current_pre_fetch_alu_clause_address = pre_fetch_alu_qwords_before_group;
      }
   }

   /* Fill remaining unbound attributes. */
   while (true) {
      uint32_t const pre_fetch_alu_qwords_before_group = code_out->pre_fetch_alu_qwords;

      for (unsigned unbound_component_index = 0; unbound_component_index < 4;
           ++unbound_component_index) {
         terakan_vertex_input_try_emit_unbound_attribute_mov(
            (unsigned)unbound_component_index, unbound_attributes_remaining_per_component,
            layout->attribute_format_fetch_word1, code_out);
      }

      if (code_out->pre_fetch_alu_qwords == pre_fetch_alu_qwords_before_group) {
         /* No unbound attributes remaining. No need to try to schedule a write on the scalar unit
          * either in this case.
          */
         break;
      }

      if (!is_r9xx) {
         /* Fill on the scalar unit. */
         int const unbound_component_index =
            terakan_vertex_input_next_unbound_attribute_scalar_component(
               unbound_attributes_remaining_per_component, 0xF);
         if (unbound_component_index >= 0) {
            terakan_vertex_input_try_emit_unbound_attribute_mov(
               (unsigned)unbound_component_index, unbound_attributes_remaining_per_component,
               layout->attribute_format_fetch_word1, code_out);
         }
      }

      code_out->pre_fetch_alu[2 * (code_out->pre_fetch_alu_qwords - 1)] |=
         S_SQ_ALU_WORD0_LAST(true);

      /* If the new instruction group doesn't fit into the current clause, split the clause before
       * the new group.
       */
      if (code_out->pre_fetch_alu_qwords - current_pre_fetch_alu_clause_address > 0x80) {
         assert(pre_fetch_alu_clause_count < TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_CLAUSES);
         pre_fetch_alu_clauses_qwords[pre_fetch_alu_clause_count++] =
            (uint8_t)(pre_fetch_alu_qwords_before_group - current_pre_fetch_alu_clause_address);
         current_pre_fetch_alu_clause_address = pre_fetch_alu_qwords_before_group;
      }
   }

   /* Add the last pre-fetch ALU clause. */
   if (code_out->pre_fetch_alu_qwords > current_pre_fetch_alu_clause_address) {
      assert(pre_fetch_alu_clause_count < TERAKAN_VERTEX_INPUT_FS_MAX_PRE_FETCH_ALU_CLAUSES);
      pre_fetch_alu_clauses_qwords[pre_fetch_alu_clause_count++] =
         (uint8_t)(code_out->pre_fetch_alu_qwords - current_pre_fetch_alu_clause_address);
   }

   /* Calculate the mega-fetch byte counts.
    * Bit 0 of `are_mega_fetch_and_mega_fetch_counts` is set if the fetch is a mega-fetch, bits [:1]
    * are the mega-fetch byte count.
    * The hardware can convert mini-fetches into mega-fetches, so the mega-fetch count needs to be
    * specified for mini-fetches too.
    */
   uint8_t are_mega_fetch_and_mega_fetch_counts[TERAKAN_RESOURCE_HW_COUNT_FETCH];
   if (!is_r9xx) {
      bool const disable_mega_fetch_coalescing =
         getenv("TERAKAN_DEBUG_DISABLE_MEGA_FETCH_COALESCING") != NULL;
      for (uint8_t sorted_attribute_index = 0;
           sorted_attribute_index < bound_used_attribute_count;) {
         uint8_t const mega_fetch_attribute_index =
            sorted_attribute_indices[sorted_attribute_index];
         uint8_t mega_fetch_count = attributes_bytes_per_element[mega_fetch_attribute_index];
         if (mega_fetch_count == 0) {
            /* Attributes with an invalid format may have a null descriptor bound rather than the
             * real binding buffer.
             */
            are_mega_fetch_and_mega_fetch_counts[mega_fetch_attribute_index] = 1;
            ++sorted_attribute_index;
            continue;
         }
         uint8_t const mega_fetch_binding = layout->attribute_bindings[mega_fetch_attribute_index];
         uint32_t const mega_fetch_offset = layout->attribute_offsets[mega_fetch_attribute_index];
         uint8_t const mega_fetch_indexing = attribute_indexings[mega_fetch_attribute_index];
         uint8_t mega_fetch_attribute_count = 1;
         for (; !disable_mega_fetch_coalescing &&
                sorted_attribute_index + mega_fetch_attribute_count < bound_used_attribute_count;
              ++mega_fetch_attribute_count) {
            uint8_t const mega_fetch_candidate_attribute_index =
               sorted_attribute_indices[sorted_attribute_index + mega_fetch_attribute_count];
            uint8_t const mega_fetch_candidate_bytes_per_element =
               attributes_bytes_per_element[mega_fetch_candidate_attribute_index];
            if (mega_fetch_candidate_bytes_per_element == 0) {
               /* Attributes with an invalid format may have a null descriptor bound rather than the
                * real binding buffer.
                */
               are_mega_fetch_and_mega_fetch_counts[mega_fetch_candidate_attribute_index] = 1;
               continue;
            }
            /* Check the hard requirements. */
            if (layout->attribute_bindings[mega_fetch_candidate_attribute_index] !=
                   mega_fetch_binding ||
                attribute_indexings[mega_fetch_candidate_attribute_index] != mega_fetch_indexing) {
               break;
            }
            uint32_t const mega_fetch_candidate_absolute_offset =
               layout->attribute_offsets[mega_fetch_candidate_attribute_index];
            assert(
               mega_fetch_candidate_absolute_offset >= mega_fetch_offset &&
               "Expecting that within one indexing function and binding, vertex fetch attributes "
               "are sorted by offset");
            uint32_t const mega_fetch_candidate_relative_offset =
               mega_fetch_candidate_absolute_offset - mega_fetch_offset;
            /* Up to 64 bytes can be specified as the mega fetch size in a fetch instruction. */
            if (mega_fetch_candidate_relative_offset >
                64 - mega_fetch_candidate_bytes_per_element) {
               break;
            }
            /* Check if coalescing the fetches into a mega-fetch would be desirable.
             *
             * Section 6.1.3 "Float4 Or Float1" of the AMD Accelerated Parallel Processing OpenCL
             * Programming Guide rev2.3 says:
             *
             *     "The internal memory paths on ATI Radeon(tm) HD 5000-series devices support
             *     128-bit transfers."
             *
             * Not coalescing fetches across a gap of 16 or more bytes.
             */
            /* TODO(Triang3l): Actually research optimal mega-fetch ranges instead of guessing. */
            if (mega_fetch_candidate_relative_offset >= mega_fetch_count + 16) {
               break;
            }
            mega_fetch_count =
               MAX2(mega_fetch_candidate_relative_offset + mega_fetch_candidate_bytes_per_element,
                    mega_fetch_count);
         }
         are_mega_fetch_and_mega_fetch_counts[mega_fetch_attribute_index] =
            1 | (mega_fetch_count << 1);
         for (uint8_t mini_fetch_index = 1; mini_fetch_index < mega_fetch_attribute_count;
              ++mini_fetch_index) {
            uint8_t const mini_fetch_attribute_index =
               sorted_attribute_indices[sorted_attribute_index + mini_fetch_index];
            are_mega_fetch_and_mega_fetch_counts[mini_fetch_attribute_index] =
               (mega_fetch_count -
                (layout->attribute_offsets[mini_fetch_attribute_index] - mega_fetch_offset))
               << 1;
         }
         sorted_attribute_index += mega_fetch_attribute_count;
      }
   }

   /* Emit the fetch instructions. */
   code_out->fetch_count = 0;
   for (uint8_t sorted_attribute_index = 0; sorted_attribute_index < bound_used_attribute_count;
        ++sorted_attribute_index) {
      uint8_t const attribute_index = sorted_attribute_indices[sorted_attribute_index];
      uint8_t const attribute_binding = layout->attribute_bindings[attribute_index];
      struct terakan_vertex_input_indexing const * const indexing =
         &indexings[attribute_indexings[attribute_index]];
      uint8_t const fetch_src_gpr_and_component =
         layout->bindings_with_2048_stride_as_1024 & BITFIELD_BIT(attribute_binding)
            ? indexing->current_gpr_and_component_2048_stride
            : indexing->current_gpr_and_component;
      uint32_t const format_fetch_word1 = layout->attribute_format_fetch_word1[attribute_index] &
                                          TERAKAN_VERTEX_INPUT_FORMAT_FETCH_WORD1_BITS;
      uint32_t * const fetch = &code_out->fetch[4 * code_out->fetch_count++];
      fetch[0] =
         S_SQ_VTX_WORD0_FETCH_TYPE(layout->instance_rate_attributes & BITFIELD_BIT(attribute_index)
                                      ? SQ_VTX_FETCH_INSTANCE_DATA
                                      : SQ_VTX_FETCH_NO_INDEX_OFFSET) |
         S_SQ_VTX_WORD0_BUFFER_ID(
            bindings_resource_indices[attribute_binding][attribute_truncations[attribute_index]]) |
         S_SQ_VTX_WORD0_SRC_GPR(fetch_src_gpr_and_component >> 2) |
         S_SQ_VTX_WORD0_SRC_SEL_X(fetch_src_gpr_and_component & 3);
      fetch[1] = S_SQ_VTX_WORD1_GPR_DST_GPR(1 + attribute_index) | format_fetch_word1;
      fetch[2] = S_SQ_VTX_WORD2_OFFSET(layout->attribute_offsets[attribute_index]);
#if UTIL_ARCH_BIG_ENDIAN
      fetch[2] |= S_SQ_VTX_WORD2_ENDIAN_SWAP(
         terascale_format_big_endian_swap[G_SQ_VTX_WORD1_DATA_FORMAT(format_fetch_word1)]);
#endif
      fetch[3] = 0;
      if (!is_r9xx) {
         uint8_t const is_mega_fetch_and_mega_fetch_count =
            are_mega_fetch_and_mega_fetch_counts[attribute_index];
         /* For simplicity, missing attributes are still implemented as fetches, with the smallest
          * possible mega-fetch byte count.
          */
         fetch[0] |=
            S_SQ_VTX_WORD0_MEGA_FETCH_COUNT(MAX2(is_mega_fetch_and_mega_fetch_count >> 1, 1u) - 1u);
         fetch[2] |= S_SQ_VTX_WORD2_MEGA_FETCH(is_mega_fetch_and_mega_fetch_count & 1);
      }
   }

   /* #Signed2101010Alpha: correct the two-bit field of signed 2_10_10_10 and 10_10_10_2
    * attributes, which the hardware decodes as unsigned. Only bound attributes are corrected: an
    * unbound one was filled with a constant 0 or 1 by the pre-fetch ALU, which is already what the
    * shader must see.
    */
   uint32_t post_fetch_alu_clauses_qwords[TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_CLAUSES];
   unsigned post_fetch_alu_clause_count = 0;
   {
      uint32_t current_post_fetch_alu_clause_address = 0;
      for (unsigned sorted_index = 0; sorted_index < bound_used_attribute_count; ++sorted_index) {
         unsigned const attribute_index = sorted_attribute_indices[sorted_index];
         unsigned number_format;
         unsigned components = terakan_vertex_input_signed_2_bit_alpha_components(
            layout->attribute_format_fetch_word1[attribute_index], &number_format);
         unsigned const destination_gpr = 1 + attribute_index;
         while (components) {
            unsigned const component_index = (unsigned)u_bit_scan(&components);
            uint32_t const qwords_before_component = code_out->post_fetch_alu_qwords;
            uint32_t const source =
               S_SQ_ALU_WORD0_SRC0_SEL(destination_gpr) |
               S_SQ_ALU_WORD0_SRC0_CHAN(component_index);
            uint32_t const destination = S_SQ_ALU_WORD1_DST_GPR(destination_gpr) |
                                         S_SQ_ALU_WORD1_DST_CHAN(component_index);

            if (number_format == TERASCALE_FORMAT_SQ_NUM_FORMAT_INT) {
               /* An integer attribute keeps the raw bits, so the two of them only have to be moved
                * to the top of the register and brought back with an arithmetic shift, which is
                * what sign extension is.
                */
               uint32_t const shift_literal[1] = {32 - 2};
               terakan_vertex_input_emit_post_fetch_alu(
                  code_out,
                  source | S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_LITERAL) |
                     S_SQ_ALU_WORD0_SRC1_CHAN(0),
                  S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_LSHL_INT) |
                     S_SQ_ALU_WORD1_OP2_WRITE_MASK(true) | destination,
                  1, shift_literal);
               terakan_vertex_input_emit_post_fetch_alu(
                  code_out,
                  source | S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_LITERAL) |
                     S_SQ_ALU_WORD0_SRC1_CHAN(0),
                  S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ASHR_INT) |
                     S_SQ_ALU_WORD1_OP2_WRITE_MASK(true) | destination,
                  1, shift_literal);
               goto component_done;
            }

            bool const is_normalized = number_format == TERASCALE_FORMAT_SQ_NUM_FORMAT_NORM;

            /* v = v * (normalized ? 0.75 : 0.25) + 0.625. The bias is deliberately not 0.5: it
             * keeps every intermediate an eighth away from the integer boundary FRACT turns on, so
             * that the inexact thirds the normalized path starts from cannot land on the wrong
             * side of it.
             */
            uint32_t const scale_literals[2] = {is_normalized ? 0x3F400000u : 0x3E800000u,
                                                0x3F200000u};
            terakan_vertex_input_emit_post_fetch_alu(
               code_out,
               source | S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_LITERAL) |
                  S_SQ_ALU_WORD0_SRC1_CHAN(0),
               S_SQ_ALU_WORD1_OP3_SRC2_SEL(V_SQ_ALU_SRC_LITERAL) |
                  S_SQ_ALU_WORD1_OP3_SRC2_CHAN(1) |
                  S_SQ_ALU_WORD1_OP3_ALU_INST(EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_MULADD) |
                  destination,
               2, scale_literals);

            /* v = fract(v), which is where the wrap that makes the value signed happens. */
            terakan_vertex_input_emit_post_fetch_alu(
               code_out, source | S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
               S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FRACT) |
                  S_SQ_ALU_WORD1_OP2_WRITE_MASK(true) | destination,
               0, NULL);

            /* v = v * 4.0 - 2.5, undoing the scale and the bias, giving 0, 1, -2, -1. */
            uint32_t const unscale_literals[2] = {0x40800000u, 0xC0200000u};
            terakan_vertex_input_emit_post_fetch_alu(
               code_out,
               source | S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_LITERAL) |
                  S_SQ_ALU_WORD0_SRC1_CHAN(0),
               S_SQ_ALU_WORD1_OP3_SRC2_SEL(V_SQ_ALU_SRC_LITERAL) |
                  S_SQ_ALU_WORD1_OP3_SRC2_CHAN(1) |
                  S_SQ_ALU_WORD1_OP3_ALU_INST(EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_MULADD) |
                  destination,
               2, unscale_literals);

            if (is_normalized) {
               /* -2 is the smallest two-bit signed value and normalizes to -1, which is what the
                * specification asks a signed normalized -2 to clamp to.
                */
               terakan_vertex_input_emit_post_fetch_alu(
                  code_out,
                  source | S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_1) |
                     S_SQ_ALU_WORD0_SRC1_NEG(true),
                  S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MAX) |
                     S_SQ_ALU_WORD1_OP2_WRITE_MASK(true) | destination,
                  0, NULL);
            }

         component_done:
            /* Split the clause before this component's sequence if it no longer fits. */
            if (code_out->post_fetch_alu_qwords - current_post_fetch_alu_clause_address > 0x80) {
               assert(post_fetch_alu_clause_count <
                      TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_CLAUSES);
               post_fetch_alu_clauses_qwords[post_fetch_alu_clause_count++] =
                  qwords_before_component - current_post_fetch_alu_clause_address;
               current_post_fetch_alu_clause_address = qwords_before_component;
            }
         }
      }
      if (code_out->post_fetch_alu_qwords > current_post_fetch_alu_clause_address) {
         assert(post_fetch_alu_clause_count < TERAKAN_VERTEX_INPUT_FS_MAX_POST_FETCH_ALU_CLAUSES);
         post_fetch_alu_clauses_qwords[post_fetch_alu_clause_count++] =
            code_out->post_fetch_alu_qwords - current_post_fetch_alu_clause_address;
      }
   }

   /* Construct the control flow program. */

   code_out->control_flow_qwords = 0;
   bool const has_fetch_clause = code_out->fetch_count != 0;
   unsigned const control_flow_qword_count = pre_fetch_alu_clause_count +
                                             (unsigned)has_fetch_clause +
                                             post_fetch_alu_clause_count + 1;
   assert(control_flow_qword_count <= TERAKAN_VERTEX_INPUT_FS_MAX_CONTROL_FLOW_QWORDS);
   unsigned next_clause_qword_offset = control_flow_qword_count;

   /* Pre-fetch ALU clauses. */
   for (unsigned pre_fetch_alu_clause_index = 0;
        pre_fetch_alu_clause_index < pre_fetch_alu_clause_count; ++pre_fetch_alu_clause_index) {
      unsigned const pre_fetch_alu_clause_qwords =
         pre_fetch_alu_clauses_qwords[pre_fetch_alu_clause_index];
      assert(pre_fetch_alu_clause_qwords <= 128);
      assert(code_out->control_flow_qwords < control_flow_qword_count);
      uint32_t * const control_flow_pre_fetch_alu_clause =
         &code_out->control_flow[2 * code_out->control_flow_qwords++];
      control_flow_pre_fetch_alu_clause[0] = S_SQ_CF_WORD0_ADDR(next_clause_qword_offset);
      control_flow_pre_fetch_alu_clause[1] =
         S_SQ_CF_ALU_WORD1_COUNT(pre_fetch_alu_clause_qwords - 1) |
         EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU;
      next_clause_qword_offset += pre_fetch_alu_clause_qwords;
   }

   /* Fetch clause. */
   if (has_fetch_clause) {
      assert(code_out->fetch_count <= 64);
      /* Fetch instructions must be aligned to 2 qwords. */
      next_clause_qword_offset = ALIGN_POT(next_clause_qword_offset, 2);
      assert(code_out->control_flow_qwords < control_flow_qword_count);
      uint32_t * const control_flow_fetch_clause =
         &code_out->control_flow[2 * code_out->control_flow_qwords++];
      control_flow_fetch_clause[0] = S_SQ_CF_WORD0_ADDR(next_clause_qword_offset);
      control_flow_fetch_clause[1] =
         S_SQ_CF_WORD1_COUNT(code_out->fetch_count - 1) |
         S_SQ_CF_WORD1_BARRIER(code_out->pre_fetch_alu_qwords != 0) |
         (is_r9xx ? EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX : EG_V_SQ_CF_WORD1_SQ_CF_INST_VTX);
      next_clause_qword_offset += 2 * code_out->fetch_count;
   }

   /* Post-fetch ALU clauses. */
   for (unsigned post_fetch_alu_clause_index = 0;
        post_fetch_alu_clause_index < post_fetch_alu_clause_count; ++post_fetch_alu_clause_index) {
      uint32_t const post_fetch_alu_clause_qwords =
         post_fetch_alu_clauses_qwords[post_fetch_alu_clause_index];
      assert(post_fetch_alu_clause_qwords <= 128);
      assert(code_out->control_flow_qwords < control_flow_qword_count);
      uint32_t * const control_flow_post_fetch_alu_clause =
         &code_out->control_flow[2 * code_out->control_flow_qwords++];
      control_flow_post_fetch_alu_clause[0] = S_SQ_CF_WORD0_ADDR(next_clause_qword_offset);
      control_flow_post_fetch_alu_clause[1] =
         S_SQ_CF_ALU_WORD1_COUNT(post_fetch_alu_clause_qwords - 1) |
         /* The first of these clauses reads what the fetch clause wrote, so it has to wait for it.
          */
         S_SQ_CF_ALU_WORD1_BARRIER(post_fetch_alu_clause_index == 0) |
         EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU;
      next_clause_qword_offset += post_fetch_alu_clause_qwords;
   }

   /* Return. */
   assert(code_out->control_flow_qwords < control_flow_qword_count);
   uint32_t * const control_flow_return =
      &code_out->control_flow[2 * code_out->control_flow_qwords++];
   control_flow_return[0] = 0;
   control_flow_return[1] = S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_RETURN;
}

void
terakan_vertex_input_combine_fs(struct terakan_vertex_input_fs_code const * const code,
                                uint32_t * const program_out)
{
   uint32_t program_offset_qwords = 0;

   util_memcpy_cpu_to_le32(program_out + 2 * program_offset_qwords, code->control_flow,
                           sizeof(uint32_t) * 2 * code->control_flow_qwords);
   program_offset_qwords += code->control_flow_qwords;

   util_memcpy_cpu_to_le32(program_out + 2 * program_offset_qwords, code->pre_fetch_alu,
                           sizeof(uint32_t) * 2 * code->pre_fetch_alu_qwords);
   program_offset_qwords += code->pre_fetch_alu_qwords;

   if (code->fetch_count != 0) {
      if (program_offset_qwords & 1) {
         /* Fetch instructions must be aligned to 2 qwords. */
         memset(program_out + 2 * program_offset_qwords, 0, sizeof(uint32_t) * 2);
         ++program_offset_qwords;
      }
      util_memcpy_cpu_to_le32(program_out + 2 * program_offset_qwords, code->fetch,
                              sizeof(uint32_t) * 4 * code->fetch_count);
      program_offset_qwords += 2 * code->fetch_count;
   }

   util_memcpy_cpu_to_le32(program_out + 2 * program_offset_qwords, code->post_fetch_alu,
                           sizeof(uint32_t) * 2 * code->post_fetch_alu_qwords);
   program_offset_qwords += code->post_fetch_alu_qwords;

   assert(program_offset_qwords == terakan_vertex_input_fs_code_qwords(code));
}
