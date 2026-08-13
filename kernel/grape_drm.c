// SPDX-License-Identifier: GPL-2.0-only

#include <linux/dma-direction.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_file.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_plane.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_rect.h>

#include "grape/gfxlink_protocol.h"

#define GRAPE_DRM_NAME "grape_drm"
#define GRAPE_DRM_DESC "GRAPE GFXLINK DRM/KMS driver"
#define GRAPE_DRM_USB_TIMEOUT_MS 3000
#define GRAPE_RESOURCE_MAX_COMMIT_ATTEMPTS 4U
#define GRAPE_IO_BUFFER_SIZE (sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD)

struct grape_drm_device {
    struct drm_device drm;
    struct usb_device *udev;
    struct usb_interface *interface;
    struct mutex io_lock;
    u8 ep_in;
    u8 ep_out;
    u32 sequence;
    bool disconnected;

    u8 *tx_buf;
    u8 *rx_buf;

    u8 protocol_version;
    u32 capabilities;
    u32 display_width;
    u32 display_height;
    u32 pixel_format;

    void *scanout_pending_buf;
    void *scanout_upload_buf;
    size_t scanout_buf_size;
    struct mutex scanout_lock;
    struct work_struct scanout_work;
    bool scanout_pending;
    bool scanout_stopping;
    bool scanout_sync_initialized;
    u64 scanout_submitted;
    u64 scanout_uploaded;
    u64 scanout_dropped;
    u32 scanout_texture;
    u32 scanout_surface;

    struct drm_plane primary_plane;
    struct drm_crtc crtc;
    struct drm_encoder encoder;
    struct drm_connector connector;
    struct drm_display_mode mode;
};


static void grape_kvfree_action(void *data)
{
    kvfree(data);
}

static inline struct grape_drm_device *to_grape(struct drm_device *drm)
{
    return container_of(drm, struct grape_drm_device, drm);
}

static inline struct grape_drm_device *connector_to_grape(struct drm_connector *connector)
{
    return container_of(connector, struct grape_drm_device, connector);
}

static s32 grape_wire_s32(int32_t value)
{
    return (s32)le32_to_cpu((__le32)(u32)value);
}

static void grape_advance_sequence(struct grape_drm_device *gdev)
{
    gdev->sequence++;
    if (!gdev->sequence)
        gdev->sequence = 1;
}

static u32 grape_crc32_ieee(const u8 *data, size_t size)
{
    u32 crc = 0xffffffffU;
    size_t i;
    unsigned int bit;

    for (i = 0; i < size; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xffffffffU;
}

static bool grape_bitmap_test(const u8 *bitmap, u32 index)
{
    return (bitmap[index >> 3] & BIT(index & 7)) != 0;
}

static int grape_status_to_errno(s32 status)
{
    switch (status) {
    case GFXLINK_STATUS_OK:
        return 0;
    case GFXLINK_STATUS_INVALID_PACKET:
        return -EPROTO;
    case GFXLINK_STATUS_UNSUPPORTED:
        return -EOPNOTSUPP;
    case GFXLINK_STATUS_INVALID_ARGUMENT:
        return -EINVAL;
    case GFXLINK_STATUS_NO_MEMORY:
        return -ENOMEM;
    case GFXLINK_STATUS_NOT_FOUND:
        return -ENOENT;
    case GFXLINK_STATUS_BUSY:
        return -EBUSY;
    case GFXLINK_STATUS_INCOMPLETE:
        return -EAGAIN;
    case GFXLINK_STATUS_CHECKSUM_MISMATCH:
        return -EBADMSG;
    case GFXLINK_STATUS_INTERNAL:
    default:
        return -EREMOTEIO;
    }
}

static int grape_gfxlink_request(struct grape_drm_device *gdev,
                                 u8 opcode,
                                 const void *payload,
                                 u32 payload_size,
                                 void *response,
                                 u32 response_capacity,
                                 u32 *response_size)
{
    size_t tx_size = sizeof(gfxlink_header_t) + payload_size;
    gfxlink_header_t *tx_header;
    gfxlink_header_t *rx_header;
    u32 sequence;
    u32 incoming_size;
    int actual;
    int ret;

    if (!response_size || payload_size > GFXLINK_MAX_PAYLOAD ||
        (payload_size && !payload) || (response_capacity && !response))
        return -EINVAL;

    mutex_lock(&gdev->io_lock);
    if (gdev->disconnected) {
        ret = -ENODEV;
        goto out_unlock;
    }

    sequence = gdev->sequence;
    tx_header = (gfxlink_header_t *)gdev->tx_buf;
    tx_header->magic = cpu_to_le32(GFXLINK_MAGIC);
    tx_header->version = GFXLINK_PROTOCOL_VERSION;
    tx_header->opcode = opcode;
    tx_header->flags = cpu_to_le16(0);
    tx_header->sequence = cpu_to_le32(sequence);
    tx_header->payload_size = cpu_to_le32(payload_size);
    if (payload_size)
        memcpy(gdev->tx_buf + sizeof(*tx_header), payload, payload_size);

    actual = 0;
    ret = usb_bulk_msg(gdev->udev,
                       usb_sndbulkpipe(gdev->udev, gdev->ep_out & USB_ENDPOINT_NUMBER_MASK),
                       gdev->tx_buf, tx_size, &actual, GRAPE_DRM_USB_TIMEOUT_MS);
    if (ret)
        goto out_unlock;
    if (actual != tx_size) {
        ret = -EIO;
        goto out_unlock;
    }

    grape_advance_sequence(gdev);

    actual = 0;
    ret = usb_bulk_msg(gdev->udev,
                       usb_rcvbulkpipe(gdev->udev, gdev->ep_in & USB_ENDPOINT_NUMBER_MASK),
                       gdev->rx_buf, GRAPE_IO_BUFFER_SIZE, &actual,
                       GRAPE_DRM_USB_TIMEOUT_MS);
    if (ret)
        goto out_unlock;
    if (actual < sizeof(*rx_header)) {
        ret = -EPROTO;
        goto out_unlock;
    }

    rx_header = (gfxlink_header_t *)gdev->rx_buf;
    incoming_size = le32_to_cpu((__le32)rx_header->payload_size);
    if (le32_to_cpu((__le32)rx_header->magic) != GFXLINK_MAGIC ||
        rx_header->version != GFXLINK_PROTOCOL_VERSION ||
        rx_header->opcode != opcode ||
        !(le16_to_cpu((__le16)rx_header->flags) & GFXLINK_FLAG_RESPONSE) ||
        le32_to_cpu((__le32)rx_header->sequence) != sequence ||
        incoming_size > GFXLINK_MAX_PAYLOAD ||
        actual != sizeof(*rx_header) + incoming_size ||
        incoming_size > response_capacity) {
        ret = -EPROTO;
        goto out_unlock;
    }

    if (incoming_size)
        memcpy(response, gdev->rx_buf + sizeof(*rx_header), incoming_size);
    *response_size = incoming_size;
    ret = 0;

out_unlock:
    mutex_unlock(&gdev->io_lock);
    return ret;
}

static int grape_gfxlink_send_parts(struct grape_drm_device *gdev,
                                    u8 opcode,
                                    u16 flags,
                                    const void *part1,
                                    u32 part1_size,
                                    const void *part2,
                                    u32 part2_size)
{
    gfxlink_header_t *header;
    u32 payload_size = part1_size + part2_size;
    size_t tx_size = sizeof(*header) + payload_size;
    int actual;
    int ret;

    if (payload_size > GFXLINK_MAX_PAYLOAD ||
        (part1_size && !part1) || (part2_size && !part2))
        return -EINVAL;

    mutex_lock(&gdev->io_lock);
    if (gdev->disconnected) {
        ret = -ENODEV;
        goto out_unlock;
    }

    header = (gfxlink_header_t *)gdev->tx_buf;
    header->magic = cpu_to_le32(GFXLINK_MAGIC);
    header->version = GFXLINK_PROTOCOL_VERSION;
    header->opcode = opcode;
    header->flags = cpu_to_le16(flags);
    header->sequence = cpu_to_le32(gdev->sequence);
    header->payload_size = cpu_to_le32(payload_size);

    if (part1_size)
        memcpy(gdev->tx_buf + sizeof(*header), part1, part1_size);
    if (part2_size)
        memcpy(gdev->tx_buf + sizeof(*header) + part1_size, part2, part2_size);

    actual = 0;
    ret = usb_bulk_msg(gdev->udev,
                       usb_sndbulkpipe(gdev->udev, gdev->ep_out & USB_ENDPOINT_NUMBER_MASK),
                       gdev->tx_buf, tx_size, &actual, GRAPE_DRM_USB_TIMEOUT_MS);
    if (!ret && actual != tx_size)
        ret = -EIO;
    if (!ret)
        grape_advance_sequence(gdev);

out_unlock:
    mutex_unlock(&gdev->io_lock);
    return ret;
}

static int grape_status_request(struct grape_drm_device *gdev,
                                u8 opcode,
                                const void *payload,
                                u32 payload_size)
{
    gfxlink_status_response_t response;
    u32 size = 0;
    int ret;

    ret = grape_gfxlink_request(gdev, opcode, payload, payload_size,
                                &response, sizeof(response), &size);
    if (ret)
        return ret;
    if (size != sizeof(response))
        return -EPROTO;

    return grape_status_to_errno(grape_wire_s32(response.status));
}

static int grape_gfxlink_hello(struct grape_drm_device *gdev)
{
    gfxlink_hello_response_t response;
    u32 size = 0;
    s32 status;
    int ret;

    ret = grape_gfxlink_request(gdev, GFXLINK_OP_HELLO, NULL, 0,
                                &response, sizeof(response), &size);
    if (ret)
        return ret;
    if (size != sizeof(response))
        return -EPROTO;

    status = grape_wire_s32(response.status);
    if (status != GFXLINK_STATUS_OK)
        return grape_status_to_errno(status);
    if (response.protocol_version != GFXLINK_PROTOCOL_VERSION)
        return -EPROTO;

    gdev->protocol_version = response.protocol_version;
    gdev->capabilities = le32_to_cpu((__le32)response.capabilities);
    return 0;
}

static int grape_gfxlink_get_info(struct grape_drm_device *gdev)
{
    gfxlink_info_response_t response;
    u32 size = 0;
    s32 status;
    int ret;

    ret = grape_gfxlink_request(gdev, GFXLINK_OP_GET_INFO, NULL, 0,
                                &response, sizeof(response), &size);
    if (ret)
        return ret;
    if (size != sizeof(response))
        return -EPROTO;

    status = grape_wire_s32(response.status);
    if (status != GFXLINK_STATUS_OK)
        return grape_status_to_errno(status);

    gdev->display_width = le32_to_cpu((__le32)response.display_width);
    gdev->display_height = le32_to_cpu((__le32)response.display_height);
    gdev->pixel_format = le32_to_cpu((__le32)response.pixel_format);

    if (!gdev->display_width || !gdev->display_height)
        return -EINVAL;

    return 0;
}

static u32 grape_resource_chunk_size(u32 total_size, u32 chunk_index)
{
    u32 offset = chunk_index * GFXLINK_RESOURCE_CHUNK_SIZE;
    u32 remaining = total_size - offset;

    return min_t(u32, remaining, GFXLINK_RESOURCE_CHUNK_SIZE);
}

static int grape_resource_create(struct grape_drm_device *gdev,
                                 u32 kind,
                                 u32 total_size,
                                 u32 *out_handle,
                                 u32 *out_chunk_count)
{
    gfxlink_resource_create_request_t request = {
        .kind = cpu_to_le32(kind),
        .total_size = cpu_to_le32(total_size),
        .flags = cpu_to_le32(0),
    };
    gfxlink_resource_create_response_t response;
    u32 size = 0;
    s32 status;
    int ret;

    ret = grape_gfxlink_request(gdev, GFXLINK_OP_RESOURCE_CREATE,
                                &request, sizeof(request),
                                &response, sizeof(response), &size);
    if (ret)
        return ret;
    if (size != sizeof(response))
        return -EPROTO;

    status = grape_wire_s32(response.status);
    if (status != GFXLINK_STATUS_OK)
        return grape_status_to_errno(status);
    if (le32_to_cpu((__le32)response.chunk_size) != GFXLINK_RESOURCE_CHUNK_SIZE)
        return -EPROTO;

    *out_handle = le32_to_cpu((__le32)response.handle);
    *out_chunk_count = le32_to_cpu((__le32)response.chunk_count);
    if (!*out_handle || !*out_chunk_count ||
        *out_chunk_count > GFXLINK_RESOURCE_MAX_CHUNKS)
        return -EPROTO;

    return 0;
}

static int grape_resource_write_chunk(struct grape_drm_device *gdev,
                                      u32 handle,
                                      u32 chunk_index,
                                      const u8 *data,
                                      u32 size)
{
    gfxlink_resource_write_request_t request = {
        .handle = cpu_to_le32(handle),
        .chunk_index = cpu_to_le32(chunk_index),
        .data_size = cpu_to_le32(size),
        .crc32 = cpu_to_le32(grape_crc32_ieee(data, size)),
    };

    return grape_gfxlink_send_parts(gdev, GFXLINK_OP_RESOURCE_WRITE,
                                    GFXLINK_FLAG_NO_RESPONSE,
                                    &request, sizeof(request), data, size);
}

static int grape_resource_commit(struct grape_drm_device *gdev,
                                 u32 handle,
                                 u32 expected_crc32,
                                 gfxlink_resource_commit_response_t *out_response,
                                 s32 *out_status)
{
    gfxlink_resource_commit_request_t request = {
        .handle = cpu_to_le32(handle),
        .expected_crc32 = cpu_to_le32(expected_crc32),
    };
    u32 size = 0;
    int ret;

    ret = grape_gfxlink_request(gdev, GFXLINK_OP_RESOURCE_COMMIT,
                                &request, sizeof(request),
                                out_response, sizeof(*out_response), &size);
    if (ret)
        return ret;
    if (size != sizeof(*out_response))
        return -EPROTO;

    *out_status = grape_wire_s32(out_response->status);
    return 0;
}

static int grape_resource_destroy(struct grape_drm_device *gdev, u32 handle)
{
    gfxlink_resource_handle_request_t request = {
        .handle = cpu_to_le32(handle),
    };

    return grape_status_request(gdev, GFXLINK_OP_RESOURCE_DESTROY,
                                &request, sizeof(request));
}

static int grape_resource_resend_reported(struct grape_drm_device *gdev,
                                          u32 handle,
                                          const u8 *data,
                                          u32 total_size,
                                          u32 chunk_count,
                                          const gfxlink_resource_commit_response_t *report)
{
    u32 i;
    bool resent = false;

    for (i = 0; i < chunk_count; ++i) {
        u32 chunk_size;
        u32 offset;
        int ret;

        if (!grape_bitmap_test(report->missing_bitmap, i) &&
            !grape_bitmap_test(report->corrupt_bitmap, i))
            continue;

        chunk_size = grape_resource_chunk_size(total_size, i);
        offset = i * GFXLINK_RESOURCE_CHUNK_SIZE;
        ret = grape_resource_write_chunk(gdev, handle, i, data + offset, chunk_size);
        if (ret)
            return ret;
        resent = true;
    }

    return resent ? 0 : -EPROTO;
}

static int grape_resource_resend_all(struct grape_drm_device *gdev,
                                     u32 handle,
                                     const u8 *data,
                                     u32 total_size,
                                     u32 chunk_count)
{
    u32 i;

    for (i = 0; i < chunk_count; ++i) {
        u32 chunk_size = grape_resource_chunk_size(total_size, i);
        u32 offset = i * GFXLINK_RESOURCE_CHUNK_SIZE;
        int ret = grape_resource_write_chunk(gdev, handle, i,
                                             data + offset, chunk_size);
        if (ret)
            return ret;
    }
    return 0;
}

static int grape_resource_upload(struct grape_drm_device *gdev,
                                 u32 kind,
                                 const u8 *data,
                                 u32 size,
                                 u32 *out_handle)
{
    gfxlink_resource_commit_response_t report;
    u32 expected_chunks;
    u32 chunk_count;
    u32 handle;
    u32 expected_crc;
    u32 i;
    int ret;

    if (!data || !size || size > GFXLINK_MAX_RESOURCE_SIZE || !out_handle)
        return -EINVAL;

    ret = grape_resource_create(gdev, kind, size, &handle, &chunk_count);
    if (ret)
        return ret;

    expected_chunks = DIV_ROUND_UP(size, GFXLINK_RESOURCE_CHUNK_SIZE);
    if (chunk_count != expected_chunks) {
        ret = -EPROTO;
        goto fail_destroy;
    }

    expected_crc = grape_crc32_ieee(data, size);
    for (i = 0; i < chunk_count; ++i) {
        u32 chunk_size = grape_resource_chunk_size(size, i);
        u32 offset = i * GFXLINK_RESOURCE_CHUNK_SIZE;

        ret = grape_resource_write_chunk(gdev, handle, i,
                                         data + offset, chunk_size);
        if (ret)
            goto fail_destroy;
    }

    for (i = 0; i < GRAPE_RESOURCE_MAX_COMMIT_ATTEMPTS; ++i) {
        s32 status;
        u32 report_chunks;

        memset(&report, 0, sizeof(report));
        ret = grape_resource_commit(gdev, handle, expected_crc, &report, &status);
        if (ret)
            goto fail_destroy;

        report_chunks = le32_to_cpu((__le32)report.chunk_count);
        if (report_chunks != chunk_count) {
            ret = -EPROTO;
            goto fail_destroy;
        }

        if (status == GFXLINK_STATUS_OK) {
            if (le32_to_cpu((__le32)report.resource_crc32) != expected_crc) {
                ret = -EPROTO;
                goto fail_destroy;
            }
            *out_handle = handle;
            return 0;
        }

        if (status == GFXLINK_STATUS_INCOMPLETE) {
            ret = grape_resource_resend_reported(gdev, handle, data, size,
                                                 chunk_count, &report);
            if (ret)
                goto fail_destroy;
            continue;
        }

        if (status == GFXLINK_STATUS_CHECKSUM_MISMATCH) {
            ret = grape_resource_resend_all(gdev, handle, data, size, chunk_count);
            if (ret)
                goto fail_destroy;
            continue;
        }

        ret = grape_status_to_errno(status);
        goto fail_destroy;
    }

    ret = -EIO;

fail_destroy:
    grape_resource_destroy(gdev, handle);
    return ret;
}

static int grape_remote_texture_create(struct grape_drm_device *gdev,
                                       u32 width,
                                       u32 height,
                                       u32 *out_handle)
{
    gfxlink_texture_create_request_t request = {
        .width = cpu_to_le32(width),
        .height = cpu_to_le32(height),
        .format = cpu_to_le32(GFXLINK_PIXEL_FORMAT_RGB565),
        .resource_handle = cpu_to_le32(0),
    };
    gfxlink_texture_create_response_t response;
    u32 size = 0;
    s32 status;
    int ret;

    ret = grape_gfxlink_request(gdev, GFXLINK_OP_TEXTURE_CREATE,
                                &request, sizeof(request),
                                &response, sizeof(response), &size);
    if (ret)
        return ret;
    if (size != sizeof(response))
        return -EPROTO;

    status = grape_wire_s32(response.status);
    if (status != GFXLINK_STATUS_OK)
        return grape_status_to_errno(status);

    *out_handle = le32_to_cpu((__le32)response.handle);
    return *out_handle ? 0 : -EPROTO;
}

static int grape_remote_texture_update(struct grape_drm_device *gdev,
                                       u32 texture,
                                       u32 resource,
                                       u32 x,
                                       u32 y,
                                       u32 width,
                                       u32 height)
{
    gfxlink_texture_update_request_t request = {
        .texture_handle = cpu_to_le32(texture),
        .resource_handle = cpu_to_le32(resource),
        .x = cpu_to_le32(x),
        .y = cpu_to_le32(y),
        .width = cpu_to_le32(width),
        .height = cpu_to_le32(height),
    };

    return grape_status_request(gdev, GFXLINK_OP_TEXTURE_UPDATE,
                                &request, sizeof(request));
}

static int grape_remote_texture_destroy(struct grape_drm_device *gdev, u32 texture)
{
    gfxlink_texture_handle_request_t request = {
        .handle = cpu_to_le32(texture),
    };

    return grape_status_request(gdev, GFXLINK_OP_TEXTURE_DESTROY,
                                &request, sizeof(request));
}

static int grape_remote_surface_create(struct grape_drm_device *gdev,
                                       u32 texture,
                                       u32 *out_handle)
{
    gfxlink_create_surface_request_t request = {
        .texture_handle = cpu_to_le32(texture),
    };
    gfxlink_create_surface_response_t response;
    u32 size = 0;
    s32 status;
    int ret;

    ret = grape_gfxlink_request(gdev, GFXLINK_OP_CREATE_SURFACE,
                                &request, sizeof(request),
                                &response, sizeof(response), &size);
    if (ret)
        return ret;
    if (size != sizeof(response))
        return -EPROTO;

    status = grape_wire_s32(response.status);
    if (status != GFXLINK_STATUS_OK)
        return grape_status_to_errno(status);

    *out_handle = le32_to_cpu((__le32)response.handle);
    return *out_handle ? 0 : -EPROTO;
}

static int grape_remote_surface_destroy(struct grape_drm_device *gdev, u32 surface)
{
    gfxlink_destroy_surface_request_t request = {
        .handle = cpu_to_le32(surface),
    };

    return grape_status_request(gdev, GFXLINK_OP_DESTROY_SURFACE,
                                &request, sizeof(request));
}

static int grape_remote_present(struct grape_drm_device *gdev)
{
    return grape_status_request(gdev, GFXLINK_OP_PRESENT, NULL, 0);
}

static int grape_scanout_ensure(struct grape_drm_device *gdev)
{
    u32 texture = 0;
    u32 surface = 0;
    int ret;

    if (gdev->scanout_texture && gdev->scanout_surface)
        return 0;

    ret = grape_remote_texture_create(gdev, gdev->display_width,
                                      gdev->display_height, &texture);
    if (ret)
        return ret;

    ret = grape_remote_surface_create(gdev, texture, &surface);
    if (ret) {
        grape_remote_texture_destroy(gdev, texture);
        return ret;
    }

    gdev->scanout_texture = texture;
    gdev->scanout_surface = surface;
    dev_info(&gdev->interface->dev,
             "remote scanout created: texture=%u surface=%u\n",
             texture, surface);
    return 0;
}

static void grape_scanout_cleanup_remote(struct grape_drm_device *gdev)
{
    if (gdev->scanout_surface) {
        grape_remote_surface_destroy(gdev, gdev->scanout_surface);
        gdev->scanout_surface = 0;
    }
    if (gdev->scanout_texture) {
        grape_remote_texture_destroy(gdev, gdev->scanout_texture);
        gdev->scanout_texture = 0;
    }
}

static int grape_scanout_upload(struct grape_drm_device *gdev,
                                const u8 *pixels,
                                u32 size)
{
    u32 resource = 0;
    int ret;
    int cleanup_ret;

    ret = grape_scanout_ensure(gdev);
    if (ret)
        return ret;

    ret = grape_resource_upload(gdev, GFXLINK_RESOURCE_TEXTURE,
                                pixels, size, &resource);
    if (ret)
        return ret;

    ret = grape_remote_texture_update(gdev, gdev->scanout_texture, resource,
                                      0, 0, gdev->display_width,
                                      gdev->display_height);
    cleanup_ret = grape_resource_destroy(gdev, resource);
    if (ret)
        return ret;
    if (cleanup_ret)
        return cleanup_ret;

    return grape_remote_present(gdev);
}

static int grape_fb_to_rgb565(struct grape_drm_device *gdev,
                              struct drm_framebuffer *fb,
                              const struct iosys_map *src,
                              void *dst_pixels)
{
    struct drm_rect rect = DRM_RECT_INIT(0, 0, fb->width, fb->height);
    struct iosys_map dst;

    if (fb->width != gdev->display_width ||
        fb->height != gdev->display_height ||
        !dst_pixels)
        return -EINVAL;

    iosys_map_set_vaddr(&dst, dst_pixels);

    switch (fb->format->format) {
    case DRM_FORMAT_RGB565:
        drm_fb_memcpy(&dst, NULL, src, fb, &rect);
        return 0;

    case DRM_FORMAT_XRGB8888: {
        struct drm_format_conv_state state = DRM_FORMAT_CONV_STATE_INIT;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
        drm_fb_xrgb8888_to_rgb565(&dst, NULL, src, fb, &rect, &state);
#else
        drm_fb_xrgb8888_to_rgb565(&dst, NULL, src, fb, &rect, &state, false);
#endif
        drm_format_conv_state_release(&state);
        return 0;
    }

    default:
        return -EINVAL;
    }
}

static void grape_scanout_work(struct work_struct *work)
{
    struct grape_drm_device *gdev =
        container_of(work, struct grape_drm_device, scanout_work);
    void *upload_buf;
    int idx;
    int ret;

    if (!drm_dev_enter(&gdev->drm, &idx))
        return;

    for (;;) {
        mutex_lock(&gdev->scanout_lock);
        if (gdev->scanout_stopping || !gdev->scanout_pending) {
            mutex_unlock(&gdev->scanout_lock);
            break;
        }

        upload_buf = gdev->scanout_upload_buf;
        gdev->scanout_upload_buf = gdev->scanout_pending_buf;
        gdev->scanout_pending_buf = upload_buf;
        upload_buf = gdev->scanout_upload_buf;
        gdev->scanout_pending = false;
        mutex_unlock(&gdev->scanout_lock);

        ret = grape_scanout_upload(gdev, upload_buf,
                                   (u32)gdev->scanout_buf_size);
        if (ret) {
            if (ret != -ENODEV && ret != -ECONNRESET &&
                ret != -ESHUTDOWN && ret != -ETIMEDOUT)
                dev_err_ratelimited(&gdev->interface->dev,
                                    "framebuffer upload failed: %d\n", ret);
            break;
        }

        mutex_lock(&gdev->scanout_lock);
        gdev->scanout_uploaded++;
        mutex_unlock(&gdev->scanout_lock);
    }

    drm_dev_exit(idx);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
static void grape_plane_atomic_update(struct drm_plane *plane,
                                      struct drm_atomic_commit *state)
#else
static void grape_plane_atomic_update(struct drm_plane *plane,
                                      struct drm_atomic_state *state)
#endif
{
    struct grape_drm_device *gdev = to_grape(plane->dev);
    struct drm_plane_state *plane_state = plane->state;
    struct drm_shadow_plane_state *shadow_state;
    struct drm_framebuffer *fb;
    int idx;
    int ret;

    (void)state;

    if (!plane_state || !plane_state->crtc || !plane_state->fb)
        return;

    fb = plane_state->fb;
    shadow_state = to_drm_shadow_plane_state(plane_state);

    if (!drm_dev_enter(&gdev->drm, &idx))
        return;

    ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
    if (ret)
        goto out;

    /*
     * Never perform USB I/O from the atomic commit path. Convert/copy the
     * newest framebuffer into the mailbox buffer, then let the worker do the
     * slow GFXLINK transfer. If another frame arrives while one is in flight,
     * it simply replaces the pending mailbox image.
     */
    mutex_lock(&gdev->scanout_lock);
    if (gdev->scanout_stopping) {
        mutex_unlock(&gdev->scanout_lock);
        drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
        goto out;
    }

    if (gdev->scanout_pending)
        gdev->scanout_dropped++;

    ret = grape_fb_to_rgb565(gdev, fb, &shadow_state->data[0],
                             gdev->scanout_pending_buf);
    if (!ret) {
        gdev->scanout_pending = true;
        gdev->scanout_submitted++;
    }
    mutex_unlock(&gdev->scanout_lock);

    drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
    if (!ret)
        queue_work(system_long_wq, &gdev->scanout_work);

out:
    drm_dev_exit(idx);
}

static enum drm_connector_status
grape_connector_detect(struct drm_connector *connector, bool force)
{
    struct grape_drm_device *gdev = connector_to_grape(connector);

    (void)force;
    return READ_ONCE(gdev->disconnected) ?
           connector_status_disconnected : connector_status_connected;
}

static int grape_connector_get_modes(struct drm_connector *connector)
{
    struct grape_drm_device *gdev = connector_to_grape(connector);
    struct drm_display_mode *mode;

    mode = drm_mode_duplicate(connector->dev, &gdev->mode);
    if (!mode)
        return 0;

    drm_mode_probed_add(connector, mode);
    return 1;
}

static const struct drm_connector_helper_funcs grape_connector_helper_funcs = {
    .get_modes = grape_connector_get_modes,
};

static const struct drm_connector_funcs grape_connector_funcs = {
    .reset = drm_atomic_helper_connector_reset,
    .detect = grape_connector_detect,
    .fill_modes = drm_helper_probe_single_connector_modes,
    .destroy = drm_connector_cleanup,
    .atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
    .atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_encoder_funcs grape_encoder_funcs = {
    .destroy = drm_encoder_cleanup,
};

static const struct drm_crtc_funcs grape_crtc_funcs = {
    .reset = drm_atomic_helper_crtc_reset,
    .set_config = drm_atomic_helper_set_config,
    .page_flip = drm_atomic_helper_page_flip,
    .destroy = drm_crtc_cleanup,
    .atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
    .atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs grape_crtc_helper_funcs = {
    .atomic_check = drm_crtc_helper_atomic_check,
};

static const struct drm_plane_helper_funcs grape_plane_helper_funcs = {
    DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
    .atomic_update = grape_plane_atomic_update,
};

static const struct drm_plane_funcs grape_plane_funcs = {
    .update_plane = drm_atomic_helper_update_plane,
    .disable_plane = drm_atomic_helper_disable_plane,
    .destroy = drm_plane_cleanup,
    DRM_GEM_SHADOW_PLANE_FUNCS,
};

static const struct drm_mode_config_funcs grape_mode_config_funcs = {
    .fb_create = drm_gem_fb_create_with_dirty,
    .atomic_check = drm_atomic_helper_check,
    .atomic_commit = drm_atomic_helper_commit,
};

static const u32 grape_primary_formats[] = {
    DRM_FORMAT_RGB565,
    DRM_FORMAT_XRGB8888,
};

static const u64 grape_primary_modifiers[] = {
    DRM_FORMAT_MOD_LINEAR,
    DRM_FORMAT_MOD_INVALID,
};

static int grape_modeset_init(struct grape_drm_device *gdev)
{
    struct drm_device *drm = &gdev->drm;
    int ret;

    ret = drmm_mode_config_init(drm);
    if (ret)
        return ret;

    drm->mode_config.min_width = gdev->display_width;
    drm->mode_config.max_width = gdev->display_width;
    drm->mode_config.min_height = gdev->display_height;
    drm->mode_config.max_height = gdev->display_height;
    drm->mode_config.preferred_depth = 16;
    drm->mode_config.funcs = &grape_mode_config_funcs;

    ret = drm_universal_plane_init(drm, &gdev->primary_plane, 0,
                                   &grape_plane_funcs,
                                   grape_primary_formats,
                                   ARRAY_SIZE(grape_primary_formats),
                                   grape_primary_modifiers,
                                   DRM_PLANE_TYPE_PRIMARY, NULL);
    if (ret)
        return ret;

    drm_plane_helper_add(&gdev->primary_plane, &grape_plane_helper_funcs);
    drm_plane_enable_fb_damage_clips(&gdev->primary_plane);

    ret = drm_crtc_init_with_planes(drm, &gdev->crtc,
                                    &gdev->primary_plane, NULL,
                                    &grape_crtc_funcs, NULL);
    if (ret)
        return ret;
    drm_crtc_helper_add(&gdev->crtc, &grape_crtc_helper_funcs);

    ret = drm_encoder_init(drm, &gdev->encoder, &grape_encoder_funcs,
                           DRM_MODE_ENCODER_NONE, NULL);
    if (ret)
        return ret;
    gdev->encoder.possible_crtcs = drm_crtc_mask(&gdev->crtc);

    ret = drm_connector_init(drm, &gdev->connector, &grape_connector_funcs,
                             DRM_MODE_CONNECTOR_USB);
    if (ret)
        return ret;
    drm_connector_helper_add(&gdev->connector, &grape_connector_helper_funcs);

    ret = drm_connector_attach_encoder(&gdev->connector, &gdev->encoder);
    if (ret)
        return ret;

    memset(&gdev->mode, 0, sizeof(gdev->mode));
    gdev->mode.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    gdev->mode.clock = (gdev->display_width * gdev->display_height * 60U) / 1000U;
    if (!gdev->mode.clock)
        gdev->mode.clock = 1;
    gdev->mode.hdisplay = gdev->display_width;
    gdev->mode.hsync_start = gdev->display_width;
    gdev->mode.hsync_end = gdev->display_width;
    gdev->mode.htotal = gdev->display_width;
    gdev->mode.vdisplay = gdev->display_height;
    gdev->mode.vsync_start = gdev->display_height;
    gdev->mode.vsync_end = gdev->display_height;
    gdev->mode.vtotal = gdev->display_height;
    drm_mode_set_name(&gdev->mode);

    drm_mode_config_reset(drm);
    return 0;
}

DEFINE_DRM_GEM_FOPS(grape_drm_fops);

static const struct drm_driver grape_drm_driver = {
    .driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
    .fops = &grape_drm_fops,
    DRM_GEM_SHMEM_DRIVER_OPS,
    .name = "grape",
    .desc = GRAPE_DRM_DESC,
    .major = 0,
    .minor = 4,
    .patchlevel = 1,
};

static int grape_usb_probe(struct usb_interface *interface,
                           const struct usb_device_id *id)
{
    struct usb_endpoint_descriptor *bulk_in;
    struct usb_endpoint_descriptor *bulk_out;
    struct device *dev = &interface->dev;
    struct grape_drm_device *gdev;
    struct usb_device *udev;
    size_t scanout_size;
    int ret;

    (void)id;

    if (interface->cur_altsetting->desc.bInterfaceNumber != GFXLINK_USB_INTERFACE)
        return -ENODEV;

    ret = usb_find_common_endpoints(interface->cur_altsetting,
                                    &bulk_in, &bulk_out, NULL, NULL);
    if (ret)
        return ret;

    if (bulk_in->bEndpointAddress != GFXLINK_USB_EP_IN ||
        bulk_out->bEndpointAddress != GFXLINK_USB_EP_OUT) {
        dev_err(dev, "unexpected GFXLINK endpoints IN=0x%02x OUT=0x%02x\n",
                bulk_in->bEndpointAddress, bulk_out->bEndpointAddress);
        return -ENODEV;
    }

    gdev = devm_drm_dev_alloc(dev, &grape_drm_driver,
                              struct grape_drm_device, drm);
    if (IS_ERR(gdev))
        return PTR_ERR(gdev);

    gdev->tx_buf = devm_kmalloc(dev, GRAPE_IO_BUFFER_SIZE, GFP_KERNEL);
    gdev->rx_buf = devm_kmalloc(dev, GRAPE_IO_BUFFER_SIZE, GFP_KERNEL);
    if (!gdev->tx_buf || !gdev->rx_buf)
        return -ENOMEM;

    udev = interface_to_usbdev(interface);
    gdev->udev = usb_get_dev(udev);
    gdev->interface = interface;
    gdev->ep_in = bulk_in->bEndpointAddress;
    gdev->ep_out = bulk_out->bEndpointAddress;
    gdev->sequence = 1;
    mutex_init(&gdev->io_lock);

    usb_set_intfdata(interface, gdev);

    ret = grape_gfxlink_hello(gdev);
    if (ret) {
        dev_err(dev, "GFXLINK HELLO failed: %d\n", ret);
        goto err_clear;
    }

    ret = grape_gfxlink_get_info(gdev);
    if (ret) {
        dev_err(dev, "GFXLINK GET_INFO failed: %d\n", ret);
        goto err_clear;
    }

    if (!(gdev->capabilities & GFXLINK_CAP_RESOURCE_STREAM) ||
        !(gdev->capabilities & GFXLINK_CAP_RELIABLE_RESOURCE_STREAM) ||
        !(gdev->capabilities & GFXLINK_CAP_TEXTURES) ||
        !(gdev->capabilities & GFXLINK_CAP_TEXTURE_UPDATE) ||
        !(gdev->capabilities & GFXLINK_CAP_EXPLICIT_PRESENT)) {
        dev_err(dev, "firmware lacks M3.1 scanout capabilities (caps=0x%08x)\n",
                gdev->capabilities);
        ret = -EOPNOTSUPP;
        goto err_clear;
    }

    if (gdev->display_width > SIZE_MAX / 2 ||
        gdev->display_height > SIZE_MAX / ((size_t)gdev->display_width * 2)) {
        ret = -EOVERFLOW;
        goto err_clear;
    }
    scanout_size = (size_t)gdev->display_width * gdev->display_height * 2;
    if (scanout_size > GFXLINK_MAX_RESOURCE_SIZE) {
        ret = -E2BIG;
        goto err_clear;
    }

    gdev->scanout_pending_buf = kvzalloc(scanout_size, GFP_KERNEL);
    if (!gdev->scanout_pending_buf) {
        ret = -ENOMEM;
        goto err_clear;
    }
    ret = devm_add_action_or_reset(dev, grape_kvfree_action,
                                   gdev->scanout_pending_buf);
    if (ret)
        goto err_clear;

    gdev->scanout_upload_buf = kvzalloc(scanout_size, GFP_KERNEL);
    if (!gdev->scanout_upload_buf) {
        ret = -ENOMEM;
        goto err_clear;
    }
    ret = devm_add_action_or_reset(dev, grape_kvfree_action,
                                   gdev->scanout_upload_buf);
    if (ret)
        goto err_clear;

    gdev->scanout_buf_size = scanout_size;
    mutex_init(&gdev->scanout_lock);
    INIT_WORK(&gdev->scanout_work, grape_scanout_work);
    gdev->scanout_sync_initialized = true;

    ret = grape_modeset_init(gdev);
    if (ret) {
        dev_err(dev, "DRM modeset init failed: %d\n", ret);
        goto err_clear;
    }

    ret = drm_dev_register(&gdev->drm, 0);
    if (ret) {
        dev_err(dev, "DRM registration failed: %d\n", ret);
        goto err_clear;
    }

    dev_info(dev,
             "GRAPE GFXLINK v%u connected: %ux%u format=%u caps=0x%08x; asynchronous dumb scanout ready\n",
             gdev->protocol_version, gdev->display_width, gdev->display_height,
             gdev->pixel_format, gdev->capabilities);
    return 0;

err_clear:
    usb_set_intfdata(interface, NULL);
    if (gdev->scanout_sync_initialized) {
        mutex_lock(&gdev->scanout_lock);
        gdev->scanout_stopping = true;
        mutex_unlock(&gdev->scanout_lock);
        cancel_work_sync(&gdev->scanout_work);
        mutex_destroy(&gdev->scanout_lock);
    }
    mutex_destroy(&gdev->io_lock);
    usb_put_dev(gdev->udev);
    return ret;
}

static void grape_usb_disconnect(struct usb_interface *interface)
{
    struct grape_drm_device *gdev = usb_get_intfdata(interface);
    bool physically_gone;

    if (!gdev)
        return;

    usb_set_intfdata(interface, NULL);
    physically_gone = gdev->udev->state == USB_STATE_NOTATTACHED;

    /*
     * Stop accepting new mailbox frames first. cancel_work_sync() may wait
     * for one already-running USB transaction, but userspace/KWin is no
     * longer blocked on that transaction because all scanout I/O lives here.
     */
    mutex_lock(&gdev->scanout_lock);
    gdev->scanout_stopping = true;
    gdev->scanout_pending = false;
    mutex_unlock(&gdev->scanout_lock);

    if (physically_gone)
        WRITE_ONCE(gdev->disconnected, true);

    cancel_work_sync(&gdev->scanout_work);

    drm_dev_unplug(&gdev->drm);
    drm_atomic_helper_shutdown(&gdev->drm);

    if (!physically_gone)
        grape_scanout_cleanup_remote(gdev);

    WRITE_ONCE(gdev->disconnected, true);

    dev_info(&interface->dev,
             "GRAPE GFXLINK disconnected (submitted=%llu uploaded=%llu dropped=%llu)\n",
             gdev->scanout_submitted, gdev->scanout_uploaded,
             gdev->scanout_dropped);

    mutex_destroy(&gdev->scanout_lock);
    mutex_destroy(&gdev->io_lock);
    usb_put_dev(gdev->udev);
}

static const struct usb_device_id grape_usb_ids[] = {
    { USB_DEVICE(GFXLINK_USB_VID, GFXLINK_USB_PID) },
    { }
};
MODULE_DEVICE_TABLE(usb, grape_usb_ids);

static struct usb_driver grape_usb_driver = {
    .name = GRAPE_DRM_NAME,
    .probe = grape_usb_probe,
    .disconnect = grape_usb_disconnect,
    .id_table = grape_usb_ids,
};

module_usb_driver(grape_usb_driver);

MODULE_AUTHOR("GRAPE project");
MODULE_DESCRIPTION(GRAPE_DRM_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION("0.3.1a");
