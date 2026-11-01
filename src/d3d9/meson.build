d3d9_version = vcs_tag(
  command: vcs_version_command,
  input:   'version.rc.in',
  output:  'version.rc')

d3d9_res = wrc_generator.process(d3d9_version)

d3d9_shaders = files([
  'shaders/d3d9_convert_yuy2_uyvy.comp',
  'shaders/d3d9_convert_l6v5u5.comp',
  'shaders/d3d9_convert_x8l8v8u8.comp',
  'shaders/d3d9_convert_a2w10v10u10.comp',
  'shaders/d3d9_convert_w11v11u10.comp',
  'shaders/d3d9_convert_nv12.comp',
  'shaders/d3d9_convert_yv12.comp'
])

d3d9_src = [
  'd3d9_adapter.cpp',
  'd3d9_adapter.h',
  'd3d9_buffer.cpp',
  'd3d9_buffer.h',
  'd3d9_caps.h',
  'd3d9_common_buffer.cpp',
  'd3d9_common_buffer.h',
  'd3d9_common_texture.cpp',
  'd3d9_common_texture.h',
  'd3d9_constant_layout.h',
  'd3d9_constant_set.h',
  'd3d9_cursor.cpp', 
  'd3d9_cursor.h',
  'd3d9_device.cpp',
  'd3d9_device.h',
  'd3d9_device_child.h',
  'd3d9_fixed_function.cpp',
  'd3d9_fixed_function.h',
  'd3d9_format.cpp',
  'd3d9_format.h',
  'd3d9_format_helpers.cpp',
  'd3d9_format_helpers.h',
  'd3d9_hud.cpp',
  'd3d9_hud.h',
  'd3d9_include.h',
  'd3d9_initializer.cpp',
  'd3d9_initializer.h',
  'd3d9_interface.cpp',
  'd3d9_interface.h',
  'd3d9_main.cpp',
  'd3d9_monitor.cpp',
  'd3d9_monitor.h',
  'd3d9_multithread.cpp',
  'd3d9_multithread.h',
  'd3d9_names.cpp',
  'd3d9_names.h',
  'd3d9_options.cpp',
  'd3d9_options.h',
  'd3d9_query.cpp',
  'd3d9_query.h',
  'd3d9_resource.h',
  'd3d9_sampler.cpp',
  'd3d9_sampler.h',
  'd3d9_shader.cpp',
  'd3d9_shader.h',
  'd3d9_shader_permutations.h',
  'd3d9_shader_validator.h',
  'd3d9_shared_res.cpp',
  'd3d9_shared_res.h',
  'd3d9_spec_constants.h',
  'd3d9_state.cpp',
  'd3d9_state.h',
  'd3d9_stateblock.cpp',
  'd3d9_stateblock.h',
  'd3d9_subresource.h',
  'd3d9_surface.cpp',
  'd3d9_surface.h',
  'd3d9_swapchain.cpp', 
  'd3d9_swapchain.h',
  'd3d9_swvp_emu.cpp',
  'd3d9_swvp_emu.h',
  'd3d9_texture.cpp',
  'd3d9_texture.h',
  'd3d9_util.cpp',
  'd3d9_util.h',
  'd3d9_vertex_declaration.cpp',
  'd3d9_vertex_declaration.h',
  'd3d9_volume.cpp',
  'd3d9_volume.h',
  'd3d9_rtx.cpp',
  'd3d9_rtx.h',
  'd3d9_rtx_utils.cpp',
  'd3d9_rtx_utils.h',
  'd3d9_rtx_geometry.cpp',
  'd3d9_swapchain_external.cpp',
]


# The unified module. D3D9 (which is how Remix gets its device), D3D11, D3D10
# and DXGI all compile into ONE DLL, because each frontend built separately
# would statically link its own copy of libdxvk -- its own RtxOptions, its own
# SceneManager, its own DxvkInstance -- and two such copies can never share a
# DxvkDevice. d3d11.dll and dxgi.dll ship as export forwarders to this.
#
# Note dxgi_src, not dxgi_common_src: the latter is a three-file subset of the
# former, and including both would duplicate every symbol in it.
unified_src = d3d9_src + frontend_d3d9_record_src + frontend_api_src
unified_args = []
if get_option('enable_d3d9_frontend')
  unified_src += frontend_d3d9_src
else
  unified_args += [ '-DREMIX_D3D9_FRONTEND=0' ]
endif
unified_deps = [ dxso_dep, dxvk_dep, util_dep, sentry_dep ]
unified_shaders = [ glsl_generator.process(d3d9_shaders) ]
if get_option('enable_d3d11')
  unified_src += dxgi_src + d3d11_src + d3d10_src + frontend_api_d3d11_src
  unified_deps += [ lib_dxgi, dxbc_dep ]
  unified_shaders += [ d3d11_compiled_shaders ]
endif

d3d9_dll = shared_library('d3d9', unified_src, dxvk_version, unified_shaders, d3d9_res,
  name_prefix         : '',

  link_with           : [ util_lib ],
  link_args           : [ usd_plugin_delay_load_args, fsr_delay_load_args ],
  cpp_args            : unified_args,
  dependencies        : unified_deps,
  include_directories : [ dxvk_include_path, dxvk_shader_include_path, usd_include_paths, remix_api_include_path, include_directories('../dxvk'), include_directories('../dxgi') ],
  objects             : not dxvk_is_msvc ? 'd3d9' + def_spec_ext : [],
  vs_module_defs      : 'd3d9'+def_spec_ext,
  override_options    : ['cpp_std='+dxvk_cpp_std],
  install: true
  )

# Copy the FSR loader and the effect DLLs it loads to the d3d9 output directory for runtime loading.
foreach fsr_dll_name : [ 'amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll', 'amd_fidelityfx_framegeneration_vk.dll' ]
  custom_target('copy_' + fsr_dll_name.underscorify(),
    output: fsr_dll_name,
    input: join_paths(global_src_root_norm, 'submodules/FidelityFX-SDK/Kits/FidelityFX/signedbin', fsr_dll_name),
    command: [python_interpreter, '-c', 'import shutil; import sys; shutil.copy(sys.argv[1], sys.argv[2])', '@INPUT@', '@OUTPUT@'],
    build_by_default: true,
    install: true,
    install_dir: get_option('bindir')
  )
endforeach

d3d9_dep = declare_dependency(
  link_with           : [ d3d9_dll ],
  include_directories : [ dxvk_include_path ])
 