#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "grape/gfxlink_protocol.h"
#include "grape/grape_drm.h"

static int get_param(int fd, uint32_t param, uint64_t *value)
{
    struct drm_grape_get_param args = {
        .param = param,
    };

    if (ioctl(fd, DRM_IOCTL_GRAPE_GET_PARAM, &args) < 0)
        return -1;
    *value = args.value;
    return 0;
}

static int open_grape_render_node(char *path, size_t path_size)
{
    for (unsigned int minor = 128; minor < 192; ++minor) {
        uint64_t version = 0;
        int fd;

        snprintf(path, path_size, "/dev/dri/renderD%u", minor);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        if (get_param(fd, GRAPE_DRM_PARAM_UAPI_VERSION, &version) == 0 &&
            version == GRAPE_DRM_UAPI_VERSION)
            return fd;

        close(fd);
    }

    errno = ENODEV;
    return -1;
}

int main(int argc, char **argv)
{
    char detected_path[64];
    const char *path = NULL;
    uint64_t uapi = 0, protocol = 0, caps = 0, max_commands = 0;
    struct drm_grape_context_create context = {0};
    struct drm_grape_gem_create gem = {
        .size = 4096,
    };
    gfxlink_gpu_command_header_t commands[64];
    struct drm_grape_submit submit = {0};
    struct drm_grape_wait wait = {0};
    struct drm_grape_context_destroy destroy = {0};
    uint8_t *mapping = MAP_FAILED;
    int fd = -1;
    int result = 1;

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [/dev/dri/renderDNNN]\n", argv[0]);
        return 2;
    }

    if (argc == 2) {
        path = argv[1];
        fd = open(path, O_RDWR | O_CLOEXEC);
    } else {
        fd = open_grape_render_node(detected_path, sizeof(detected_path));
        path = detected_path;
    }

    if (fd < 0) {
        fprintf(stderr, "Unable to open GRAPE render node: %s\n", strerror(errno));
        return 1;
    }

    if (get_param(fd, GRAPE_DRM_PARAM_UAPI_VERSION, &uapi) < 0 ||
        get_param(fd, GRAPE_DRM_PARAM_GFXLINK_VERSION, &protocol) < 0 ||
        get_param(fd, GRAPE_DRM_PARAM_GFXLINK_CAPABILITIES, &caps) < 0 ||
        get_param(fd, GRAPE_DRM_PARAM_MAX_COMMAND_BYTES, &max_commands) < 0) {
        fprintf(stderr, "%s is not an M4.0 GRAPE render node: %s\n",
                path, strerror(errno));
        goto out;
    }

    printf("render node: %s\n", path);
    printf("UAPI v%" PRIu64 ", GFXLINK v%" PRIu64
           ", caps=0x%08" PRIx64 ", max_command_bytes=%" PRIu64 "\n",
           uapi, protocol, caps, max_commands);

    if (ioctl(fd, DRM_IOCTL_GRAPE_CONTEXT_CREATE, &context) < 0) {
        fprintf(stderr, "CONTEXT_CREATE failed: %s\n", strerror(errno));
        goto out;
    }
    printf("context: %u\n", context.context_id);

    if (ioctl(fd, DRM_IOCTL_GRAPE_GEM_CREATE, &gem) < 0) {
        fprintf(stderr, "GEM_CREATE failed: %s\n", strerror(errno));
        goto out_context;
    }
    printf("GEM: handle=%u size=%" PRIu64 " mmap_offset=0x%" PRIx64 "\n",
           gem.handle, (uint64_t)gem.size, (uint64_t)gem.mmap_offset);

    mapping = mmap(NULL, gem.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)gem.mmap_offset);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "GEM mmap failed: %s\n", strerror(errno));
        goto out_context;
    }

    for (size_t i = 0; i < 64; ++i)
        mapping[i] = (uint8_t)(i ^ 0xA5U);
    printf("GEM mmap: writable host buffer verified\n");

    memset(commands, 0, sizeof(commands));
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        commands[i].opcode = GFXLINK_GPU_CMD_NOP;
        commands[i].size = sizeof(commands[i]);
        commands[i].flags = 0;
    }

    submit.context_id = context.context_id;
    submit.commands = (uint64_t)(uintptr_t)commands;
    submit.command_size = sizeof(commands);
    if (ioctl(fd, DRM_IOCTL_GRAPE_SUBMIT, &submit) < 0) {
        fprintf(stderr, "SUBMIT failed: %s\n", strerror(errno));
        goto out_context;
    }
    printf("submitted 64 NOP commands: seqno=%" PRIu64 "\n",
           (uint64_t)submit.seqno);

    wait.seqno = submit.seqno;
    wait.timeout_ns = 2LL * 1000LL * 1000LL * 1000LL;
    if (ioctl(fd, DRM_IOCTL_GRAPE_WAIT, &wait) < 0) {
        fprintf(stderr, "WAIT failed: %s\n", strerror(errno));
        goto out_context;
    }
    if (wait.status != 0) {
        fprintf(stderr, "GPU submission completed with status %d\n", wait.status);
        goto out_context;
    }

    printf("submission completed successfully on the P4\n");
    result = 0;

out_context:
    if (mapping != MAP_FAILED)
        munmap(mapping, gem.size);
    destroy.context_id = context.context_id;
    if (context.context_id && ioctl(fd, DRM_IOCTL_GRAPE_CONTEXT_DESTROY, &destroy) < 0)
        fprintf(stderr, "CONTEXT_DESTROY failed: %s\n", strerror(errno));
out:
    close(fd);
    return result;
}
