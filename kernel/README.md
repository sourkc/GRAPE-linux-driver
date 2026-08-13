# GRAPE DRM/KMS driver

`grape_drm` is the kernel-side GFXLINK driver.

M3.1 adds dumb/shmem GEM scanout on top of the M3.0 USB + KMS skeleton. Linux
can now allocate an RGB565 or XRGB8888 framebuffer, modeset it through normal
DRM/KMS, and have the driver convert/upload it into one persistent full-screen
GRAPE RGB565 texture and surface.

This is deliberately still unaccelerated scanout. Each plane update currently
uploads the full 720x1280 frame through the existing reliable GFXLINK resource
stream. Damage-rectangle scanout belongs to M3.2.

## Build

Install the headers for the running kernel so that
`/lib/modules/$(uname -r)/build` exists, then run:

```sh
make module
```

The top-level Makefile checks the running kernel's generated config. If the
kernel was built with Clang (as CachyOS commonly is), it automatically passes
`LLVM=1` to Kbuild so the external module uses the matching compiler family.
You can override this with `KBUILD_TOOLCHAIN=...` if necessary.

## First scanout test

Load the module:

```sh
sudo insmod kernel/grape_drm.ko
sudo dmesg | tail -n 40
modetest -M grape -c
```

Then inspect the IDs:

```sh
modetest -M grape -p
```

For the common M3.1 layout (one connector, one CRTC, one primary plane), use
`modetest` to set the preferred 720x1280 mode and a test framebuffer. The exact
connector/CRTC IDs are printed by `modetest -M grape -c` and `-p` and can vary
between boots, so do not hard-code the numeric IDs in scripts yet.

A typical command is:

```sh
modetest -M grape -s <connector_id>@<crtc_id>:720x1280-60
```

`modetest` should allocate a dumb framebuffer and put its default test pattern
on the physical GRAPE panel. The first frame can take hundreds of milliseconds
because M3.1 intentionally uses the existing ~5 MiB/s full-frame upload path.

Unload the module to return the USB interface to `libusb`/`grapectl`:

```sh
sudo rmmod grape_drm
```

To install it for the running kernel:

```sh
sudo make install-module
sudo modprobe grape_drm
```

The kernel module source in this directory is GPL-2.0-only. The userspace
portion of the repository remains under the repository's top-level license.
