{
  pkgs,
  inputs,
  ...
}:

let
  gccAttrs = pkgs.gcc16;
  voltProj = pkgs.callPackage ./package.nix { inherit inputs; };
  voltBuild = pkgs.callPackage ./volt-build.nix { };
in
pkgs.mkShell {
  inputsFrom = [ voltProj ];

  nativeBuildInputs = with pkgs; [
    gccAttrs
    valgrind
    gdb
    gtest
    clang-tools
    cmake-lint
    mold
    ruby-lsp
    graphify
    voltBuild
  ];

  shellHook = ''
    export CXX=g++
    export CC=gcc
    export NIX_CFLAGS_LINK="-fuse-ld=mold"

    python3 scripts/graphify/update_graphify.py
  '';
}
