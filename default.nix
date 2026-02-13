{
  stdenv,
  lib,
  nixComponents,
  pkgs,
}:

let
  revision = "1";
in
stdenv.mkDerivation {
  pname = "nix-eval-jobs";
  version = "${lib.versions.majorMinor nixComponents.nix-cli.version}.${revision}";
  src = lib.fileset.toSource {
    fileset = lib.fileset.unions [
      ./.clang-tidy
      ./meson.build
      ./src/meson.build
      (lib.fileset.fileFilter (file: file.hasExt "cc") ./src)
      (lib.fileset.fileFilter (file: file.hasExt "hh") ./src)
    ];
    root = ./.;
  };
  buildInputs = with pkgs; [
    nlohmann_json
    curl
    nixComponents.nix-store
    nixComponents.nix-fetchers
    nixComponents.nix-expr
    nixComponents.nix-flake
    nixComponents.nix-main
    nixComponents.nix-cmd
  ];
  nativeBuildInputs =
    with pkgs;
    [
      meson
      pkg-config
      ninja
      # nlohmann_json can be only discovered via cmake files
      cmake
    ]
    ++ lib.optional stdenv.cc.isClang (lib.hiPrio pkgs.llvmPackages.clang-tools);

  passthru = {
    inherit nixComponents;
  };

  meta = {
    description = "Hydra's builtin hydra-eval-jobs as a standalone";
    homepage = "https://github.com/nix-community/nix-eval-jobs";
    license = lib.licenses.gpl3;
    maintainers = with lib.maintainers; [
      adisbladis
      mic92
    ];
    platforms = lib.platforms.unix;
  };
}
