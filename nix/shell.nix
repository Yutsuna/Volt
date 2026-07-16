{
  pkgs,
  inputs,
  ...
}:

let
  llvmAttrs = pkgs.llvmPackages_latest;
  voltProj = pkgs.callPackage ./package.nix { inherit inputs; };
in
pkgs.mkShell {
  inputsFrom = [ voltProj ];

  nativeBuildInputs = with pkgs; [
    llvmAttrs.clang
    llvmAttrs.lldb
    valgrind
    gdb
    gtest
    clang-tools
    cmake-lint
    mold
  ];

  shellHook = ''
    export CXX=clang++
    export CC=clang
    export NIX_CFLAGS_LINK="-fuse-ld=mold"
  '';
}
