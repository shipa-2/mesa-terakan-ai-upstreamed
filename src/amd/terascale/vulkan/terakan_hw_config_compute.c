/*
 * Copyright © 2026 Terakan contributors
 */

#include "terakan_hw_config_compute.h"

#include "terakan_command_buffer.h"
#include "terakan_physical_device.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/bitscan.h"
#include "util/macros.h"

static void
terakan_hw_config_compute_emit_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_physical_device_chip_info const * const chip_info =
      &terakan_gfx_command_writer_physical_device(command_writer)->chip_info;

   unsigned packet_dwords = 2 + 1 + 2 + 1 + 2 + 1;
   if (!chip_info->is_r9xx) {
      packet_dwords += 2 + 1;
   }

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, packet_dwords);
   if (unlikely(packet == NULL)) {
      return;
   }

   if (!chip_info->is_r9xx) {
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE;
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028838_SQ_DYN_GPR_RESOURCE_LIMIT_1);
      *packet++ = S_028838_PS_GPRS(0x1e) | S_028838_VS_GPRS(0x1e) | S_028838_GS_GPRS(0x1e) |
                  S_028838_ES_GPRS(0x1e) | S_028838_HS_GPRS(0x1e) | S_028838_LS_GPRS(0x1e);
   }

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE;
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028A40_VGT_GS_MODE);
   *packet++ = S_028A40_COMPUTE_MODE(1) | S_028A40_PARTIAL_THD_AT_EOI(1);

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE;
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028B54_VGT_SHADER_STAGES_EN);
   *packet++ = 2; /* CS_ON */

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE;
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_0286E8_SPI_COMPUTE_INPUT_CNTL);
   *packet++ = S_0286E8_TID_IN_GROUP_ENA(1) | S_0286E8_TGID_ENA(1) |
               S_0286E8_DISABLE_INDEX_PACK(1);

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_compute_emit_sq_pgm_ls(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_shader_static const * const shader =
      command_writer->hw_config_compute.sq_pgm_ls_;
   if (shader == NULL) {
      return;
   }

   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 3, 1, 1, 0);
   if (unlikely(packet == NULL)) {
      return;
   }

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 3, 0) | TERAKAN_PACKET3_COMPUTE;
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_0288D0_SQ_PGM_START_LS);
   uint32_t const * const packet_pgm_start = packet;
   *packet++ = shader->program_va_shr8;
   *packet++ = shader->sq_pgm_resources[0];
   *packet++ = shader->sq_pgm_resources[1];
   terakan_gfx_command_writer_add_relocation(
      command_writer, &packet, packet_pgm_start, *packet_pgm_start,
      TERASCALE_WDDM_PATCH_IDS_SQ_PGM_START_LS,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                shader->program_bo, true, false,
                                                TERAKAN_BO_PRIORITY_SHADER_BINARY));
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_compute_emit_spi_compute_num_thread(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_compute const * const config = &command_writer->hw_config_compute;

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 3);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 3, 0) | TERAKAN_PACKET3_COMPUTE;
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_0286EC_SPI_COMPUTE_NUM_THREAD_X);
   *packet++ = config->block_size_[0];
   *packet++ = config->block_size_[1];
   *packet++ = config->block_size_[2];
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_compute_emit_sq_lds_alloc(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE;
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_0288E8_SQ_LDS_ALLOC);
   *packet++ = command_writer->hw_config_compute.lds_alloc_;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_compute_emit_vgt_compute_dispatch(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_compute const * const config = &command_writer->hw_config_compute;

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1 + 2 + 3 + 2 + 1);
   if (unlikely(packet == NULL)) {
      return;
   }

   *packet++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
   *packet++ = TERAKAN_CONFIG_REG_OFFSET(R_008970_VGT_NUM_INDICES);
   *packet++ = config->vgt_num_indices_;

   *packet++ = PKT3(PKT3_SET_CONFIG_REG, 3, 0);
   *packet++ = TERAKAN_CONFIG_REG_OFFSET(R_00899C_VGT_COMPUTE_START_X);
   *packet++ = 0;
   *packet++ = 0;
   *packet++ = 0;

   *packet++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
   *packet++ = TERAKAN_CONFIG_REG_OFFSET(R_0089AC_VGT_COMPUTE_THREAD_GROUP_SIZE);
   *packet++ = config->vgt_compute_thread_group_size_;

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

typedef void (*terakan_hw_config_compute_emit_fn)(struct terakan_gfx_command_writer *);

static terakan_hw_config_compute_emit_fn const terakan_hw_config_compute_emit_functions
   [TERAKAN_HW_CONFIG_COMPUTE_ENTRY_COUNT] = {
      [TERAKAN_HW_CONFIG_COMPUTE_ENTRY_ENABLE] = terakan_hw_config_compute_emit_enable,
      [TERAKAN_HW_CONFIG_COMPUTE_ENTRY_SQ_PGM_LS] = terakan_hw_config_compute_emit_sq_pgm_ls,
      [TERAKAN_HW_CONFIG_COMPUTE_ENTRY_SPI_COMPUTE_NUM_THREAD] =
         terakan_hw_config_compute_emit_spi_compute_num_thread,
      [TERAKAN_HW_CONFIG_COMPUTE_ENTRY_SQ_LDS_ALLOC] = terakan_hw_config_compute_emit_sq_lds_alloc,
      [TERAKAN_HW_CONFIG_COMPUTE_ENTRY_VGT_COMPUTE_DISPATCH] =
         terakan_hw_config_compute_emit_vgt_compute_dispatch,
   };

void
terakan_hw_config_compute_reset(struct terakan_hw_config_compute * const config)
{
   config->sq_pgm_ls_ = NULL;
   config->block_size_[0] = 1;
   config->block_size_[1] = 1;
   config->block_size_[2] = 1;
   config->lds_alloc_ = 0;
   config->vgt_num_indices_ = 1;
   config->vgt_compute_thread_group_size_ = 1;
   terakan_hw_config_compute_set_all_modified(config);
}

void
terakan_hw_config_compute_set_all_modified(struct terakan_hw_config_compute * const config)
{
   BITSET_ONES(config->entries_modified_);
}

void
terakan_hw_config_compute_set_sq_pgm_ls(struct terakan_hw_config_compute * const config,
                                        struct terakan_shader_static const * const shader)
{
   if (config->sq_pgm_ls_ == shader) {
      return;
   }
   config->sq_pgm_ls_ = shader;
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_COMPUTE_ENTRY_SQ_PGM_LS);
}

void
terakan_hw_config_compute_set_dispatch_params(struct terakan_hw_config_compute * const config,
                                              uint32_t const block_size_x,
                                              uint32_t const block_size_y,
                                              uint32_t const block_size_z,
                                              uint32_t const lds_dwords, uint32_t const num_waves)
{
   bool modified = false;

   if (config->block_size_[0] != block_size_x || config->block_size_[1] != block_size_y ||
       config->block_size_[2] != block_size_z) {
      config->block_size_[0] = block_size_x;
      config->block_size_[1] = block_size_y;
      config->block_size_[2] = block_size_z;
      BITSET_SET(config->entries_modified_,
                 TERAKAN_HW_CONFIG_COMPUTE_ENTRY_SPI_COMPUTE_NUM_THREAD);
      modified = true;
   }

   uint32_t const group_size = block_size_x * block_size_y * block_size_z;
   if (config->vgt_num_indices_ != group_size ||
       config->vgt_compute_thread_group_size_ != group_size) {
      config->vgt_num_indices_ = group_size;
      config->vgt_compute_thread_group_size_ = group_size;
      BITSET_SET(config->entries_modified_,
                 TERAKAN_HW_CONFIG_COMPUTE_ENTRY_VGT_COMPUTE_DISPATCH);
      modified = true;
   }

   uint32_t const lds_alloc = lds_dwords | (num_waves << 14);
   if (config->lds_alloc_ != lds_alloc) {
      config->lds_alloc_ = lds_alloc;
      BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_COMPUTE_ENTRY_SQ_LDS_ALLOC);
      modified = true;
   }

   (void)modified;
}

void
terakan_hw_config_compute_emit_modified(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_compute * const config = &command_writer->hw_config_compute;

   unsigned entry_index;
   BITSET_FOREACH_SET (entry_index, config->entries_modified_,
                       TERAKAN_HW_CONFIG_COMPUTE_ENTRY_COUNT) {
      terakan_hw_config_compute_emit_functions[entry_index](command_writer);
   }
   BITSET_ZERO(config->entries_modified_);
}
