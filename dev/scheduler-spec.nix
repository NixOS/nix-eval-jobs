{ runCommand, quint }:
runCommand "nix-eval-jobs-scheduler-spec" { nativeBuildInputs = [ quint ]; } ''
  export HOME=$TMPDIR
  cd ${../spec}
  quint run scheduler.qnt --main main --invariant inv --max-steps 150 --max-samples 2000
  quint test scheduler.qnt --main noOomKiller --max-samples 200
  quint test scheduler.qnt --main main --match terminates --max-samples 200
  touch $out
''
