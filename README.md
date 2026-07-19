# atk-info

a tiny linux hid kernel driver that reads the battery level of atk / compx
wireless mice and drops it into the kernel `power_supply` subsystem, so it just
shows up in upower, gnome settings > power, and anything else that reads
`/sys/class/power_supply`.

> [!WARNING]
> **this thing is vibecoded.** and only lightly tested. it might be a security threat.

## supported devices

compx / atk, usb vendor `0x373b`:

| mode     | usb id      | name                             |
|----------|-------------|----------------------------------|
| wireless | `373b:1278` | Compx Wireless mouse 8k dongle-L |
| wired    | `373b:128e` | Compx ATK A9 Mini+               |
| wireless | `373b:10c9` | ATK Nearlink Mouse Dongle        |
| wired    | `373b:1115` | ATK A9 Plus                      |

other atk mice that speak the same `0x08` command set are easy to add,
see [adding another atk mouse](#adding-another-atk-mouse).

## tested kernels

- 6.18

## install (nixos flake)

```nix
# flake.nix
{
  inputs.atk-info.url = "github:surfaceflinger/atk-info";   # or path:/home/nat/Projects/atk-info

  outputs = { nixpkgs, atk-info, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      modules = [
        atk-info.nixosModules.default
        { hardware.atk-info.enable = true; }
      ];
    };
  };
}
```

`nixos-rebuild switch` builds the module against your running kernel
(`config.boot.kernelPackages`), installs it via `boot.extraModulePackages`, and
loads it. it also autoloads on hotplug via the hid modalias.

or just build the module on its own:

```sh
nix build .#atk-info         # ./result/lib/modules/<ver>/misc/atk_info.ko
```

## install (plain make, non-nix)

```sh
make                       # needs /lib/modules/$(uname -r)/build
sudo make install
sudo depmod -a
sudo modprobe atk_info
```

## verify

```sh
upower -d | grep -A10 atk-battery           # or: gnome-control-center power
cat /sys/class/power_supply/atk-battery-*/capacity
dmesg | grep -i atk
```

module params (`poll_interval_ms`, `model_name`):

```sh
sudo modprobe atk_info poll_interval_ms=30000 model_name="ATK A9 Mini+"
# or persist:
#   boot.extraModprobeConfig = ''options atk_info poll_interval_ms=30000 model_name="ATK A9 Mini+"'';
```

## development

```sh
# build against the matching kernel flavor. swap linuxPackages_xanmod for yours:
# linuxPackages (default), linuxPackages_latest, linuxPackages_zen, ...
nix build --impure --out-link result --expr '
  let p = (builtins.getFlake (toString ./.)).inputs.nixpkgs.legacyPackages.${builtins.currentSystem};
  in p.linuxPackages_xanmod.callPackage ./nix/package.nix { }'

# (re)load and check:
sudo rmmod atk_info 2>/dev/null
sudo insmod result/lib/modules/*/misc/atk_info.ko
dmesg | tail
upower -d | grep -A10 atk-battery
```

format to kernel style before committing (uses the bundled `.clang-format`):

```sh
nix run nixpkgs#clang-tools -- clang-format -i atk_info.c
```

## adding another atk mouse

got a different atk mouse that uses the same `0x08` command set? it's basically a
two-line change. check the protocol first by reading its battery from userspace
with [`atk-tool`](https://github.com/noosxe/atk-tool). if that works, this driver
will too.

1. find the usb ids (both the wired mouse and its dongle, if it has one):

   ```sh
   lsusb | grep -iE '373b|atk|compx'
   #   Bus 005 Device 002: ID 373b:XXXX Compx ...
   ```

2. add an entry (vid, pid, and a friendly display name) to `atk_info_devices[]`
   in `atk_info.c` (add a new `USB_VENDOR_ID_*` too if the vendor isn't
   `0x373b`):

   ```c
   #define USB_DEVICE_ID_ATK_MY_MOUSE 0xXXXX

   static const struct hid_device_id atk_info_devices[] = {
           /* ...existing entries... */
           { HID_USB_DEVICE(USB_VENDOR_ID_COMPX, USB_DEVICE_ID_ATK_MY_MOUSE),
             .driver_data = (kernel_ulong_t)"My ATK Mouse" },
           {}
   };
   ```

3. rebuild and reload (see [development](#development)),
   or `nixos-rebuild switch`. for a manual `make install`, run `sudo depmod -a` so
   it autoloads on plug.

## credits

protocol reverse-engineering: [noosxe/atk-tool](https://github.com/noosxe/atk-tool)
and [zeon256/atkzero-battery](https://github.com/zeon256/atkzero-battery).
