/*
 * Copyright © 2026 Terakan contributors
 */

#ifndef TERAKAN_VK_PIPELINE_COMPUTE_H
#define TERAKAN_VK_PIPELINE_COMPUTE_H

#include "terakan_bo.h"
#include "terakan_shader.h"

#include "vk_pipeline.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_vk_pipeline_compute {
   struct vk_pipeline vk;

   struct terakan_bo * shader_bo;
   struct terakan_shader_impl shader;

   uint32_t block_size_[3];
   uint32_t lds_dwords_;
   uint32_t num_waves_;
   uint32_t cb_target_mask_;
};

uint32_t terakan_vk_pipeline_compute_cb_target_mask(
   struct terakan_shader_impl const * shader);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_VK_PIPELINE_COMPUTE_H */
