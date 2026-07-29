{
  pkgs,
  ...
}:
let
  gccStdenv = pkgs.gcc16Stdenv;
  deps = import ./deps.nix { inherit pkgs; };
  version = with builtins; head (split "\n" (readFile ../VERSION.md));
in
gccStdenv.mkDerivation {
  pname = "volt";
  inherit version;
  src = pkgs.lib.cleanSource ../.;

  nativeBuildInputs = deps.nativeBuildInputs ++ [ pkgs.autoPatchelfHook ];
  inherit (deps) buildInputs;

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DVOLT_USE_CCACHE=ON"
    "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON"
    "-DVOLT_ENABLE_LLVM=ON"
    "-DCMAKE_INSTALL_RPATH=${placeholder "out"}/lib"
  ];

  installPhase = ''
    mkdir -p $out/bin $out/lib $out/share/volt/Lib
    cp bin/volt $out/bin/
    cp lib/*.so $out/lib 2>/dev/null || true
    cp -r $src/source/Lib/. $out/share/volt/Lib/
  '';

  meta = with pkgs.lib; {
    description = "Volt is a compiled and interpreted programming language";
    license = licenses.mit;
    platforms = platforms.unix;
  };

}
