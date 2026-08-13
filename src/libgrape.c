#include <endian.h>
#include <libusb-1.0/libusb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "grape/grape.h"

#define GFXLINK_TIMEOUT_MS 3000

struct grape_device {
    libusb_context *usb;
    libusb_device_handle *handle;
    uint32_t sequence;
};

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return htole32(bits);
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

static int request(grape_device_t *device,
                   uint8_t opcode,
                   const void *payload,
                   uint32_t payload_size,
                   void *response,
                   uint32_t response_capacity,
                   uint32_t *response_size)
{
    if (!device || !device->handle || !response_size ||
        payload_size > GFXLINK_MAX_PAYLOAD ||
        (payload_size > 0U && !payload)) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    size_t tx_size = sizeof(gfxlink_header_t) + payload_size;
    uint8_t *tx = malloc(tx_size);
    uint8_t *rx = malloc(sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD);
    if (!tx || !rx) {
        free(tx);
        free(rx);
        return GRAPE_ERROR_NO_MEMORY;
    }

    gfxlink_header_t header = {
        .magic = htole32(GFXLINK_MAGIC),
        .version = GFXLINK_PROTOCOL_VERSION,
        .opcode = opcode,
        .flags = htole16(0),
        .sequence = htole32(device->sequence),
        .payload_size = htole32(payload_size),
    };

    memcpy(tx, &header, sizeof(header));
    if (payload_size > 0U) {
        memcpy(tx + sizeof(header), payload, payload_size);
    }

    int transferred = 0;
    int ret = libusb_bulk_transfer(
        device->handle,
        GFXLINK_USB_EP_OUT,
        tx,
        (int)tx_size,
        &transferred,
        GFXLINK_TIMEOUT_MS
    );
    free(tx);

    if (ret != 0 || transferred != (int)tx_size) {
        free(rx);
        return GRAPE_ERROR_USB;
    }

    size_t received = 0U;
    size_t expected = 0U;
    const size_t rx_capacity = sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD;

    while (expected == 0U || received < expected) {
        if (received == rx_capacity) {
            free(rx);
            return GRAPE_ERROR_PROTOCOL;
        }

        transferred = 0;
        ret = libusb_bulk_transfer(
            device->handle,
            GFXLINK_USB_EP_IN,
            rx + received,
            (int)(rx_capacity - received),
            &transferred,
            GFXLINK_TIMEOUT_MS
        );
        if (ret != 0 || transferred <= 0) {
            free(rx);
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
                le32toh(incoming.sequence) != device->sequence ||
                incoming_size > GFXLINK_MAX_PAYLOAD) {
                free(rx);
                return GRAPE_ERROR_PROTOCOL;
            }
            expected = sizeof(gfxlink_header_t) + incoming_size;
        }

        if (expected != 0U && received > expected) {
            free(rx);
            return GRAPE_ERROR_PROTOCOL;
        }
    }

    uint32_t size = (uint32_t)(expected - sizeof(gfxlink_header_t));
    if (size > response_capacity || (size > 0U && !response)) {
        free(rx);
        return GRAPE_ERROR_PROTOCOL;
    }

    if (size > 0U) {
        memcpy(response, rx + sizeof(gfxlink_header_t), size);
    }
    free(rx);

    *response_size = size;
    device->sequence++;
    if (device->sequence == 0U) {
        device->sequence = 1U;
    }
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

int grape_resource_create(grape_device_t *device,
                          gfxlink_resource_kind_t kind,
                          uint32_t total_size,
                          grape_handle_t *out_handle)
{
    if (!out_handle || total_size == 0U || total_size > GFXLINK_MAX_RESOURCE_SIZE) {
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

    *out_handle = le32toh(response.handle);
    return GRAPE_OK;
}

int grape_resource_write(grape_device_t *device,
                         grape_handle_t handle,
                         uint32_t offset,
                         const void *data,
                         uint32_t size)
{
    const uint32_t header_size = sizeof(gfxlink_resource_write_request_t);
    if (!data || size == 0U || size > GFXLINK_MAX_PAYLOAD - header_size) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    uint32_t payload_size = header_size + size;
    uint8_t *payload = malloc(payload_size);
    if (!payload) {
        return GRAPE_ERROR_NO_MEMORY;
    }

    gfxlink_resource_write_request_t request_header = {
        .handle = htole32(handle),
        .offset = htole32(offset),
    };
    memcpy(payload, &request_header, sizeof(request_header));
    memcpy(payload + sizeof(request_header), data, size);

    int result = status_request(device, GFXLINK_OP_RESOURCE_WRITE,
                                payload, payload_size);
    free(payload);
    return result;
}

int grape_resource_commit(grape_device_t *device, grape_handle_t handle)
{
    gfxlink_resource_handle_request_t request_payload = {
        .handle = htole32(handle),
    };
    return status_request(device, GFXLINK_OP_RESOURCE_COMMIT,
                          &request_payload, sizeof(request_payload));
}

int grape_resource_destroy(grape_device_t *device, grape_handle_t handle)
{
    gfxlink_resource_handle_request_t request_payload = {
        .handle = htole32(handle),
    };
    return status_request(device, GFXLINK_OP_RESOURCE_DESTROY,
                          &request_payload, sizeof(request_payload));
}

int grape_resource_upload(grape_device_t *device,
                           gfxlink_resource_kind_t kind,
                           const void *data,
                           uint32_t size,
                           grape_handle_t *out_handle)
{
    if (!device || !data || !out_handle || size == 0U) {
        return GRAPE_ERROR_INVALID_ARGUMENT;
    }

    grape_handle_t handle = 0U;
    int result = grape_resource_create(device, kind, size, &handle);
    if (result != GRAPE_OK) {
        return result;
    }

    const uint8_t *bytes = data;
    const uint32_t max_chunk =
        GFXLINK_MAX_PAYLOAD - sizeof(gfxlink_resource_write_request_t);

    uint32_t offset = 0U;
    while (offset < size) {
        uint32_t remaining = size - offset;
        uint32_t chunk = remaining < max_chunk ? remaining : max_chunk;
        result = grape_resource_write(device, handle, offset, bytes + offset, chunk);
        if (result != GRAPE_OK) {
            grape_resource_destroy(device, handle);
            return result;
        }
        offset += chunk;
    }

    result = grape_resource_commit(device, handle);
    if (result != GRAPE_OK) {
        grape_resource_destroy(device, handle);
        return result;
    }

    *out_handle = handle;
    return GRAPE_OK;
}
