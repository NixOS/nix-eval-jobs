{ pkgs, lib, ... }:
let
  supportsDeno =
    lib.meta.availableOn pkgs.stdenv.buildPlatform pkgs.deno
    && (builtins.tryEval pkgs.deno.outPath).success;
in
{
  # Used to find the project root
  projectRootFile = "flake.lock";

  programs.deno.enable = supportsDeno;
  programs.yamlfmt.enable = true;

  programs.clang-format.enable = true;
  programs.clang-format.package = pkgs.llvmPackages_latest.clang-tools;

  programs.deadnix.enable = true;
  programs.nixfmt.enable = true;
  programs.mypy = {
    enable = true;
    directories = {
      "tests" = {
        extraPythonPackages = [ pkgs.python3Packages.pytest ];
      };
    };
  };
  programs.ruff.format = true;
  programs.ruff.check = true;
}
