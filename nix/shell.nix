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
      jq
    ]);

  shellHook = ''
    export CC="gcc"
    export CXX="g++"
    export NIX_CFLAGS_LINK="-fuse-ld=mold"
  '';
}
