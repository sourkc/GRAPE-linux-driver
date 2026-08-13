# GRAPE DRM/KMS driver

`grape_drm` is the kernel-side GFXLINK driver. The M3.0 implementation binds to
GRAPE's `cafe:4750` USB interface, performs `HELLO` and `GET_INFO`, and exposes
the attached GRAPE display through DRM/KMS.

M3.0 intentionally does not implement framebuffer allocation or scanout yet.
Its success condition is that the module registers a DRM card and userspace can
enumerate a connected fixed mode reported by the GRAPE firmware.

## Build

A matching kernel headers package must be installed so that
`/lib/modules/$(uname -r)/build` exists.

```sh
make module
```

For a first test without installing it permanently:

```sh
sudo insmod kernel/grape_drm.ko
sudo dmesg | tail -n 30
ls -l /dev/dri
modetest -M grape -c
```

Unload it to return the USB interface to `libusb`/`grapectl`:

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
