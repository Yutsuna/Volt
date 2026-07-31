{
  pkgs,
}:
let
  overrideMetadata =
    pkg:
    (pkg.overridePythonAttrs or (_: pkg)) (_: {
      dontCheckPythonMetadata = true;
    });
in
with pkgs;
graphify.overridePythonAttrs (
  old:
  lib.genAttrs [ "dependencies" "propagateBuildInputs" ] (
    key: map overrideMetadata (old.${key} or [ ])
  )
)
