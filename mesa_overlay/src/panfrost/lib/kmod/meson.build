# Copyright © 2023 Collabora
# SPDX-License-Identifier: MIT

libpankmod_lib_files = files(
  'pan_kmod.c',
  'panfrost_kmod.c',
  'panthor_kmod.c',
)

panthor_kbase_adapter_proj = subproject('panthor_kbase_adapter')
panthor_kbase_adapter_dep = panthor_kbase_adapter_proj.get_variable('panthor_kbase_adapter_dep')

libpankmod_lib = static_library(
  'pankmod_lib',
  [libpankmod_lib_files],
  include_directories : [inc_include, inc_src, inc_panfrost],
  c_args : [no_override_init_args, '-DHAVE_PANTHOR_KBASE_UAPI_ADAPTER=1'],
  gnu_symbol_visibility : 'hidden',
  dependencies: [dep_libdrm, idep_mesautil, idep_pan_packers, libpanfrost_model_dep, panthor_kbase_adapter_dep],
  build_by_default : false,
)

libpankmod_dep = declare_dependency(
  include_directories: [inc_include, inc_src],
  dependencies: [dep_libdrm, panthor_kbase_adapter_dep],
)
