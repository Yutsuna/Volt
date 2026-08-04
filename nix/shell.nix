{
  pkgs,
  inputs,
  ...
}:

let
  deps = import ./deps.nix { inherit pkgs; };
in
pkgs.mkShell {

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
    export CC="gcc"
    export CXX="g++"
    export NIX_CFLAGS_LINK="-fuse-ld=mold"

    if [ ! -d ".venv" ]; then
      echo ">> initializing virtual environment"
      uv venv .venv
      uv pip install --python .venv/bin/python graphifyy
    fi

    source .venv/bin/activate
  '';
}
