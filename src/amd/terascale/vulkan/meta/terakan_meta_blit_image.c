/*
 * Copyright © 2026 Vitaliy Triang3l Kuzmin
 *
 * SPDX-License-Identifier: MIT
 */

#include "terakan_meta_impl.h"

#include "terakan_barrier.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_sampler.h"

#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_debug.h"
#include "util/u_math.h"

#include <assert.h>
#include <stdlib.h>

enum {
   TERAKAN_META_BLIT_CONST_SCALE_X,
   TERAKAN_META_BLIT_CONST_OFF_X,
   TERAKAN_META_BLIT_CONST_SCALE_Y,
   TERAKAN_META_BLIT_CONST_OFF_Y,

   TERAKAN_META_BLIT_CONSTS_COUNT,
};

static uint32_t const terakan_meta_blit_image_ps_r8xx[] = {
   S_SQ_CF_WORD0_ADDR(3) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_CONST_SCALE_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* R0.xy = R0.xy * scale + offset (single cycle; 2-cycle MUL+ADD PV hangs on Palm). */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_SCALE_X) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_BLIT_CONST_OFF_X) |
      TERAKAN_SHADER_OP3(false, 0, 'X', MULADD_IEEE, EG, 0, 'X', 0, 0, 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_SCALE_Y) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_BLIT_CONST_OFF_Y) |
      TERAKAN_SHADER_OP3(true, 0, 'Y', MULADD_IEEE, EG, 0, 'Y', 0, 0, 0, 0, VEC_012),

   0,
   0,
   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

static uint32_t const terakan_meta_blit_image_ps_r9xx[] = {
   S_SQ_CF_WORD0_ADDR(4) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_CONST_SCALE_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   TERAKAN_SHADER_CF_END_R9XX,

   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_SCALE_X) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_BLIT_CONST_OFF_X) |
      TERAKAN_SHADER_OP3(false, 0, 'X', MULADD_IEEE, EG, 0, 'X', 0, 0, 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_SCALE_Y) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_BLIT_CONST_OFF_Y) |
      TERAKAN_SHADER_OP3(true, 0, 'Y', MULADD_IEEE, EG, 0, 'Y', 0, 0, 0, 0, VEC_012),

   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

struct terakan_meta_shader const terakan_meta_blit_image_ps = {
   .r8xx =
      {
         .program = terakan_meta_blit_image_ps_r8xx,
         .program_size_bytes = sizeof(terakan_meta_blit_image_ps_r8xx),
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
         .program = terakan_meta_blit_image_ps_r9xx,
         .program_size_bytes = sizeof(terakan_meta_blit_image_ps_r9xx),
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

static void
terakan_meta_blit_set_fs_sampler(struct terakan_gfx_command_writer * const command_writer,
                                  VkFilter const filter)
{
   struct terakan_hw_config_sqk * const sqk = &command_writer->hw_config_sqk;
   uint32_t * const sampler =
      sqk->stages_[MESA_SHADER_FRAGMENT]
         .samplers[TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META]
         .sampler;
   bool const is_r9xx =
      terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx;
   if (!is_r9xx) {
      /* R8xx: keep sqk_reset D3D11 null sampler — explicit writes hang Palm. */
      return;
   }
   uint32_t const xy_filter = terakan_sampler_translate_filter(filter, false);
   uint32_t const clamp = terakan_sampler_translate_address_mode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

   sampler[0] = S_03C000_CLAMP_X(clamp) | S_03C000_CLAMP_Y(clamp) | S_03C000_CLAMP_Z(clamp) |
                S_03C000_XY_MAG_FILTER(xy_filter) | S_03C000_XY_MIN_FILTER(xy_filter) |
                S_03C000_MIP_FILTER(V_03C000_SQ_TEX_XY_FILTER_POINT) |
                S_03C000_BORDER_COLOR_TYPE(V_03C000_SQ_TEX_BORDER_COLOR_TRANS_BLACK);
   sampler[1] = S_03C004_MIN_LOD(0) | S_03C004_MAX_LOD(0x3ff);
   sampler[2] = 0;
   sampler[3] = 0;
   sqk->stages_[MESA_SHADER_FRAGMENT].modified.samplers |=
      BITFIELD_BIT(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META);
   sqk->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_FRAGMENT);
}

static void
terakan_meta_blit_emit_pre_draw_barriers(struct terakan_gfx_command_writer * const command_writer,
                                          bool const same_image)
{
   if (!terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      /* R8xx: rely on app barriers; unconditional flush here hangs Palm on mip chains. */
      return;
   }

   enum terakan_barrier_action_flags actions =
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS |
      TERAKAN_BARRIER_ACTION_INV_TC;

   if (same_image) {
      actions |= TERAKAN_BARRIER_ACTION_INV_VC;
   }

   actions |= command_writer->post_color_image_copy_write_barrier_actions;

   terakan_barrier_emit_actions_unconditionally(command_writer, actions);
   command_writer->post_color_image_copy_write_barrier_actions &= ~actions;
}

static void
terakan_meta_blit_region_scale_off(int32_t const src0, int32_t const src1, int32_t const dst0,
                                   int32_t const dst1, float * const scale_out,
                                   float * const off_out)
{
   int32_t norm_src0 = src0;
   int32_t norm_src1 = src1;
   int32_t norm_dst0 = dst0;
   int32_t norm_dst1 = dst1;

   if (norm_dst0 > norm_dst1) {
      norm_dst0 = dst1;
      norm_dst1 = dst0;
      norm_src0 = src1;
      norm_src1 = src0;
   } else if (norm_src0 > norm_src1) {
      norm_src0 = src1;
      norm_src1 = src0;
   }

   float const src_size = (float)(norm_src1 - norm_src0);
   float const dst_size = (float)(norm_dst1 - norm_dst0);

   *scale_out = src_size / dst_size;
   *off_out = (float)norm_src0 - (float)norm_dst0 * (*scale_out);
}

static bool
terakan_meta_blit_region_to_copy(VkImageBlit2 const * const blit, VkImageCopy2 * const copy_out)
{
   int32_t const src_x0 = blit->srcOffsets[0].x;
   int32_t const src_x1 = blit->srcOffsets[1].x;
   int32_t const src_y0 = blit->srcOffsets[0].y;
   int32_t const src_y1 = blit->srcOffsets[1].y;
   int32_t const src_z0 = blit->srcOffsets[0].z;
   int32_t const src_z1 = blit->srcOffsets[1].z;

   int32_t const dst_x0 = blit->dstOffsets[0].x;
   int32_t const dst_x1 = blit->dstOffsets[1].x;
   int32_t const dst_y0 = blit->dstOffsets[0].y;
   int32_t const dst_y1 = blit->dstOffsets[1].y;
   int32_t const dst_z0 = blit->dstOffsets[0].z;
   int32_t const dst_z1 = blit->dstOffsets[1].z;

   uint32_t const src_width = (uint32_t)abs(src_x1 - src_x0);
   uint32_t const src_height = (uint32_t)abs(src_y1 - src_y0);
   uint32_t const dst_width = (uint32_t)abs(dst_x1 - dst_x0);
   uint32_t const dst_height = (uint32_t)abs(dst_y1 - dst_y0);
   uint32_t const src_depth = (uint32_t)abs(src_z1 - src_z0);
   uint32_t const dst_depth = (uint32_t)abs(dst_z1 - dst_z0);

   if (src_width == 0 || src_height == 0 || dst_width == 0 || dst_height == 0 ||
       src_depth == 0 || dst_depth == 0) {
      return false;
   }

   if (src_width != dst_width || src_height != dst_height || src_depth != dst_depth) {
      return false;
   }

   *copy_out = (VkImageCopy2){
      .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
      .srcSubresource = blit->srcSubresource,
      .dstSubresource = blit->dstSubresource,
      .srcOffset =
         {
            .x = MIN2(src_x0, src_x1),
            .y = MIN2(src_y0, src_y1),
            .z = MIN2(src_z0, src_z1),
         },
      .dstOffset =
         {
            .x = MIN2(dst_x0, dst_x1),
            .y = MIN2(dst_y0, dst_y1),
            .z = MIN2(dst_z0, dst_z1),
         },
      .extent =
         {
            .width = src_width,
            .height = src_height,
            .depth = src_depth,
         },
   };

   return true;
}

#define TERAKAN_META_BLIT_TILE_SIZE 128

static void
terakan_meta_blit_image_region_draw(struct terakan_gfx_command_writer * const command_writer,
                                    VkBlitImageInfo2 const * const blit_info,
                                    VkImageBlit2 const * const region)
{
   struct terakan_image const * const dst_image =
      terakan_image_from_handle(blit_info->dstImage);
   struct terakan_image const * const src_image =
      terakan_image_from_handle(blit_info->srcImage);

   int32_t const dst_x0 = MIN2(region->dstOffsets[0].x, region->dstOffsets[1].x);
   int32_t const dst_y0 = MIN2(region->dstOffsets[0].y, region->dstOffsets[1].y);
   uint32_t const dst_width = (uint32_t)abs(region->dstOffsets[1].x - region->dstOffsets[0].x);
   uint32_t const dst_height = (uint32_t)abs(region->dstOffsets[1].y - region->dstOffsets[0].y);

   if (dst_width == 0 || dst_height == 0) {
      return;
   }

   struct terakan_image_descriptor_create_info dst_descriptor_create_info = {.image = dst_image};
   struct terakan_image_descriptor_create_info src_descriptor_create_info = {.image = src_image};

   unsigned src_vk_aspect_mask_remaining = (unsigned)region->srcSubresource.aspectMask;
   u_foreach_bit (dst_vk_aspect_bit_index, region->dstSubresource.aspectMask) {
      dst_descriptor_create_info.image_aspect_index = terakan_format_aspect_index(
         dst_image->format_info.aspect_map, (VkImageAspectFlags)1 << dst_vk_aspect_bit_index, 0);
      dst_descriptor_create_info.view_format = terakan_meta_transfer_image_block_format_info(
         terascale_format_bytes_per_block
            [dst_image->format_info.aspect_formats[dst_descriptor_create_info.image_aspect_index]
                .format]);
      src_descriptor_create_info.image_aspect_index = terakan_format_aspect_index(
         src_image->format_info.aspect_map,
         (VkImageAspectFlags)1 << u_bit_scan(&src_vk_aspect_mask_remaining), 0);
      src_descriptor_create_info.view_format = terakan_meta_transfer_image_block_format_info(
         terascale_format_bytes_per_block
            [src_image->format_info.aspect_formats[src_descriptor_create_info.image_aspect_index]
                .format]);

      dst_descriptor_create_info.subresource_range.base_mip_level = region->dstSubresource.mipLevel;
      dst_descriptor_create_info.subresource_range.max_level_count = 1;
      src_descriptor_create_info.subresource_range.base_mip_level = region->srcSubresource.mipLevel;
      src_descriptor_create_info.subresource_range.max_level_count = 1;

      if (src_image->vk.image_type == VK_IMAGE_TYPE_3D) {
         src_descriptor_create_info.subresource_range.base_z_or_array_layer =
            (uint32_t)MIN2(region->srcOffsets[0].z, region->srcOffsets[1].z);
         src_descriptor_create_info.subresource_range.max_depth_or_layer_count =
            (uint32_t)abs(region->srcOffsets[1].z - region->srcOffsets[0].z);
      } else {
         src_descriptor_create_info.subresource_range.base_z_or_array_layer =
            region->srcSubresource.baseArrayLayer;
         src_descriptor_create_info.subresource_range.max_depth_or_layer_count =
            region->srcSubresource.layerCount;
      }
      dst_descriptor_create_info.subresource_range.base_z_or_array_layer =
         dst_image->vk.image_type == VK_IMAGE_TYPE_3D
            ? (uint32_t)MIN2(region->dstOffsets[0].z, region->dstOffsets[1].z)
            : region->dstSubresource.baseArrayLayer;
      dst_descriptor_create_info.subresource_range.max_depth_or_layer_count =
         src_descriptor_create_info.subresource_range.max_depth_or_layer_count;

      if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
                      dst_image, &dst_descriptor_create_info.subresource_range, false) ||
                   !terakan_image_descriptor_subresource_range_sanitize(
                      src_image, &src_descriptor_create_info.subresource_range, false))) {
         continue;
      }

      if (!terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
         /* Palm: full pre-blit barrier hangs; invalidate TC so PS can sample the source mip. */
         terakan_barrier_emit_actions_unconditionally(command_writer,
                                                      TERAKAN_BARRIER_ACTION_INV_TC);
      }

      struct terakan_resource_descriptor src_descriptor;
      if (unlikely(!terakan_image_create_resource_descriptor(&src_descriptor_create_info,
                                                             V_030000_SQ_TEX_DIM_2D_ARRAY, NULL,
                                                             &src_descriptor))) {
         continue;
      }

      src_descriptor_create_info.subresource_range.max_depth_or_layer_count =
         MIN2(src_descriptor_create_info.subresource_range.max_depth_or_layer_count,
              dst_descriptor_create_info.subresource_range.max_depth_or_layer_count);
      dst_descriptor_create_info.subresource_range.max_depth_or_layer_count =
         src_descriptor_create_info.subresource_range.max_depth_or_layer_count;

      do {
         struct terakan_color_descriptor dst_descriptor;
         uint32_t const dst_descriptor_slices = terakan_image_create_color_descriptor(
            &dst_descriptor_create_info, V_028C70_TEXTURE2DARRAY, &dst_descriptor, NULL);
         if (unlikely(dst_descriptor_slices == 0)) {
            break;
         }

         terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control(
            command_writer, 0xF, &dst_image->bo, &dst_descriptor, NULL,
            TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);

         terakan_hw_config_sqk_set_resource_fs(
            &command_writer->hw_config_sqk,
            TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META, src_image->bo, &src_descriptor);

         struct terakan_screen_rect const screen_bounds = {
            .bounds = {[1] = {G_028C78_WIDTH_MAX(dst_descriptor.dim) + 1,
                              G_028C78_HEIGHT_MAX(dst_descriptor.dim) + 1}},
         };

         for (uint32_t tile_y = 0; tile_y < dst_height; tile_y += TERAKAN_META_BLIT_TILE_SIZE) {
            uint32_t const tile_h = MIN2(TERAKAN_META_BLIT_TILE_SIZE, dst_height - tile_y);
            for (uint32_t tile_x = 0; tile_x < dst_width; tile_x += TERAKAN_META_BLIT_TILE_SIZE) {
               uint32_t const tile_w = MIN2(TERAKAN_META_BLIT_TILE_SIZE, dst_width - tile_x);

               VkOffset3D const tile_offset_blocks = vk_image_offset_to_elements(
                  &dst_image->vk,
                  (VkOffset3D){.x = dst_x0 + (int32_t)tile_x, .y = dst_y0 + (int32_t)tile_y, .z = 0});
               VkExtent3D const tile_extent_blocks =
                  vk_image_extent_to_elements(&dst_image->vk,
                                              (VkExtent3D){.width = tile_w, .height = tile_h, .depth = 1});
               VkRect2D const tile_region_rect = {
                  .offset = {.x = tile_offset_blocks.x, .y = tile_offset_blocks.y},
                  .extent = {.width = tile_extent_blocks.width, .height = tile_extent_blocks.height},
               };

               terakan_meta_draw_rect(
                  command_writer,
                  terakan_vk_rect_to_screen_rect(tile_region_rect, screen_bounds),
                  dst_descriptor_slices);
            }
         }

         dst_descriptor_create_info.subresource_range.base_z_or_array_layer += dst_descriptor_slices;
         dst_descriptor_create_info.subresource_range.max_depth_or_layer_count -=
            dst_descriptor_slices;
         src_descriptor.resource[5] = (src_descriptor.resource[5] & C_030014_BASE_ARRAY) |
                                      S_030014_BASE_ARRAY(G_030014_BASE_ARRAY(
                                         src_descriptor.resource[5] + dst_descriptor_slices));
      } while (dst_descriptor_create_info.subresource_range.max_depth_or_layer_count != 0);
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBlitImage2(VkCommandBuffer const commandBuffer,
                      VkBlitImageInfo2 const * const pBlitImageInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   if (debug_get_bool_option("TERAKAN_SKIP_SCALED_BLIT", false)) {
      STACK_ARRAY(VkImageCopy2, copies, pBlitImageInfo->regionCount);
      uint32_t copy_count = 0;

      for (uint32_t region_index = 0; region_index < pBlitImageInfo->regionCount; ++region_index) {
         if (terakan_meta_blit_region_to_copy(&pBlitImageInfo->pRegions[region_index],
                                              &copies[copy_count])) {
            copy_count++;
         }
      }

      if (copy_count > 0) {
         VkCopyImageInfo2 const copy_info = {
            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
            .srcImage = pBlitImageInfo->srcImage,
            .srcImageLayout = pBlitImageInfo->srcImageLayout,
            .dstImage = pBlitImageInfo->dstImage,
            .dstImageLayout = pBlitImageInfo->dstImageLayout,
            .regionCount = copy_count,
            .pRegions = copies,
         };
         terakan_CmdCopyImage2(commandBuffer, &copy_info);
      }

      STACK_ARRAY_FINISH(copies);
      return;
   }

   STACK_ARRAY(VkImageCopy2, copies, pBlitImageInfo->regionCount);
   uint32_t copy_count = 0;
   bool has_scaled_regions = false;
   bool same_image_scaled = false;

   for (uint32_t region_index = 0; region_index < pBlitImageInfo->regionCount; ++region_index) {
      VkImageBlit2 const * const region = &pBlitImageInfo->pRegions[region_index];

      if (terakan_meta_blit_region_to_copy(region, &copies[copy_count])) {
         copy_count++;
      } else {
         has_scaled_regions = true;
         if (pBlitImageInfo->srcImage == pBlitImageInfo->dstImage) {
            same_image_scaled = true;
         }
      }
   }

   if (has_scaled_regions) {
      if (!command_writer->meta_blit_draw_session_active) {
         terakan_meta_blit_emit_pre_draw_barriers(command_writer, same_image_scaled);

         struct terakan_meta_config_draw_begin_options const meta_begin_options = {
            .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
            .cb_and_db_shader_control_mode =
               TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_WITH_RTV_AND_DYNAMIC_DB_SHADER_CONTROL,
            .rasterization = {.enable = true},
         };
         terakan_meta_config_draw_begin(command_writer, &meta_begin_options);
         terakan_meta_config_draw_set_sq_pgm_vs(
            command_writer, TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
         terakan_meta_config_draw_set_sq_pgm_ps(command_writer, TERAKAN_META_SHADER_BLIT_IMAGE_PS);
         terakan_meta_blit_set_fs_sampler(command_writer, pBlitImageInfo->filter);
         command_writer->meta_blit_draw_session_active = true;
      }

      for (uint32_t region_index = 0; region_index < pBlitImageInfo->regionCount; ++region_index) {
         VkImageBlit2 const * const region = &pBlitImageInfo->pRegions[region_index];

         if (terakan_meta_blit_region_to_copy(region, &copies[0])) {
            continue;
         }

         float scale_x, scale_y, off_x, off_y;
         terakan_meta_blit_region_scale_off(region->srcOffsets[0].x, region->srcOffsets[1].x,
                                            region->dstOffsets[0].x, region->dstOffsets[1].x,
                                            &scale_x, &off_x);
         terakan_meta_blit_region_scale_off(region->srcOffsets[0].y, region->srcOffsets[1].y,
                                            region->dstOffsets[0].y, region->dstOffsets[1].y,
                                            &scale_y, &off_y);

         uint32_t constants[TERAKAN_META_BLIT_CONSTS_COUNT] = {
            fui(scale_x),
            fui(off_x),
            fui(scale_y),
            fui(off_y),
         };
         terakan_meta_config_draw_set_kcache_push_constants(command_writer, sizeof(constants),
                                                            constants, false, true);
         terakan_meta_blit_image_region_draw(command_writer, pBlitImageInfo, region);
      }

      command_writer->post_color_image_copy_write_barrier_actions |=
         TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
         TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;
      if (same_image_scaled) {
         command_writer->post_color_image_copy_write_barrier_actions |=
            TERAKAN_BARRIER_ACTION_INV_TC | TERAKAN_BARRIER_ACTION_INV_VC;
      }
   }

   if (copy_count > 0) {
      VkCopyImageInfo2 const copy_info = {
         .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
         .srcImage = pBlitImageInfo->srcImage,
         .srcImageLayout = pBlitImageInfo->srcImageLayout,
         .dstImage = pBlitImageInfo->dstImage,
         .dstImageLayout = pBlitImageInfo->dstImageLayout,
         .regionCount = copy_count,
         .pRegions = copies,
      };
      terakan_CmdCopyImage2(commandBuffer, &copy_info);
   }

   STACK_ARRAY_FINISH(copies);

   if (has_scaled_regions) {
      command_writer->meta_blit_draw_session_active = false;
   }
}
