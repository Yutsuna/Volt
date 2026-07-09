{
  pkgs,
  inputs',
  ...
}:
with pkgs;
let
  benchScript = writeScriptBin "bench" ''
    #!${stdenv.shell}
    set -euo pipefail

    cd "$GIT_ROOT"

    krystal -r

    BUILD=./bin/Volt

    [[ -f "$BUILD" ]] && echo "using $BUILD"

    CHECK=0
    [[ "''${1:-}" == "--check" ]] && CHECK=1

    declare -A EXPECT=(
      [Recursive/Fibonacci]="Fibonacci of 35 is 9227465"
      [Primes/IsPrime]="Number of primes up to 100000: 9592"
      [OOP/OOP]="Struct: 1550000000.0\nPolymorphism: 696349540.8569596\nRAII Completed."
    )

    function run_check()
    {
      local name="$1" got
      got="$("$BUILD" run "Benchmarks/$name.vl" 2>&1)" || true

      if [[ "$got" == "''${EXPECT[$name]}" ]]; then
        echo "  ok   $name"
      else
        echo "  FAIL $name"
        echo "       expected: ''${EXPECT[$name]}"
        echo "       got:      $got"
        return 1
      fi
    }

    if [[ $CHECK -eq 1 ]]; then
      status=0
      for name in "''${!EXPECT[@]}"; do run_check "$name" || status=1; done
      exit $status
    fi

    function have()
    {
        command -v "$1" >/dev/null 2>&1;
    }

    function bench_one()
    {
      local name="$1"; shift
      echo
      echo "=== $name ==="

      local -a labels=() cmds=()
      labels+=("volt"); cmds+=("$BUILD run Benchmarks/$name.vl")
      have lua      && { labels+=("lua");    cmds+=("lua Benchmarks/$name.lua"); }
      have ruby     && { labels+=("ruby");   cmds+=("ruby Benchmarks/$name.rb"); }
      have php      && { labels+=("php");    cmds+=("php Benchmarks/$name.php"); }
      have python3  && { labels+=("python"); cmds+=("python3 Benchmarks/$name.py"); }
      have node     && { labels+=("node");   cmds+=("node Benchmarks/$name.js"); }

      if have hyperfine; then
        local -a hargs=()
        for i in "''${!cmds[@]}"; do hargs+=(-n "''${labels[$i]}" "''${cmds[$i]}"); done
        hyperfine --warmup 1 --min-runs 5 "''${hargs[@]}"
      else
        echo "(hyperfine missing — falling back to a single \`time\` per runtime)"
        for i in "''${!cmds[@]}"; do
          printf '  %-8s ' "''${labels[$i]}"
          { /usr/bin/env time -f '%e s' bash -c "''${cmds[$i]} >/dev/null"; } 2>&1 || echo "error"
        done
      fi
    }

    for name in "''${!EXPECT[@]}"; do
      bench_one "$name"
    done
  '';
in
mkShell {
  name = "Volt";

  nativeBuildInputs = [
    lua
    python3
    php
    ruby
    hyperfine
    benchScript
    graphify
  ];

  buildInputs = [
    inputs'.krystal-app.packages.default
  ];

  shellHook = ''
    export GIT_ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo ".")
  '';
}
