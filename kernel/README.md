# GRAPE DRM/KMS driver

`grape_drm` is the kernel-side GFXLINK driver.

M3.2a keeps the asynchronous dumb-scanout path from M3.1/M3.2, but no longer
relies on compositor-provided `FB_DAMAGE_CLIPS` for performance. In the tested
KWin/Wayland path those clips resolve to a full-plane update on every commit,
which makes cursor movement as expensive as transferring the entire display.

Each accepted KMS update is converted to a coherent RGB565 frame in the
mailbox. The USB worker compares the newest frame with a second RGB565 shadow
that represents pixels successfully presented on the P4. The comparison uses
32x32 tiles, so dropped compositor frames cannot hide changes: the baseline is
advanced only after the corresponding GFXLINK texture updates and `PRESENT`
succeed.

Dirty tile runs are combined vertically, then reduced to at most eight upload
rectangles. Merging is allowed to include up to 32 KiB of unchanged RGB565
pixels when that avoids another resource/update transaction. Highly fragmented
updates, or updates covering at least 70% of the tile grid/frame, fall back to a
full-screen transfer.

The first frame is always full-screen because the remote texture has no known
contents yet. Any failed partial transfer invalidates the shadow baseline so the
next accepted frame also repairs the remote texture with a full refresh.

The atomic commit path still performs no USB I/O. It only snapshots/converts the
latest framebuffer into the two-buffer mailbox; slow GFXLINK resource uploads
remain on `system_long_wq` and stale pending frames are replaced by the newest
one rather than queued.

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

Load the module:

```sh
sudo insmod kernel/grape_drm.ko
sudo dmesg | tail -n 50
modetest -M grape -c
modetest -M grape -p
```

The connect log should include:

```text
32x32 framebuffer-diff scanout ready
```

KWin/Plasma can then use the display normally. Cursor movement, hover changes,
terminal output, and other small updates should produce only a few dirty tiles
instead of a 1,843,200-byte RGB565 full-frame upload.

For an explicit modeset test, use the connector and CRTC IDs reported by
`modetest`:

```sh
modetest -M grape -s <connector_id>@<crtc_id>:720x1280-60
```

On disconnect the driver logs submitted/uploaded/dropped frames, unchanged
frames, uploaded rectangles and bytes, total dirty tiles, full-frame fallbacks,
and rectangle merges. Those counters make it easy to verify that localized UI
activity is no longer becoming a full-screen transfer.

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
