{
  pkgs,
  inputs,
  ...
}:

let
  gccAttrs = pkgs.gcc16;
  deps = import ./deps.nix { inherit pkgs; };
  graphify-pkg = import ./graphify.nix { inherit pkgs; };
in
pkgs.mkShell {

  nativeBuildInputs =
    deps.nativeBuildInputs
    ++ deps.buildInputs
    ++ (with pkgs; [
      gccAttrs
      valgrind
      gdb
      gtest
      clang-tools
      cmake-lint
      graphify-pkg
    ]);

  shellHook = ''
    # mkdir -p .git/ccache-bin 2>/dev/null || true
    # ln -sf $(command -v ccache) .git/ccache-bin/gcc 2>/dev/null || true
    # ln -sf $(command -v ccache) .git/ccache-bin/g++ 2>/dev/null || true
    # export PATH="$PWD/.git/ccache-bin:$PATH"
    export CC="gcc"
    export CXX="g++"
    export NIX_CFLAGS_LINK="-fuse-ld=mold"
  '';
}
