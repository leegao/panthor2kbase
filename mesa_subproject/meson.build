# SPDX-License-Identifier: MIT
# Self-contained Mesa subproject: src/panfrost/uapi_adapter/meson.build
# Can be built either as an LD_PRELOAD shared library or linked directly into libpankmod_lib

libpanthor_kbase_adapter = shared_library(
  'panthor_kbase_adapter',
  files(
      '../src/adapter/panthor_kbase_adapter.c',
      '../src/adapter/log.c',
    ),
  include_directories : [inc_include, include_directories('../include')],
  dependencies : [dep_dl, dep_thread],
  install : true,
)
