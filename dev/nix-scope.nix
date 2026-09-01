# Package scope with Nix's own dependency overrides and components, so
# nix-eval-jobs links against exactly the libraries Nix was built with.
{
  pkgs,
  lib,
  nix,
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
    }
  );
in
nixDependencies
