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
  # module builds; KDIR is what our Makefile's `make -C` targets.
  makeFlags = kernelModuleMakeFlags ++ [
    "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
  ];

  installPhase = ''
    runHook preInstall
    install -D atk_info.ko \
      "$out/lib/modules/${kernel.modDirVersion}/misc/atk_info.ko"
    runHook postInstall
  '';

  meta = {
    description = "Linux HID driver for ATK/Compx mice (battery via UPower)";
    homepage = "https://github.com/surfaceflinger/atk-info";
    license = lib.licenses.gpl2Plus;
    platforms = lib.platforms.linux;
  };
}
