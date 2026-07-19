{
  description = "ATK/Compx mouse HID driver for Linux (battery via power_supply / UPower)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs =
    inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = inputs.nixpkgs.lib.systems.flakeExposed;

      flake = {
        nixosModules.default = ./nix/module.nix;

        overlays.default = final: _prev: {
          atk-info = final.linuxPackages.callPackage ./nix/package.nix { };
        };
      };

      perSystem = { pkgs, lib, ... }: {
        # Kernel modules only build on Linux; keep evaluation happy on the
        # Darwin members of flakeExposed by exposing nothing there.
        packages = lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux (
          let
            atk-info = pkgs.linuxPackages.callPackage ./nix/package.nix { };
          in
          {
            inherit atk-info;
            default = atk-info;
          }
        );
      };
    };
}
