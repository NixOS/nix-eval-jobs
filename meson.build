project(
  'nix-eval-jobs',
  'cpp',
  version: '0.1.6',
  license: 'GPL-3.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=3',
    'unity=on',
    'unity_size=999'
  ]
)

cpp = meson.get_compiler('cpp')
# Unity builds merge translation units, so coroutine frames embed
# lambda types with no linkage; harmless, but GCC warns about it.
add_project_arguments(
  cpp.get_supported_arguments('-Wno-subobject-linkage'),
  language: 'cpp',
)

threads_dep = dependency('threads', required: true)
nlohmann_json_dep = dependency('nlohmann_json', required: true)
boost_dep = dependency('boost', required: true)
libcurl_dep = dependency('libcurl', required: true)
# Same allocator as the nix binary. Listed first so it overrides malloc.
mimalloc_dep = dependency('mimalloc', required: true)

nix_store_dep = dependency('nix-store', required: true)
nix_fetchers_dep = dependency('nix-fetchers', required: true)
nix_expr_dep = dependency('nix-expr', required: true)
nix_flake_dep = dependency('nix-flake', required: true)
nix_main_dep = dependency('nix-main', required: true)
nix_cmd_dep = dependency('nix-cmd', required: true)

subdir('src')

gtest_dep = dependency('gtest', main: true, required: false)
if gtest_dep.found()
  subdir('tests-unit')
endif
