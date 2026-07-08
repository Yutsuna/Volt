{
  description = "Volt is a compiled and interpreted programming language";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    krystal-app = {
      url = "github:Yutsuna/Krystal";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    inputs@{
      self,
      nixpkgs,
      flake-parts,
      ...
    }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];

      perSystem = { pkgs, inputs', ... }: {
        devShells.default = pkgs.callPackage ./Nix/Shell.nix { inherit inputs'; };

        packages.default = pkgs.callPackage ./Nix/Package.nix {
          krystal = inputs'.krystal-app.packages.default;
        };
      };
    };
}
