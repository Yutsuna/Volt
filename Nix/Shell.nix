{
  pkgs,
  inputs',
  ...
}:
with pkgs;
mkShell {
  name = "Volt";

  nativeBuildInputs = [
    lua
    python3
    php
    ruby
    hyperfine
  ];

  buildInputs = [
    inputs'.krystal-app.packages.default
  ];
}
