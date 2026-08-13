CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0)

UDEV_RULES_DIR ?= /etc/udev/rules.d

.PHONY: all clean install-udev uninstall-udev
all: grapectl
grapectl: src/grapectl.c include/gfxlink_protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -o $@ src/grapectl.c $(LDLIBS)
clean:
	rm -f grapectl

install-udev: udev/70-grape.rules
	install -Dm644 $< $(UDEV_RULES_DIR)/70-grape.rules
	udevadm control --reload-rules
	@echo "GRAPE udev rule installed. Replug the device to apply it."

uninstall-udev:
	rm -f $(UDEV_RULES_DIR)/70-grape.rules
	udevadm control --reload-rules
