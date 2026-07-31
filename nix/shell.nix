{
  pkgs,
  inputs,
  ...
}:

let
  gccAttrs = pkgs.gcc16;
  deps = import ./deps.nix { inherit pkgs; };
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
      graphify
    ]);

  shellHook = ''
    export CXX="ccache $(command -v g++)"
    export CC="ccache $(command -v gcc)"
    export NIX_CFLAGS_LINK="-fuse-ld=mold"
  '';
}
