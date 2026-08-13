CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0)

BUILD_DIR ?= build
UDEV_RULES_DIR ?= /etc/udev/rules.d

LIBGRAPE_OBJ := $(BUILD_DIR)/libgrape.o
LIBGRAPE := libgrape.a

.PHONY: all clean install-udev uninstall-udev

all: grapectl

$(BUILD_DIR):
	mkdir -p $@

$(LIBGRAPE_OBJ): src/libgrape.c include/grape/grape.h include/grape/gfxlink_protocol.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -c -o $@ $<

$(LIBGRAPE): $(LIBGRAPE_OBJ)
	$(AR) rcs $@ $^

grapectl: src/grapectl.c $(LIBGRAPE) include/grape/grape.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -o $@ src/grapectl.c $(LIBGRAPE) $(LDLIBS)

clean:
	rm -rf $(BUILD_DIR) $(LIBGRAPE) grapectl

install-udev: udev/70-grape.rules
	install -Dm644 $< $(UDEV_RULES_DIR)/70-grape.rules
	udevadm control --reload-rules
	@echo "GRAPE udev rule installed. Replug the device to apply it."

uninstall-udev:
	rm -f $(UDEV_RULES_DIR)/70-grape.rules
	udevadm control --reload-rules
