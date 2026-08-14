CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0)
GRAPECTL_CPPFLAGS := $(shell pkg-config --cflags libpng)
GRAPECTL_LDLIBS := $(shell pkg-config --libs libpng)

BUILD_DIR ?= build
UDEV_RULES_DIR ?= /etc/udev/rules.d
KDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_MODULE_DIR := $(CURDIR)/kernel
MODULE_INSTALL_DIR ?= extra/grape

# External modules must use the same compiler family as the running kernel.
# CachyOS commonly ships Clang-built kernels, whose Kbuild flags GCC cannot
# parse. Detect that from the generated kernel config and select LLVM
# automatically. Override with KBUILD_TOOLCHAIN=... if ever needed.
KERNEL_CONFIG_FILES := $(KDIR)/include/config/auto.conf $(KDIR)/.config
KBUILD_TOOLCHAIN ?= $(shell if grep -qs '^CONFIG_CC_IS_CLANG=y' $(KERNEL_CONFIG_FILES) 2>/dev/null; then printf 'LLVM=1'; fi)

LIBGRAPE_OBJ := $(BUILD_DIR)/libgrape.o
LIBGRAPE := libgrape.a

.PHONY: all clean module module-clean install-module uninstall-module install-udev uninstall-udev

all: grapectl grape-gpu-smoke

$(BUILD_DIR):
	mkdir -p $@

$(LIBGRAPE_OBJ): src/libgrape.c include/grape/grape.h include/grape/gfxlink_protocol.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -c -o $@ $<

$(LIBGRAPE): $(LIBGRAPE_OBJ)
	$(AR) rcs $@ $^

grapectl: src/grapectl.c $(LIBGRAPE) include/grape/grape.h
	$(CC) $(CPPFLAGS) $(GRAPECTL_CPPFLAGS) $(CFLAGS) -std=gnu11 -o $@ src/grapectl.c $(LIBGRAPE) $(LDLIBS) $(GRAPECTL_LDLIBS)

grape-gpu-smoke: src/grape_gpu_smoke.c include/grape/grape_drm.h include/grape/gfxlink_protocol.h
	$(CC) -Iinclude $(CFLAGS) -std=gnu11 -o $@ src/grape_gpu_smoke.c

clean:
	rm -rf $(BUILD_DIR) $(LIBGRAPE) grapectl grape-gpu-smoke

module:
	@if [ -n "$(KBUILD_TOOLCHAIN)" ]; then echo "Kernel toolchain: $(KBUILD_TOOLCHAIN) (auto-detected)"; fi
	$(MAKE) -C $(KDIR) M=$(KERNEL_MODULE_DIR) $(KBUILD_TOOLCHAIN) modules

module-clean:
	$(MAKE) -C $(KDIR) M=$(KERNEL_MODULE_DIR) $(KBUILD_TOOLCHAIN) clean

install-module: module
	$(MAKE) -C $(KDIR) M=$(KERNEL_MODULE_DIR) $(KBUILD_TOOLCHAIN) INSTALL_MOD_DIR=$(MODULE_INSTALL_DIR) modules_install
	depmod -a

uninstall-module:
	find /lib/modules/$(shell uname -r)/$(MODULE_INSTALL_DIR) -type f -name 'grape_drm.ko*' -delete 2>/dev/null || true
	depmod -a

install-udev: udev/70-grape.rules
	install -Dm644 $< $(UDEV_RULES_DIR)/70-grape.rules
	udevadm control --reload-rules
	@echo "GRAPE udev rule installed. Replug the device to apply it."

uninstall-udev:
	rm -f $(UDEV_RULES_DIR)/70-grape.rules
	udevadm control --reload-rules
