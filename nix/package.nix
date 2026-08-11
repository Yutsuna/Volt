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

  nativeBuildInputs = deps.nativeBuildInputs;
  buildInputs = deps.buildInputs ++ [ pkgs.gcc16.cc.lib ];

  mesonBuildType = "release";
  mesonFlags = [
    "-Denable_llvm=true"
    "-Ddefault_library=static"
  ];

  postInstall = ''
    mkdir -p $out/share/volt/Lib
    cp -r $src/source/Lib/. $out/share/volt/Lib/
  '';

  meta = with pkgs.lib; {
    description = "Volt is a compiled and interpreted programming language";
    license = licenses.mit;
    platforms = platforms.unix;
  };
}
