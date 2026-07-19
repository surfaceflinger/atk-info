# Out-of-tree build for the atk_info HID driver.
#
#   make            # build atk_info.ko against the running kernel
#   make clean
#   sudo make install && sudo modprobe atk_info
#
# The Nix package overrides KERNELRELEASE / KDIR / INSTALL_MOD_PATH.

KERNELRELEASE   ?= $(shell uname -r)
KDIR            ?= /lib/modules/$(KERNELRELEASE)/build
INSTALL_MOD_PATH ?=
PWD             := $(CURDIR)

obj-m += atk_info.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	install -D -m 0644 atk_info.ko \
		$(INSTALL_MOD_PATH)/lib/modules/$(KERNELRELEASE)/extra/atk_info.ko

.PHONY: all clean install
