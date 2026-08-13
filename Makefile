CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0)

.PHONY: all clean
all: grapectl
grapectl: src/grapectl.c include/gfxlink_protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -o $@ src/grapectl.c $(LDLIBS)
clean:
	rm -f grapectl
