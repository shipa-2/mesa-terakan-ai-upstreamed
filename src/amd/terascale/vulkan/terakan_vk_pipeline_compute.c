/*
 * Copyright © 2026 Terakan contributors
 */

#include "terakan_vk_pipeline_compute.h"

#include "terakan_app_config_compute.h"
#include "terakan_bo.h"
#include "terakan_command_buffer.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_physical_device.h"
#include "terakan_pipeline_layout.h"
#include "terakan_shader.h"

#include "nir/nir.h"
#include "spirv/nir_spirv.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_log.h"
#include "vk_pipeline.h"
#include "vk_shader_module.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

uint32_t
terakan_vk_pipeline_compute_cb_target_mask(struct terakan_shader_impl const * const shader)
{
   uint32_t mask = 0;
   unsigned uav_index;
   BITSET_FOREACH_SET (uav_index, shader->uavs_for_mutable_resources_needed,
                       TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL) {
      mask |= 0xF << (4 * uav_index);
   }
   return mask;
}

static uint32_t
terakan_vk_pipeline_compute_num_waves(
   struct terakan_physical_device_chip_info const * const chip_info, uint32_t const block_size_x,
   uint32_t const block_size_y, uint32_t const block_size_z)
{
   uint32_t const group_size = block_size_x * block_size_y * block_size_z;
   uint32_t const num_pipes = MAX2(1u, 1u << chip_info->max_render_backends_log2);
   uint32_t const wave_divisor = 16 * num_pipes;
   return DIV_ROUND_UP(group_size, wave_divisor);
}

static void
terakan_vk_pipeline_compute_cmd_bind(struct vk_command_buffer * const command_buffer_base,
                                     struct vk_pipeline * const pipeline_base)
{
   struct terakan_command_buffer * const command_buffer =
      container_of(command_buffer_base, struct terakan_command_buffer, vk);
   struct terakan_gfx_command_writer * const command_writer = command_buffer->command_writer.gfx;
   struct terakan_vk_pipeline_compute const * const pipeline =
      container_of(pipeline_base, struct terakan_vk_pipeline_compute const, vk);

   terakan_app_config_compute_bind_shader(
      command_writer, &pipeline->shader, pipeline->block_size_[0], pipeline->block_size_[1],
      pipeline->block_size_[2], pipeline->lds_dwords_, pipeline->num_waves_,
      pipeline->cb_target_mask_);
}

static void
terakan_vk_pipeline_compute_destroy(struct vk_device * const device_base,
                                    struct vk_pipeline * const pipeline_base,
                                    VkAllocationCallbacks const * const allocator)
{
   struct terakan_vk_pipeline_compute * const pipeline =
      container_of(pipeline_base, struct terakan_vk_pipeline_compute, vk);

   terakan_shader_impl_finish(&pipeline->shader);
   terakan_bo_free(pipeline->shader_bo, allocator);
   vk_pipeline_free(device_base, allocator, pipeline_base);
}

static VkResult
terakan_vk_pipeline_compute_get_executable_properties(
   UNUSED struct vk_device * const device_base, UNUSED struct vk_pipeline * const pipeline_base,
   uint32_t * const executable_count, UNUSED VkPipelineExecutablePropertiesKHR * const properties)
{
   *executable_count = 0;
   return VK_SUCCESS;
}

struct vk_pipeline_ops const terakan_vk_pipeline_compute_ops = {
   .destroy = terakan_vk_pipeline_compute_destroy,
   .get_executable_properties = terakan_vk_pipeline_compute_get_executable_properties,
   .cmd_bind = terakan_vk_pipeline_compute_cmd_bind,
};

static VkResult
terakan_vk_pipeline_compute_create(struct terakan_device * const device,
                                   VkComputePipelineCreateInfo const * const create_info,
                                   VkAllocationCallbacks const * const allocator,
                                   struct terakan_vk_pipeline_compute ** const pipeline_out)
{
   struct terakan_vk_pipeline_compute * const pipeline = vk_pipeline_zalloc(
      &device->vk, &terakan_vk_pipeline_compute_ops, VK_PIPELINE_BIND_POINT_COMPUTE,
      vk_compute_pipeline_create_flags(create_info), allocator,
      sizeof(struct terakan_vk_pipeline_compute));
   if (pipeline == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   struct terakan_pipeline_layout const * const pipeline_layout =
      terakan_pipeline_layout_from_handle(create_info->layout);

   struct vk_shader_module const * const module =
      vk_shader_module_from_handle(create_info->stage.module);
   if (module == NULL) {
      terakan_vk_pipeline_compute_destroy(&device->vk, &pipeline->vk, allocator);
      return vk_error(device, VK_ERROR_UNKNOWN);
   }

   struct terakan_shader_impl * const shader = &pipeline->shader;
   shader->push_constants_usage.app_extent_bytes =
      pipeline_layout->shader_app_push_constants_extents_bytes[MESA_SHADER_COMPUTE];

   nir_shader * nir = terakan_shader_spirv_to_nir(
      device, module->size, (uint32_t const *)module->data, MESA_SHADER_COMPUTE,
      create_info->stage.pName, create_info->stage.pSpecializationInfo);
   if (nir == NULL) {
      terakan_vk_pipeline_compute_destroy(&device->vk, &pipeline->vk, allocator);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   pipeline->block_size_[0] = nir->info.workgroup_size[0];
   pipeline->block_size_[1] = nir->info.workgroup_size[1];
   pipeline->block_size_[2] = nir->info.workgroup_size[2];
   pipeline->lds_dwords_ = nir->info.shared_size / 4;

   terakan_shader_lower_and_optimize_post_link(
      nir, pipeline_layout, &shader->sqk_usage, shader->uavs_for_mutable_resources_needed,
      &shader->push_constants_usage.driver_constants, NULL);

   union r600_shader_key shader_key = {};
   VkResult result = terakan_shader_impl_compile(shader, device, &shader_key, nir, allocator);
   ralloc_free(nir);
   if (result != VK_SUCCESS) {
      terakan_vk_pipeline_compute_destroy(&device->vk, &pipeline->vk, allocator);
      return result;
   }

   struct terakan_physical_device_chip_info const * const chip_info =
      &terakan_device_physical_device(device)->chip_info;
   pipeline->num_waves_ = terakan_vk_pipeline_compute_num_waves(
      chip_info, pipeline->block_size_[0], pipeline->block_size_[1], pipeline->block_size_[2]);
   pipeline->cb_target_mask_ = terakan_vk_pipeline_compute_cb_target_mask(shader);

   uint32_t const shader_bo_bytes_shr8 =
      DIV_ROUND_UP(shader->shader.bc.ndw, (uint32_t)1 << (8 - 2));
   result = device->winsys_fn->bo->allocate_device_memory(
      device, shader_bo_bytes_shr8 << 8, TERAKAN_SHADER_PROGRAM_ALIGNMENT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      0, allocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &pipeline->shader_bo);
   if (result != VK_SUCCESS) {
      terakan_vk_pipeline_compute_destroy(&device->vk, &pipeline->vk, allocator);
      return vk_error(device, result);
   }

   uint32_t * const shader_bo_mapping = terakan_bo_map(pipeline->shader_bo);
   if (shader_bo_mapping == NULL) {
      terakan_vk_pipeline_compute_destroy(&device->vk, &pipeline->vk, allocator);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   shader->static_state.program_va_shr8 = 0;
   util_memcpy_cpu_to_le32(shader_bo_mapping, shader->shader.bc.bytecode,
                           (uint32_t)sizeof(uint32_t) * shader->shader.bc.ndw);
   shader->static_state.program_bo = pipeline->shader_bo;
   shader->static_state.program_va_shr8 += (uint32_t)(pipeline->shader_bo->va >> 8);
   r600_bytecode_clear(&shader->shader.bc);
   terakan_bo_unmap(pipeline->shader_bo);

   *pipeline_out = pipeline;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateComputePipelines(VkDevice const deviceHandle, VkPipelineCache const pipelineCache,
                               uint32_t const createInfoCount,
                               VkComputePipelineCreateInfo const * const pCreateInfos,
                               VkAllocationCallbacks const * const pAllocator,
                               VkPipeline * const pPipelines)
{
   (void)pipelineCache;

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);
   VkResult first_error = VK_SUCCESS;

   memset(pPipelines, 0, createInfoCount * sizeof(*pPipelines));

   for (uint32_t i = 0; i < createInfoCount; ++i) {
      struct terakan_vk_pipeline_compute * pipeline = NULL;
      VkResult result =
         terakan_vk_pipeline_compute_create(device, &pCreateInfos[i], pAllocator, &pipeline);
      if (result == VK_SUCCESS) {
         pPipelines[i] = vk_pipeline_to_handle(&pipeline->vk);
         continue;
      }
      if (first_error == VK_SUCCESS) {
         first_error = result;
      }
      if (result != VK_PIPELINE_COMPILE_REQUIRED) {
         return result;
      }
      if (vk_compute_pipeline_create_flags(&pCreateInfos[i]) &
          VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR) {
         return result;
      }
   }

   return first_error;
}
