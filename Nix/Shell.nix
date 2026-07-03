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
  ];

  buildInputs = [
    inputs'.krystal-app.packages.default
  ];
}
