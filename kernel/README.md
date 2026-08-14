# GRAPE DRM/KMS driver

`grape_drm` is the kernel-side GFXLINK driver.

M4.0 keeps the M3.2b dumb-scanout fallback and adds the first render-side DRM
ABI. The driver now advertises `DRIVER_RENDER`, so DRM creates a
`/dev/dri/renderD*` node in addition to the KMS primary node. Render-only private
ioctls are marked `DRM_RENDER_ALLOW`.

The M4.0 render ABI provides:

- device parameter queries;
- per-file GRAPE GPU contexts;
- shmem GEM buffer objects with mmap offsets;
- asynchronous command submission through an ordered kernel workqueue;
- monotonically increasing submission sequence numbers and a wait ioctl.

The only command accepted by M4.0 is an 8-byte `NOP`. This is intentional: it
proves the full userspace -> render node -> DRM ioctl -> kernel queue -> GFXLINK
v7 -> P4 parser -> completion path before rendering commands and resource
bindings are added in later milestones.

GFXLINK v7 adds GPU context create/destroy and GPU submit operations. P4 GPU
contexts are dynamically allocated linked-list objects rather than another
small fixed-size object table. M4.0 submissions are validated independently by
both the kernel and firmware.

The existing KMS path still uses the 32x32 framebuffer-diff fallback from
M3.2b. Small framebuffer updates are written directly into the persistent
remote scanout texture with `TEXTURE_WRITE_RECT`, followed by one `PRESENT`.

## Build

Install the headers for the running kernel so that
`/lib/modules/$(uname -r)/build` exists, then run:

```sh
make module
```

The top-level Makefile checks the running kernel's generated config. If the
kernel was built with Clang, it automatically passes `LLVM=1` to Kbuild.

`make` also builds `grape-gpu-smoke`, a userspace M4.0 render-node test.

## Test

Flash matching GFXLINK v7 firmware first, then load the module:

```sh
sudo insmod kernel/grape_drm.ko
sudo dmesg | tail -n 50
ls -l /dev/dri
```

The connect log should include:

```text
M4.0 render node + GPU submit ready
```

There should be a GRAPE `renderD*` node. Run:

```sh
./grape-gpu-smoke
```

The tool auto-detects the GRAPE render node. It queries the UAPI, creates a GPU
context, creates and mmaps a 4 KiB GEM object, submits 64 NOP commands, waits for
the P4 to validate/complete the submission, and destroys the context.

You can also specify the render node explicitly:

```sh
./grape-gpu-smoke /dev/dri/renderD129
```

The KMS/display side remains available through the primary node and can still be
inspected with:

```sh
modetest -M grape -c
modetest -M grape -p
```

The kernel module source in this directory is GPL-2.0-only. The userspace
portion of the repository remains under the repository's top-level license.
