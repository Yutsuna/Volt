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
      ruby-lsp
      graphify
    ]);

  shellHook = ''
    export CXX="$(command -v g++)"
    export CC="$(command -v gcc)"
    export NIX_CFLAGS_LINK="-fuse-ld=mold"
  '';
}
