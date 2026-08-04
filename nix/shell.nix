{
  pkgs,
  inputs,
  ...
}:

let
  deps = import ./deps.nix { inherit pkgs; };
  ccacheStdenv = pkgs.overrideCC pkgs.stdenv pkgs.ccacheWrapper;
in
pkgs.mkShell.override { stdenv = ccacheStdenv; } {

  nativeBuildInputs =
    deps.nativeBuildInputs
    ++ deps.buildInputs
    ++ (with pkgs; [
      gcc16
      valgrind
      gdb
      gtest
      clang-tools
      jq
      uv
    ]);

  shellHook = ''
    export CCACHE_DIR="$PWD/.ccache"
    export CCACHE_SLOPPINESS="pch_defines,time_macros"
    export CC="ccache gcc"
    export CXX="ccache g++"
    export NIX_CFLAGS_LINK="-fuse-ld=mold"

    if [ ! -d ".venv" ]; then
      echo ">> initializing virtual environment"
      uv venv .venv
      uv pip install --python .venv/bin/python graphifyy
    fi

    source .venv/bin/activate
  '';
}
