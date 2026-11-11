# SPDX-License-Identifier: MIT
# Self-contained Mesa subproject: subprojects/panthor_kbase_adapter/meson.build
# Linked into src/panfrost/lib/kmod/meson.build via subproject('panthor_kbase_adapter').

project(
  'panthor_kbase_adapter',
  ['c', 'cpp'],
  version : '1.0.0',
  default_options : ['c_std=c11', 'cpp_std=c++17'],
)

cc = meson.get_compiler('c')
dep_dl = cc.find_library('dl', required : false)
dep_thread = dependency('threads')
dep_mali_emu = cc.find_library(
  'mali_kbase_emu',
  dirs : [
    meson.current_source_dir() / '../../build-x86-64',
    meson.current_source_dir() / '../../build-android',
  ],
  required : false,
)

inc_adapter = include_directories('include', 'src/adapter')

libpanthor_kbase_adapter = static_library(
  'panthor_kbase_adapter',
  files(
    'src/adapter/panthor_kbase_adapter.cpp',
    'src/adapter/mali_kbase_caps.cpp',
    'src/adapter/panthor_drm_capture_replay.cpp',
    'src/adapter/log.cpp',
  ),
  include_directories : inc_adapter,
  dependencies : [dep_dl, dep_thread, dep_mali_emu],
  pic : true,
)

panthor_kbase_adapter_dep = declare_dependency(
  link_whole : libpanthor_kbase_adapter,
  dependencies : [dep_dl, dep_thread, dep_mali_emu],
)
