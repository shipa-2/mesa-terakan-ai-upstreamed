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

# The generation label recorded into the build (VkPhysicalDeviceProperties2::driverInfo and a
# preprocessor define), not a functional restriction: the driver still detects the chip family of
# whichever GPU it actually runs on at Vulkan device creation time, the same as always. See
# terakan-target-generation's description in meson.options for what r600/r700 selection does and
# does not do.
terakan_target_generation = get_option('terakan-target-generation')
if terakan_target_generation == 'auto'
  detect_result = run_command(
    prog_python, files('terakan_detect_generation.py'),
    files('../../../../include/pci_ids/r600_pci_ids.h'),
    check : false,
  )
  detected_generation = detect_result.returncode() == 0 ? detect_result.stdout().strip() : 'none'
  if detected_generation in ['r600', 'r700', 'r800', 'r900']
    terakan_target_generation = detected_generation
    message('Terakan: auto-detected build target generation @0@ from a local AMD PCI device'
            .format(terakan_target_generation))
  else
    terakan_target_generation = 'r900'
    message(('Terakan: no single AMD TeraScale PCI device found on the build machine (@0@); ' +
             'defaulting the build target generation to r900').format(detected_generation))
  endif
endif
if terakan_target_generation == 'r600'
  warning('Terakan: R600 logical-device and image-layout bring-up is enabled, but command ' +
          'submission remains disabled pending hardware readback validation.')
elif terakan_target_generation == 'r700'
  warning('Terakan: R700 logical-device and image-layout bring-up is enabled, but command ' +
          'submission remains disabled pending hardware readback validation.')
endif
terakan_target_generation_args = [
  '-DTERAKAN_BUILD_TARGET_GENERATION_NAME="' + terakan_target_generation + '"',
  '-DTERAKAN_BUILD_TARGET_GENERATION_' + terakan_target_generation.to_upper(),
]

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
  'meta/terakan_meta_nir.c',
  'meta/terakan_meta_nir.h',
  'meta/terakan_meta_query_accum.c',
  'meta/terakan_meta_query_copy.c',
  'meta/terakan_meta_resolve.c',
  'nir/terakan_nir.h',
  'nir/terakan_nir_alu.c',
  'nir/terakan_nir_buffer.c',
  'nir/terakan_nir_compact_rtv_dsb_exports.c',
  'nir/terakan_nir_lower_bindings.c',
  'nir/terakan_nir_lower_subgroups.c',
  'nir/terakan_nir_lower_integer_cube_fetch.c',
  'nir/terakan_nir_lower_unnormalized_coordinates.c',
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
  'terakan_cp_dma_limits.h',
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
  'terakan_event.c',
  'terakan_event.h',
  'terakan_format.c',
  'terakan_format.h',
  'terakan_hw_config_draw.c',
  'terakan_hw_config_draw.h',
  'terakan_hw_config_compute.c',
  'terakan_hw_config_compute.h',
  'terakan_hw_config_compute_terascale_1.c',
  'terakan_hw_config_compute_terascale_1.h',
  'terakan_hw_config_loop_constants.c',
  'terakan_hw_config_loop_constants.h',
  'terakan_hw_config_shared.c',
  'terakan_hw_config_shared.h',
  'terakan_hw_config_shared_terascale_1.c',
  'terakan_hw_config_shared_terascale_1.h',
  'terakan_hw_config_draw_terascale_1.c',
  'terakan_hw_config_draw_terascale_1.h',
  'terakan_hw_config_sqk.c',
  'terakan_hw_config_sqk.h',
  'terakan_image.c',
  'terakan_image.h',
  'terakan_image_tiling_terascale_1.c',
  'terakan_image_tiling_terascale_1.h',
  'terakan_image_surface_terascale_1.c',
  'terakan_image_surface_terascale_1.h',
  'terakan_instance.c',
  'terakan_instance.h',
  'terakan_limits.h',
  'terakan_physical_device.c',
  'terakan_physical_device.h',
  'terakan_physical_device_backend_count.c',
  'terakan_physical_device_backend_count.h',
  'terakan_physical_device_tiling_config.c',
  'terakan_physical_device_tiling_config.h',
  'terakan_pipeline_layout.c',
  'terakan_pipeline_layout.h',
  'terakan_push_constants.c',
  'terakan_push_constants.h',
  'terakan_query.c',
  'terakan_query.h',
  'terakan_resource_descriptor_terascale_1.c',
  'terakan_resource_descriptor_terascale_1.h',
  'terakan_queue.c',
  'terakan_queue.h',
  'terakan_sampler.c',
  'terakan_sampler.h',
  'terakan_sampler_terascale_1.c',
  'terakan_sampler_terascale_1.h',
  'terakan_screen_rect.h',
  'terakan_shader.c',
  'terakan_shader.h',
  'terakan_shader_generation.c',
  'terakan_shader_generation.h',
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
    terakan_target_generation_args,
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

if with_tests
  terakan_sfn_lowering_test = executable(
    'terakan_sfn_lowering_test',
    [
      'nir/terakan_nir_lower_subgroups.c',
      'tests/terakan_sfn_lowering_test.cpp',
    ],
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
    dependencies : [
      idep_amd_terascale_compiler,
      idep_mesautil,
      idep_nir,
    ],
    cpp_args : cpp_msvc_compat_args,
  )
  test('terakan_sfn_lowering', terakan_sfn_lowering_test)

  terakan_hw_config_loop_constants_test = executable(
    'terakan_hw_config_loop_constants_test',
    [
      'terakan_hw_config_loop_constants.c',
      'tests/terakan_hw_config_loop_constants_test.c',
    ],
    include_directories : [
      inc_include,
      inc_src,
      inc_gallium,
    ],
    c_args : c_msvc_compat_args,
  )
  test('terakan_hw_config_loop_constants', terakan_hw_config_loop_constants_test)

  terakan_hw_config_compute_terascale_1_test = executable(
    'terakan_hw_config_compute_terascale_1_test',
    [
      'terakan_hw_config_compute_terascale_1.c',
      'tests/terakan_hw_config_compute_terascale_1_test.c',
    ],
    include_directories : [
      inc_include,
      inc_src,
    ],
    c_args : c_msvc_compat_args,
  )
  test('terakan_hw_config_compute_terascale_1', terakan_hw_config_compute_terascale_1_test)

  terakan_hw_config_shared_terascale_1_test = executable(
    'terakan_hw_config_shared_terascale_1_test',
    [
      'terakan_hw_config_shared_terascale_1.c',
      'tests/terakan_hw_config_shared_terascale_1_test.c',
    ],
    include_directories : [
      inc_include,
      inc_src,
      inc_gallium,
    ],
    c_args : c_msvc_compat_args,
  )
  test('terakan_hw_config_shared_terascale_1', terakan_hw_config_shared_terascale_1_test)

  terakan_cp_dma_terascale_1_test = executable(
    'terakan_cp_dma_terascale_1_test',
    'tests/terakan_cp_dma_terascale_1_test.c',
    include_directories : [
      inc_include,
      inc_src,
    ],
    dependencies : idep_vulkan_util_headers,
    c_args : c_msvc_compat_args,
  )
  test('terakan_cp_dma_terascale_1', terakan_cp_dma_terascale_1_test)

  terakan_sampler_terascale_1_test = executable(
    'terakan_sampler_terascale_1_test',
    [
      'terakan_sampler_terascale_1.c',
      'tests/terakan_sampler_terascale_1_test.c',
    ],
    include_directories : [
      inc_include,
      inc_src,
      inc_gallium,
    ],
    dependencies : idep_vulkan_util_headers,
    c_args : c_msvc_compat_args,
  )
  test('terakan_sampler_terascale_1', terakan_sampler_terascale_1_test)

  terakan_resource_descriptor_terascale_1_test = executable(
    'terakan_resource_descriptor_terascale_1_test',
    [
      'terakan_resource_descriptor_terascale_1.c',
      'tests/terakan_resource_descriptor_terascale_1_test.c',
    ],
    include_directories : [
      inc_include,
      inc_src,
      inc_gallium,
    ],
    c_args : c_msvc_compat_args,
  )
  test('terakan_resource_descriptor_terascale_1',
       terakan_resource_descriptor_terascale_1_test)

  terakan_image_tiling_terascale_1_test = executable(
    'terakan_image_tiling_terascale_1_test',
    [
      'terakan_image_tiling_terascale_1.c',
      'tests/terakan_image_tiling_terascale_1_test.c',
    ],
    include_directories : [
      inc_include,
      inc_src,
    ],
    c_args : c_msvc_compat_args,
  )
  test('terakan_image_tiling_terascale_1', terakan_image_tiling_terascale_1_test)

  terakan_hw_config_draw_terascale_1_test = executable(
    'terakan_hw_config_draw_terascale_1_test',
    [
      'terakan_hw_config_draw_terascale_1.c',
      'tests/terakan_hw_config_draw_terascale_1_test.c',
    ],
    include_directories : [
      inc_include,
      inc_src,
      inc_gallium,
    ],
    c_args : c_msvc_compat_args,
  )
  test('terakan_hw_config_draw_terascale_1', terakan_hw_config_draw_terascale_1_test)

  terakan_physical_device_tiling_config_test = executable(
    'terakan_physical_device_tiling_config_test',
    [
      'terakan_physical_device_tiling_config.c',
      'tests/terakan_physical_device_tiling_config_test.c',
    ],
    include_directories : [
      inc_include,
      inc_src,
    ],
    c_args : c_msvc_compat_args,
  )
  test('terakan_physical_device_tiling_config', terakan_physical_device_tiling_config_test)

  terakan_physical_device_backend_count_test = executable(
    'terakan_physical_device_backend_count_test',
    [
      'terakan_physical_device_backend_count.c',
      'tests/terakan_physical_device_backend_count_test.c',
    ],
    include_directories : [
      inc_include,
      inc_src,
    ],
    c_args : c_msvc_compat_args,
  )
  test('terakan_physical_device_backend_count', terakan_physical_device_backend_count_test)

  terakan_shader_generation_test = executable(
    'terakan_shader_generation_test',
    [
      'terakan_shader_generation.c',
      'tests/terakan_shader_generation_test.c',
    ],
    include_directories : [
      inc_include,
      inc_src,
      inc_gallium,
      inc_gallium_aux,
      inc_amd,
      inc_amd_common,
      inc_compiler,
      inc_util,
    ],
    dependencies : [
      idep_amd_terascale_compiler,
      idep_mesautil,
      idep_nir,
    ],
    c_args : c_msvc_compat_args,
  )
  test('terakan_shader_generation', terakan_shader_generation_test)

  terakan_descriptor_buffer_test = executable(
    'terakan_descriptor_buffer_test',
    [
      'terakan_descriptor.c',
      'tests/terakan_descriptor_buffer_test.c',
    ],
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
    c_args : c_msvc_compat_args,
  )
  test('terakan_descriptor_buffer', terakan_descriptor_buffer_test)

  terakan_vertex_input_test = executable(
    'terakan_vertex_input_test',
    [
      'terakan_vertex_input.c',
      'tests/terakan_vertex_input_test.c',
    ],
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
    dependencies : [
      idep_amd_terascale_common,
      idep_mesautil,
    ],
    c_args : [
      no_override_init_args,
      c_msvc_compat_args,
    ],
  )
  test('terakan_vertex_input', terakan_vertex_input_test)

  vulkan_loader_dep = dependency('vulkan', required : false)
  if vulkan_loader_dep.found()
    terakan_image_array_copy_test = executable(
      'terakan_image_array_copy_test',
      'tests/terakan_image_array_copy_test.cpp',
      dependencies : vulkan_loader_dep,
      cpp_args : cpp_msvc_compat_args,
    )
    test(
      'terakan_image_array_copy',
      terakan_image_array_copy_test,
      depends : [libvulkan_terascale, _dev_icd],
      env : {
        'VK_DRIVER_FILES' : _dev_icd.full_path(),
        'VK_ICD_FILENAMES' : _dev_icd.full_path(),
        'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
      },
      timeout : 10,
    )

    if prog_glslang.found()
      terakan_instance_dynamic_ssbo_vertex_spv = custom_target(
        'terakan_instance_dynamic_ssbo.vert.spv',
        input : 'tests/terakan_instance_dynamic_ssbo.vert',
        output : 'terakan_instance_dynamic_ssbo.vert.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_instance_dynamic_ssbo.vert.spv.d',
      )
      terakan_instance_dynamic_ssbo_depth_vertex_spv = custom_target(
        'terakan_instance_dynamic_ssbo_depth.vert.spv',
        input : 'tests/terakan_instance_dynamic_ssbo_depth.vert',
        output : 'terakan_instance_dynamic_ssbo_depth.vert.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_instance_dynamic_ssbo_depth.vert.spv.d',
      )
      terakan_instance_dynamic_ssbo_fragment_spv = custom_target(
        'terakan_instance_dynamic_ssbo.frag.spv',
        input : 'tests/terakan_instance_dynamic_ssbo.frag',
        output : 'terakan_instance_dynamic_ssbo.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_instance_dynamic_ssbo.frag.spv.d',
      )
      terakan_instance_dynamic_ssbo_test = executable(
        'terakan_instance_dynamic_ssbo_test',
        'tests/terakan_instance_dynamic_ssbo_test.cpp',
        dependencies : vulkan_loader_dep,
        cpp_args : cpp_msvc_compat_args,
      )
      test(
        'terakan_instance_dynamic_ssbo',
        terakan_instance_dynamic_ssbo_test,
        args : [
          terakan_instance_dynamic_ssbo_vertex_spv,
          terakan_instance_dynamic_ssbo_depth_vertex_spv,
          terakan_instance_dynamic_ssbo_fragment_spv,
        ],
        depends : [
          libvulkan_terascale,
          _dev_icd,
          terakan_instance_dynamic_ssbo_vertex_spv,
          terakan_instance_dynamic_ssbo_depth_vertex_spv,
          terakan_instance_dynamic_ssbo_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 5,
      )

      terakan_bc6_cube_vertex_spv = custom_target(
        'terakan_bc6_cube.vert.spv',
        input : 'tests/terakan_bc6_cube.vert',
        output : 'terakan_bc6_cube.vert.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_bc6_cube.vert.spv.d',
      )
      terakan_bc6_cube_fragment_spv = custom_target(
        'terakan_bc6_cube.frag.spv',
        input : 'tests/terakan_bc6_cube.frag',
        output : 'terakan_bc6_cube.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_bc6_cube.frag.spv.d',
      )
      terakan_bc6_array_fragment_spv = custom_target(
        'terakan_bc6_array.frag.spv',
        input : 'tests/terakan_bc6_array.frag',
        output : 'terakan_bc6_array.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_bc6_array.frag.spv.d',
      )
      terakan_bc6_cube_test = executable(
        'terakan_bc6_cube_test',
        'tests/terakan_bc6_cube_test.cpp',
        dependencies : [vulkan_loader_dep, idep_mesautil],
        cpp_args : cpp_msvc_compat_args,
      )
      test(
        'terakan_bc6_cube',
        terakan_bc6_cube_test,
        args : [
          terakan_bc6_cube_vertex_spv,
          terakan_bc6_cube_fragment_spv,
        ],
        depends : [
          libvulkan_terascale,
          _dev_icd,
          terakan_bc6_cube_vertex_spv,
          terakan_bc6_cube_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 10,
      )
      test(
        'terakan_bc6_cube_single_level_views',
        terakan_bc6_cube_test,
        args : [
          terakan_bc6_cube_vertex_spv,
          terakan_bc6_cube_fragment_spv,
          '--single-level-views',
        ],
        depends : [
          libvulkan_terascale,
          _dev_icd,
          terakan_bc6_cube_vertex_spv,
          terakan_bc6_cube_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 10,
      )
      test(
        'terakan_bc6_array_view',
        terakan_bc6_cube_test,
        args : [
          terakan_bc6_cube_vertex_spv,
          terakan_bc6_array_fragment_spv,
          '--2d-array-view',
        ],
        depends : [
          libvulkan_terascale,
          _dev_icd,
          terakan_bc6_cube_vertex_spv,
          terakan_bc6_array_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 10,
      )

      terakan_depth_msaa_fetch_spv = custom_target(
        'terakan_depth_msaa_fetch.spv.h',
        input : 'tests/terakan_depth_msaa_fetch.comp',
        output : 'terakan_depth_msaa_fetch.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_depth_msaa_fetch.spv.h.d',
      )
      terakan_depth_msaa_fetch_test = executable(
        'terakan_depth_msaa_fetch_test',
        [
          'tests/terakan_depth_msaa_fetch_test.cpp',
          terakan_depth_msaa_fetch_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_depth_msaa_fetch',
        terakan_depth_msaa_fetch_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 15,
      )

      terakan_stencil_readback_test = executable(
        'terakan_stencil_readback_test',
        ['tests/terakan_stencil_readback_test.cpp'],
        dependencies : vulkan_loader_dep,
      )

      test(
        'terakan_stencil_readback',
        terakan_stencil_readback_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 10,
      )

      terakan_frame_chain_vertex_spv = custom_target(
        'terakan_frame_chain.vert.spv',
        input : 'tests/terakan_frame_chain.vert',
        output : 'terakan_frame_chain.vert.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_frame_chain.vert.spv.d',
      )
      terakan_frame_chain_fragment_spv = custom_target(
        'terakan_frame_chain.frag.spv',
        input : 'tests/terakan_frame_chain.frag',
        output : 'terakan_frame_chain.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_frame_chain.frag.spv.d',
      )
      terakan_frame_chain_compute_spv = custom_target(
        'terakan_frame_chain.comp.spv',
        input : 'tests/terakan_frame_chain.comp',
        output : 'terakan_frame_chain.comp.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_frame_chain.comp.spv.d',
      )
      terakan_frame_chain_depth_fragment_spv = custom_target(
        'terakan_frame_chain_depth.frag.spv',
        input : 'tests/terakan_frame_chain_depth.frag',
        output : 'terakan_frame_chain_depth.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_frame_chain_depth.frag.spv.d',
      )
      terakan_frame_chain_ssbo_compute_spv = custom_target(
        'terakan_frame_chain_ssbo.comp.spv',
        input : 'tests/terakan_frame_chain_ssbo.comp',
        output : 'terakan_frame_chain_ssbo.comp.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_frame_chain_ssbo.comp.spv.d',
      )
      terakan_frame_chain_ssbo_fragment_spv = custom_target(
        'terakan_frame_chain_ssbo.frag.spv',
        input : 'tests/terakan_frame_chain_ssbo.frag',
        output : 'terakan_frame_chain_ssbo.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_frame_chain_ssbo.frag.spv.d',
      )
      terakan_frame_chain_test = executable(
        'terakan_frame_chain_test',
        ['tests/terakan_frame_chain_test.cpp'],
        dependencies : vulkan_loader_dep,
      )

      test(
        'terakan_frame_chain',
        terakan_frame_chain_test,
        args : [terakan_frame_chain_vertex_spv, terakan_frame_chain_fragment_spv],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_frame_chain_vertex_spv, terakan_frame_chain_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )
      test(
        'terakan_frame_chain_compute',
        terakan_frame_chain_test,
        args : [
          terakan_frame_chain_vertex_spv, terakan_frame_chain_fragment_spv,
          terakan_frame_chain_compute_spv, '--compute',
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_frame_chain_vertex_spv, terakan_frame_chain_fragment_spv,
          terakan_frame_chain_compute_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      test(
        'terakan_frame_chain_depth',
        terakan_frame_chain_test,
        args : [
          terakan_frame_chain_vertex_spv, terakan_frame_chain_fragment_spv,
          terakan_frame_chain_depth_fragment_spv, '--depth',
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_frame_chain_vertex_spv, terakan_frame_chain_fragment_spv,
          terakan_frame_chain_depth_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      test(
        'terakan_frame_chain_ssbo',
        terakan_frame_chain_test,
        args : [
          terakan_frame_chain_vertex_spv, terakan_frame_chain_fragment_spv,
          terakan_frame_chain_ssbo_compute_spv, terakan_frame_chain_ssbo_fragment_spv,
          '--compute-ssbo',
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_frame_chain_vertex_spv, terakan_frame_chain_fragment_spv,
          terakan_frame_chain_ssbo_compute_spv, terakan_frame_chain_ssbo_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      test(
        'terakan_frame_chain_multi_size',
        terakan_frame_chain_test,
        args : [
          terakan_frame_chain_vertex_spv, terakan_frame_chain_fragment_spv, '--multi-size',
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_frame_chain_vertex_spv, terakan_frame_chain_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      test(
        'terakan_frame_chain_multi_pipeline',
        terakan_frame_chain_test,
        args : [
          terakan_frame_chain_vertex_spv, terakan_frame_chain_fragment_spv,
          terakan_frame_chain_compute_spv, '--compute-multi-pipeline',
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_frame_chain_vertex_spv, terakan_frame_chain_fragment_spv,
          terakan_frame_chain_compute_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_dynamic_rendering_vertex_spv = custom_target(
        'terakan_dynamic_rendering.vert.spv',
        input : 'tests/terakan_dynamic_rendering.vert',
        output : 'terakan_dynamic_rendering.vert.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_dynamic_rendering.vert.spv.d',
      )
      terakan_dynamic_rendering_fragment_spv = custom_target(
        'terakan_dynamic_rendering.frag.spv',
        input : 'tests/terakan_dynamic_rendering.frag',
        output : 'terakan_dynamic_rendering.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_dynamic_rendering.frag.spv.d',
      )
      terakan_dynamic_rendering_test = executable(
        'terakan_dynamic_rendering_test',
        ['tests/terakan_dynamic_rendering_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_dynamic_rendering',
        terakan_dynamic_rendering_test,
        args : [
          terakan_dynamic_rendering_vertex_spv, terakan_dynamic_rendering_fragment_spv,
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_dynamic_rendering_vertex_spv, terakan_dynamic_rendering_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_depth_resolve_subresource_test = executable(
        'terakan_depth_resolve_subresource_test',
        ['tests/terakan_depth_resolve_subresource_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_depth_resolve_subresource',
        terakan_depth_resolve_subresource_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_resolve_modes_vertex_spv = custom_target(
        'terakan_resolve_modes.vert.spv',
        input : 'tests/terakan_resolve_modes.vert',
        output : 'terakan_resolve_modes.vert.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_resolve_modes.vert.spv.d',
      )
      terakan_resolve_modes_fragment_spv = custom_target(
        'terakan_resolve_modes.frag.spv',
        input : 'tests/terakan_resolve_modes.frag',
        output : 'terakan_resolve_modes.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_resolve_modes.frag.spv.d',
      )
      terakan_resolve_modes_test = executable(
        'terakan_resolve_modes_test',
        ['tests/terakan_resolve_modes_test.cpp'],
        dependencies : vulkan_loader_dep,
      )

      # Reuses the resolve-modes shaders: a full-target quad plus an empty fragment shader is
      # exactly what a stencil-only STENCIL_REPLACE draw needs too.
      terakan_stencil_only_render_test = executable(
        'terakan_stencil_only_render_test',
        ['tests/terakan_stencil_only_render_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_stencil_only_render',
        terakan_stencil_only_render_test,
        args : [
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv,
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )
      test(
        'terakan_resolve_modes_2x',
        terakan_resolve_modes_test,
        args : [
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv, '2',
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )
      test(
        'terakan_resolve_modes_4x',
        terakan_resolve_modes_test,
        args : [
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv, '4',
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )
      test(
        'terakan_resolve_modes_8x',
        terakan_resolve_modes_test,
        args : [
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv, '8',
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_stencil_resolve_modes_test = executable(
        'terakan_stencil_resolve_modes_test',
        ['tests/terakan_stencil_resolve_modes_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_stencil_resolve_modes_2x',
        terakan_stencil_resolve_modes_test,
        args : [
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv, '2',
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )
      test(
        'terakan_stencil_resolve_modes_4x',
        terakan_stencil_resolve_modes_test,
        args : [
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv, '4',
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )
      test(
        'terakan_stencil_resolve_modes_8x',
        terakan_stencil_resolve_modes_test,
        args : [
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv, '8',
        ],
        depends : [
          libvulkan_terascale, _dev_icd,
          terakan_resolve_modes_vertex_spv, terakan_resolve_modes_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_blit_3d_test = executable(
        'terakan_blit_3d_test',
        ['tests/terakan_blit_3d_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_blit_3d',
        terakan_blit_3d_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_blit_format_matrix_test = executable(
        'terakan_blit_format_matrix_test',
        ['tests/terakan_blit_format_matrix_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_blit_format_matrix',
        terakan_blit_format_matrix_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_copy_image_subresource_test = executable(
        'terakan_copy_image_subresource_test',
        ['tests/terakan_copy_image_subresource_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_copy_image_subresource',
        terakan_copy_image_subresource_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_copy_image_multisample_test = executable(
        'terakan_copy_image_multisample_test',
        ['tests/terakan_copy_image_multisample_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_copy_image_multisample',
        terakan_copy_image_multisample_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_color_resolve_subresource_test = executable(
        'terakan_color_resolve_subresource_test',
        ['tests/terakan_color_resolve_subresource_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_color_resolve_subresource',
        terakan_color_resolve_subresource_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_stencil_fetch_spv = custom_target(
        'terakan_stencil_fetch.spv.h',
        input : 'tests/terakan_stencil_fetch.comp',
        output : 'terakan_stencil_fetch.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_stencil_fetch.spv.h.d',
      )
      terakan_stencil_fetch_test = executable(
        'terakan_stencil_fetch_test',
        ['tests/terakan_stencil_fetch_test.cpp', terakan_stencil_fetch_spv],
        dependencies : vulkan_loader_dep,
      )

      test(
        'terakan_stencil_fetch',
        terakan_stencil_fetch_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 10,
      )

      terakan_stencil_msaa_fetch_spv = custom_target(
        'terakan_stencil_msaa_fetch.spv.h',
        input : 'tests/terakan_stencil_msaa_fetch.comp',
        output : 'terakan_stencil_msaa_fetch.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_stencil_msaa_fetch.spv.h.d',
      )
      terakan_stencil_msaa_fetch_test = executable(
        'terakan_stencil_msaa_fetch_test',
        [
          'tests/terakan_stencil_msaa_fetch_test.cpp',
          terakan_stencil_msaa_fetch_spv,
        ],
        dependencies : vulkan_loader_dep,
      )

      test(
        'terakan_stencil_msaa_fetch',
        terakan_stencil_msaa_fetch_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 15,
      )

      terakan_color_msaa_fetch_spv = custom_target(
        'terakan_color_msaa_fetch.spv.h',
        input : 'tests/terakan_color_msaa_fetch.comp',
        output : 'terakan_color_msaa_fetch.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_color_msaa_fetch.spv.h.d',
      )
      terakan_color_msaa_write_vertex_spv = custom_target(
        'terakan_color_msaa_write.vert.spv.h',
        input : 'tests/terakan_color_msaa_write.vert',
        output : 'terakan_color_msaa_write.vert.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_color_msaa_write.vert.spv.h.d',
      )
      terakan_color_msaa_write_fragment_spv = custom_target(
        'terakan_color_msaa_write.frag.spv.h',
        input : 'tests/terakan_color_msaa_write.frag',
        output : 'terakan_color_msaa_write.frag.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_color_msaa_write.frag.spv.h.d',
      )
      terakan_color_msaa_fetch_test = executable(
        'terakan_color_msaa_fetch_test',
        [
          'tests/terakan_color_msaa_fetch_test.cpp',
          terakan_color_msaa_fetch_spv,
          terakan_color_msaa_write_vertex_spv,
          terakan_color_msaa_write_fragment_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      terakan_color_resolve_multivalued_test = executable(
        'terakan_color_resolve_multivalued_test',
        [
          'tests/terakan_color_resolve_multivalued_test.cpp',
          terakan_color_msaa_write_vertex_spv,
          terakan_color_msaa_write_fragment_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      foreach _color_resolve_samples : ['2', '4', '8']
        test(
          'terakan_color_resolve_multivalued_' + _color_resolve_samples + 'x',
          terakan_color_resolve_multivalued_test,
          args : ['--samples=' + _color_resolve_samples],
          depends : [libvulkan_terascale, _dev_icd],
          env : {
            'VK_DRIVER_FILES' : _dev_icd.full_path(),
            'VK_ICD_FILENAMES' : _dev_icd.full_path(),
            'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
          },
          timeout : 15,
        )
      endforeach

      foreach _color_msaa_samples : ['2', '4', '8']
        test(
          'terakan_color_msaa_fetch_' + _color_msaa_samples + 'x',
          terakan_color_msaa_fetch_test,
          args : ['--samples=' + _color_msaa_samples],
          depends : [libvulkan_terascale, _dev_icd],
          env : {
            'VK_DRIVER_FILES' : _dev_icd.full_path(),
            'VK_ICD_FILENAMES' : _dev_icd.full_path(),
            'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
          },
          timeout : 15,
        )
      endforeach

      terakan_extended_format_storage_image_spv = custom_target(
        'terakan_extended_format_storage_image.spv.h',
        input : 'tests/terakan_extended_format_storage_image.comp',
        output : 'terakan_extended_format_storage_image.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_extended_format_storage_image.spv.h.d',
      )
      terakan_extended_format_storage_image_load_spv = custom_target(
        'terakan_extended_format_storage_image_load.spv.h',
        input : 'tests/terakan_extended_format_storage_image_load.comp',
        output : 'terakan_extended_format_storage_image_load.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_extended_format_storage_image_load.spv.h.d',
      )
      terakan_extended_format_storage_image_test = executable(
        'terakan_extended_format_storage_image_test',
        [
          'tests/terakan_extended_format_storage_image_test.cpp',
          terakan_extended_format_storage_image_spv,
          terakan_extended_format_storage_image_load_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_extended_format_storage_image',
        terakan_extended_format_storage_image_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 15,
      )

      terakan_layered_depth_render_test = executable(
        'terakan_layered_depth_render_test',
        ['tests/terakan_layered_depth_render_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_layered_depth_render',
        terakan_layered_depth_render_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_shadow_compare_spv = custom_target(
        'terakan_shadow_compare.spv.h',
        input : 'tests/terakan_shadow_compare.comp',
        output : 'terakan_shadow_compare.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_shadow_compare.spv.h.d',
      )
      terakan_shadow_compare_test = executable(
        'terakan_shadow_compare_test',
        ['tests/terakan_shadow_compare_test.cpp', terakan_shadow_compare_spv],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_shadow_compare',
        terakan_shadow_compare_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_storage_format_spvs = []
      foreach _storage_format_class : ['float', 'uint', 'sint']
        terakan_storage_format_spvs += custom_target(
          'terakan_storage_format_' + _storage_format_class + '.spv.h',
          input : 'tests/terakan_storage_format_' + _storage_format_class + '.comp',
          output : 'terakan_storage_format_' + _storage_format_class + '.spv.h',
          command : [
            prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
            glslang_quiet, glslang_depfile,
          ],
          depfile : 'terakan_storage_format_' + _storage_format_class + '.spv.h.d',
        )
      endforeach
      terakan_storage_format_matrix_test = executable(
        'terakan_storage_format_matrix_test',
        ['tests/terakan_storage_format_matrix_test.cpp'] + terakan_storage_format_spvs,
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_storage_format_matrix',
        terakan_storage_format_matrix_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 60,
      )

      terakan_storage_image_atomic_spv = custom_target(
        'terakan_storage_image_atomic.spv.h',
        input : 'tests/terakan_storage_image_atomic.comp',
        output : 'terakan_storage_image_atomic.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_storage_image_atomic.spv.h.d',
      )
      terakan_storage_image_atomic_test = executable(
        'terakan_storage_image_atomic_test',
        [
          'tests/terakan_storage_image_atomic_test.cpp',
          terakan_storage_image_atomic_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_storage_image_atomic',
        terakan_storage_image_atomic_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_descriptor_set_shape_fragment_spv = custom_target(
        'terakan_descriptor_set_shape.frag.spv.h',
        input : 'tests/terakan_descriptor_set_shape.frag',
        output : 'terakan_descriptor_set_shape.frag.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_descriptor_set_shape.frag.spv.h.d',
      )
      terakan_descriptor_set_shape_test = executable(
        'terakan_descriptor_set_shape_test',
        [
          'tests/terakan_descriptor_set_shape_test.cpp',
          terakan_color_msaa_write_vertex_spv,
          terakan_descriptor_set_shape_fragment_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      # Reduced from a `descriptorset_random` case that lost ten of its sixty-four texels: a
      # dynamically indexed store inside a branch and an indexed read after it shared one
      # `SET_CF_IDX0` that sat inside the branch, so the read landed on the store's resource.
      test(
        'terakan_descriptor_set_shape',
        terakan_descriptor_set_shape_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_attachmentless_atomic_fragment_spv = custom_target(
        'terakan_attachmentless_atomic.frag.spv.h',
        input : 'tests/terakan_attachmentless_atomic.frag',
        output : 'terakan_attachmentless_atomic.frag.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_attachmentless_atomic.frag.spv.h.d',
      )
      terakan_attachmentless_atomic_test = executable(
        'terakan_attachmentless_atomic_test',
        [
          'tests/terakan_attachmentless_atomic_test.cpp',
          terakan_color_msaa_write_vertex_spv,
          terakan_attachmentless_atomic_fragment_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_attachmentless_atomic',
        terakan_attachmentless_atomic_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_query_sync_test = executable(
        'terakan_query_sync_test',
        [
          'tests/terakan_query_sync_test.cpp',
          terakan_color_msaa_write_vertex_spv,
          terakan_color_msaa_write_fragment_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_query_sync',
        terakan_query_sync_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 30,
      )

      terakan_depth_resolve_test = executable(
        'terakan_depth_resolve_test',
        ['tests/terakan_depth_resolve_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_depth_stencil_resolve',
        terakan_depth_resolve_test,
        args : ['--stencil'],
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 15,
      )
      test(
        'terakan_depth_resolve',
        terakan_depth_resolve_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 15,
      )

      terakan_depth_readback_test = executable(
        'terakan_depth_readback_test',
        ['tests/terakan_depth_readback_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_depth_readback',
        terakan_depth_readback_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 10,
      )

      terakan_attrib_divisor_vertex_spv = custom_target(
        'terakan_attrib_divisor.vert.spv',
        input : 'tests/terakan_attrib_divisor.vert',
        output : 'terakan_attrib_divisor.vert.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_attrib_divisor.vert.spv.d',
      )
      terakan_attrib_divisor_fragment_spv = custom_target(
        'terakan_attrib_divisor.frag.spv',
        input : 'tests/terakan_attrib_divisor.frag',
        output : 'terakan_attrib_divisor.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_attrib_divisor.frag.spv.d',
      )
      terakan_attrib_divisor_test = executable(
        'terakan_attrib_divisor_test',
        ['tests/terakan_attrib_divisor_test.c'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_attrib_divisor',
        terakan_attrib_divisor_test,
        args : [terakan_attrib_divisor_vertex_spv, terakan_attrib_divisor_fragment_spv],
        depends : [
          libvulkan_terascale,
          _dev_icd,
          terakan_attrib_divisor_vertex_spv,
          terakan_attrib_divisor_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_line_width_vertex_spv = custom_target(
        'terakan_line_width.vert.spv',
        input : 'tests/terakan_line_width.vert',
        output : 'terakan_line_width.vert.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_line_width.vert.spv.d',
      )
      terakan_line_width_fragment_spv = custom_target(
        'terakan_line_width.frag.spv',
        input : 'tests/terakan_line_width.frag',
        output : 'terakan_line_width.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_line_width.frag.spv.d',
      )
      terakan_line_width_test = executable(
        'terakan_line_width_test',
        ['tests/terakan_line_width_test.c'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_line_width',
        terakan_line_width_test,
        args : [terakan_line_width_vertex_spv, terakan_line_width_fragment_spv],
        depends : [
          libvulkan_terascale,
          _dev_icd,
          terakan_line_width_vertex_spv,
          terakan_line_width_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_depth_stencil_clear_multisample_spv = custom_target(
        'terakan_depth_stencil_clear_multisample.spv.h',
        input : 'tests/terakan_depth_stencil_clear_multisample.comp',
        output : 'terakan_depth_stencil_clear_multisample.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_depth_stencil_clear_multisample.spv.h.d',
      )
      terakan_depth_stencil_clear_multisample_test = executable(
        'terakan_depth_stencil_clear_multisample_test',
        [
          'tests/terakan_depth_stencil_clear_multisample_test.c',
          terakan_depth_stencil_clear_multisample_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_depth_stencil_clear_multisample',
        terakan_depth_stencil_clear_multisample_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 15,
      )

      terakan_sample_id_depth_vertex_spv = custom_target(
        'terakan_sample_id_depth.vert.spv',
        input : 'tests/terakan_sample_id_depth.vert',
        output : 'terakan_sample_id_depth.vert.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_sample_id_depth.vert.spv.d',
      )
      terakan_sample_id_depth_fragment_spv = custom_target(
        'terakan_sample_id_depth.frag.spv',
        input : 'tests/terakan_sample_id_depth.frag',
        output : 'terakan_sample_id_depth.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_sample_id_depth.frag.spv.d',
      )
      terakan_sample_id_depth_compute_spv = custom_target(
        'terakan_sample_id_depth.comp.spv',
        input : 'tests/terakan_sample_id_depth.comp',
        output : 'terakan_sample_id_depth.comp.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_sample_id_depth.comp.spv.d',
      )
      terakan_sample_id_depth_test = executable(
        'terakan_sample_id_depth_test',
        ['tests/terakan_sample_id_depth_test.c'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_sample_id_depth',
        terakan_sample_id_depth_test,
        args : [
          terakan_sample_id_depth_vertex_spv,
          terakan_sample_id_depth_fragment_spv,
          terakan_sample_id_depth_compute_spv,
        ],
        depends : [
          libvulkan_terascale,
          _dev_icd,
          terakan_sample_id_depth_vertex_spv,
          terakan_sample_id_depth_fragment_spv,
          terakan_sample_id_depth_compute_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_input_attachment_vertex_spv = custom_target(
        'terakan_input_attachment.vert.spv',
        input : 'tests/terakan_input_attachment.vert',
        output : 'terakan_input_attachment.vert.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_input_attachment.vert.spv.d',
      )
      terakan_input_attachment_fragment_spv = custom_target(
        'terakan_input_attachment.frag.spv',
        input : 'tests/terakan_input_attachment.frag',
        output : 'terakan_input_attachment.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_input_attachment.frag.spv.d',
      )
      terakan_input_attachment_test = executable(
        'terakan_input_attachment_test',
        ['tests/terakan_input_attachment_test.c'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_input_attachment',
        terakan_input_attachment_test,
        args : [terakan_input_attachment_vertex_spv, terakan_input_attachment_fragment_spv],
        depends : [
          libvulkan_terascale,
          _dev_icd,
          terakan_input_attachment_vertex_spv,
          terakan_input_attachment_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_clip_distance_vertex_spv = custom_target(
        'terakan_clip_distance.vert.spv',
        input : 'tests/terakan_clip_distance.vert',
        output : 'terakan_clip_distance.vert.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_clip_distance.vert.spv.d',
      )
      terakan_clip_distance_fragment_spv = custom_target(
        'terakan_clip_distance.frag.spv',
        input : 'tests/terakan_clip_distance.frag',
        output : 'terakan_clip_distance.frag.spv',
        command : [
          prog_glslang, '-V', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_clip_distance.frag.spv.d',
      )
      terakan_clip_distance_test = executable(
        'terakan_clip_distance_test',
        ['tests/terakan_clip_distance_test.cpp'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_clip_distance',
        terakan_clip_distance_test,
        args : [terakan_clip_distance_vertex_spv, terakan_clip_distance_fragment_spv],
        depends : [
          libvulkan_terascale,
          _dev_icd,
          terakan_clip_distance_vertex_spv,
          terakan_clip_distance_fragment_spv,
        ],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 10,
      )

      terakan_compute_loop_spv = custom_target(
        'terakan_compute_loop.spv.h',
        input : 'tests/terakan_compute_loop.comp',
        output : 'terakan_compute_loop.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_compute_loop.spv.h.d',
      )
      terakan_compute_loop_test = executable(
        'terakan_compute_loop_test',
        [
          'tests/terakan_compute_loop_test.c',
          terakan_compute_loop_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_compute_loop',
        terakan_compute_loop_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 5,
      )

      terakan_dynamic_offset_bounds_spv = custom_target(
        'terakan_dynamic_offset_bounds.spv.h',
        input : 'tests/terakan_dynamic_offset_bounds.comp',
        output : 'terakan_dynamic_offset_bounds.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_dynamic_offset_bounds.spv.h.d',
      )
      terakan_dynamic_offset_bounds_test = executable(
        'terakan_dynamic_offset_bounds_test',
        [
          'tests/terakan_dynamic_offset_bounds_test.c',
          terakan_dynamic_offset_bounds_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_dynamic_offset_bounds',
        terakan_dynamic_offset_bounds_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 5,
      )

      terakan_dynamic_uav_bounds_spv = custom_target(
        'terakan_dynamic_uav_bounds.spv.h',
        input : 'tests/terakan_dynamic_uav_bounds.comp',
        output : 'terakan_dynamic_uav_bounds.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_dynamic_uav_bounds.spv.h.d',
      )
      terakan_dynamic_uav_bounds_test = executable(
        'terakan_dynamic_uav_bounds_test',
        [
          'tests/terakan_dynamic_uav_bounds_test.c',
          terakan_dynamic_uav_bounds_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_dynamic_uav_bounds',
        terakan_dynamic_uav_bounds_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_image_bounds_spv = custom_target(
        'terakan_image_bounds.spv.h',
        input : 'tests/terakan_image_bounds.comp',
        output : 'terakan_image_bounds.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_image_bounds.spv.h.d',
      )
      terakan_image_bounds_test = executable(
        'terakan_image_bounds_test',
        [
          'tests/terakan_image_bounds_test.c',
          terakan_image_bounds_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_image_bounds',
        terakan_image_bounds_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_indexed_image_array_spv = custom_target(
        'terakan_indexed_image_array.spv.h',
        input : 'tests/terakan_indexed_image_array.comp',
        output : 'terakan_indexed_image_array.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_indexed_image_array.spv.h.d',
      )
      terakan_indexed_image_array_test = executable(
        'terakan_indexed_image_array_test',
        [
          'tests/terakan_indexed_image_array_test.c',
          terakan_indexed_image_array_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_indexed_image_array',
        terakan_indexed_image_array_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_image_gather_spv = custom_target(
        'terakan_image_gather.spv.h',
        input : 'tests/terakan_image_gather.comp',
        output : 'terakan_image_gather.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_image_gather.spv.h.d',
      )
      terakan_image_gather_test = executable(
        'terakan_image_gather_test',
        [
          'tests/terakan_image_gather_test.c',
          terakan_image_gather_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_image_gather',
        terakan_image_gather_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_uniform_texel_buffer_spv = custom_target(
        'terakan_uniform_texel_buffer.spv.h',
        input : 'tests/terakan_uniform_texel_buffer.comp',
        output : 'terakan_uniform_texel_buffer.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_uniform_texel_buffer.spv.h.d',
      )
      terakan_uniform_texel_buffer_test = executable(
        'terakan_uniform_texel_buffer_test',
        [
          'tests/terakan_uniform_texel_buffer_test.c',
          terakan_uniform_texel_buffer_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_uniform_texel_buffer',
        terakan_uniform_texel_buffer_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_storage_image_base_slice_spv = custom_target(
        'terakan_storage_image_base_slice.spv.h',
        input : 'tests/terakan_storage_image_base_slice.comp',
        output : 'terakan_storage_image_base_slice.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_storage_image_base_slice.spv.h.d',
      )
      terakan_storage_image_base_slice_test = executable(
        'terakan_storage_image_base_slice_test',
        [
          'tests/terakan_storage_image_base_slice_test.c',
          terakan_storage_image_base_slice_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_storage_image_base_slice',
        terakan_storage_image_base_slice_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_render_area_mip_test = executable(
        'terakan_render_area_mip_test',
        ['tests/terakan_render_area_mip_test.c'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_render_area_mip',
        terakan_render_area_mip_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_depth_stencil_clear_ranges_test = executable(
        'terakan_depth_stencil_clear_ranges_test',
        ['tests/terakan_depth_stencil_clear_ranges_test.c'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_depth_stencil_clear_ranges',
        terakan_depth_stencil_clear_ranges_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_copy_expand_3x_test = executable(
        'terakan_copy_expand_3x_test',
        ['tests/terakan_copy_expand_3x_test.c'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_copy_expand_3x',
        terakan_copy_expand_3x_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_clear_expand_3x_test = executable(
        'terakan_clear_expand_3x_test',
        ['tests/terakan_clear_expand_3x_test.c'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_clear_expand_3x',
        terakan_clear_expand_3x_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_blit_3d_depth_filter_test = executable(
        'terakan_blit_3d_depth_filter_test',
        ['tests/terakan_blit_3d_depth_filter_test.c'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_blit_3d_depth_filter',
        terakan_blit_3d_depth_filter_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_texture_array_spv = custom_target(
        'terakan_texture_array.spv.h',
        input : 'tests/terakan_texture_array.comp',
        output : 'terakan_texture_array.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_texture_array.spv.h.d',
      )
      terakan_texture_array_test = executable(
        'terakan_texture_array_test',
        [
          'tests/terakan_texture_array_test.c',
          terakan_texture_array_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_texture_array',
        terakan_texture_array_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_blit_mixed_regions_test = executable(
        'terakan_blit_mixed_regions_test',
        ['tests/terakan_blit_mixed_regions_test.c'],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_blit_mixed_regions',
        terakan_blit_mixed_regions_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 20,
      )

      terakan_vertex_fetch_bounds_vertex_spv = custom_target(
        'terakan_vertex_fetch_bounds.vert.spv.h',
        input : 'tests/terakan_vertex_fetch_bounds.vert',
        output : 'terakan_vertex_fetch_bounds.vert.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_vertex_fetch_bounds.vert.spv.h.d',
      )
      terakan_vertex_fetch_bounds_fragment_spv = custom_target(
        'terakan_vertex_fetch_bounds.frag.spv.h',
        input : 'tests/terakan_vertex_fetch_bounds.frag',
        output : 'terakan_vertex_fetch_bounds.frag.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_vertex_fetch_bounds.frag.spv.h.d',
      )
      # Deliberately not wired into test(): this is a characterization tool whose output is a table
      # to read, not a pass/fail result. See the comment at the top of its source.
      terakan_vertex_fetch_bounds_probe = executable(
        'terakan_vertex_fetch_bounds_probe',
        [
          'tests/terakan_vertex_fetch_bounds_probe.c',
          terakan_vertex_fetch_bounds_vertex_spv,
          terakan_vertex_fetch_bounds_fragment_spv,
        ],
        dependencies : vulkan_loader_dep,
      )

      terakan_application_constant_vertex_spv = custom_target(
        'terakan_application_constant.vert.spv.h',
        input : 'tests/terakan_application_constant.vert',
        output : 'terakan_application_constant.vert.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_application_constant.vert.spv.h.d',
      )
      terakan_application_constant_fragment_spv = custom_target(
        'terakan_application_constant.frag.spv.h',
        input : 'tests/terakan_application_constant.frag',
        output : 'terakan_application_constant.frag.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_application_constant.frag.spv.h.d',
      )

      # Deliberately not wired into test(): a characterization tool whose output is a table to
      # read, not a pass/fail result. See the comment at the top of its source.
      terakan_depth_clear_extent_probe = executable(
        'terakan_depth_clear_extent_probe',
        ['tests/terakan_depth_clear_extent_probe.c'],
        dependencies : vulkan_loader_dep,
      )

      terakan_border_color_swizzle_spv = custom_target(
        'terakan_border_color_swizzle.spv.h',
        input : 'tests/terakan_border_color_swizzle.comp',
        output : 'terakan_border_color_swizzle.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_border_color_swizzle.spv.h.d',
      )
      terakan_vertex_ssbo_read_vert_spv = custom_target(
        'terakan_vertex_ssbo_read.vert.spv.h',
        input : 'tests/terakan_vertex_ssbo_read.vert',
        output : 'terakan_vertex_ssbo_read.vert.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_vertex_ssbo_read.vert.spv.h.d',
      )
      terakan_vertex_ssbo_read_frag_spv = custom_target(
        'terakan_vertex_ssbo_read.frag.spv.h',
        input : 'tests/terakan_vertex_ssbo_read.frag',
        output : 'terakan_vertex_ssbo_read.frag.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_vertex_ssbo_read.frag.spv.h.d',
      )
      terakan_vertex_ssbo_read_probe = executable(
        'terakan_vertex_ssbo_read_probe',
        [
          'tests/terakan_vertex_ssbo_read_probe.c',
          terakan_vertex_ssbo_read_vert_spv,
          terakan_vertex_ssbo_read_frag_spv,
        ],
        dependencies : vulkan_loader_dep,
      )

      terakan_cube_gather_spv = custom_target(
        'terakan_cube_gather.spv.h',
        input : 'tests/terakan_cube_gather.comp',
        output : 'terakan_cube_gather.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_cube_gather.spv.h.d',
      )
      terakan_cube_gather_probe = executable(
        'terakan_cube_gather_probe',
        [
          'tests/terakan_cube_gather_probe.c',
          terakan_cube_gather_spv,
        ],
        dependencies : vulkan_loader_dep,
      )

      terakan_border_color_swizzle_probe = executable(
        'terakan_border_color_swizzle_probe',
        [
          'tests/terakan_border_color_swizzle_probe.c',
          terakan_border_color_swizzle_spv,
        ],
        dependencies : vulkan_loader_dep,
      )

      terakan_vertex_format_2_10_10_10_vertex_spv = custom_target(
        'terakan_vertex_format_2_10_10_10.vert.spv.h',
        input : 'tests/terakan_vertex_format_2_10_10_10.vert',
        output : 'terakan_vertex_format_2_10_10_10.vert.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_vertex_format_2_10_10_10.vert.spv.h.d',
      )
      terakan_vertex_format_2_10_10_10_fragment_spv = custom_target(
        'terakan_vertex_format_2_10_10_10.frag.spv.h',
        input : 'tests/terakan_vertex_format_2_10_10_10.frag',
        output : 'terakan_vertex_format_2_10_10_10.frag.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_vertex_format_2_10_10_10.frag.spv.h.d',
      )
      # Deliberately not wired into test(): a characterization tool whose output is a table to
      # read, not a pass/fail result. See the comment at the top of its source.
      terakan_vertex_format_2_10_10_10_probe = executable(
        'terakan_vertex_format_2_10_10_10_probe',
        [
          'tests/terakan_vertex_format_2_10_10_10_probe.c',
          terakan_vertex_format_2_10_10_10_vertex_spv,
          terakan_vertex_format_2_10_10_10_fragment_spv,
        ],
        dependencies : vulkan_loader_dep,
      )

      terakan_formatless_image_store_spv = custom_target(
        'terakan_formatless_image_store.spv.h',
        input : 'tests/terakan_formatless_image_store.comp',
        output : 'terakan_formatless_image_store.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_formatless_image_store.spv.h.d',
      )
      terakan_formatless_image_load_spv = custom_target(
        'terakan_formatless_image_load.spv.h',
        input : 'tests/terakan_formatless_image_load.comp',
        output : 'terakan_formatless_image_load.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_formatless_image_load.spv.h.d',
      )
      terakan_rgba16f_image_store_spv = custom_target(
        'terakan_rgba16f_image_store.spv.h',
        input : 'tests/terakan_rgba16f_image_store.comp',
        output : 'terakan_rgba16f_image_store.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_rgba16f_image_store.spv.h.d',
      )
      terakan_rgba16f_image_load_spv = custom_target(
        'terakan_rgba16f_image_load.spv.h',
        input : 'tests/terakan_rgba16f_image_load.comp',
        output : 'terakan_rgba16f_image_load.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_rgba16f_image_load.spv.h.d',
      )
      terakan_sampled_to_rat_spv = custom_target(
        'terakan_sampled_to_rat.spv.h',
        input : 'tests/terakan_sampled_to_rat.comp',
        output : 'terakan_sampled_to_rat.spv.h',
        command : [
          prog_glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@',
          glslang_quiet, glslang_depfile,
        ],
        depfile : 'terakan_sampled_to_rat.spv.h.d',
      )
      terakan_formatless_image_store_test = executable(
        'terakan_formatless_image_store_test',
        [
          'tests/terakan_formatless_image_store_test.c',
          terakan_formatless_image_store_spv,
          terakan_formatless_image_load_spv,
          terakan_rgba16f_image_store_spv,
          terakan_rgba16f_image_load_spv,
          terakan_sampled_to_rat_spv,
        ],
        dependencies : vulkan_loader_dep,
      )
      test(
        'terakan_formatless_image_store',
        terakan_formatless_image_store_test,
        depends : [libvulkan_terascale, _dev_icd],
        env : {
          'VK_DRIVER_FILES' : _dev_icd.full_path(),
          'VK_ICD_FILENAMES' : _dev_icd.full_path(),
          'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
        },
        timeout : 5,
      )
    endif

    terakan_physical_device_properties_test = executable(
      'terakan_physical_device_properties_test',
      'tests/terakan_physical_device_properties_test.c',
      dependencies : vulkan_loader_dep,
    )
    executable(
      'terakan_shader_corpus_test',
      'tests/terakan_shader_corpus_test.cpp',
      dependencies : vulkan_loader_dep,
      cpp_args : cpp_msvc_compat_args,
    )
    test(
      'terakan_physical_device_properties',
      terakan_physical_device_properties_test,
      depends : [libvulkan_terascale, _dev_icd],
      env : {
        'VK_DRIVER_FILES' : _dev_icd.full_path(),
        'VK_ICD_FILENAMES' : _dev_icd.full_path(),
        'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
      },
    )

    terakan_terascale_1_enumeration_test = executable(
      'terakan_terascale_1_enumeration_test',
      [
        'tests/terakan_terascale_1_enumeration_test.c',
        terakan_vertex_fetch_bounds_vertex_spv,
        terakan_vertex_fetch_bounds_fragment_spv,
        terakan_application_constant_vertex_spv,
        terakan_application_constant_fragment_spv,
      ],
      dependencies : vulkan_loader_dep,
    )
    test(
      'terakan_terascale_1_enumeration',
      terakan_terascale_1_enumeration_test,
      depends : [libvulkan_terascale, _dev_icd],
      env : {
        'VK_DRIVER_FILES' : _dev_icd.full_path(),
        'VK_ICD_FILENAMES' : _dev_icd.full_path(),
        'VK_LOADER_LAYERS_DISABLE' : '~implicit~',
      },
    )
  endif
endif
