{
  pkgs,
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
    autoPatchelfHook
  ];

  buildInputs = with llvmAttrs; [
    llvm
    libllvm
  ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DVOLT_USE_CCACHE=ON"
    "-DVOLT_UNITY_BUILD=ON"
    "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON"
    "-DCMAKE_INSTALL_RPATH=${placeholder "out"}/lib"
  ];

  installPhase = ''
    mkdir -p $out/bin $out/lib
    cp bin/Volt $out/bin/
    cp lib/*.so $out/lib 2>/dev/null || true
  '';

  meta = with pkgs.lib; {
    description = "Volt is a compiled and interpreted programming language";
    license = licenses.mit;
    platforms = platforms.unix;
  };

}
