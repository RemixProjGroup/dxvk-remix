RemixAPI_Flow_exe = executable(
  'RemixAPI_Flow',
  files('./remixapi_flow_example.cpp'),
  include_directories : [ remix_api_include_path ],
  override_options    : ['cpp_std=c++20']
)

RemixAPI_Flow_exepath = join_paths(meson.current_build_dir(), RemixAPI_Flow_exe.name() + '.exe')
