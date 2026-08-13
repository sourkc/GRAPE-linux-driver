// SPDX-License-Identifier: GPL-2.0-only

#include <linux/types.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_plane.h>
#include <drm/drm_probe_helper.h>

#include "grape/gfxlink_protocol.h"

#define GRAPE_DRM_NAME "grape_drm"
#define GRAPE_DRM_DESC "GRAPE GFXLINK DRM/KMS driver"
#define GRAPE_DRM_USB_TIMEOUT_MS 3000

struct grape_drm_device {
    struct drm_device drm;
    struct usb_device *udev;
    struct usb_interface *interface;
    struct mutex io_lock;
    u8 ep_in;
    u8 ep_out;
    u32 sequence;
    bool disconnected;

    u8 protocol_version;
    u32 capabilities;
    u32 display_width;
    u32 display_height;
    u32 pixel_format;

    struct drm_plane primary_plane;
    struct drm_crtc crtc;
    struct drm_encoder encoder;
    struct drm_connector connector;
    struct drm_display_mode mode;
};

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

static int grape_gfxlink_request(struct grape_drm_device *gdev,
                                 u8 opcode,
                                 const void *payload,
                                 u32 payload_size,
                                 void *response,
                                 u32 response_capacity,
                                 u32 *response_size)
{
    size_t tx_size = sizeof(gfxlink_header_t) + payload_size;
    const size_t rx_capacity = sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD;
    gfxlink_header_t *tx_header;
    gfxlink_header_t *rx_header;
    u8 *tx = NULL;
    u8 *rx = NULL;
    u32 sequence;
    u32 incoming_size;
    int actual;
    int ret;

    if (!response_size || payload_size > GFXLINK_MAX_PAYLOAD ||
        (payload_size && !payload) || (response_capacity && !response))
        return -EINVAL;

    tx = kmalloc(tx_size, GFP_KERNEL);
    rx = kmalloc(rx_capacity, GFP_KERNEL);
    if (!tx || !rx) {
        ret = -ENOMEM;
        goto out;
    }

    mutex_lock(&gdev->io_lock);
    if (gdev->disconnected) {
        ret = -ENODEV;
        goto out_unlock;
    }

    sequence = gdev->sequence;
    tx_header = (gfxlink_header_t *)tx;
    tx_header->magic = cpu_to_le32(GFXLINK_MAGIC);
    tx_header->version = GFXLINK_PROTOCOL_VERSION;
    tx_header->opcode = opcode;
    tx_header->flags = cpu_to_le16(0);
    tx_header->sequence = cpu_to_le32(sequence);
    tx_header->payload_size = cpu_to_le32(payload_size);
    if (payload_size)
        memcpy(tx + sizeof(*tx_header), payload, payload_size);

    actual = 0;
    ret = usb_bulk_msg(gdev->udev,
                       usb_sndbulkpipe(gdev->udev, gdev->ep_out & USB_ENDPOINT_NUMBER_MASK),
                       tx, tx_size, &actual, GRAPE_DRM_USB_TIMEOUT_MS);
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
                       rx, rx_capacity, &actual, GRAPE_DRM_USB_TIMEOUT_MS);
    if (ret)
        goto out_unlock;
    if (actual < sizeof(*rx_header)) {
        ret = -EPROTO;
        goto out_unlock;
    }

    rx_header = (gfxlink_header_t *)rx;
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
        memcpy(response, rx + sizeof(*rx_header), incoming_size);
    *response_size = incoming_size;
    ret = 0;

out_unlock:
    mutex_unlock(&gdev->io_lock);
out:
    kfree(rx);
    kfree(tx);
    return ret;
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
        return -EREMOTEIO;
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
        return -EREMOTEIO;

    gdev->display_width = le32_to_cpu((__le32)response.display_width);
    gdev->display_height = le32_to_cpu((__le32)response.display_height);
    gdev->pixel_format = le32_to_cpu((__le32)response.pixel_format);

    if (!gdev->display_width || !gdev->display_height)
        return -EINVAL;

    return 0;
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

static const struct drm_plane_funcs grape_plane_funcs = {
    .update_plane = drm_atomic_helper_update_plane,
    .disable_plane = drm_atomic_helper_disable_plane,
    .destroy = drm_plane_cleanup,
    .reset = drm_atomic_helper_plane_reset,
    .atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
    .atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const struct drm_mode_config_funcs grape_mode_config_funcs = {
    .fb_create = drm_gem_fb_create,
    .atomic_check = drm_atomic_helper_check,
    .atomic_commit = drm_atomic_helper_commit,
};

static const u32 grape_primary_formats[] = {
    DRM_FORMAT_RGB565,
    DRM_FORMAT_XRGB8888,
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
                                   NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
    if (ret)
        return ret;

    ret = drm_crtc_init_with_planes(drm, &gdev->crtc,
                                    &gdev->primary_plane, NULL,
                                    &grape_crtc_funcs, NULL);
    if (ret)
        return ret;

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
    .name = "grape",
    .desc = GRAPE_DRM_DESC,
    .major = 0,
    .minor = 3,
};

static int grape_usb_probe(struct usb_interface *interface,
                           const struct usb_device_id *id)
{
    struct usb_endpoint_descriptor *bulk_in;
    struct usb_endpoint_descriptor *bulk_out;
    struct device *dev = &interface->dev;
    struct grape_drm_device *gdev;
    struct usb_device *udev;
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
             "GRAPE GFXLINK v%u connected: %ux%u format=%u caps=0x%08x\n",
             gdev->protocol_version, gdev->display_width, gdev->display_height,
             gdev->pixel_format, gdev->capabilities);
    return 0;

err_clear:
    usb_set_intfdata(interface, NULL);
    mutex_destroy(&gdev->io_lock);
    usb_put_dev(gdev->udev);
    return ret;
}

static void grape_usb_disconnect(struct usb_interface *interface)
{
    struct grape_drm_device *gdev = usb_get_intfdata(interface);

    if (!gdev)
        return;

    usb_set_intfdata(interface, NULL);

    mutex_lock(&gdev->io_lock);
    gdev->disconnected = true;
    mutex_unlock(&gdev->io_lock);

    drm_dev_unplug(&gdev->drm);
    drm_atomic_helper_shutdown(&gdev->drm);
    usb_put_dev(gdev->udev);
    mutex_destroy(&gdev->io_lock);

    dev_info(&interface->dev, "GRAPE GFXLINK disconnected\n");
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
MODULE_VERSION("0.3.0");
