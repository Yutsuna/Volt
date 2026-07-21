{
  lib,
  stdenv,
  ruby,
  makeWrapper,
}:

stdenv.mkDerivation {
  pname = "volt-build";
  version = "0.1.0";

  src = ../scripts;

  nativeBuildInputs = [ makeWrapper ];

  installPhase = ''
    mkdir -p $out/bin
    mkdir -p $out/share/volt-build

    cp -r * $out/share/volt-build/

    makeWrapper ${ruby}/bin/ruby $out/bin/volt-build \
      --add-flags "$out/share/volt-build/build.rb"
  '';

  meta = with lib; {
    description = "Volt compiler build tool";
    platforms = platforms.unix;
  };
}
