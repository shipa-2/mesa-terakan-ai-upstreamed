/*
 * Copyright © 2026 Terakan contributors
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

#ifndef TERAKAN_HW_CONFIG_COMPUTE_H
#define TERAKAN_HW_CONFIG_COMPUTE_H

#include "terakan_shader.h"

#include "util/bitset.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum terakan_hw_config_compute_entry {
   TERAKAN_HW_CONFIG_COMPUTE_ENTRY_ENABLE,
   TERAKAN_HW_CONFIG_COMPUTE_ENTRY_SQ_PGM_LS,
   TERAKAN_HW_CONFIG_COMPUTE_ENTRY_SPI_COMPUTE_NUM_THREAD,
   TERAKAN_HW_CONFIG_COMPUTE_ENTRY_SQ_LDS_ALLOC,
   TERAKAN_HW_CONFIG_COMPUTE_ENTRY_VGT_COMPUTE_DISPATCH,

   TERAKAN_HW_CONFIG_COMPUTE_ENTRY_COUNT,
};

struct terakan_hw_config_compute {
   BITSET_DECLARE(entries_modified_, TERAKAN_HW_CONFIG_COMPUTE_ENTRY_COUNT);

   struct terakan_shader_static const * sq_pgm_ls_;

   uint32_t block_size_[3];
   uint32_t lds_alloc_;
   uint32_t vgt_num_indices_;
   uint32_t vgt_compute_thread_group_size_;
};

void terakan_hw_config_compute_reset(struct terakan_hw_config_compute * config);

void terakan_hw_config_compute_set_all_modified(struct terakan_hw_config_compute * config);

void terakan_hw_config_compute_set_sq_pgm_ls(struct terakan_hw_config_compute * config,
                                             struct terakan_shader_static const * shader);

void terakan_hw_config_compute_set_dispatch_params(struct terakan_hw_config_compute * config,
                                                   uint32_t block_size_x, uint32_t block_size_y,
                                                   uint32_t block_size_z, uint32_t lds_dwords,
                                                   uint32_t num_waves);

struct terakan_gfx_command_writer;

void terakan_hw_config_compute_emit_modified(struct terakan_gfx_command_writer * command_writer);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_HW_CONFIG_COMPUTE_H */
