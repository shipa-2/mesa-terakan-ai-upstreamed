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

#include "terakan_vk_state.h"

#include "terakan_app_config_draw.h"
#include "terakan_buffer.h"
#include "terakan_command_buffer.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_vertex_input.h"

#include "amd/terascale/common/terascale_format.h"
#include "gallium/drivers/r600/evergreend.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "vk_command_buffer.h"
#include "vk_enum_to_str.h"
#include "vk_format.h"
#include "vk_graphics_state.h"

#include <stdlib.h>
#include "vk_log.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

uint32_t
terakan_vk_state_primitive_topology_vgt_primitive_type(VkPrimitiveTopology const primitive_topology)
{
   switch (primitive_topology) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      return V_008958_DI_PT_POINTLIST;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      return V_008958_DI_PT_LINELIST;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      return V_008958_DI_PT_LINESTRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      return V_008958_DI_PT_TRILIST;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      return V_008958_DI_PT_TRISTRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      return V_008958_DI_PT_TRIFAN;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      return V_008958_DI_PT_LINELIST_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
      return V_008958_DI_PT_LINESTRIP_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
      return V_008958_DI_PT_TRILIST_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
      return V_008958_DI_PT_TRISTRIP_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
      return V_008958_DI_PT_PATCH;
   default:
      assert(!"Unsupported primitive topology");
      return V_008958_DI_PT_NONE;
   }
}

struct terakan_app_config_draw_pa_vport
terakan_vk_state_viewport_to_hw(VkViewport const * const viewport)
{
   /* While viewport boundaries must be within `viewportBoundsRange`, it's easy to forget about that
    * on the application side, especially in emulation cases, so perform clamping here at least to
    * work with numeric types consistently even in case of invalid usage, and also to make sure
    * unused viewports that aren't valid don't make the guard band behave incorrectly for other
    * viewports.
    */

   if (unlikely(isnan(viewport->x) || isnan(viewport->y) || isnan(viewport->width) ||
                isnan(viewport->height))) {
      return TERAKAN_APP_CONFIG_DRAW_PA_VPORT_EMPTY;
   }

   float const x =
      CLAMP(viewport->x, TERAKAN_HW_CONFIG_DRAW_PA_CL_GB_MIN, TERAKAN_HW_CONFIG_DRAW_PA_CL_GB_MAX);
   float const y =
      CLAMP(viewport->y, TERAKAN_HW_CONFIG_DRAW_PA_CL_GB_MIN, TERAKAN_HW_CONFIG_DRAW_PA_CL_GB_MAX);

   float const width = CLAMP(viewport->width, 0.0f, TERAKAN_HW_CONFIG_DRAW_PA_CL_GB_MAX - x);
   float const height = CLAMP(viewport->height, TERAKAN_HW_CONFIG_DRAW_PA_CL_GB_MIN - y,
                              TERAKAN_HW_CONFIG_DRAW_PA_CL_GB_MAX - y);
   bool const origin_bottom = height < 0.0f;

   /* [TL, BR][X, Y]. */
   float bounds[2][2];
   bounds[0][0] = x;
   bounds[1][0] = x + width;
   bounds[origin_bottom][1] = y;
   bounds[!origin_bottom][1] = y + height;

   struct terakan_app_config_draw_pa_vport vport;

   /* Calculate the implicit scissor rectangle.
    *
    * Vulkan defines only exact clipping to -w...w, but many implementations, including Terakan, use
    * a guard band to reduce the clipping performance costs, and instead use the implicit scissor
    * to restrict rendering to the viewport bounds. However, TeraScale accepts the viewport scissor
    * rectangle with pixel granularity, so it's not possible to reproduce the clipping behavior
    * exactly with it.
    *
    * The calculation of the implicit scissor is not defined in Vulkan and in OpenGL. In the latter,
    * the issue was raised in the `GL_ARB_viewport_array` specification, but it was left unresolved,
    * while also suggesting that:
    *
    *     "Direct3D 11 specifies that rasterization along the one-pixel edges of fractional
    *     viewports to be undefined.  If implementations want defined behavior with fractional
    *     viewports, they can program a slightly wider viewport and scissor away the pixels along
    *     the edge of the expanded viewport."
    *
    * Direct3D 11, however, explicitly permits the guard band in its specification, and also defines
    * how the implicit viewport scissor must be calculated.
    * Section 15.7 "Scissor Test" of the Direct3D 11.3 Functional Specification says:
    *
    *     "The implicit scissor to the viewport (mentioned in the Viewport section) rounds the
    *     viewport X and Y extents to negative infinity. This way the scissor extents are always
    *     integers. The rounding to derive scissor extents applies to the locations where the
    *     fractional left/right/top/bottom edges would be after the float viewport transform. E.g.
    *     the viewport width and height cannot be rounded; they must be added to unrounded TopLeftX
    *     and TopLeftY to determine the right and bottom extents, which then get rounded to
    *     determine the scissor extents."
    *
    * For better compatibility with Direct3D translation layers and applications designed primarily
    * for Direct3D, and also to make sure Direct3D 9 half-pixel offset emulation via a .5 viewport
    * origin doesn't result in more pixels covered than specified in the original integer viewport,
    * apply the same rounding as in Direct3D 11. This also matches how the GPUOpen release of PAL
    * calculates the implicit scissor (note that explicit denormal flushing done by PAL is not
    * needed because render area boundaries are always non-negative in Vulkan).
    */
   for (unsigned corner = 0; corner <= 1; ++corner) {
      for (unsigned axis = 0; axis <= 1; ++axis) {
         vport.implicit_scissor.bounds[corner][axis] =
            (uint16_t)CLAMP(bounds[corner][axis], 0.0f, (float)TERAKAN_IMAGE_MAX_WIDTH_HEIGHT);
      }
   }

   if (vport.implicit_scissor.bounds[0][0] >= vport.implicit_scissor.bounds[1][0] ||
       vport.implicit_scissor.bounds[0][1] >= vport.implicit_scissor.bounds[1][1]) {
      /* Make all empty viewports use the same register values and not affect the guard band.
       * Also, `TERAKAN_IMAGE_MAX_WIDTH_HEIGHT` is valid for the BR register, but not for TL.
       */
      return TERAKAN_APP_CONFIG_DRAW_PA_VPORT_EMPTY;
   }

   vport.xy_scale_offset[0][0] = width * 0.5f;
   vport.xy_scale_offset[0][1] = x + width * 0.5f;
   vport.xy_scale_offset[1][0] = height * 0.5f;
   vport.xy_scale_offset[1][1] = y + height * 0.5f;

   for (unsigned axis = 0; axis <= 1; ++axis) {
      float const axis_scale_abs = fabsf(vport.xy_scale_offset[axis][0]);
      vport.gb_vert_horz_clip_adj[axis ^ 1] =
         (MIN2(bounds[0][axis] - TERAKAN_HW_CONFIG_DRAW_PA_CL_GB_MIN,
               TERAKAN_HW_CONFIG_DRAW_PA_CL_GB_MAX - bounds[1][axis]) +
          axis_scale_abs) /
         axis_scale_abs;
   }

   vport.z_near_far[0] = viewport->minDepth;
   vport.z_near_far[1] = viewport->maxDepth;

   return vport;
}

void
terakan_vk_state_sample_locations_to_hw(VkSampleCountFlagBits const per_pixel,
                                        VkExtent2D const grid_size,
                                        VkSampleLocationEXT const * const locations,
                                        uint8_t * const hw_16_samples_2x2_locations_out)
{
   /* #MemoryIntegrity: Don't overwrite data after `hw_16_samples_2x2_locations_out` in case of
    * invalid usage.
    */
   unsigned const sample_count = MIN2((uint32_t)per_pixel, 16);

   unsigned const pixel_mask_x = (unsigned)(grid_size.width >= 2);
   unsigned const pixel_mask_y = (unsigned)(grid_size.height >= 2);

   for (unsigned pixel_y = 0; pixel_y <= pixel_mask_y; ++pixel_y) {
      for (unsigned pixel_x = 0; pixel_x <= pixel_mask_x; ++pixel_x) {
         VkSampleLocationEXT const * const pixel_vk_locations =
            locations + ((uint32_t)per_pixel * (grid_size.width * pixel_y + pixel_x));
         uint8_t * const pixel_hw_locations =
            hw_16_samples_2x2_locations_out + (2 * pixel_y + pixel_x);
         for (unsigned sample_index = 0; sample_index < sample_count; ++sample_index) {
            VkSampleLocationEXT const * const sample_vk_location =
               &pixel_vk_locations[sample_index];
            pixel_hw_locations[4 * sample_index] =
               terakan_hw_config_draw_pa_sc_aa_sample_loc_for_tl_0_to_br_1(sample_vk_location->x,
                                                                           sample_vk_location->y);
         }
      }
   }

   /* Replicate if the application-provided grid is smaller than 2x2. */
   unsigned const pixel_mask_xy = (pixel_mask_y << 1) | pixel_mask_x;
   if (pixel_mask_xy != 0b11) {
      for (unsigned sample_index = 0; sample_index < sample_count; ++sample_index) {
         uint8_t * const pixel_hw_locations = hw_16_samples_2x2_locations_out + 4 * sample_index;
         /* Top-left to top-right, top-left to bottom-left, then top and left to bottom-right. */
         for (unsigned pixel_index = 1; pixel_index <= 3; ++pixel_index) {
            pixel_hw_locations[pixel_index] = pixel_hw_locations[pixel_index & pixel_mask_xy];
         }
      }
   }

   if (sample_count < 16) {
      memset(hw_16_samples_2x2_locations_out + 4 * sample_count, 0,
             (unsigned)sizeof(hw_16_samples_2x2_locations_out[0]) * 4 * (16 - sample_count));
   }
}

enum terakan_hw_config_draw_cb_color_control_rop3
terakan_vk_state_logic_op_rop3(VkLogicOp const logic_op)
{
   switch (logic_op) {
   case VK_LOGIC_OP_CLEAR:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_CLEAR;
   case VK_LOGIC_OP_AND:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_AND;
   case VK_LOGIC_OP_AND_REVERSE:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_AND_REVERSE;
   case VK_LOGIC_OP_COPY:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_COPY;
   case VK_LOGIC_OP_AND_INVERTED:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_AND_INVERTED;
   case VK_LOGIC_OP_NO_OP:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_NO_OP;
   case VK_LOGIC_OP_XOR:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_XOR;
   case VK_LOGIC_OP_OR:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_OR;
   case VK_LOGIC_OP_NOR:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_NOR;
   case VK_LOGIC_OP_EQUIVALENT:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_EQUIVALENT;
   case VK_LOGIC_OP_INVERT:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_INVERT;
   case VK_LOGIC_OP_OR_REVERSE:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_OR_REVERSE;
   case VK_LOGIC_OP_COPY_INVERTED:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_COPY_INVERTED;
   case VK_LOGIC_OP_OR_INVERTED:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_OR_INVERTED;
   case VK_LOGIC_OP_NAND:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_NAND;
   case VK_LOGIC_OP_SET:
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_SET;
   default:
      assert(!"Unsupported logical operation");
      return TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_COPY;
   }
}

uint32_t
terakan_vk_state_blend_factor_to_hw(VkBlendFactor const blend_factor)
{
   switch (blend_factor) {
   case VK_BLEND_FACTOR_ZERO:
      return V_028780_BLEND_ZERO;
   case VK_BLEND_FACTOR_ONE:
      return V_028780_BLEND_ONE;
   case VK_BLEND_FACTOR_SRC_COLOR:
      return V_028780_BLEND_SRC_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
      return V_028780_BLEND_ONE_MINUS_SRC_COLOR;
   case VK_BLEND_FACTOR_DST_COLOR:
      return V_028780_BLEND_DST_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
      return V_028780_BLEND_ONE_MINUS_DST_COLOR;
   case VK_BLEND_FACTOR_SRC_ALPHA:
      return V_028780_BLEND_SRC_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
      return V_028780_BLEND_ONE_MINUS_SRC_ALPHA;
   case VK_BLEND_FACTOR_DST_ALPHA:
      return V_028780_BLEND_DST_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
      return V_028780_BLEND_ONE_MINUS_DST_ALPHA;
   case VK_BLEND_FACTOR_CONSTANT_COLOR:
      return V_028780_BLEND_CONST_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
      return V_028780_BLEND_ONE_MINUS_CONST_COLOR;
   case VK_BLEND_FACTOR_CONSTANT_ALPHA:
      return V_028780_BLEND_CONST_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
      return V_028780_BLEND_ONE_MINUS_CONST_ALPHA;
   case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
      return V_028780_BLEND_SRC_ALPHA_SATURATE;
   case VK_BLEND_FACTOR_SRC1_COLOR:
      return V_028780_BLEND_SRC1_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
      return V_028780_BLEND_INV_SRC1_COLOR;
   case VK_BLEND_FACTOR_SRC1_ALPHA:
      return V_028780_BLEND_SRC1_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
      return V_028780_BLEND_INV_SRC1_ALPHA;
   default:
      assert(!"Unsupported blend factor");
      return V_028780_BLEND_ZERO;
   }
}

uint32_t
terakan_vk_state_blend_op_to_hw(VkBlendOp const blend_op)
{
   switch (blend_op) {
   case VK_BLEND_OP_ADD:
      return V_028780_COMB_DST_PLUS_SRC;
   case VK_BLEND_OP_SUBTRACT:
      return V_028780_COMB_SRC_MINUS_DST;
   case VK_BLEND_OP_REVERSE_SUBTRACT:
      return V_028780_COMB_DST_MINUS_SRC;
   case VK_BLEND_OP_MIN:
      return V_028780_COMB_MIN_DST_SRC;
   case VK_BLEND_OP_MAX:
      return V_028780_COMB_MAX_DST_SRC;
   default:
      assert(!"Unsupported blend operation");
      return V_028780_COMB_DST_PLUS_SRC;
   }
}

typedef void (*terakan_vk_state_dynamic_apply_function)(
   struct terakan_gfx_command_writer * command_writer);

static void
terakan_vk_state_dynamic_apply_vi(struct terakan_gfx_command_writer * const command_writer)
{
   static bool terakan_debug = false;
   static bool terakan_debug_initialized = false;
   if (!terakan_debug_initialized) {
      terakan_debug = getenv("TERAKAN_DEBUG") != NULL && getenv("TERAKAN_DEBUG")[0] != '\0';
      terakan_debug_initialized = true;
   }

   terakan_app_config_draw_set_sq_pgm_fetch_static_fs(&command_writer->app_config_draw, NULL);
   struct vk_vertex_input_state const * const vertex_input_state =
      command_writer->base.command_buffer->vk.dynamic_graphics_state.vi;

   if (terakan_debug) {
      fprintf(stderr, "[TERAKAN] apply_vi: bindings_valid=0x%x attrs_valid=0x%x\n",
              vertex_input_state->bindings_valid, vertex_input_state->attributes_valid);
      u_foreach_bit (bi, vertex_input_state->bindings_valid &
                              BITFIELD_MASK(TERAKAN_VK_STATE_MAX_VERTEX_BINDINGS)) {
         struct vk_vertex_binding_state const * const b = &vertex_input_state->bindings[bi];
         fprintf(stderr, "[TERAKAN]   binding[%u] stride=%u rate=%u divisor=%u\n",
                 bi, b->stride, b->input_rate, b->divisor);
      }
   }

   uint32_t attributes_unbound = BITFIELD_MASK(TERAKAN_VK_STATE_MAX_VERTEX_ATTRIBUTES);
   u_foreach_bit (attribute_index, vertex_input_state->attributes_valid &
                                      BITFIELD_MASK(TERAKAN_VK_STATE_MAX_VERTEX_ATTRIBUTES)) {
      struct vk_vertex_attribute_state const * const attribute =
         &vertex_input_state->attributes[attribute_index];
      if (!(vertex_input_state->bindings_valid & BITFIELD_BIT(attribute->binding))) {
         /* VUID-vkCmdSetVertexInputEXT-binding-04793: "For every `binding` specified by each
          * element of `pVertexAttributeDescriptions`, a `VkVertexInputBindingDescription2EXT` must
          * exist in `pVertexBindingDescriptions` with the same value of `binding`".
          */
         continue;
      }
      struct vk_vertex_binding_state const * const binding =
         &vertex_input_state->bindings[attribute->binding];
      /* Disregarding whether the format is supported here, because `terakan_app_config` will handle
       * that anyway, and also VUID-VkVertexInputAttributeDescription2EXT-format-04805: "The format
       * features of `format` must contain `VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT`".
       */
      terakan_app_config_draw_set_sq_pgm_fetch_dynamic_attribute(
         &command_writer->app_config_draw, attribute_index,
         terakan_vertex_input_format_fetch_word1(
            &terascale_format_info_r8xx[vk_format_to_pipe_format(attribute->format)]),
         attribute->binding, (uint16_t)attribute->offset,
         binding->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE, binding->divisor);
      attributes_unbound &= ~BITFIELD_BIT(attribute_index);
   }
   u_foreach_bit (attribute_index, attributes_unbound) {
      terakan_app_config_draw_set_sq_pgm_fetch_dynamic_attribute_unbound(
         &command_writer->app_config_draw, attribute_index);
   }

   /* Apply strides from dynamic vertex input state. When vkCmdSetVertexInputEXT is used,
    * strides must be propagated to the resource descriptor stride field, otherwise vertex fetch
    * reads with wrong stride (causes torn textures / all vertices at same position).
    */
   bool const use_2048_stride_as_1024 = terakan_vk_state_vertex_input_uses_2048_stride_as_1024(
      terakan_gfx_command_writer_physical_device(command_writer));
   u_foreach_bit (binding_index, vertex_input_state->bindings_valid &
                                    BITFIELD_MASK(TERAKAN_VK_STATE_MAX_VERTEX_BINDINGS)) {
      struct vk_vertex_binding_state const * const binding =
         &vertex_input_state->bindings[binding_index];
      terakan_app_config_draw_set_sq_pgm_and_resource_fetch_stride(
         &command_writer->app_config_draw, binding_index, binding->stride,
         use_2048_stride_as_1024);
   }
}

/* `MESA_VK_DYNAMIC_VI_BINDING_STRIDES` is deliberately not handled due to the complex interactions
 * with `MESA_VK_DYNAMIC_VI_BINDINGS_VALID`, setting the strides in `terakan_app_config_draw`
 * directly from `vkCmdBindPipeline` or `vkCmdBindVertexBuffers2` instead.
 */

static void
terakan_vk_state_dynamic_apply_ia_primitive_topology(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_vgt_primitive_type(
      &command_writer->app_config_draw,
      terakan_vk_state_primitive_topology_vgt_primitive_type(
         command_writer->base.command_buffer->vk.dynamic_graphics_state.ia.primitive_topology));
}

static void
terakan_vk_state_dynamic_apply_vp_viewport_count(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t vport_count =
      command_writer->base.command_buffer->vk.dynamic_graphics_state.vp.viewport_count;
   assert(vport_count <= TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   /* #MemoryIntegrity: Prevent writing out of the array bounds in `terakan_app_config` in case of
    * invalid usage.
    */
   vport_count = MIN2(vport_count, TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   terakan_app_config_draw_set_pa_vport_vport_count(&command_writer->app_config_draw, vport_count);
}

static void
terakan_vk_state_dynamic_apply_vp_viewports(struct terakan_gfx_command_writer * const command_writer)
{
   struct vk_viewport_state const * const viewport_state =
      &command_writer->base.command_buffer->vk.dynamic_graphics_state.vp;
   uint32_t vport_count;
   if (BITSET_TEST(command_writer->base.command_buffer->graphics_state_is_dynamic,
                   MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT)) {
      vport_count = viewport_state->viewport_count;
      assert(vport_count <= TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
      /* #MemoryIntegrity: Prevent writing out of the array bounds in `terakan_app_config` in case
       * of invalid usage.
       */
      vport_count = MIN2(vport_count, TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
      /* TODO(Triang3l): Dynamic `VP_VIEWPORT_COUNT` may be increased without `VP_VIEWPORTS` made
       * dirty, thus viewports beyond the count may be lost.
       */
   } else {
      /* The viewports are dynamic, but the viewport count isn't, so
       * `viewport_state->viewport_count` is out of date, because Terakan doesn't use
       * `vk_dynamic_graphics_state_copy`.
       *
       * Note that it's safe to skip setting dynamic viewports beyond the static viewport count
       * here, because the count can be dynamic only if the viewports themselves are dynamic as well
       * (via `VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT`), so `VP_VIEWPORTS` will be marked as dirty as
       * well if `VP_VIEWPORT_COUNT` is made static.
       */
      vport_count =
         terakan_app_config_draw_get_pa_vport_vport_count(&command_writer->app_config_draw);
   }
   for (uint32_t vport_index = 0; vport_index < vport_count; ++vport_index) {
      struct terakan_app_config_draw_pa_vport const vport =
         terakan_vk_state_viewport_to_hw(&viewport_state->viewports[vport_index]);
      terakan_app_config_draw_set_pa_vport_vport(&command_writer->app_config_draw, vport_index,
                                                 &vport);
   }
}

static void
terakan_vk_state_dynamic_apply_vp_scissor_count(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t scissor_count =
      command_writer->base.command_buffer->vk.dynamic_graphics_state.vp.scissor_count;
   assert(scissor_count <= TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   /* #MemoryIntegrity: Prevent writing out of the array bounds in `terakan_app_config` in case of
    * invalid usage.
    */
   scissor_count = MIN2(scissor_count, TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   terakan_app_config_draw_set_pa_vport_explicit_scissor_count(&command_writer->app_config_draw,
                                                               scissor_count);
}

static void
terakan_vk_state_dynamic_apply_vp_scissors(struct terakan_gfx_command_writer * const command_writer)
{
   struct vk_viewport_state const * const viewport_state =
      &command_writer->base.command_buffer->vk.dynamic_graphics_state.vp;
   uint32_t scissor_count;
   if (BITSET_TEST(command_writer->base.command_buffer->graphics_state_is_dynamic,
                   MESA_VK_DYNAMIC_VP_SCISSOR_COUNT)) {
      scissor_count = viewport_state->scissor_count;
      assert(scissor_count <= TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
      /* #MemoryIntegrity: Prevent writing out of the array bounds in `terakan_app_config` in case
       * of invalid usage.
       */
      scissor_count = MIN2(scissor_count, TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
      /* TODO(Triang3l): Dynamic `VP_SCISSOR_COUNT` may be increased without `VP_SCISSORS` made
       * dirty, thus scissors beyond the count may be lost.
       */
   } else {
      /* The scissors are dynamic, but the scissor count isn't, so `viewport_state->scissor_count`
       * is out of date, because Terakan doesn't use `vk_dynamic_graphics_state_copy`.
       *
       * Note that it's safe to skip setting dynamic scissors beyond the static scissor count here,
       * because the count can be dynamic only if the scissors themselves are dynamic as well (via
       * `VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT`), so `VP_SCISSORS` will be marked as dirty as well if
       * `VP_SCISSOR_COUNT` is made static.
       */
      scissor_count = terakan_app_config_draw_get_pa_vport_explicit_scissor_count(
         &command_writer->app_config_draw);
   }
   struct terakan_screen_rect const screen =
      terakan_screen_rect_square(TERAKAN_IMAGE_MAX_WIDTH_HEIGHT);
   for (uint32_t scissor_index = 0; scissor_index < scissor_count; ++scissor_index) {
      terakan_app_config_draw_set_pa_vport_explicit_scissor(
         &command_writer->app_config_draw, scissor_index,
         terakan_vk_rect_to_screen_rect(viewport_state->scissors[scissor_index], screen));
   }
}

static void
terakan_vk_state_dynamic_apply_vp_depth_clip_negative_one_to_one(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_pa_cl_clip_cntl_dx_clip_space_def(
      &command_writer->app_config_draw,
      !command_writer->base.command_buffer->vk.dynamic_graphics_state.vp
          .depth_clip_negative_one_to_one);
}

static void
terakan_vk_state_dynamic_apply_vp_depth_clamp_range(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct vk_viewport_state const * const viewport_state =
      &command_writer->base.command_buffer->vk.dynamic_graphics_state.vp;
   if (viewport_state->depth_clamp_mode != VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT) {
      terakan_app_config_draw_set_pa_vport_user_defined_zmin_zmax_enable(
         &command_writer->app_config_draw, false);
      return;
   }
   terakan_app_config_draw_set_pa_vport_user_defined_zmin_zmax_enable(
      &command_writer->app_config_draw, true);
   terakan_app_config_draw_set_pa_vport_user_defined_zmin_zmax(
      &command_writer->app_config_draw, viewport_state->depth_clamp_range.minDepthClamp,
      viewport_state->depth_clamp_range.maxDepthClamp);
}

static void
terakan_vk_state_dynamic_apply_rs_rasterizer_discard_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_pa_cl_clip_cntl_dx_rasterization_kill(
      &command_writer->app_config_draw,
      !command_writer->base.command_buffer->vk.dynamic_graphics_state.rs.rasterizer_discard_enable);
}

static void
terakan_vk_state_dynamic_apply_rs_depth_clamp_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_pa_cl_clip_cntl_z_clamp_enable(
      &command_writer->app_config_draw,
      !command_writer->base.command_buffer->vk.dynamic_graphics_state.rs.depth_clamp_enable);
}

static void
terakan_vk_state_dynamic_apply_rs_depth_clip_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_pa_cl_clip_cntl_z_clip_enable_override(
      &command_writer->app_config_draw,
      terakan_vk_state_depth_clip_enable_to_override(
         command_writer->base.command_buffer->vk.dynamic_graphics_state.rs.depth_clip_enable));
}

static void
terakan_vk_state_dynamic_apply_rs_polygon_mode(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_pa_su_sc_mode_cntl(
      &command_writer->app_config_draw, TERAKAN_VK_STATE_POLYGON_MODE_PA_SU_SC_MODE_CNTL_CLEAR,
      terakan_vk_state_polygon_mode_pa_su_sc_mode_cntl(
         command_writer->base.command_buffer->vk.dynamic_graphics_state.rs.polygon_mode));
}

static void
terakan_vk_state_dynamic_apply_rs_cull_mode(struct terakan_gfx_command_writer * const command_writer)
{
   VkCullModeFlags const cull_mode =
      command_writer->base.command_buffer->vk.dynamic_graphics_state.rs.cull_mode;
   terakan_app_config_draw_set_pa_su_sc_mode_cntl(
      &command_writer->app_config_draw, C_028814_CULL_FRONT & C_028814_CULL_BACK,
      S_028814_CULL_FRONT((cull_mode & VK_CULL_MODE_FRONT_BIT) != 0) |
         S_028814_CULL_BACK((cull_mode & VK_CULL_MODE_BACK_BIT) != 0));
}

static void
terakan_vk_state_dynamic_apply_rs_front_face(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_pa_su_sc_mode_cntl(
      &command_writer->app_config_draw, C_028814_FACE,
      S_028814_FACE(command_writer->base.command_buffer->vk.dynamic_graphics_state.rs.front_face));
}

static void
terakan_vk_state_dynamic_apply_rs_provoking_vertex(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_pa_su_sc_mode_cntl(
      &command_writer->app_config_draw, C_028814_PROVOKING_VTX_LAST,
      S_028814_PROVOKING_VTX_LAST(
         command_writer->base.command_buffer->vk.dynamic_graphics_state.rs.provoking_vertex ==
         VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT));
}

static void
terakan_vk_state_dynamic_apply_rs_depth_bias_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_pa_su_sc_mode_cntl(
      &command_writer->app_config_draw, TERAKAN_VK_STATE_DEPTH_BIAS_ENABLE_PA_SU_SC_MODE_CNTL_CLEAR,
      command_writer->base.command_buffer->vk.dynamic_graphics_state.rs.depth_bias.enable
         ? TERAKAN_VK_STATE_DEPTH_BIAS_ENABLE_PA_SU_SC_MODE_CNTL
         : 0);
}

static void
terakan_vk_state_dynamic_apply_rs_depth_bias_factors(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct vk_rasterization_state const * const rasterization_state =
      &command_writer->base.command_buffer->vk.dynamic_graphics_state.rs;
   terakan_app_config_draw_set_pa_su_poly_offset(
      &command_writer->app_config_draw,
      (struct terakan_hw_config_draw_pa_su_poly_offset){
         .slope_scale_per_16th_subpixel = rasterization_state->depth_bias.slope_factor * 16.0f,
         .constant_offset = rasterization_state->depth_bias.constant_factor,
         .clamp = rasterization_state->depth_bias.clamp,
      });
   terakan_app_config_draw_set_pa_su_poly_offset_db_fmt_cntl(
      &command_writer->app_config_draw,
      terakan_vk_state_depth_bias_poly_offset_representation(
         rasterization_state->depth_bias.representation),
      rasterization_state->depth_bias.exact);
}

static void
terakan_vk_state_dynamic_apply_rs_line_stipple_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_pa_sc_line_stipple_enable(
      &command_writer->app_config_draw,
      command_writer->base.command_buffer->vk.dynamic_graphics_state.rs.line.stipple.enable);
}

static void
terakan_vk_state_dynamic_apply_rs_line_stipple(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct vk_rasterization_state const * const rasterization_state =
      &command_writer->base.command_buffer->vk.dynamic_graphics_state.rs;
   terakan_app_config_draw_set_pa_sc_line_stipple_pattern(
      &command_writer->app_config_draw,
      S_028A0C_LINE_PATTERN(rasterization_state->line.stipple.pattern) |
         S_028A0C_REPEAT_COUNT(rasterization_state->line.stipple.factor - 1));
}

static void
terakan_vk_state_dynamic_apply_ms_rasterization_samples(
   struct terakan_gfx_command_writer * const command_writer)
{
   VkSampleCountFlagBits const rasterization_samples =
      command_writer->base.command_buffer->vk.dynamic_graphics_state.ms.rasterization_samples;
   int const rasterization_samples_log2 = terakan_image_vk_sample_count_to_hw_log2(
      rasterization_samples,
      terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx);
   if (unlikely(rasterization_samples_log2 < 0)) {
      /* #MemoryIntegrity. */
      vk_loge(
         VK_LOG_OBJS(terakan_device_log_obj(terakan_gfx_command_writer_device(command_writer))),
         "Dynamic pipeline state rasterization sample count %s is not supported",
         vk_SampleCountFlagBits_to_str(rasterization_samples));
      vk_command_buffer_set_error(&command_writer->base.command_buffer->vk,
                                  VK_ERROR_VALIDATION_FAILED_EXT);
      return;
   }
   terakan_app_config_draw_set_pa_sc_aa_config_msaa_num_samples_log2(
      &command_writer->app_config_draw, (uint8_t)rasterization_samples_log2);
}

static void
terakan_vk_state_dynamic_apply_ms_sample_mask(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_pa_sc_aa_mask(
      &command_writer->app_config_draw,
      (uint16_t)command_writer->base.command_buffer->vk.dynamic_graphics_state.ms.sample_mask);
}

static void
terakan_vk_state_dynamic_apply_ms_alpha_to_coverage_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_db_alpha_to_mask(
      &command_writer->app_config_draw, C_028B70_ALPHA_TO_MASK_ENABLE,
      S_028B70_ALPHA_TO_MASK_ENABLE(command_writer->base.command_buffer->vk.dynamic_graphics_state
                                       .ms.alpha_to_coverage_enable));
}

static void
terakan_vk_state_dynamic_apply_ms_sample_locations_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_pa_sc_aa_sample_locs_custom_enable(
      &command_writer->app_config_draw,
      command_writer->base.command_buffer->vk.dynamic_graphics_state.ms.sample_locations_enable);
}

static void
terakan_vk_state_dynamic_apply_ms_sample_locations(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint8_t sample_locs[16][4];
   terakan_vk_state_sample_locations_mesa_to_hw(
      command_writer->base.command_buffer->vk.dynamic_graphics_state.ms.sample_locations,
      sample_locs[0]);
   terakan_app_config_draw_set_pa_sc_aa_sample_locs_custom_16_samples_2x2_locs(
      &command_writer->app_config_draw, sample_locs[0]);
}

static void
terakan_vk_state_dynamic_apply_ds_depth_test_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_db_depth_control(
      &command_writer->app_config_draw, C_028800_Z_ENABLE,
      S_028800_Z_ENABLE(
         command_writer->base.command_buffer->vk.dynamic_graphics_state.ds.depth.test_enable));
}

static void
terakan_vk_state_dynamic_apply_ds_depth_write_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_db_depth_control(
      &command_writer->app_config_draw, C_028800_Z_WRITE_ENABLE,
      S_028800_Z_WRITE_ENABLE(
         command_writer->base.command_buffer->vk.dynamic_graphics_state.ds.depth.write_enable));
}

static void
terakan_vk_state_dynamic_apply_ds_depth_compare_op(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_db_depth_control(
      &command_writer->app_config_draw, C_028800_ZFUNC,
      S_028800_ZFUNC(
         command_writer->base.command_buffer->vk.dynamic_graphics_state.ds.depth.compare_op));
}

static void
terakan_vk_state_dynamic_apply_ds_stencil_test_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_db_depth_control(
      &command_writer->app_config_draw, TERAKAN_VK_STATE_STENCIL_TEST_ENABLE_DB_DEPTH_CONTROL_CLEAR,
      command_writer->base.command_buffer->vk.dynamic_graphics_state.ds.stencil.test_enable
         ? TERAKAN_VK_STATE_STENCIL_TEST_ENABLE_DB_DEPTH_CONTROL
         : 0);
}

static void
terakan_vk_state_dynamic_apply_ds_stencil_op(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct vk_depth_stencil_state const * const depth_stencil_state =
      &command_writer->base.command_buffer->vk.dynamic_graphics_state.ds;
   terakan_app_config_draw_set_db_depth_control(
      &command_writer->app_config_draw, TERAKAN_VK_STATE_STENCIL_OP_DB_DEPTH_CONTROL_CLEAR,
      S_028800_STENCILFUNC(depth_stencil_state->stencil.front.op.compare) |
         S_028800_STENCILFAIL(depth_stencil_state->stencil.front.op.fail) |
         S_028800_STENCILZPASS(depth_stencil_state->stencil.front.op.pass) |
         S_028800_STENCILZFAIL(depth_stencil_state->stencil.front.op.depth_fail) |
         S_028800_STENCILFUNC_BF(depth_stencil_state->stencil.back.op.compare) |
         S_028800_STENCILFAIL_BF(depth_stencil_state->stencil.back.op.fail) |
         S_028800_STENCILZPASS_BF(depth_stencil_state->stencil.back.op.pass) |
         S_028800_STENCILZFAIL_BF(depth_stencil_state->stencil.back.op.depth_fail));
}

static void
terakan_vk_state_dynamic_apply_ds_stencil_compare_mask(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct vk_depth_stencil_state const * const depth_stencil_state =
      &command_writer->base.command_buffer->vk.dynamic_graphics_state.ds;
   terakan_app_config_draw_set_db_stencilrefmask(
      &command_writer->app_config_draw, false, C_028430_STENCILMASK,
      S_028430_STENCILMASK(depth_stencil_state->stencil.front.compare_mask));
   terakan_app_config_draw_set_db_stencilrefmask(
      &command_writer->app_config_draw, true, C_028430_STENCILMASK,
      S_028430_STENCILMASK(depth_stencil_state->stencil.back.compare_mask));
}

static void
terakan_vk_state_dynamic_apply_ds_stencil_write_mask(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct vk_depth_stencil_state const * const depth_stencil_state =
      &command_writer->base.command_buffer->vk.dynamic_graphics_state.ds;
   terakan_app_config_draw_set_db_stencilrefmask(
      &command_writer->app_config_draw, false, C_028430_STENCILWRITEMASK,
      S_028430_STENCILWRITEMASK(depth_stencil_state->stencil.front.write_mask));
   terakan_app_config_draw_set_db_stencilrefmask(
      &command_writer->app_config_draw, true, C_028430_STENCILWRITEMASK,
      S_028430_STENCILWRITEMASK(depth_stencil_state->stencil.back.write_mask));
}

static void
terakan_vk_state_dynamic_apply_ds_stencil_reference(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct vk_depth_stencil_state const * const depth_stencil_state =
      &command_writer->base.command_buffer->vk.dynamic_graphics_state.ds;
   terakan_app_config_draw_set_db_stencilrefmask(
      &command_writer->app_config_draw, false, C_028430_STENCILREF,
      S_028430_STENCILREF(depth_stencil_state->stencil.front.reference));
   terakan_app_config_draw_set_db_stencilrefmask(
      &command_writer->app_config_draw, true, C_028430_STENCILREF,
      S_028430_STENCILREF(depth_stencil_state->stencil.back.reference));
}

static void
terakan_vk_state_dynamic_apply_cb_logic_op_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_cb_rop3_enable(
      &command_writer->app_config_draw,
      command_writer->base.command_buffer->vk.dynamic_graphics_state.cb.logic_op_enable);
}

static void
terakan_vk_state_dynamic_apply_cb_logic_op(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_cb_rop3(
      &command_writer->app_config_draw,
      terakan_vk_state_logic_op_rop3(
         command_writer->base.command_buffer->vk.dynamic_graphics_state.cb.logic_op));
}

static void
terakan_vk_state_dynamic_apply_cb_color_write_enables(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_cb_color_rtv_write_enable_mask_all(
      &command_writer->app_config_draw, (uint8_t)command_writer->base.command_buffer->vk
                                           .dynamic_graphics_state.cb.color_write_enables);
}

static void
terakan_vk_state_dynamic_apply_cb_blend_enables(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct vk_color_blend_attachment_state const * const attachments =
      command_writer->base.command_buffer->vk.dynamic_graphics_state.cb.attachments;
   for (unsigned attachment_index = 0; attachment_index < TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS;
        ++attachment_index) {
      terakan_app_config_draw_set_cb_blend_control(
         &command_writer->app_config_draw, attachment_index, C_028780_BLEND_CONTROL_ENABLE,
         S_028780_BLEND_CONTROL_ENABLE(attachments[attachment_index].blend_enable));
   }
}

static void
terakan_vk_state_dynamic_apply_cb_blend_equations(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct vk_color_blend_attachment_state const * const attachments =
      command_writer->base.command_buffer->vk.dynamic_graphics_state.cb.attachments;
   for (unsigned attachment_index = 0; attachment_index < TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS;
        ++attachment_index) {
      struct vk_color_blend_attachment_state const * const attachment =
         &attachments[attachment_index];
      terakan_app_config_draw_set_cb_blend_control(
         &command_writer->app_config_draw, attachment_index,
         C_028780_COLOR_SRCBLEND & C_028780_COLOR_DESTBLEND & C_028780_COLOR_COMB_FCN &
            C_028780_ALPHA_SRCBLEND & C_028780_ALPHA_DESTBLEND & C_028780_ALPHA_COMB_FCN &
            C_028780_SEPARATE_ALPHA_BLEND,
         S_028780_COLOR_SRCBLEND(terakan_vk_state_blend_factor_to_hw(
            (VkBlendFactor)attachment->src_color_blend_factor)) |
            S_028780_COLOR_DESTBLEND(terakan_vk_state_blend_factor_to_hw(
               (VkBlendFactor)attachment->dst_color_blend_factor)) |
            S_028780_COLOR_COMB_FCN(
               terakan_vk_state_blend_op_to_hw((VkBlendOp)attachment->color_blend_op)) |
            S_028780_ALPHA_SRCBLEND(
               terakan_hw_config_draw_cb_blend_control_color_factors_for_color_alpha
                  [terakan_vk_state_blend_factor_to_hw(
                     (VkBlendFactor)attachment->src_alpha_blend_factor)]) |
            S_028780_ALPHA_DESTBLEND(
               terakan_hw_config_draw_cb_blend_control_color_factors_for_color_alpha
                  [terakan_vk_state_blend_factor_to_hw(
                     (VkBlendFactor)attachment->dst_alpha_blend_factor)]) |
            S_028780_ALPHA_COMB_FCN(
               terakan_vk_state_blend_op_to_hw((VkBlendOp)attachment->alpha_blend_op)) |
            S_028780_SEPARATE_ALPHA_BLEND(1));
   }
}

static void
terakan_vk_state_dynamic_apply_cb_write_masks(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct vk_color_blend_attachment_state const * const attachments =
      command_writer->base.command_buffer->vk.dynamic_graphics_state.cb.attachments;
   uint32_t write_mask = 0b0;
   for (unsigned attachment_index = 0; attachment_index < TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS;
        ++attachment_index) {
      write_mask |= (uint32_t)(attachments[attachment_index].write_mask & 0xF)
                    << (4 * attachment_index);
   }
   terakan_app_config_draw_set_cb_color_rtv_write_component_mask_all(
      &command_writer->app_config_draw, write_mask);
}

static void
terakan_vk_state_dynamic_apply_cb_blend_constants(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_app_config_draw_set_cb_blend_constants(
      &command_writer->app_config_draw,
      command_writer->base.command_buffer->vk.dynamic_graphics_state.cb.blend_constants);
}

static terakan_vk_state_dynamic_apply_function const
   terakan_vk_state_dynamic_apply_functions[MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX] = {
      [MESA_VK_DYNAMIC_VI] = terakan_vk_state_dynamic_apply_vi,
      [MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY] =
         terakan_vk_state_dynamic_apply_ia_primitive_topology,
      [MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT] = terakan_vk_state_dynamic_apply_vp_viewport_count,
      [MESA_VK_DYNAMIC_VP_VIEWPORTS] = terakan_vk_state_dynamic_apply_vp_viewports,
      [MESA_VK_DYNAMIC_VP_SCISSOR_COUNT] = terakan_vk_state_dynamic_apply_vp_scissor_count,
      [MESA_VK_DYNAMIC_VP_SCISSORS] = terakan_vk_state_dynamic_apply_vp_scissors,
      [MESA_VK_DYNAMIC_VP_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE] =
         terakan_vk_state_dynamic_apply_vp_depth_clip_negative_one_to_one,
      [MESA_VK_DYNAMIC_VP_DEPTH_CLAMP_RANGE] = terakan_vk_state_dynamic_apply_vp_depth_clamp_range,
      [MESA_VK_DYNAMIC_RS_RASTERIZER_DISCARD_ENABLE] =
         terakan_vk_state_dynamic_apply_rs_rasterizer_discard_enable,
      [MESA_VK_DYNAMIC_RS_DEPTH_CLAMP_ENABLE] =
         terakan_vk_state_dynamic_apply_rs_depth_clamp_enable,
      [MESA_VK_DYNAMIC_RS_DEPTH_CLIP_ENABLE] = terakan_vk_state_dynamic_apply_rs_depth_clip_enable,
      [MESA_VK_DYNAMIC_RS_POLYGON_MODE] = terakan_vk_state_dynamic_apply_rs_polygon_mode,
      [MESA_VK_DYNAMIC_RS_CULL_MODE] = terakan_vk_state_dynamic_apply_rs_cull_mode,
      [MESA_VK_DYNAMIC_RS_FRONT_FACE] = terakan_vk_state_dynamic_apply_rs_front_face,
      [MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX] = terakan_vk_state_dynamic_apply_rs_provoking_vertex,
      [MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE] = terakan_vk_state_dynamic_apply_rs_depth_bias_enable,
      [MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS] =
         terakan_vk_state_dynamic_apply_rs_depth_bias_factors,
      [MESA_VK_DYNAMIC_RS_LINE_STIPPLE_ENABLE] =
         terakan_vk_state_dynamic_apply_rs_line_stipple_enable,
      [MESA_VK_DYNAMIC_RS_LINE_STIPPLE] = terakan_vk_state_dynamic_apply_rs_line_stipple,
      [MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES] =
         terakan_vk_state_dynamic_apply_ms_rasterization_samples,
      [MESA_VK_DYNAMIC_MS_SAMPLE_MASK] = terakan_vk_state_dynamic_apply_ms_sample_mask,
      [MESA_VK_DYNAMIC_MS_ALPHA_TO_COVERAGE_ENABLE] =
         terakan_vk_state_dynamic_apply_ms_alpha_to_coverage_enable,
      [MESA_VK_DYNAMIC_MS_SAMPLE_LOCATIONS_ENABLE] =
         terakan_vk_state_dynamic_apply_ms_sample_locations_enable,
      [MESA_VK_DYNAMIC_MS_SAMPLE_LOCATIONS] = terakan_vk_state_dynamic_apply_ms_sample_locations,
      [MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE] = terakan_vk_state_dynamic_apply_ds_depth_test_enable,
      [MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE] =
         terakan_vk_state_dynamic_apply_ds_depth_write_enable,
      [MESA_VK_DYNAMIC_DS_DEPTH_COMPARE_OP] = terakan_vk_state_dynamic_apply_ds_depth_compare_op,
      [MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE] =
         terakan_vk_state_dynamic_apply_ds_stencil_test_enable,
      [MESA_VK_DYNAMIC_DS_STENCIL_OP] = terakan_vk_state_dynamic_apply_ds_stencil_op,
      [MESA_VK_DYNAMIC_DS_STENCIL_COMPARE_MASK] =
         terakan_vk_state_dynamic_apply_ds_stencil_compare_mask,
      [MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK] =
         terakan_vk_state_dynamic_apply_ds_stencil_write_mask,
      [MESA_VK_DYNAMIC_DS_STENCIL_REFERENCE] = terakan_vk_state_dynamic_apply_ds_stencil_reference,
      [MESA_VK_DYNAMIC_CB_COLOR_WRITE_ENABLES] =
         terakan_vk_state_dynamic_apply_cb_color_write_enables,
      [MESA_VK_DYNAMIC_CB_LOGIC_OP_ENABLE] = terakan_vk_state_dynamic_apply_cb_logic_op_enable,
      [MESA_VK_DYNAMIC_CB_LOGIC_OP] = terakan_vk_state_dynamic_apply_cb_logic_op,
      [MESA_VK_DYNAMIC_CB_BLEND_ENABLES] = terakan_vk_state_dynamic_apply_cb_blend_enables,
      [MESA_VK_DYNAMIC_CB_BLEND_EQUATIONS] = terakan_vk_state_dynamic_apply_cb_blend_equations,
      [MESA_VK_DYNAMIC_CB_WRITE_MASKS] = terakan_vk_state_dynamic_apply_cb_write_masks,
      [MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS] = terakan_vk_state_dynamic_apply_cb_blend_constants,
};

void
terakan_vk_state_dynamic_apply(struct terakan_gfx_command_writer * const command_writer)
{
   BITSET_DECLARE(apply_state, MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX);
   struct terakan_command_buffer * const command_buffer = command_writer->base.command_buffer;
   BITSET_AND(apply_state, command_buffer->vk.dynamic_graphics_state.dirty,
              command_buffer->graphics_state_is_dynamic);
   BITSET_ANDNOT(command_buffer->vk.dynamic_graphics_state.dirty,
                 command_buffer->vk.dynamic_graphics_state.dirty, apply_state);
   unsigned state_index;
   BITSET_FOREACH_SET (state_index, apply_state, MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX) {
      terakan_vk_state_dynamic_apply_function const apply_function =
         terakan_vk_state_dynamic_apply_functions[state_index];
      if (apply_function == NULL) {
         continue;
      }
      apply_function(command_writer);
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBindVertexBuffers2(VkCommandBuffer const commandBuffer, uint32_t const firstBinding,
                              uint32_t const bindingCount, VkBuffer const * const pBuffers,
                              VkDeviceSize const * const pOffsets,
                              VkDeviceSize const * const pSizes,
                              VkDeviceSize const * const pStrides)
{
   assert(firstBinding <= TERAKAN_VK_STATE_MAX_VERTEX_BINDINGS &&
          bindingCount <= TERAKAN_VK_STATE_MAX_VERTEX_BINDINGS - firstBinding);

   struct terakan_command_buffer * const command_buffer =
      terakan_command_buffer_from_handle(commandBuffer);
   struct terakan_gfx_command_writer * const command_writer = command_buffer->command_writer.gfx;

   for (uint32_t binding_array_index = 0;
        binding_array_index < bindingCount &&
        firstBinding + binding_array_index < TERAKAN_VK_STATE_MAX_VERTEX_BINDINGS;
        ++binding_array_index) {
      uint32_t const binding = firstBinding + binding_array_index;
      VkDeviceSize size = pSizes != NULL ? pSizes[binding_array_index] : VK_WHOLE_SIZE;
      if (size != 0) {
         struct terakan_buffer const * const buffer =
            terakan_buffer_from_handle(pBuffers[binding_array_index]);
         if (buffer != NULL && buffer->bo != NULL) {
            /* #MemoryIntegrity. */
            uint64_t const offset = pOffsets[binding_array_index];
            if (offset < buffer->vk.size) {
               /* The #MemoryIntegrity ensuring clamping also handles `VK_WHOLE_SIZE`, which is the
                * largest representable value.
                */
               size = MIN3(size, buffer->vk.size - offset, (VkDeviceSize)UINT32_MAX + 1);
               assert(size != 0);
               terakan_app_config_draw_set_sq_resource_fetch_base_size(
                  &command_writer->app_config_draw, binding, buffer->bo, buffer->va + offset,
                  (uint32_t)(size - 1));
               continue;
            }
         }
      }
      /* Unbind if didn't bind a valid buffer. */
      terakan_app_config_draw_set_sq_resource_fetch_base_size(&command_writer->app_config_draw,
                                                              binding, NULL, 0, 0);
   }

   if (pStrides != NULL) {
      /* Update the strides in the dynamic state. */
      vk_cmd_set_vertex_binding_strides(&command_buffer->vk, firstBinding, bindingCount, pStrides);
      bool const use_2048_stride_as_1024 = terakan_vk_state_vertex_input_uses_2048_stride_as_1024(
         terakan_gfx_command_writer_physical_device(command_writer));
      for (uint32_t binding_array_index = 0;
           binding_array_index < bindingCount &&
           firstBinding + binding_array_index < TERAKAN_VK_STATE_MAX_VERTEX_BINDINGS;
           ++binding_array_index) {
         terakan_app_config_draw_set_sq_pgm_and_resource_fetch_stride(
            &command_writer->app_config_draw, firstBinding + binding_array_index,
            (uint16_t)pStrides[binding_array_index], use_2048_stride_as_1024);
      }
   }
}
