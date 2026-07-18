{
  pkgs,
  inputs,
  ...
}:
let
  gccStdenv = pkgs.gcc16Stdenv;
  llvmAttrs = pkgs.llvmPackages_latest;
  version = with builtins; head (split "\n" (readFile ../VERSION.md));
in
gccStdenv.mkDerivation {
  pname = "volt";
  inherit version;
  src = pkgs.lib.cleanSource ../.;

  nativeBuildInputs = with pkgs; [
    cmake
    ninja
    pkg-config
    ccache
    mold
  ];

  buildInputs = with llvmAttrs; [
    llvm
    libllvm
  ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DVOLT_USE_CCACHE=ON"
    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold"
    "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=mold"
  ];

  meta = with pkgs.lib; {
    description = "Volt is a compiled and interpreted programming language";
    license = licenses.mit;
    platforms = platforms.unix;
  };

}
