{
  description = "Volt is a compiled and interpreted programming language";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
  };

  outputs = inputs@{
    self,
    nixpkgs,
    ...
  }:
  let
    allSystems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
    forAllSystems = f: nixpkgs.lib.genAttrs allSystems (system: f (import nixpkgs { inherit system; } ));
  in
  {
  packages = forAllSystems (pkgs: {
    default = pkgs.callPackage ./nix/package.nix { inherit inputs; };
  });
  devShells = forAllSystems (pkgs: {
    default = pkgs.callPackage ./nix/shell.nix { inherit inputs; };
  });
  };
}
