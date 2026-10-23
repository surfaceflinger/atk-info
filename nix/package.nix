# Kernel module derivation. Call with a kernel-scoped `callPackage` so the
# correct `kernel` and `kernelModuleMakeFlags` are injected, e.g.
#
#   pkgs.linuxPackages.callPackage ./nix/package.nix { }
#   config.boot.kernelPackages.callPackage ./nix/package.nix { }   # (nixos module)
{
  lib,
  stdenv,
  kernel,
  kernelModuleMakeFlags,
}:

stdenv.mkDerivation {
  pname = "atk-info";
  version = "0.1.0";

  src = lib.cleanSource ../.;

  hardeningDisable = [ "pic" ];
  nativeBuildInputs = kernel.moduleBuildDependencies;

  # kernelModuleMakeFlags carries the toolchain + KBUILD_OUTPUT for out-of-tree
  # module builds, including cmd_sign=kmodigest for deterministic signatures
  # (nixpkgs#571181). KDIR is what our Makefile's `make -C` targets.
  # Install via kbuild's modules_install so the module is stripped, signed,
  # and its .ko.cer emitted; the aggregator merges it into modules.cer.
  makeFlags = kernelModuleMakeFlags ++ [
    "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "INSTALL_MOD_PATH=${placeholder "out"}"
    "INSTALL_MOD_DIR=misc"
    "INSTALL_MOD_STRIP=1"
    "DEPMOD=true"
  ];

  installTargets = [ "modules_install" ];

  installFlags = [
    "-C"
    "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "M=$(PWD)"
  ];

  meta = {
    description = "Linux HID driver for ATK/Compx mice (battery via UPower)";
    homepage = "https://github.com/surfaceflinger/atk-info";
    license = lib.licenses.gpl2Plus;
    platforms = lib.platforms.linux;
  };
}
