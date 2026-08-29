{
  system ? "x86_64-linux",
}:
let
  mkDrv =
    name:
    derivation {
      inherit name system;
      builder = "/bin/sh";
    };
  # ~1.5 GiB of list cells. Slow enough for the memory monitor to notice.
  hog = builtins.foldl' (acc: x: acc + x) 0 (
    builtins.concatLists (builtins.genList (_: builtins.genList (x: x) 100000) 400)
  );
in
{
  small = mkDrv "small";
  fat = mkDrv "fat-${toString hog}";
}
