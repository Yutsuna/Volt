{
  description = "Volt is a compiled and interpreted programming language";

  nixConfig = {
    extra-substituters = [ "https://yutsuna.cachix.org" ];
    extra-trusted-public-keys = [ "yutsuna.cachix.org-1:zKFPrNQ4xW03bvgLY49cUDL4xC37Mju1yHGsRK6s+Ug=" ];
  };

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
  };

  outputs =
    inputs@{
      self,
      nixpkgs,
      ...
    }:
    let
      allSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs allSystems (system: f (import nixpkgs { inherit system; }));
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
