# Package scope with Nix's own dependency overrides and components, so
# nix-eval-jobs links against exactly the libraries Nix was built with.
# Everything the flake exposes is a member of this scope.
{
  pkgs,
  lib,
  nix,
  treefmt-nix,
  self,
}:
let
  nixDependencies = lib.makeScope pkgs.newScope (
    scope:
    let
      super = import (nix + "/packaging/dependencies.nix") {
        inherit pkgs;
        inherit (pkgs) stdenv;
        inputs = { };
      } scope;
    in
    super
    // {
      # `dependencies.nix` patches boost for
      # https://github.com/NixOS/nix/issues/16174, but the nixpkgs we
      # track already backports that commit (boostorg/context@5883212),
      # so cut the extra patches.
      #
      # FIXME avoid messing up in Nix itself instead.
      boost = super.boost.overrideAttrs (_: {
        patches = pkgs.boost.patches;
      });

      nixComponents = lib.makeScope scope.newScope (
        import (nix + "/packaging/components.nix") {
          officialRelease = true;
          inherit lib pkgs;
          src = nix;
          maintainers = [ ];
        }
      );

      nix-eval-jobs = scope.callPackage ../default.nix { };
      clangStdenv-nix-eval-jobs = scope.callPackage ../default.nix { stdenv = pkgs.clangStdenv; };

      shell = scope.callPackage ../shell.nix { };
      clangShell = scope.callPackage ../shell.nix { stdenv = pkgs.clangStdenv; };

      treefmt = treefmt-nix.lib.evalModule pkgs ./treefmt.nix;
      treefmtCheck = scope.treefmt.config.build.check self;
      scheduler-spec = scope.callPackage ./scheduler-spec.nix { };
      functional-tests = scope.callPackage ./functional-tests.nix { };
      clang-tidy-fix = scope.callPackage ./clang-tidy.nix { };
    }
  );
in
nixDependencies
