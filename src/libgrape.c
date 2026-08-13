#include <endian.h>
#include <libusb-1.0/libusb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "grape/grape.h"

#define GFXLINK_TIMEOUT_MS 3000
#define GFXLINK_RESOURCE_MAX_COMMIT_ATTEMPTS 4U

struct grape_device {
    libusb_context *usb;
    libusb_device_handle *handle;
    uint32_t sequence;
    uint8_t tx_buffer[sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD];
    uint8_t rx_buffer[sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD];
};

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return htole32(bits);
}

static float float_from_bits(uint32_t value)
{
    uint32_t bits = le32toh(value);
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static size_t pixel_bytes(grape_pixel_format_t format)
{
    switch (format) {
        case GRAPE_PIXEL_FORMAT_RGB565: return 2U;
        case GRAPE_PIXEL_FORMAT_RGB888: return 3U;
        case GRAPE_PIXEL_FORMAT_A8: return 1U;
        default: return 0U;
    }
}

static uint32_t s_crc32_table[256];
static bool s_crc32_table_ready;

static void crc32_init_table(void)
{
    if (s_crc32_table_ready) {
        return;
    }
    for (uint32_t i = 0U; i < 256U; ++i) {
        uint32_t crc = i;
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
        }
        s_crc32_table[i] = crc;
    }
    s_crc32_table_ready = true;
}

static uint32_t crc32_ieee(const uint8_t *data, size_t size)
{
    crc32_init_table();
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0U; i < size; ++i) {
        crc = s_crc32_table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

static bool bitmap_test(const uint8_t *bitmap, uint32_t index)
{
    return (bitmap[index >> 3U] & (uint8_t)(1U << (index & 7U))) != 0U;
}

static void advance_sequence(grape_device_t *device)
{
    device->sequence++;
    if (device->sequence == 0U) {
        device->sequence = 1U;
    }
}

static int remote_status_to_result(int32_t status)
{
    if (status == GFXLINK_STATUS_OK) {
        return GRAPE_OK;
    }
    if (status > 0 || status < -999) {
        return GRAPE_ERROR_PROTOCOL;
    }
    return GRAPE_ERROR_REMOTE_BASE + status;
}

static int response_status(const void *payload, uint32_t size)
{
    if (!payload || size < sizeof(gfxlink_status_response_t)) {
        return GRAPE_ERROR_PROTOCOL;
    }

    int32_t value;
    memcpy(&value, payload, sizeof(value));
    value = (int32_t)le32toh((uint32_t)value);
    return remote_status_to_result(value);
}

static int map_libusb_open_error(int error)
{
    if (error == LIBUSB_ERROR_NOT_FOUND) {
        return GRAPE_ERROR_DEVICE_NOT_FOUND;
    }
    if (error == LIBUSB_ERROR_ACCESS) {
        return GRAPE_ERROR_PERMISSION;
    }
    return GRAPE_ERROR_USB;
}

static int open_grape_device(libusb_context *usb, libusb_device_handle **out_device)
{
    *out_device = NULL;

    libusb_device **devices = NULL;
    ssize_t count = libusb_get_device_list(usb, &devices);
    if (count < 0) {
        return GRAPE_ERROR_USB;
    }

    int result = GRAPE_ERROR_DEVICE_NOT_FOUND;
    bool found = false;

    for (ssize_t i = 0; i < count; ++i) {
        struct libusb_device_descriptor descriptor;
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0) {
            continue;
        }
        if (descriptor.idVendor != GFXLINK_USB_VID ||
            descriptor.idProduct != GFXLINK_USB_PID) {
            continue;
        }

        found = true;
        int ret = libusb_open(devices[i], out_device);
        if (ret == 0) {
            result = GRAPE_OK;
            break;
        }
        result = map_libusb_open_error(ret);
    }

    libusb_free_device_list(devices, 1);
    if (!found) {
        return GRAPE_ERROR_DEVICE_NOT_FOUND;
    }
    return result;
}

static int send_packet_parts(grape_device_t *device,
                             uint8_t opcode,
                             uint16_t flags,
                             const void *prefix,
                             uint32_t prefix_size,
                             const void *data,
                             uint32_t data_size,
                             uint32_t *out_sequence)
{
    uint32_t payload_size = prefix_size + data_size;
    if (!device || !device->handle ||
        payload_size > GFXLINK_MAX_PAYLOAD ||
        (prefix_size > 0U && !prefix) ||
        (data_size > 0U && !data)) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    uint32_t sequence = device->sequence;
    gfxlink_header_t header = {
        .magic = htole32(GFXLINK_MAGIC),
        .version = GFXLINK_PROTOCOL_VERSION,
        .opcode = opcode,
        .flags = htole16(flags),
        .sequence = htole32(sequence),
        .payload_size = htole32(payload_size),
    };

    uint8_t *tx = device->tx_buffer;
    memcpy(tx, &header, sizeof(header));
    uint32_t offset = sizeof(header);
    if (prefix_size > 0U) {
        memcpy(tx + offset, prefix, prefix_size);
        offset += prefix_size;
    }
    if (data_size > 0U) {
        memcpy(tx + offset, data, data_size);
    }

    int tx_size = (int)(sizeof(header) + payload_size);
    int transferred = 0;
    int ret = libusb_bulk_transfer(
        device->handle,
        GFXLINK_USB_EP_OUT,
        tx,
        tx_size,
        &transferred,
        GFXLINK_TIMEOUT_MS
    );
    if (ret != 0 || transferred != tx_size) {
        return GRAPE_ERROR_USB;
    }

    if (out_sequence) {
        *out_sequence = sequence;
    }
    advance_sequence(device);
    return GRAPE_OK;
}

static int send_packet(grape_device_t *device,
                       uint8_t opcode,
                       uint16_t flags,
                       const void *payload,
                       uint32_t payload_size,
                       uint32_t *out_sequence)
{
    return send_packet_parts(device, opcode, flags,
                             payload, payload_size, NULL, 0U,
                             out_sequence);
}

static int request(grape_device_t *device,
                   uint8_t opcode,
                   const void *payload,
                   uint32_t payload_size,
                   void *response,
                   uint32_t response_capacity,
                   uint32_t *response_size)
{
    if (!device || !response_size) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    const size_t rx_capacity = sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD;
    uint8_t *rx = device->rx_buffer;

    uint32_t sequence = 0U;
    int result = send_packet(device, opcode, 0U, payload, payload_size, &sequence);
    if (result != GRAPE_OK) {
        return result;
    }

    size_t received = 0U;
    size_t expected = 0U;
    while (expected == 0U || received < expected) {
        if (received == rx_capacity) {
            return GRAPE_ERROR_PROTOCOL;
        }

        int transferred = 0;
        int ret = libusb_bulk_transfer(
            device->handle,
            GFXLINK_USB_EP_IN,
            rx + received,
            (int)(rx_capacity - received),
            &transferred,
            GFXLINK_TIMEOUT_MS
        );
        if (ret != 0 || transferred <= 0) {
            return GRAPE_ERROR_USB;
        }

        received += (size_t)transferred;
        if (expected == 0U && received >= sizeof(gfxlink_header_t)) {
            gfxlink_header_t incoming;
            memcpy(&incoming, rx, sizeof(incoming));
            uint32_t incoming_size = le32toh(incoming.payload_size);
            if (le32toh(incoming.magic) != GFXLINK_MAGIC ||
                incoming.version != GFXLINK_PROTOCOL_VERSION ||
                incoming.opcode != opcode ||
                (le16toh(incoming.flags) & GFXLINK_FLAG_RESPONSE) == 0U ||
                le32toh(incoming.sequence) != sequence ||
                incoming_size > GFXLINK_MAX_PAYLOAD) {
                return GRAPE_ERROR_PROTOCOL;
            }
            expected = sizeof(gfxlink_header_t) + incoming_size;
        }

        if (expected != 0U && received > expected) {
            return GRAPE_ERROR_PROTOCOL;
        }
    }

    uint32_t size = (uint32_t)(expected - sizeof(gfxlink_header_t));
    if (size > response_capacity || (size > 0U && !response)) {
        return GRAPE_ERROR_PROTOCOL;
    }
    if (size > 0U) {
        memcpy(response, rx + sizeof(gfxlink_header_t), size);
    }
    *response_size = size;
    return GRAPE_OK;
}

static int status_request(grape_device_t *device,
                          uint8_t opcode,
                          const void *payload,
                          uint32_t payload_size)
{
    gfxlink_status_response_t response;
    uint32_t response_size = 0U;
    int result = request(device, opcode, payload, payload_size,
                         &response, sizeof(response), &response_size);
    if (result != GRAPE_OK) {
        return result;
    }
    if (response_size != sizeof(response)) {
        return GRAPE_ERROR_PROTOCOL;
    }
    return response_status(&response, response_size);
}

int grape_open(grape_device_t **out_device)
{
    if (!out_device) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }
    *out_device = NULL;

    grape_device_t *device = calloc(1, sizeof(*device));
    if (!device) {
        return GRAPE_ERROR_NO_MEMORY;
    }

    if (libusb_init(&device->usb) != 0) {
        free(device);
        return GRAPE_ERROR_USB;
    }

    int result = open_grape_device(device->usb, &device->handle);
    if (result != GRAPE_OK) {
        libusb_exit(device->usb);
        free(device);
        return result;
    }

    libusb_set_auto_detach_kernel_driver(device->handle, 1);
    int ret = libusb_claim_interface(device->handle, GFXLINK_USB_INTERFACE);
    if (ret != 0) {
        libusb_close(device->handle);
        libusb_exit(device->usb);
        free(device);
        return ret == LIBUSB_ERROR_ACCESS ? GRAPE_ERROR_PERMISSION : GRAPE_ERROR_USB;
    }

    device->sequence = 1U;
    *out_device = device;
    return GRAPE_OK;
}

void grape_close(grape_device_t *device)
{
    if (!device) {
        return;
    }
    if (device->handle) {
        libusb_release_interface(device->handle, GFXLINK_USB_INTERFACE);
        libusb_close(device->handle);
    }
    if (device->usb) {
        libusb_exit(device->usb);
    }
    free(device);
}

const char *grape_error_name(int result)
{
    switch (result) {
        case GRAPE_OK:
            return "ok";
        case GRAPE_ERROR_INVALID_ARGUMENT:
            return "invalid argument";
        case GRAPE_ERROR_NO_MEMORY:
            return "out of memory";
        case GRAPE_ERROR_PROTOCOL:
            return "GFXLINK protocol error";
        case GRAPE_ERROR_DEVICE_NOT_FOUND:
            return "GRAPE device not found";
        case GRAPE_ERROR_PERMISSION:
            return "permission denied opening GRAPE";
        case GRAPE_ERROR_USB:
            return "USB transport error";
        case GRAPE_ERROR_RETRY_LIMIT:
            return "GRAPE resource upload retry limit reached";
        case GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_INVALID_PACKET:
            return "GRAPE rejected invalid packet";
        case GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_UNSUPPORTED:
            return "operation unsupported by GRAPE";
        case GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_INVALID_ARGUMENT:
            return "GRAPE rejected invalid argument";
        case GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_NO_MEMORY:
            return "GRAPE out of memory";
        case GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_NOT_FOUND:
            return "GRAPE resource not found";
        case GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_BUSY:
            return "GRAPE resource busy";
        case GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_INTERNAL:
            return "GRAPE internal error";
        case GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_INCOMPLETE:
            return "GRAPE resource upload incomplete";
        case GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_CHECKSUM_MISMATCH:
            return "GRAPE resource checksum mismatch";
        default:
            return "unknown GRAPE error";
    }
}

int grape_hello(grape_device_t *device, grape_hello_info_t *out_info)
{
    if (!out_info) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    gfxlink_hello_response_t response;
    uint32_t size = 0U;
    int result = request(device, GFXLINK_OP_HELLO, NULL, 0U,
                         &response, sizeof(response), &size);
    if (result != GRAPE_OK) {
        return result;
    }
    if (size != sizeof(response)) {
        return GRAPE_ERROR_PROTOCOL;
    }

    result = response_status(&response, size);
    if (result != GRAPE_OK) {
        return result;
    }

    out_info->protocol_version = response.protocol_version;
    out_info->capabilities = le32toh(response.capabilities);
    out_info->max_payload = le32toh(response.max_payload);
    out_info->max_resource_size = le32toh(response.max_resource_size);
    return GRAPE_OK;
}

int grape_get_info(grape_device_t *device, grape_device_info_t *out_info)
{
    if (!out_info) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    gfxlink_info_response_t response;
    uint32_t size = 0U;
    int result = request(device, GFXLINK_OP_GET_INFO, NULL, 0U,
                         &response, sizeof(response), &size);
    if (result != GRAPE_OK) {
        return result;
    }
    if (size != sizeof(response)) {
        return GRAPE_ERROR_PROTOCOL;
    }

    result = response_status(&response, size);
    if (result != GRAPE_OK) {
        return result;
    }

    out_info->display_width = le32toh(response.display_width);
    out_info->display_height = le32toh(response.display_height);
    out_info->pixel_format = le32toh(response.pixel_format);
    out_info->max_surfaces = le32toh(response.max_surfaces);
    out_info->max_resources = le32toh(response.max_resources);
    out_info->max_textures = le32toh(response.max_textures);
    out_info->max_paths = le32toh(response.max_paths);
    out_info->max_svg_documents = le32toh(response.max_svg_documents);
    out_info->max_fonts = le32toh(response.max_fonts);
    return GRAPE_OK;
}

int grape_present(grape_device_t *device)
{
    return status_request(device, GFXLINK_OP_PRESENT, NULL, 0U);
}

int grape_create_solid_surface(grape_device_t *device,
                               float x,
                               float y,
                               uint32_t width,
                               uint32_t height,
                               uint8_t r,
                               uint8_t g,
                               uint8_t b,
                               uint8_t a,
                               grape_handle_t *out_handle)
{
    if (!out_handle || width == 0U || height == 0U) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    gfxlink_create_solid_surface_request_t request_payload = {
        .width = htole32(width),
        .height = htole32(height),
        .x_bits = float_bits(x),
        .y_bits = float_bits(y),
        .r = r,
        .g = g,
        .b = b,
        .a = a,
    };

    gfxlink_create_surface_response_t response;
    uint32_t size = 0U;
    int result = request(device, GFXLINK_OP_CREATE_SOLID_SURFACE,
                         &request_payload, sizeof(request_payload),
                         &response, sizeof(response), &size);
    if (result != GRAPE_OK) {
        return result;
    }
    if (size != sizeof(response)) {
        return GRAPE_ERROR_PROTOCOL;
    }

    result = response_status(&response, size);
    if (result != GRAPE_OK) {
        return result;
    }

    *out_handle = le32toh(response.handle);
    return GRAPE_OK;
}

int grape_surface_set_position(grape_device_t *device,
                               grape_handle_t handle,
                               float x,
                               float y)
{
    gfxlink_set_surface_position_request_t request_payload = {
        .handle = htole32(handle),
        .x_bits = float_bits(x),
        .y_bits = float_bits(y),
    };
    return status_request(device, GFXLINK_OP_SET_SURFACE_POSITION,
                          &request_payload, sizeof(request_payload));
}

int grape_surface_set_color(grape_device_t *device,
                            grape_handle_t handle,
                            uint8_t r,
                            uint8_t g,
                            uint8_t b,
                            uint8_t a)
{
    gfxlink_set_surface_color_request_t request_payload = {
        .handle = htole32(handle),
        .r = r,
        .g = g,
        .b = b,
        .a = a,
    };
    return status_request(device, GFXLINK_OP_SET_SURFACE_COLOR,
                          &request_payload, sizeof(request_payload));
}

int grape_surface_destroy(grape_device_t *device, grape_handle_t handle)
{
    gfxlink_destroy_surface_request_t request_payload = {
        .handle = htole32(handle),
    };
    return status_request(device, GFXLINK_OP_DESTROY_SURFACE,
                          &request_payload, sizeof(request_payload));
}

int grape_surface_create(grape_device_t *device,
                         grape_handle_t texture,
                         grape_handle_t *out_surface)
{
    if (!device || texture == 0U || !out_surface) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }
    gfxlink_create_surface_request_t request_payload = {
        .texture_handle = htole32(texture),
    };
    gfxlink_create_surface_response_t response;
    uint32_t size = 0U;
    int result = request(device, GFXLINK_OP_CREATE_SURFACE,
                         &request_payload, sizeof(request_payload),
                         &response, sizeof(response), &size);
    if (result != GRAPE_OK) return result;
    if (size != sizeof(response)) return GRAPE_ERROR_PROTOCOL;
    result = response_status(&response, size);
    if (result != GRAPE_OK) return result;
    *out_surface = le32toh(response.handle);
    return *out_surface != 0U ? GRAPE_OK : GRAPE_ERROR_PROTOCOL;
}

int grape_surface_set_texture(grape_device_t *device,
                              grape_handle_t surface,
                              grape_handle_t texture)
{
    gfxlink_set_surface_texture_request_t request_payload = {
        .handle = htole32(surface),
        .texture_handle = htole32(texture),
    };
    return status_request(device, GFXLINK_OP_SET_SURFACE_TEXTURE,
                          &request_payload, sizeof(request_payload));
}

int grape_surface_set_transform(grape_device_t *device,
                                grape_handle_t surface,
                                const grape_transform_t *transform)
{
    if (!device || surface == 0U || !transform) return GRAPE_ERROR_INVALID_ARGUMENT;
    gfxlink_set_surface_transform_request_t request_payload = {
        .handle = htole32(surface),
        .x_bits = float_bits(transform->x),
        .y_bits = float_bits(transform->y),
        .scale_x_bits = float_bits(transform->scale_x),
        .scale_y_bits = float_bits(transform->scale_y),
        .rotation_bits = float_bits(transform->rotation),
        .origin_x_bits = float_bits(transform->origin_x),
        .origin_y_bits = float_bits(transform->origin_y),
    };
    return status_request(device, GFXLINK_OP_SET_SURFACE_TRANSFORM,
                          &request_payload, sizeof(request_payload));
}

int grape_surface_set_scale(grape_device_t *device,
                            grape_handle_t surface,
                            float scale_x,
                            float scale_y)
{
    gfxlink_set_surface_scale_request_t request_payload = {
        .handle = htole32(surface),
        .scale_x_bits = float_bits(scale_x),
        .scale_y_bits = float_bits(scale_y),
    };
    return status_request(device, GFXLINK_OP_SET_SURFACE_SCALE,
                          &request_payload, sizeof(request_payload));
}

int grape_surface_set_rotation(grape_device_t *device,
                               grape_handle_t surface,
                               float radians)
{
    gfxlink_set_surface_rotation_request_t request_payload = {
        .handle = htole32(surface),
        .rotation_bits = float_bits(radians),
    };
    return status_request(device, GFXLINK_OP_SET_SURFACE_ROTATION,
                          &request_payload, sizeof(request_payload));
}

int grape_surface_set_origin(grape_device_t *device,
                             grape_handle_t surface,
                             float origin_x,
                             float origin_y)
{
    gfxlink_set_surface_origin_request_t request_payload = {
        .handle = htole32(surface),
        .origin_x_bits = float_bits(origin_x),
        .origin_y_bits = float_bits(origin_y),
    };
    return status_request(device, GFXLINK_OP_SET_SURFACE_ORIGIN,
                          &request_payload, sizeof(request_payload));
}

int grape_surface_set_z(grape_device_t *device,
                        grape_handle_t surface,
                        int32_t z)
{
    gfxlink_set_surface_z_request_t request_payload = {
        .handle = htole32(surface),
        .z = (int32_t)htole32((uint32_t)z),
    };
    return status_request(device, GFXLINK_OP_SET_SURFACE_Z,
                          &request_payload, sizeof(request_payload));
}

int grape_surface_set_opacity(grape_device_t *device,
                              grape_handle_t surface,
                              uint8_t opacity)
{
    gfxlink_set_surface_opacity_request_t request_payload = {
        .handle = htole32(surface),
        .opacity = opacity,
        .reserved = {0, 0, 0},
    };
    return status_request(device, GFXLINK_OP_SET_SURFACE_OPACITY,
                          &request_payload, sizeof(request_payload));
}

int grape_surface_set_visible(grape_device_t *device,
                              grape_handle_t surface,
                              bool visible)
{
    gfxlink_set_surface_visible_request_t request_payload = {
        .handle = htole32(surface),
        .visible = visible ? 1U : 0U,
        .reserved = {0, 0, 0},
    };
    return status_request(device, GFXLINK_OP_SET_SURFACE_VISIBLE,
                          &request_payload, sizeof(request_payload));
}

static int upload_texture_resource(grape_device_t *device,
                                   const void *pixels,
                                   uint32_t size,
                                   grape_handle_t *out_resource)
{
    if (!pixels || size == 0U || !out_resource) return GRAPE_ERROR_INVALID_ARGUMENT;
    return grape_resource_upload(device, GFXLINK_RESOURCE_TEXTURE,
                                 pixels, size, out_resource);
}

int grape_texture_create(grape_device_t *device,
                         uint32_t width,
                         uint32_t height,
                         grape_pixel_format_t format,
                         const void *pixels,
                         uint32_t size,
                         grape_texture_info_t *out_texture)
{
    if (!device || !out_texture || width == 0U || height == 0U) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }
    size_t bpp = pixel_bytes(format);
    if (bpp == 0U || width > SIZE_MAX / bpp) return GRAPE_ERROR_INVALID_ARGUMENT;
    size_t expected = (size_t)width * bpp;
    if (height > SIZE_MAX / expected) return GRAPE_ERROR_INVALID_ARGUMENT;
    expected *= height;
    if (expected > UINT32_MAX || (pixels && size != expected) || (!pixels && size != 0U)) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    grape_handle_t resource = 0U;
    int result = GRAPE_OK;
    if (pixels) {
        result = upload_texture_resource(device, pixels, size, &resource);
        if (result != GRAPE_OK) return result;
    }

    gfxlink_texture_create_request_t request_payload = {
        .width = htole32(width),
        .height = htole32(height),
        .format = htole32((uint32_t)format),
        .resource_handle = htole32(resource),
    };
    gfxlink_texture_create_response_t response;
    uint32_t response_size = 0U;
    result = request(device, GFXLINK_OP_TEXTURE_CREATE,
                     &request_payload, sizeof(request_payload),
                     &response, sizeof(response), &response_size);
    if (result == GRAPE_OK) {
        if (response_size != sizeof(response)) {
            result = GRAPE_ERROR_PROTOCOL;
        } else {
            result = response_status(&response, response_size);
        }
    }

    int cleanup = resource ? grape_resource_destroy(device, resource) : GRAPE_OK;
    if (result != GRAPE_OK) return result;
    if (cleanup != GRAPE_OK) return cleanup;

    out_texture->handle = le32toh(response.handle);
    out_texture->width = width;
    out_texture->height = height;
    out_texture->format = format;
    out_texture->stride = le32toh(response.stride);
    out_texture->size = le32toh(response.size);
    if (out_texture->handle == 0U || out_texture->size != expected ||
        out_texture->stride < width * bpp) {
        return GRAPE_ERROR_PROTOCOL;
    }
    return GRAPE_OK;
}

int grape_texture_update_rect(grape_device_t *device,
                              grape_handle_t texture,
                              uint32_t x,
                              uint32_t y,
                              uint32_t width,
                              uint32_t height,
                              const void *pixels,
                              uint32_t size)
{
    if (!device || texture == 0U || width == 0U || height == 0U ||
        !pixels || size == 0U) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    grape_handle_t resource = 0U;
    int result = upload_texture_resource(device, pixels, size, &resource);
    if (result != GRAPE_OK) return result;

    gfxlink_texture_update_request_t request_payload = {
        .texture_handle = htole32(texture),
        .resource_handle = htole32(resource),
        .x = htole32(x),
        .y = htole32(y),
        .width = htole32(width),
        .height = htole32(height),
    };
    result = status_request(device, GFXLINK_OP_TEXTURE_UPDATE,
                            &request_payload, sizeof(request_payload));
    int cleanup = grape_resource_destroy(device, resource);
    return result != GRAPE_OK ? result : cleanup;
}

int grape_texture_update(grape_device_t *device,
                         grape_handle_t texture,
                         uint32_t width,
                         uint32_t height,
                         const void *pixels,
                         uint32_t size)
{
    return grape_texture_update_rect(device, texture, 0U, 0U,
                                     width, height, pixels, size);
}

int grape_texture_destroy(grape_device_t *device, grape_handle_t texture)
{
    gfxlink_texture_handle_request_t request_payload = {
        .handle = htole32(texture),
    };
    return status_request(device, GFXLINK_OP_TEXTURE_DESTROY,
                          &request_payload, sizeof(request_payload));
}

int grape_resource_create(grape_device_t *device,
                          gfxlink_resource_kind_t kind,
                          uint32_t total_size,
                          grape_resource_info_t *out_info)
{
    if (!device || !out_info ||
        kind > GFXLINK_RESOURCE_SVG ||
        total_size == 0U || total_size > GFXLINK_MAX_RESOURCE_SIZE) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    gfxlink_resource_create_request_t request_payload = {
        .kind = htole32((uint32_t)kind),
        .total_size = htole32(total_size),
        .flags = htole32(0U),
    };

    gfxlink_resource_create_response_t response;
    uint32_t size = 0U;
    int result = request(device, GFXLINK_OP_RESOURCE_CREATE,
                         &request_payload, sizeof(request_payload),
                         &response, sizeof(response), &size);
    if (result != GRAPE_OK) {
        return result;
    }
    if (size != sizeof(response)) {
        return GRAPE_ERROR_PROTOCOL;
    }

    result = response_status(&response, size);
    if (result != GRAPE_OK) {
        return result;
    }

    out_info->handle = le32toh(response.handle);
    out_info->chunk_size = le32toh(response.chunk_size);
    out_info->chunk_count = le32toh(response.chunk_count);
    if (out_info->handle == 0U ||
        out_info->chunk_size != GFXLINK_RESOURCE_CHUNK_SIZE ||
        out_info->chunk_count == 0U ||
        out_info->chunk_count > GFXLINK_RESOURCE_MAX_CHUNKS) {
        return GRAPE_ERROR_PROTOCOL;
    }
    return GRAPE_OK;
}

static int resource_write_chunk(grape_device_t *device,
                                grape_handle_t handle,
                                uint32_t chunk_index,
                                const void *data,
                                uint32_t size)
{
    if (!device || handle == 0U || !data || size == 0U ||
        size > GFXLINK_RESOURCE_CHUNK_SIZE ||
        chunk_index >= GFXLINK_RESOURCE_MAX_CHUNKS) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    gfxlink_resource_write_request_t request_header = {
        .handle = htole32(handle),
        .chunk_index = htole32(chunk_index),
        .data_size = htole32(size),
        .crc32 = htole32(crc32_ieee(data, size)),
    };
    return send_packet_parts(device, GFXLINK_OP_RESOURCE_WRITE,
                             GFXLINK_FLAG_NO_RESPONSE,
                             &request_header, sizeof(request_header),
                             data, size, NULL);
}

int grape_resource_write(grape_device_t *device,
                         grape_handle_t handle,
                         uint32_t offset,
                         const void *data,
                         uint32_t size)
{
    if (offset % GFXLINK_RESOURCE_CHUNK_SIZE != 0U) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }
    return resource_write_chunk(device, handle,
                                offset / GFXLINK_RESOURCE_CHUNK_SIZE,
                                data, size);
}

int grape_resource_commit(grape_device_t *device,
                          grape_handle_t handle,
                          uint32_t expected_crc32,
                          grape_resource_commit_report_t *out_report)
{
    if (!device || handle == 0U || !out_report) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    gfxlink_resource_commit_request_t request_payload = {
        .handle = htole32(handle),
        .expected_crc32 = htole32(expected_crc32),
    };
    gfxlink_resource_commit_response_t response;
    uint32_t size = 0U;
    int result = request(device, GFXLINK_OP_RESOURCE_COMMIT,
                         &request_payload, sizeof(request_payload),
                         &response, sizeof(response), &size);
    if (result != GRAPE_OK) {
        return result;
    }
    if (size != sizeof(response)) {
        return GRAPE_ERROR_PROTOCOL;
    }

    memset(out_report, 0, sizeof(*out_report));
    out_report->chunk_count = le32toh(response.chunk_count);
    out_report->resource_crc32 = le32toh(response.resource_crc32);
    memcpy(out_report->missing_bitmap, response.missing_bitmap,
           sizeof(out_report->missing_bitmap));
    memcpy(out_report->corrupt_bitmap, response.corrupt_bitmap,
           sizeof(out_report->corrupt_bitmap));
    if (out_report->chunk_count > GFXLINK_RESOURCE_MAX_CHUNKS) {
        return GRAPE_ERROR_PROTOCOL;
    }
    return response_status(&response, size);
}

int grape_resource_destroy(grape_device_t *device, grape_handle_t handle)
{
    gfxlink_resource_handle_request_t request_payload = {
        .handle = htole32(handle),
    };
    return status_request(device, GFXLINK_OP_RESOURCE_DESTROY,
                          &request_payload, sizeof(request_payload));
}

static uint32_t chunk_size_for_upload(uint32_t total_size, uint32_t chunk_index)
{
    uint32_t offset = chunk_index * GFXLINK_RESOURCE_CHUNK_SIZE;
    uint32_t remaining = total_size - offset;
    return remaining < GFXLINK_RESOURCE_CHUNK_SIZE ? remaining : GFXLINK_RESOURCE_CHUNK_SIZE;
}

static int resend_reported_chunks(grape_device_t *device,
                                  grape_handle_t handle,
                                  const uint8_t *data,
                                  uint32_t total_size,
                                  const grape_resource_commit_report_t *report,
                                  grape_resource_upload_stats_t *stats)
{
    bool resent = false;
    for (uint32_t i = 0U; i < report->chunk_count; ++i) {
        if (!bitmap_test(report->missing_bitmap, i) &&
            !bitmap_test(report->corrupt_bitmap, i)) {
            continue;
        }

        uint32_t size = chunk_size_for_upload(total_size, i);
        uint32_t offset = i * GFXLINK_RESOURCE_CHUNK_SIZE;
        int result = resource_write_chunk(device, handle, i, data + offset, size);
        if (result != GRAPE_OK) {
            return result;
        }
        resent = true;
        if (stats) {
            stats->chunks_sent++;
            stats->chunks_retransmitted++;
        }
    }
    return resent ? GRAPE_OK : GRAPE_ERROR_PROTOCOL;
}

static int resend_all_chunks(grape_device_t *device,
                             grape_handle_t handle,
                             const uint8_t *data,
                             uint32_t total_size,
                             uint32_t chunk_count,
                             grape_resource_upload_stats_t *stats)
{
    for (uint32_t i = 0U; i < chunk_count; ++i) {
        uint32_t size = chunk_size_for_upload(total_size, i);
        uint32_t offset = i * GFXLINK_RESOURCE_CHUNK_SIZE;
        int result = resource_write_chunk(device, handle, i, data + offset, size);
        if (result != GRAPE_OK) {
            return result;
        }
        if (stats) {
            stats->chunks_sent++;
            stats->chunks_retransmitted++;
        }
    }
    return GRAPE_OK;
}

int grape_resource_upload_ex(grape_device_t *device,
                             gfxlink_resource_kind_t kind,
                             const void *data,
                             uint32_t size,
                             grape_handle_t *out_handle,
                             grape_resource_upload_stats_t *out_stats)
{
    if (!device || !data || !out_handle || size == 0U ||
        size > GFXLINK_MAX_RESOURCE_SIZE) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    grape_resource_upload_stats_t stats = {0};
    grape_resource_info_t info;
    int result = grape_resource_create(device, kind, size, &info);
    if (result != GRAPE_OK) {
        return result;
    }

    uint32_t expected_chunks =
        (size + GFXLINK_RESOURCE_CHUNK_SIZE - 1U) / GFXLINK_RESOURCE_CHUNK_SIZE;
    if (info.chunk_count != expected_chunks) {
        grape_resource_destroy(device, info.handle);
        return GRAPE_ERROR_PROTOCOL;
    }

    const uint8_t *bytes = data;
    uint32_t expected_crc32 = crc32_ieee(bytes, size);
    for (uint32_t i = 0U; i < info.chunk_count; ++i) {
        uint32_t chunk_size = chunk_size_for_upload(size, i);
        uint32_t offset = i * GFXLINK_RESOURCE_CHUNK_SIZE;
        result = resource_write_chunk(device, info.handle, i,
                                      bytes + offset, chunk_size);
        if (result != GRAPE_OK) {
            grape_resource_destroy(device, info.handle);
            return result;
        }
        stats.chunks_sent++;
    }

    for (uint32_t attempt = 0U;
         attempt < GFXLINK_RESOURCE_MAX_COMMIT_ATTEMPTS;
         ++attempt) {
        grape_resource_commit_report_t report;
        result = grape_resource_commit(device, info.handle,
                                       expected_crc32, &report);
        stats.commit_attempts++;

        if (result == GRAPE_OK) {
            if (report.chunk_count != info.chunk_count ||
                report.resource_crc32 != expected_crc32) {
                result = GRAPE_ERROR_PROTOCOL;
                break;
            }
            *out_handle = info.handle;
            if (out_stats) {
                *out_stats = stats;
            }
            return GRAPE_OK;
        }

        if (report.chunk_count != info.chunk_count) {
            result = GRAPE_ERROR_PROTOCOL;
            break;
        }

        if (result == GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_INCOMPLETE) {
            result = resend_reported_chunks(device, info.handle, bytes, size,
                                            &report, &stats);
            if (result != GRAPE_OK) {
                break;
            }
            continue;
        }

        if (result == GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_CHECKSUM_MISMATCH) {
            result = resend_all_chunks(device, info.handle, bytes, size,
                                       info.chunk_count, &stats);
            if (result != GRAPE_OK) {
                break;
            }
            continue;
        }

        break;
    }

    grape_resource_destroy(device, info.handle);
    if (out_stats) {
        *out_stats = stats;
    }
    if (result == GRAPE_OK ||
        result == GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_INCOMPLETE ||
        result == GRAPE_ERROR_REMOTE_BASE + GFXLINK_STATUS_CHECKSUM_MISMATCH) {
        return GRAPE_ERROR_RETRY_LIMIT;
    }
    return result;
}

int grape_resource_upload(grape_device_t *device,
                          gfxlink_resource_kind_t kind,
                          const void *data,
                          uint32_t size,
                          grape_handle_t *out_handle)
{
    return grape_resource_upload_ex(device, kind, data, size, out_handle, NULL);
}

static int create_handle_request(grape_device_t *device,
                                 uint8_t opcode,
                                 const void *payload,
                                 uint32_t payload_size,
                                 grape_handle_t *out_handle)
{
    if (!device || !out_handle) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    gfxlink_create_surface_response_t response;
    uint32_t size = 0U;
    int result = request(device, opcode, payload, payload_size,
                         &response, sizeof(response), &size);
    if (result != GRAPE_OK) {
        return result;
    }
    if (size != sizeof(response)) {
        return GRAPE_ERROR_PROTOCOL;
    }

    result = response_status(&response, size);
    if (result != GRAPE_OK) {
        return result;
    }

    grape_handle_t handle = le32toh(response.handle);
    if (handle == 0U) {
        return GRAPE_ERROR_PROTOCOL;
    }
    *out_handle = handle;
    return GRAPE_OK;
}

int grape_path_create(grape_device_t *device,
                      const grape_path_command_t *commands,
                      uint32_t command_count,
                      grape_handle_t *out_path)
{
    if (!device || !commands || !out_path || command_count == 0U ||
        command_count > GFXLINK_MAX_RESOURCE_SIZE / sizeof(gfxlink_path_command_t)) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    size_t wire_size = (size_t)command_count * sizeof(gfxlink_path_command_t);
    gfxlink_path_command_t *wire = calloc(command_count, sizeof(*wire));
    if (!wire) {
        return GRAPE_ERROR_NO_MEMORY;
    }

    for (uint32_t i = 0U; i < command_count; ++i) {
        const grape_path_command_t *src = &commands[i];
        if (src->type < GRAPE_PATH_MOVE_TO || src->type > GRAPE_PATH_CLOSE) {
            free(wire);
            return GRAPE_ERROR_INVALID_ARGUMENT;
        }

        wire[i].type = (uint8_t)src->type;
        uint32_t value_count = 0U;
        switch (src->type) {
            case GRAPE_PATH_MOVE_TO:
            case GRAPE_PATH_LINE_TO:
                value_count = 2U;
                break;
            case GRAPE_PATH_QUAD_TO:
                value_count = 4U;
                break;
            case GRAPE_PATH_CUBIC_TO:
                value_count = 6U;
                break;
            case GRAPE_PATH_ARC_TO:
                value_count = 5U;
                if (src->large_arc) wire[i].flags |= GFXLINK_PATH_FLAG_LARGE_ARC;
                if (src->sweep) wire[i].flags |= GFXLINK_PATH_FLAG_SWEEP;
                break;
            case GRAPE_PATH_CLOSE:
                value_count = 0U;
                break;
        }
        if (src->type != GRAPE_PATH_ARC_TO && (src->large_arc || src->sweep)) {
            free(wire);
            return GRAPE_ERROR_INVALID_ARGUMENT;
        }

        for (uint32_t j = 0U; j < value_count; ++j) {
            wire[i].values[j] = float_bits(src->values[j]);
        }
    }

    grape_handle_t resource = 0U;
    int result = grape_resource_upload(device, GFXLINK_RESOURCE_VECTOR,
                                       wire, (uint32_t)wire_size, &resource);
    free(wire);
    if (result != GRAPE_OK) {
        return result;
    }

    gfxlink_path_create_request_t request_payload = {
        .resource_handle = htole32(resource),
        .command_count = htole32(command_count),
    };
    result = create_handle_request(device, GFXLINK_OP_PATH_CREATE,
                                   &request_payload, sizeof(request_payload),
                                   out_path);
    int cleanup = grape_resource_destroy(device, resource);
    return result != GRAPE_OK ? result : cleanup;
}

int grape_path_rasterize(grape_device_t *device,
                         grape_handle_t path,
                         float pixels_per_unit,
                         uint8_t samples_per_axis,
                         uint32_t padding_pixels,
                         grape_path_raster_info_t *out_raster)
{
    if (!device || path == 0U || !out_raster ||
        !(pixels_per_unit > 0.0f) ||
        samples_per_axis == 0U || samples_per_axis > 8U) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    gfxlink_path_rasterize_request_t request_payload = {
        .path_handle = htole32(path),
        .pixels_per_unit_bits = float_bits(pixels_per_unit),
        .samples_per_axis = samples_per_axis,
        .reserved = {0, 0, 0},
        .padding_pixels = htole32(padding_pixels),
    };
    gfxlink_raster_texture_response_t response;
    uint32_t size = 0U;
    int result = request(device, GFXLINK_OP_PATH_RASTERIZE,
                         &request_payload, sizeof(request_payload),
                         &response, sizeof(response), &size);
    if (result != GRAPE_OK) return result;
    if (size != sizeof(response)) return GRAPE_ERROR_PROTOCOL;
    result = response_status(&response, size);
    if (result != GRAPE_OK) return result;

    memset(out_raster, 0, sizeof(*out_raster));
    out_raster->texture_handle = le32toh(response.texture_handle);
    out_raster->width = le32toh(response.width);
    out_raster->height = le32toh(response.height);
    out_raster->stride = le32toh(response.stride);
    out_raster->origin_x = float_from_bits(response.origin_x_bits);
    out_raster->origin_y = float_from_bits(response.origin_y_bits);
    out_raster->pixels_per_unit = float_from_bits(response.pixels_per_unit_bits);
    if (out_raster->texture_handle == 0U ||
        out_raster->width == 0U || out_raster->height == 0U) {
        return GRAPE_ERROR_PROTOCOL;
    }
    return GRAPE_OK;
}

int grape_path_destroy(grape_device_t *device, grape_handle_t path)
{
    gfxlink_path_handle_request_t request_payload = { .handle = htole32(path) };
    return status_request(device, GFXLINK_OP_PATH_DESTROY,
                          &request_payload, sizeof(request_payload));
}

int grape_svg_create(grape_device_t *device,
                     const char *svg_text,
                     uint32_t svg_size,
                     const grape_svg_config_t *config,
                     grape_svg_info_t *out_svg)
{
    if (!device || !svg_text || !config || !out_svg ||
        config->samples_per_axis == 0U || config->samples_per_axis > 8U) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }
    if (svg_size == 0U) {
        size_t measured = strlen(svg_text);
        if (measured >= GFXLINK_MAX_RESOURCE_SIZE) {
            return GRAPE_ERROR_INVALID_ARGUMENT;
        }
        svg_size = (uint32_t)measured;
    }
    if (svg_size >= GFXLINK_MAX_RESOURCE_SIZE) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    uint8_t *nul_terminated = malloc((size_t)svg_size + 1U);
    if (!nul_terminated) return GRAPE_ERROR_NO_MEMORY;
    memcpy(nul_terminated, svg_text, svg_size);
    nul_terminated[svg_size] = '\0';

    grape_handle_t resource = 0U;
    int result = grape_resource_upload(device, GFXLINK_RESOURCE_SVG,
                                       nul_terminated, svg_size + 1U, &resource);
    free(nul_terminated);
    if (result != GRAPE_OK) return result;

    gfxlink_svg_create_request_t request_payload = {
        .resource_handle = htole32(resource),
        .x_bits = float_bits(config->x),
        .y_bits = float_bits(config->y),
        .width_bits = float_bits(config->width),
        .height_bits = float_bits(config->height),
        .samples_per_axis = config->samples_per_axis,
        .reserved = {0, 0, 0},
        .padding_pixels = htole32(config->padding_pixels),
        .z_base = (int32_t)htole32((uint32_t)config->z_base),
    };
    gfxlink_svg_create_response_t response;
    uint32_t size = 0U;
    result = request(device, GFXLINK_OP_SVG_CREATE,
                     &request_payload, sizeof(request_payload),
                     &response, sizeof(response), &size);
    int cleanup = grape_resource_destroy(device, resource);
    if (result != GRAPE_OK) return result;
    if (cleanup != GRAPE_OK) return cleanup;
    if (size != sizeof(response)) return GRAPE_ERROR_PROTOCOL;
    result = response_status(&response, size);
    if (result != GRAPE_OK) return result;

    memset(out_svg, 0, sizeof(*out_svg));
    out_svg->handle = le32toh(response.handle);
    out_svg->layer_count = le32toh(response.layer_count);
    out_svg->view_box_min_x = float_from_bits(response.view_box_min_x_bits);
    out_svg->view_box_min_y = float_from_bits(response.view_box_min_y_bits);
    out_svg->view_box_width = float_from_bits(response.view_box_width_bits);
    out_svg->view_box_height = float_from_bits(response.view_box_height_bits);
    return out_svg->handle != 0U ? GRAPE_OK : GRAPE_ERROR_PROTOCOL;
}

int grape_svg_destroy(grape_device_t *device, grape_handle_t svg)
{
    gfxlink_svg_handle_request_t request_payload = { .handle = htole32(svg) };
    return status_request(device, GFXLINK_OP_SVG_DESTROY,
                          &request_payload, sizeof(request_payload));
}

int grape_font_create(grape_device_t *device,
                      const void *ttf_data,
                      uint32_t ttf_size,
                      grape_font_info_t *out_font)
{
    if (!device || !ttf_data || ttf_size == 0U || !out_font) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    grape_handle_t resource = 0U;
    int result = grape_resource_upload(device, GFXLINK_RESOURCE_FONT,
                                       ttf_data, ttf_size, &resource);
    if (result != GRAPE_OK) return result;

    gfxlink_font_create_request_t request_payload = {
        .resource_handle = htole32(resource),
    };
    gfxlink_font_create_response_t response;
    uint32_t size = 0U;
    result = request(device, GFXLINK_OP_FONT_CREATE,
                     &request_payload, sizeof(request_payload),
                     &response, sizeof(response), &size);
    if (result != GRAPE_OK) {
        grape_resource_destroy(device, resource);
        return result;
    }
    if (size != sizeof(response)) {
        grape_resource_destroy(device, resource);
        return GRAPE_ERROR_PROTOCOL;
    }
    result = response_status(&response, size);
    if (result != GRAPE_OK) {
        grape_resource_destroy(device, resource);
        return result;
    }

    /* FONT_CREATE consumes the committed FONT resource on success. */
    memset(out_font, 0, sizeof(*out_font));
    out_font->handle = le32toh(response.handle);
    out_font->units_per_em = le32toh(response.units_per_em);
    out_font->glyph_count = le32toh(response.glyph_count);
    out_font->cmap_format = le32toh(response.cmap_format);
    out_font->ascender = (int32_t)le32toh((uint32_t)response.ascender);
    out_font->descender = (int32_t)le32toh((uint32_t)response.descender);
    out_font->line_gap = (int32_t)le32toh((uint32_t)response.line_gap);
    return out_font->handle != 0U ? GRAPE_OK : GRAPE_ERROR_PROTOCOL;
}

int grape_font_destroy(grape_device_t *device, grape_handle_t font)
{
    gfxlink_font_handle_request_t request_payload = { .handle = htole32(font) };
    return status_request(device, GFXLINK_OP_FONT_DESTROY,
                          &request_payload, sizeof(request_payload));
}

int grape_text_rasterize_utf8(grape_device_t *device,
                              grape_handle_t font,
                              const char *utf8,
                              float pixels_per_unit,
                              uint8_t samples_per_axis,
                              uint32_t padding_pixels,
                              grape_text_raster_info_t *out_raster)
{
    if (!device || font == 0U || !utf8 || !out_raster ||
        !(pixels_per_unit > 0.0f) ||
        samples_per_axis == 0U || samples_per_axis > 8U) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    size_t text_size = strlen(utf8) + 1U;
    if (text_size > GFXLINK_MAX_RESOURCE_SIZE) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    grape_handle_t resource = 0U;
    int result = grape_resource_upload(device, GFXLINK_RESOURCE_TEXT,
                                       utf8, (uint32_t)text_size, &resource);
    if (result != GRAPE_OK) return result;

    gfxlink_text_rasterize_request_t request_payload = {
        .font_handle = htole32(font),
        .text_resource_handle = htole32(resource),
        .pixels_per_unit_bits = float_bits(pixels_per_unit),
        .samples_per_axis = samples_per_axis,
        .reserved = {0, 0, 0},
        .padding_pixels = htole32(padding_pixels),
    };
    gfxlink_text_rasterize_response_t response;
    uint32_t size = 0U;
    result = request(device, GFXLINK_OP_TEXT_RASTERIZE,
                     &request_payload, sizeof(request_payload),
                     &response, sizeof(response), &size);
    int cleanup = grape_resource_destroy(device, resource);
    if (result != GRAPE_OK) return result;
    if (cleanup != GRAPE_OK) return cleanup;
    if (size != sizeof(response)) return GRAPE_ERROR_PROTOCOL;
    result = response_status(&response, size);
    if (result != GRAPE_OK) return result;

    memset(out_raster, 0, sizeof(*out_raster));
    out_raster->texture_handle = le32toh(response.texture_handle);
    out_raster->width = le32toh(response.width);
    out_raster->height = le32toh(response.height);
    out_raster->stride = le32toh(response.stride);
    out_raster->origin_x = float_from_bits(response.origin_x_bits);
    out_raster->origin_y = float_from_bits(response.origin_y_bits);
    out_raster->pixels_per_unit = float_from_bits(response.pixels_per_unit_bits);
    out_raster->advance_width = float_from_bits(response.advance_width_bits);
    out_raster->glyph_count = le32toh(response.glyph_count);
    if (out_raster->texture_handle == 0U ||
        out_raster->width == 0U || out_raster->height == 0U) {
        return GRAPE_ERROR_PROTOCOL;
    }
    return GRAPE_OK;
}
