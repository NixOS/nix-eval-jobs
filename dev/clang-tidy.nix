{
  lib,
  nix-eval-jobs,
  git,
  llvmPackages,
}:
nix-eval-jobs.overrideAttrs (old: {
  pname = "nix-eval-jobs-clang-tidy";
  nativeBuildInputs = old.nativeBuildInputs ++ [
    git
    (lib.hiPrio llvmPackages.clang-tools)
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
})
