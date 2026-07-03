{
  pkgs,
  krystal,
  ...
}:
with pkgs;
stdenv.mkDerivation {
  pname = "volt";
  version = "0.1.0";

  src = ../.;

  nativeBuildInputs = [
    makeWrapper
    krystal
  ];

  buildInputs = [
    crystal
    mold
  ];

  buildPhase = ''
    runHook preBuild
    krystal -f
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    cp volt $out/bin/
    runHook postInstall
  '';

  postInstall = ''
    wrapProgram $out/bin/volt \
      --prefix PATH : ${
        pkgs.lib.makeBinPath [
          pkgs.crystal
          pkgs.mold
        ]
      }
  '';
}
