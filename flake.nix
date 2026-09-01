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
      eachSystem = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system} system);

      scopeFor =
        pkgs:
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
            }
          );
          nixComponents = lib.makeScope nixDependencies.newScope (
            import (nix + "/packaging/components.nix") {
              officialRelease = true;
              inherit lib pkgs;
              src = nix;
              maintainers = [ ];
            }
          );
        in
        {
          inherit (nixDependencies) callPackage;
          drvArgs = { inherit nixComponents; };
        };

      treefmtFor = pkgs: treefmt-nix.lib.evalModule pkgs ./dev/treefmt.nix;
    in
    {
      packages = eachSystem (
        pkgs: system:
        let
          inherit (scopeFor pkgs) callPackage drvArgs;
        in
        {
          nix-eval-jobs = callPackage ./default.nix drvArgs;
          clangStdenv-nix-eval-jobs = callPackage ./default.nix (drvArgs // { stdenv = pkgs.clangStdenv; });
          default = self.packages.${system}.nix-eval-jobs;
        }
      );

      devShells = eachSystem (
        pkgs: _system:
        let
          inherit (scopeFor pkgs) callPackage drvArgs;
        in
        {
          default = callPackage ./shell.nix drvArgs;
          clang = callPackage ./shell.nix (drvArgs // { stdenv = pkgs.clangStdenv; });
        }
      );

      formatter = eachSystem (pkgs: _system: (treefmtFor pkgs).config.build.wrapper);

      checks = eachSystem (
        pkgs: system:
        let
          self' = {
            packages = self.packages.${system};
            devShells = self.devShells.${system};
          };
        in
        builtins.removeAttrs self'.packages [ "default" ]
        // lib.optionalAttrs (system != "riscv64-linux") {
          treefmt = (treefmtFor pkgs).config.build.check self;
        }
        // {
          shell = self'.devShells.default;
          scheduler-spec =
            pkgs.runCommand "nix-eval-jobs-scheduler-spec" { nativeBuildInputs = [ pkgs.quint ]; }
              ''
                  export HOME=$TMPDIR
                  cd ${./spec}
                  quint run scheduler.qnt --main main --invariant inv --max-steps 150 --max-samples 2000
                  quint test scheduler.qnt --main noOomKiller --max-samples 200
                quint test scheduler.qnt --main main --match terminates --max-samples 200
                  touch $out
              '';
          functional-tests =
            pkgs.runCommand "nix-eval-jobs-functional-tests"
              {
                src = lib.fileset.toSource {
                  fileset = lib.fileset.unions [
                    ./tests-functional
                  ];
                  root = ./.;
                };

                nativeBuildInputs = [
                  self'.packages.nix-eval-jobs
                  pkgs.python3.pkgs.pytest
                  pkgs.nix
                  pkgs.gitMinimal
                ];
              }
              ''
                # Copy test files
                cp -r $src/tests-functional .

                # Set up test environment
                export HOME=$TMPDIR
                export NIX_STATE_DIR=$TMPDIR/nix-state
                export NIX_STORE_DIR=$TMPDIR/nix-store
                export NIX_DATA_DIR=$TMPDIR/nix-data
                export NIX_LOG_DIR=$TMPDIR/nix-log
                export NIX_CONF_DIR=$TMPDIR/nix-conf

                # Use the pre-built nix-eval-jobs binary
                export NIX_EVAL_JOBS_BIN=${self'.packages.nix-eval-jobs}/bin/nix-eval-jobs

                # Run the tests
                pytest tests-functional/ -v

                # Create output marker
                touch $out
              '';
          clang-tidy-fix = self'.packages.nix-eval-jobs.overrideAttrs (old: {
            pname = "nix-eval-jobs-clang-tidy";
            nativeBuildInputs = old.nativeBuildInputs ++ [
              pkgs.git
              (lib.hiPrio pkgs.llvmPackages.clang-tools)
            ];
            buildPhase = ''
              export HOME=$TMPDIR
              cat > $HOME/.gitconfig <<EOF
              [user]
                name = Nix
                email = nix@localhost
              [init]
                defaultBranch = main
              EOF
              pushd ..
              git init
              git add .
              git commit -m init --quiet
              popd
              echo "Verifying clang-tidy configuration..."
              clang-tidy --verify-config
              ninja clang-tidy-fix
              git status
              if ! git --no-pager diff --exit-code; then
                echo "clang-tidy-fix failed, please run `ninja clang-tidy-fix` and commit the changes"
                exit 1
              fi
            '';
            installPhase = ''
              touch $out
            '';
          });
        }
      );
    };
}
