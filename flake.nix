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
          f system (
            nixpkgs.legacyPackages.${system}.callPackage ./dev/nix-scope.nix {
              inherit nix treefmt-nix self;
            }
          )
        );
    in
    {
      packages = eachSystem (
        _system: scope: {
          inherit (scope) nix-eval-jobs clangStdenv-nix-eval-jobs;
          default = scope.nix-eval-jobs;
        }
      );

      devShells = eachSystem (
        _system: scope: {
          default = scope.shell;
          clang = scope.clangShell;
        }
      );

      formatter = eachSystem (_system: scope: scope.treefmt.config.build.wrapper);

      checks = eachSystem (
        system: scope:
        {
          inherit (scope)
            nix-eval-jobs
            clangStdenv-nix-eval-jobs
            shell
            scheduler-spec
            functional-tests
            clang-tidy-fix
            ;
        }
        // lib.optionalAttrs (system != "riscv64-linux") {
          treefmt = scope.treefmtCheck;
        }
      );
    };
}
