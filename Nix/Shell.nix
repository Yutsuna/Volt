{
  pkgs,
  inputs',
  ...
}:
with pkgs;
let
  benchScript = import ./Benchmarks.nix { inherit pkgs; };
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
