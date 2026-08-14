#pragma once

#include <linux/types.h>

#ifndef __KERNEL__
#include <sys/ioctl.h>
#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE 'd'
#endif
#ifndef DRM_COMMAND_BASE
#define DRM_COMMAND_BASE 0x40
#endif
#ifndef DRM_IOWR
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)
#endif
#endif

#define GRAPE_DRM_UAPI_VERSION 1u

#define GRAPE_DRM_PARAM_UAPI_VERSION 0u
#define GRAPE_DRM_PARAM_GFXLINK_VERSION 1u
#define GRAPE_DRM_PARAM_GFXLINK_CAPABILITIES 2u
#define GRAPE_DRM_PARAM_MAX_COMMAND_BYTES 3u
#define GRAPE_DRM_PARAM_COMPLETED_SEQNO 4u

#define GRAPE_DRM_GEM_FLAG_NONE 0u
#define GRAPE_DRM_CONTEXT_FLAG_NONE 0u
#define GRAPE_DRM_SUBMIT_FLAG_NONE 0u
#define GRAPE_DRM_WAIT_FLAG_NONE 0u

struct drm_grape_get_param {
    __u32 param;
    __u32 pad;
    __u64 value;
};

struct drm_grape_context_create {
    __u32 flags;
    __u32 context_id;
};

struct drm_grape_context_destroy {
    __u32 context_id;
    __u32 pad;
};

struct drm_grape_gem_create {
    __u64 size;
    __u32 flags;
    __u32 handle;
    __u64 mmap_offset;
};

struct drm_grape_submit {
    __u32 context_id;
    __u32 flags;
    __u64 commands;
    __u32 command_size;
    __u32 pad;
    __u64 seqno;
};

struct drm_grape_wait {
    __u64 seqno;
    __s64 timeout_ns;
    __s32 status;
    __u32 flags;
};

#define DRM_GRAPE_GET_PARAM 0x00
#define DRM_GRAPE_CONTEXT_CREATE 0x01
#define DRM_GRAPE_CONTEXT_DESTROY 0x02
#define DRM_GRAPE_GEM_CREATE 0x03
#define DRM_GRAPE_SUBMIT 0x04
#define DRM_GRAPE_WAIT 0x05

#define DRM_IOCTL_GRAPE_GET_PARAM \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_GRAPE_GET_PARAM, struct drm_grape_get_param)
#define DRM_IOCTL_GRAPE_CONTEXT_CREATE \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_GRAPE_CONTEXT_CREATE, struct drm_grape_context_create)
#define DRM_IOCTL_GRAPE_CONTEXT_DESTROY \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_GRAPE_CONTEXT_DESTROY, struct drm_grape_context_destroy)
#define DRM_IOCTL_GRAPE_GEM_CREATE \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_GRAPE_GEM_CREATE, struct drm_grape_gem_create)
#define DRM_IOCTL_GRAPE_SUBMIT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_GRAPE_SUBMIT, struct drm_grape_submit)
#define DRM_IOCTL_GRAPE_WAIT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_GRAPE_WAIT, struct drm_grape_wait)
