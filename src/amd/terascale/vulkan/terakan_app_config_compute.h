/*
 * Copyright © 2026 Terakan contributors
 */

#ifndef TERAKAN_APP_CONFIG_COMPUTE_H
#define TERAKAN_APP_CONFIG_COMPUTE_H

#include "terakan_shader.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_app_config_compute {
   struct terakan_shader_impl const * shader;

   uint32_t block_size_[3];
   uint32_t lds_dwords_;
   uint32_t num_waves_;

   uint32_t cb_target_mask_;

   bool pending_;
};

struct terakan_gfx_command_writer;

void terakan_app_config_compute_reset(struct terakan_app_config_compute * config);

void terakan_app_config_compute_clear_binding(struct terakan_app_config_compute * config);

void terakan_app_config_compute_bind_shader(struct terakan_gfx_command_writer * command_writer,
                                            struct terakan_shader_impl const * shader,
                                            uint32_t block_size_x, uint32_t block_size_y,
                                            uint32_t block_size_z, uint32_t lds_dwords,
                                            uint32_t num_waves, uint32_t cb_target_mask);

void terakan_app_config_compute_apply_pending(struct terakan_gfx_command_writer * command_writer);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_APP_CONFIG_COMPUTE_H */
