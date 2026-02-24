{
  outputs =
    { self, ... }:
    let
      system = "x86_64-linux";

      # Simple replacement for writeText
      makeTextDrv =
        name: text:
        derivation {
          inherit name system;
          builder = "/bin/sh";
          args = [
            "-c"
            "echo '${text}' > $out"
          ];
        };
    in
    {
      hydraJobs = import ./ci.nix { inherit system; };

      legacyPackages.x86_64-linux = {
        warningPkgs = {
          # Test package that emits a warning during evaluation
          packageWithWarning = builtins.warn "this is a test warning" (derivation {
            name = "package-with-warning";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo 'content' > $out"
            ];
          });
          # Package with multiple warnings
          packageWithMultipleWarnings = builtins.warn "first warning" (
            builtins.warn "second warning" (derivation {
              name = "package-with-multiple-warnings";
              inherit system;
              builder = "/bin/sh";
              args = [
                "-c"
                "echo 'content' > $out"
              ];
            })
          );
          # Package with warning followed by unrelated error
          warningThenError = builtins.warn "warning before error" (throw "unrelated error after warning");
        };
        emptyNeeded = rec {
          # This is a reproducer for issue #369 where neededBuilds and neededSubstitutes are empty
          # when they should contain values
          nginx = derivation {
            name = "nginx-1.24.0";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo 'content' > $out"
            ];
          };
          proxyWrapper = derivation {
            name = "proxyWrapper";
            system = "aarch64-linux";
            builder = "/bin/sh";
            args = [
              "-c"
              "echo '${nginx}' > $out"
            ];
          };
          webService = derivation {
            name = "webService";
            system = "aarch64-linux";
            builder = "/bin/sh";
            args = [
              "-c"
              "echo '${proxyWrapper}' > $out"
            ];
          };
        };
        brokenPkgs = {
          brokenPackage = throw "this is an evaluation error";
        };
        infiniteRecursionPkgs = {
          packageWithInfiniteRecursion =
            let
              recursion = [ recursion ];
            in
            derivation {
              inherit system;
              name = "drvB";
              recursiveAttr = recursion;
              builder = ":";
            };
        };
        success = {
          indirect_aggregate = derivation {
            name = "indirect_aggregate";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
            _hydraAggregate = true;
            constituents = [
              "anotherone"
            ];
          };
          direct_aggregate = derivation {
            name = "direct_aggregate";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
            _hydraAggregate = true;
            constituents = [
              self.hydraJobs.builtJob
            ];
          };
          mixed_aggregate = derivation {
            name = "mixed_aggregate";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
            _hydraAggregate = true;
            constituents = [
              self.hydraJobs.builtJob
              "anotherone"
            ];
          };
          anotherone = makeTextDrv "constituent" "text";
        };
        failures = {
          aggregate = derivation {
            name = "aggregate";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
            _hydraAggregate = true;
            constituents = [
              "doesntexist"
              "doesnteval"
            ];
          };
          doesnteval = makeTextDrv "constituent" (toString { });
        };
        glob1 = {
          constituentA = derivation {
            name = "constituentA";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
          };
          constituentB = derivation {
            name = "constituentB";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
          };
          aggregate = derivation {
            name = "aggregate";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
            _hydraAggregate = true;
            _hydraGlobConstituents = true;
            constituents = [ "*" ];
          };
        };
        cycle = {
          aggregate0 = derivation {
            name = "aggregate0";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
            _hydraAggregate = true;
            _hydraGlobConstituents = true;
            constituents = [ "aggregate1" ];
          };
          aggregate1 = derivation {
            name = "aggregate1";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
            _hydraAggregate = true;
            _hydraGlobConstituents = true;
            constituents = [ "aggregate0" ];
          };
        };
        glob2 = rec {
          packages = {
            recurseForDerivations = true;
            constituentA = derivation {
              name = "constituentA";
              inherit system;
              builder = "/bin/sh";
              args = [
                "-c"
                "echo done > $out"
              ];
            };
            constituentB = derivation {
              name = "constituentB";
              inherit system;
              builder = "/bin/sh";
              args = [
                "-c"
                "echo done > $out"
              ];
            };
          };
          aggregate0 = derivation {
            name = "aggregate0";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
            _hydraAggregate = true;
            _hydraGlobConstituents = true;
            constituents = [
              "packages.*"
            ];
          };
          aggregate1 = derivation {
            name = "aggregate1";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
            _hydraAggregate = true;
            _hydraGlobConstituents = true;
            constituents = [
              "tests.*"
            ];
          };
          indirect_aggregate0 = derivation {
            name = "indirect_aggregate0";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
            _hydraAggregate = true;
            constituents = [
              "aggregate0"
            ];
          };
          mix_aggregate0 = derivation {
            name = "mix_aggregate0";
            inherit system;
            builder = "/bin/sh";
            args = [
              "-c"
              "echo done > $out"
            ];
            _hydraAggregate = true;
            constituents = [
              "aggregate0"
              packages.constituentA
            ];
          };
        };
      };
    };
}
