{
  description = "Hydra's builtin hydra-eval-jobs as a standalone";

  inputs.nixpkgs.url = "https://nixos.org/channels/nixpkgs-unstable/nixexprs.tar.xz";
  inputs.nix = {
    url = "github:NixOS/nix/2.35-maintenance";
    # We want to control the deps precisely
    flake = false;
  };
  inputs.treefmt-nix.url = "github:numtide/treefmt-nix";
  inputs.treefmt-nix.inputs.nixpkgs.follows = "nixpkgs";

  outputs =
    {
      self,
      nixpkgs,
      nix,
      treefmt-nix,
    }:
    let
      inherit (nixpkgs) lib;
      systems = [
        "aarch64-linux"
        "riscv64-linux"
        "x86_64-linux"

        "aarch64-darwin"
      ];
      eachSystem =
        f:
        lib.genAttrs systems (
          system:
          let
            pkgs = nixpkgs.legacyPackages.${system};
          in
          f {
            inherit system pkgs;
            scope = pkgs.callPackage ./dev/nix-scope.nix { inherit nix; };
            treefmt = treefmt-nix.lib.evalModule pkgs ./dev/treefmt.nix;
          }
        );
    in
    {
      packages = eachSystem (
        { scope, pkgs, ... }:
        rec {
          nix-eval-jobs = scope.callPackage ./default.nix { };
          clangStdenv-nix-eval-jobs = scope.callPackage ./default.nix { stdenv = pkgs.clangStdenv; };
          default = nix-eval-jobs;
        }
      );

      devShells = eachSystem (
        { scope, pkgs, ... }:
        {
          default = scope.callPackage ./shell.nix { };
          clang = scope.callPackage ./shell.nix { stdenv = pkgs.clangStdenv; };
        }
      );

      formatter = eachSystem ({ treefmt, ... }: treefmt.config.build.wrapper);

      checks = eachSystem (
        {
          system,
          pkgs,
          treefmt,
          ...
        }:
        let
          packages = self.packages.${system};
          callPackage = lib.callPackageWith (pkgs // { inherit (packages) nix-eval-jobs; });
        in
        builtins.removeAttrs packages [ "default" ]
        // {
          shell = self.devShells.${system}.default;
          scheduler-spec = callPackage ./dev/scheduler-spec.nix { };
          functional-tests = callPackage ./dev/functional-tests.nix { };
          clang-tidy-fix = callPackage ./dev/clang-tidy.nix { };
        }
        // lib.optionalAttrs (system != "riscv64-linux") {
          treefmt = treefmt.config.build.check self;
        }
      );
    };
}
