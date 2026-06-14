# Copyright © 2024 Vitaliy Triang3l Kuzmin

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

terakan_entrypoints = custom_target(
  'terakan_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['terakan_entrypoints.h', 'terakan_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@',
    '--xml', '@INPUT1@',
    '--proto',
    '--weak',
    '--out-h', '@OUTPUT0@',
    '--out-c', '@OUTPUT1@',
    '--prefix', 'terakan',
    '--beta', with_vulkan_beta.to_string(),
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

libterakan_files = files(
  'meta/terakan_meta.c',
  'meta/terakan_meta.h',
  'meta/terakan_meta_2d.c',
  'meta/terakan_meta_clear.c',
  'meta/terakan_meta_copy_buffer_image.c',
  'meta/terakan_meta_copy_expand_3x.c',
  'meta/terakan_meta_copy_image.c',
  'meta/terakan_meta_blit_image.c',
  'meta/terakan_meta_dummy.c',
  'meta/terakan_meta_impl.h',
  'meta/terakan_meta_query_accum.c',
  'meta/terakan_meta_query_copy.c',
  'nir/terakan_nir.h',
  'nir/terakan_nir_alu.c',
  'nir/terakan_nir_buffer.c',
  'nir/terakan_nir_compact_rtv_dsb_exports.c',
  'nir/terakan_nir_lower_bindings.c',
  'terakan_app_config_compute.c',
  'terakan_app_config_compute.h',
  'terakan_app_config_draw.c',
  'terakan_app_config_draw.h',
  'terakan_barrier.c',
  'terakan_bo.c',
  'terakan_bo.h',
  'terakan_buffer.c',
  'terakan_buffer.h',
  'terakan_command_buffer.c',
  'terakan_command_buffer.h',
  'terakan_cp_dma.c',
  'terakan_cp_dma.h',
  'terakan_descriptor.c',
  'terakan_descriptor.h',
  'terakan_descriptor_set.c',
  'terakan_descriptor_set.h',
  'terakan_descriptor_set_layout.c',
  'terakan_descriptor_set_layout.h',
  'terakan_device.c',
  'terakan_device.h',
  'terakan_device_memory.c',
  'terakan_device_memory.h',
  'terakan_format.c',
  'terakan_format.h',
  'terakan_hw_config_draw.c',
  'terakan_hw_config_draw.h',
  'terakan_hw_config_compute.c',
  'terakan_hw_config_compute.h',
  'terakan_hw_config_shared.c',
  'terakan_hw_config_shared.h',
  'terakan_hw_config_sqk.c',
  'terakan_hw_config_sqk.h',
  'terakan_image.c',
  'terakan_image.h',
  'terakan_instance.c',
  'terakan_instance.h',
  'terakan_limits.h',
  'terakan_physical_device.c',
  'terakan_physical_device.h',
  'terakan_pipeline_layout.c',
  'terakan_pipeline_layout.h',
  'terakan_push_constants.c',
  'terakan_push_constants.h',
  'terakan_query.c',
  'terakan_query.h',
  'terakan_queue.c',
  'terakan_queue.h',
  'terakan_sampler.c',
  'terakan_sampler.h',
  'terakan_screen_rect.h',
  'terakan_shader.c',
  'terakan_shader.h',
  'terakan_shader_sfn.cpp',
  'terakan_sync_completion.c',
  'terakan_sync_completion.h',
  'terakan_vertex_input.c',
  'terakan_vertex_input.h',
  'terakan_vk_draw.c',
  'terakan_vk_dispatch.c',
  'terakan_vk_pipeline_compute.c',
  'terakan_vk_pipeline_compute.h',
  'terakan_vk_pipeline_graphics.c',
  'terakan_vk_pipeline_graphics.h',
  'terakan_vk_render_pass.c',
  'terakan_vk_state.c',
  'terakan_vk_state.h',
  'terakan_wsi.c',
  'terakan_wsi.h',
)

libterakan_deps = [
  dep_thread,
  dep_dl,
  dep_m,
  idep_amd_terascale_common,
  idep_amd_terascale_compiler,
  idep_mesautil,
  idep_nir,
  idep_vulkan_util,
  idep_vulkan_wsi,
  idep_vulkan_runtime,
  idep_xmlconfig,
]

if with_platform_windows
  libterakan_files += files(
    'winsys/wddm/terakan_bo_wddm.c',
    'winsys/wddm/terakan_bo_wddm.h',
    'winsys/wddm/terakan_device_wddm.c',
    'winsys/wddm/terakan_device_wddm.h',
    'winsys/wddm/terakan_instance_wddm.c',
    'winsys/wddm/terakan_instance_wddm.h',
    'winsys/wddm/terakan_physical_device_wddm.c',
    'winsys/wddm/terakan_physical_device_wddm.h',
    'winsys/wddm/terakan_queue_wddm.c',
    'winsys/wddm/terakan_queue_wddm.h',
    'winsys/wddm/terakan_wddm_d3dkmthk.h',
  )
  libterakan_deps += [
    cc.find_library('cfgmgr32'),
    cc.find_library('dxgi'),
    cc.find_library('gdi32'),
  ]
elif system_has_kms_drm
  libterakan_files += files(
    'winsys/drm_radeon/terakan_bo_drm_radeon.c',
    'winsys/drm_radeon/terakan_bo_drm_radeon.h',
    'winsys/drm_radeon/terakan_device_drm_radeon.c',
    'winsys/drm_radeon/terakan_device_drm_radeon.h',
    'winsys/drm_radeon/terakan_instance_drm_radeon.c',
    'winsys/drm_radeon/terakan_instance_drm_radeon.h',
    'winsys/drm_radeon/terakan_physical_device_drm_radeon.c',
    'winsys/drm_radeon/terakan_physical_device_drm_radeon.h',
    'winsys/drm_radeon/terakan_queue_drm_radeon.c',
    'winsys/drm_radeon/terakan_queue_drm_radeon.h',
  )
  libterakan_deps += dep_libdrm
endif

libvulkan_terascale = shared_library(
  'vulkan_terascale',
  [
    libterakan_files,
    terakan_entrypoints,
    sha1_h,
  ],
  vs_module_defs : vulkan_api_def,
  include_directories : [
    inc_include,
    inc_src,
    inc_mapi,
    inc_mesa,
    inc_gallium,
    inc_gallium_aux,
    inc_amd,
    inc_amd_common,
    inc_compiler,
    inc_util,
  ],
  dependencies : libterakan_deps,
  c_args : [
    no_override_init_args,
    c_msvc_compat_args,
  ],
  cpp_args : [
    cpp_msvc_compat_args,
  ],
  link_args : [
    ld_args_build_id,
    ld_args_bsymbolic,
    ld_args_gc_sections,
    vulkan_icd_link_args,
  ],
  link_depends : vulkan_icd_link_depends,
  gnu_symbol_visibility : 'hidden',
  name_prefix : host_machine.system() == 'windows' ? '' : [],
  install : true,
)

icd_lib_path = join_paths(get_option('prefix'), get_option('libdir'))
icd_file_name = 'libvulkan_terascale.so'
if with_platform_windows
  icd_lib_path = join_paths(get_option('prefix'), get_option('bindir'))
  icd_file_name = 'vulkan_terascale.dll'
endif

icd_command = [
  prog_python, '@INPUT0@',
  '--api-version', '1.1',
  '--xml', '@INPUT1@',
  '--lib-path', join_paths(icd_lib_path, icd_file_name),
  '--out', '@OUTPUT@',
]
if with_platform_windows
  icd_command += '--use-backslash'
endif

terascale_icd = custom_target(
  'terascale_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'terascale_icd.@0@.json'.format(host_machine.cpu()),
  command : icd_command,
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install : true,
)

_dev_icdname = 'terascale_devenv_icd.@0@.json'.format(host_machine.cpu())
_dev_icd = custom_target(
  'terascale_devenv_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : _dev_icdname,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.1',
    '--xml', '@INPUT1@',
    '--lib-path', meson.current_build_dir() / icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
)

devenv.append('VK_ICD_FILENAMES', _dev_icd.full_path())
