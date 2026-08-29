common_src = [
  'eval-args.cc',
  'drv.cc',
  'response.cc',
  'buffered-io.cc',
  'crash-handler.cc',
  'constituents.cc',
  'worker.cc',
  'output-stream-lock.cc',
  'cache-status-resolver.cc',
  'daemon-settings.cc',
  'rss.cc',
  'proc.cc',
]

common_deps = [
  threads_dep,
  nlohmann_json_dep,
  boost_dep,
  libcurl_dep,

  nix_store_dep,
  nix_fetchers_dep,
  nix_expr_dep,
  nix_flake_dep,
  nix_main_dep,
  nix_cmd_dep,
]

nix_eval_jobs_lib = static_library(
  'nix-eval-jobs-lib',
  common_src,
  dependencies: common_deps,
)

nix_eval_jobs_dep = declare_dependency(
  link_with: nix_eval_jobs_lib,
  dependencies: common_deps,
  include_directories: include_directories('.'),
)

executable(
  'nix-eval-jobs',
  'nix-eval-jobs.cc',
  dependencies: [nix_eval_jobs_dep],
  install: true,
)
