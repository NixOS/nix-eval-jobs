{
  lib,
  runCommand,
  nix-eval-jobs,
  python3,
  nix,
  gitMinimal,
}:
runCommand "nix-eval-jobs-functional-tests"
  {
    src = lib.fileset.toSource {
      fileset = ../tests-functional;
      root = ../.;
    };
    nativeBuildInputs = [
      nix-eval-jobs
      python3.pkgs.pytest
      nix
      gitMinimal
    ];
  }
  ''
    cp -r $src/tests-functional .

    export HOME=$TMPDIR
    export NIX_STATE_DIR=$TMPDIR/nix-state
    export NIX_STORE_DIR=$TMPDIR/nix-store
    export NIX_DATA_DIR=$TMPDIR/nix-data
    export NIX_LOG_DIR=$TMPDIR/nix-log
    export NIX_CONF_DIR=$TMPDIR/nix-conf
    export NIX_EVAL_JOBS_BIN=${nix-eval-jobs}/bin/nix-eval-jobs

    pytest tests-functional/ -v
    touch $out
  ''
