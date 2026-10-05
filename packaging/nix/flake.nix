{
  description = "simpledir - named directory shortcuts for the shell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in {
        packages.default = pkgs.callPackage ./package.nix { };
        apps.default = {
          type = "app";
          # the move half. `sdcfg` is a symlink to it and picks its own half
          # from argv[0], so there is no second app to declare.
          program = "${self.packages.${system}.default}/bin/sd";
        };
        devShells.default = pkgs.mkShell { };
      });
}