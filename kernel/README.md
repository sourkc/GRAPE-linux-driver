# GRAPE DRM/KMS driver

`grape_drm` is the kernel-side GFXLINK driver.

M3.2 adds damage-aware asynchronous scanout. Linux still exposes the same fixed
720x1280 DRM/KMS display with RGB565 and XRGB8888 dumb/shmem framebuffers, but
plane updates now consume DRM `FB_DAMAGE_CLIPS` and upload only changed regions
to the persistent GRAPE RGB565 texture.

The atomic commit path never performs USB I/O. Changed pixels are converted into
a two-buffer mailbox and a worker sends the latest pending damage over GFXLINK.
If more commits arrive while an upload is active, their damage is accumulated
and the mailbox keeps the newest pixels for those regions instead of queueing
stale frames.

Up to eight exact damaged rectangles are retained. If damage becomes more
fragmented than that, or covers at least 70% of the display, the driver converts
and queues a fresh full frame. This keeps the partial-update shadow state correct
while avoiding excessive per-rectangle GFXLINK resource transactions.

M3.2 also validates the primary plane as fixed 1:1 fullscreen scanout. GRAPE does
not currently implement scaling, cropping, or plane positioning.

The first frame after connect is always a full-frame update so the remote texture
starts from a known state. A failed damage upload also forces the next update to
refresh the full screen.

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

KWin/Plasma can use the display normally. Small updates such as cursor movement,
hover changes, terminal output, and localized window damage should now transfer
far less than the ~1.76 MiB full RGB565 frame.

For an explicit modeset test, use the connector and CRTC IDs reported by
`modetest`:

```sh
modetest -M grape -s <connector_id>@<crtc_id>:720x1280-60
```

On disconnect the driver logs scanout counters including submitted/uploaded
frames, dropped mailbox frames, uploaded rectangles, uploaded pixel bytes, and
how often fragmented/large damage collapsed to a full refresh.

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
