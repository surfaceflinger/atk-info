# NixOS module. Enable with:
#
#   imports = [ inputs.atk-info.nixosModules.default ];
#   hardware.atk-info.enable = true;
{ config, lib, ... }:

let
  cfg = config.hardware.atk-info;
  # Build against the kernel this NixOS host actually runs.
  atkInfo = config.boot.kernelPackages.callPackage ./package.nix { };
in
{
  options.hardware.atk-info.enable = lib.mkEnableOption "ATK/Compx mouse kernel module (battery status via UPower)";

  config = lib.mkIf cfg.enable {
    boot.extraModulePackages = [ atkInfo ];
    # Autoloads on plug via the HID modalias; loading it here is harmless if the
    # mouse is absent (the driver just waits) and makes the behaviour explicit.
    boot.kernelModules = [ "atk_info" ];
  };
}
