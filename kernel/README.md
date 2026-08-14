# GRAPE DRM/KMS driver

`grape_drm` is the kernel-side GFXLINK driver.

M3.2b keeps the 32x32 framebuffer-diff fallback from M3.2a, but removes the
latency-heavy generic resource lifecycle from dumb scanout. Small framebuffer
updates are now written directly into the persistent remote scanout texture with
GFXLINK v6 `TEXTURE_WRITE_RECT` packets. The packets are fire-and-forget USB
bulk transfers; one normal `PRESENT` response at the end of the batch reports
any deferred firmware-side write error.

The worker still compares the newest coherent RGB565 frame with a second RGB565
shadow representing what was successfully presented on the P4. Dirty 32x32 tile
runs are merged into at most eight upload rectangles. Large or highly fragmented
changes fall back to a full-screen update. The baseline advances only after the
rectangle writes and `PRESENT` succeed, so dropped KWin frames cannot hide
changes.

On the firmware side, direct texture writes are accumulated until `PRESENT`. The
renderer then invalidates only the changed texture regions before presenting.
For the 1:1 fullscreen scanout surface this keeps both USB traffic and GRAPE
renderer damage local instead of invalidating the whole 720x1280 surface for
every cursor movement.

The atomic commit path performs no USB I/O. It only converts/snapshots the latest
framebuffer into the two-buffer mailbox; stale pending frames are replaced by the
newest one rather than queued.

The primary plane remains fixed 1:1 fullscreen scanout. GRAPE does not currently
implement scaling, cropping, or plane positioning.

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

## Test

Load the matching GFXLINK v6 firmware first, then load the module:

```sh
sudo insmod kernel/grape_drm.ko
sudo dmesg | tail -n 50
modetest -M grape -c
modetest -M grape -p
```

The connect log should include:

```text
32x32 diff + direct texture-write scanout ready
```

KWin/Plasma can then use the display normally. Cursor movement, hover changes,
terminal output, and other small updates should transfer only the dirty
rectangles without creating, committing, updating from, and destroying a
temporary GFXLINK resource for every rectangle.

For an explicit modeset test, use the connector and CRTC IDs reported by
`modetest`:

```sh
modetest -M grape -s <connector_id>@<crtc_id>:720x1280-60
```

On disconnect the driver logs submitted/uploaded/dropped frames, unchanged
frames, uploaded rectangles and bytes, total dirty tiles, full-frame fallbacks,
rectangle merges, and the number of direct texture-write packets.

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
