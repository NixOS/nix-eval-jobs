let
  mk =
    name:
    derivation {
      inherit name;
      system = "x86_64-linux";
      builder = "/bin/sh";
    };
  self = {
    recurseForDerivations = true;
    ok = mk "in-cycle";
    again = self;
  };
in
{
  "quo\"te" = mk "quote";
  "" = mk "empty";
  "1" = mk "numeric";
  cycle = self;
}
