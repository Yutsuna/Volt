{
  pkgs,
}:
let
  overrideMetadata =
    pkg:
    (pkg.overridePythonAttrs or (_: pkg)) (_: {
      dontCheckPythonMetadata = true;
      doCheck = false;
    });
in
with pkgs;
graphify.overridePythonAttrs (
  old:
  lib.genAttrs [ "dependencies" "propagatedBuildInputs" ] (
    key: map overrideMetadata (old.${key} or [ ])
  )
)
