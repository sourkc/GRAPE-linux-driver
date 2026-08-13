#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <libusb-1.0/libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfxlink_protocol.h"

#define GFXLINK_TIMEOUT_MS 1500

static uint32_t s_sequence = 1;

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return htole32(bits);
}

static int32_t response_status(const void *payload)
{
    int32_t value;
    memcpy(&value, payload, sizeof(value));
    return (int32_t)le32toh((uint32_t)value);
}

static int request(libusb_device_handle *device,
                   uint8_t opcode,
                   const void *payload,
                   uint32_t payload_size,
                   void *response,
                   uint32_t response_capacity,
                   uint32_t *response_size)
{
    uint8_t tx[sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD];
    uint8_t rx[sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD];

    if (!device || !response_size || payload_size > GFXLINK_MAX_PAYLOAD ||
        (payload_size > 0 && !payload)) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    gfxlink_header_t header = {
        .magic = htole32(GFXLINK_MAGIC),
        .version = GFXLINK_PROTOCOL_VERSION,
        .opcode = opcode,
        .flags = htole16(0),
        .sequence = htole32(s_sequence),
        .payload_size = htole32(payload_size),
    };

    memcpy(tx, &header, sizeof(header));
    if (payload_size > 0) {
        memcpy(tx + sizeof(header), payload, payload_size);
    }

    int transferred = 0;
    int ret = libusb_bulk_transfer(
        device,
        GFXLINK_USB_EP_OUT,
        tx,
        (int)(sizeof(header) + payload_size),
        &transferred,
        GFXLINK_TIMEOUT_MS
    );
    if (ret != 0) {
        return ret;
    }
    if (transferred != (int)(sizeof(header) + payload_size)) {
        return LIBUSB_ERROR_IO;
    }

    size_t received = 0;
    size_t expected = 0;
    while (expected == 0 || received < expected) {
        if (received == sizeof(rx)) {
            return LIBUSB_ERROR_OVERFLOW;
        }

        ret = libusb_bulk_transfer(
            device,
            GFXLINK_USB_EP_IN,
            rx + received,
            (int)(sizeof(rx) - received),
            &transferred,
            GFXLINK_TIMEOUT_MS
        );
        if (ret != 0) {
            return ret;
        }
        if (transferred <= 0) {
            return LIBUSB_ERROR_IO;
        }

        received += (size_t)transferred;

        if (expected == 0 && received >= sizeof(gfxlink_header_t)) {
            gfxlink_header_t incoming;
            memcpy(&incoming, rx, sizeof(incoming));

            if (le32toh(incoming.magic) != GFXLINK_MAGIC ||
                incoming.version != GFXLINK_PROTOCOL_VERSION ||
                incoming.opcode != opcode ||
                (le16toh(incoming.flags) & GFXLINK_FLAG_RESPONSE) == 0 ||
                le32toh(incoming.sequence) != s_sequence) {
                return LIBUSB_ERROR_IO;
            }

            uint32_t size = le32toh(incoming.payload_size);
            if (size > GFXLINK_MAX_PAYLOAD) {
                return LIBUSB_ERROR_OVERFLOW;
            }
            expected = sizeof(gfxlink_header_t) + size;
        }
    }

    uint32_t size = (uint32_t)(expected - sizeof(gfxlink_header_t));
    if (size > response_capacity || (size > 0 && !response)) {
        return LIBUSB_ERROR_OVERFLOW;
    }

    if (size > 0) {
        memcpy(response, rx + sizeof(gfxlink_header_t), size);
    }
    *response_size = size;
    s_sequence++;
    return 0;
}

static int check_status(const void *payload, uint32_t size)
{
    if (!payload || size < sizeof(gfxlink_status_response_t)) {
        fprintf(stderr, "Malformed GFXLINK response\n");
        return 1;
    }

    int32_t status = response_status(payload);
    if (status != GFXLINK_STATUS_OK) {
        fprintf(stderr, "GRAPE returned GFXLINK status %" PRId32 "\n", status);
        return 1;
    }
    return 0;
}

static int parse_float(const char *text, float *value)
{
    char *end = NULL;
    errno = 0;
    float parsed = strtof(text, &end);
    if (errno != 0 || !end || *end != '\0') {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || !end || *end != '\0' || parsed > UINT32_MAX) {
        return -1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int parse_color(const char *input, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a)
{
    const char *text = input[0] == '#' ? input + 1 : input;
    size_t length = strlen(text);
    if (length != 6 && length != 8) {
        return -1;
    }

    char *end = NULL;
    errno = 0;
    unsigned long color = strtoul(text, &end, 16);
    if (errno != 0 || !end || *end != '\0') {
        return -1;
    }

    if (length == 6) {
        *r = (uint8_t)(color >> 16);
        *g = (uint8_t)(color >> 8);
        *b = (uint8_t)color;
        *a = 255;
    } else {
        *r = (uint8_t)(color >> 24);
        *g = (uint8_t)(color >> 16);
        *b = (uint8_t)(color >> 8);
        *a = (uint8_t)color;
    }
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s hello\n"
        "  %s info\n"
        "  %s rect <x> <y> <width> <height> <RRGGBB|RRGGBBAA>\n"
        "  %s move <handle> <x> <y>\n"
        "  %s color <handle> <RRGGBB|RRGGBBAA>\n"
        "  %s destroy <handle>\n",
        program, program, program, program, program, program);
}

static int command_hello(libusb_device_handle *device)
{
    gfxlink_hello_response_t response;
    uint32_t size = 0;
    int ret = request(device, GFXLINK_OP_HELLO, NULL, 0,
                      &response, sizeof(response), &size);
    if (ret != 0) {
        fprintf(stderr, "USB error: %s\n", libusb_error_name(ret));
        return 1;
    }
    if (size != sizeof(response) || check_status(&response, size) != 0) {
        return 1;
    }

    printf("GFXLINK v%u capabilities=0x%08" PRIx32 "\n",
           response.protocol_version,
           le32toh(response.capabilities));
    return 0;
}

static int command_info(libusb_device_handle *device)
{
    gfxlink_info_response_t response;
    uint32_t size = 0;
    int ret = request(device, GFXLINK_OP_GET_INFO, NULL, 0,
                      &response, sizeof(response), &size);
    if (ret != 0) {
        fprintf(stderr, "USB error: %s\n", libusb_error_name(ret));
        return 1;
    }
    if (size != sizeof(response) || check_status(&response, size) != 0) {
        return 1;
    }

    printf("display=%" PRIu32 "x%" PRIu32 " format=%" PRIu32 " max_surfaces=%" PRIu32 "\n",
           le32toh(response.display_width),
           le32toh(response.display_height),
           le32toh(response.pixel_format),
           le32toh(response.max_surfaces));
    return 0;
}

static int command_rect(libusb_device_handle *device, int argc, char **argv)
{
    if (argc != 7) {
        return 2;
    }

    float x = 0.0f;
    float y = 0.0f;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t r = 0, g = 0, b = 0, a = 0;

    if (parse_float(argv[2], &x) != 0 ||
        parse_float(argv[3], &y) != 0 ||
        parse_u32(argv[4], &width) != 0 ||
        parse_u32(argv[5], &height) != 0 ||
        width == 0 || height == 0 ||
        parse_color(argv[6], &r, &g, &b, &a) != 0) {
        return 2;
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
    uint32_t size = 0;
    int ret = request(device, GFXLINK_OP_CREATE_SOLID_SURFACE,
                      &request_payload, sizeof(request_payload),
                      &response, sizeof(response), &size);
    if (ret != 0) {
        fprintf(stderr, "USB error: %s\n", libusb_error_name(ret));
        return 1;
    }
    if (size != sizeof(response) || check_status(&response, size) != 0) {
        return 1;
    }

    printf("handle=%" PRIu32 "\n", le32toh(response.handle));
    return 0;
}

static int command_move(libusb_device_handle *device, int argc, char **argv)
{
    if (argc != 5) {
        return 2;
    }

    uint32_t handle = 0;
    float x = 0.0f;
    float y = 0.0f;
    if (parse_u32(argv[2], &handle) != 0 ||
        parse_float(argv[3], &x) != 0 ||
        parse_float(argv[4], &y) != 0) {
        return 2;
    }

    gfxlink_set_surface_position_request_t request_payload = {
        .handle = htole32(handle),
        .x_bits = float_bits(x),
        .y_bits = float_bits(y),
    };
    gfxlink_status_response_t response;
    uint32_t size = 0;
    int ret = request(device, GFXLINK_OP_SET_SURFACE_POSITION,
                      &request_payload, sizeof(request_payload),
                      &response, sizeof(response), &size);
    if (ret != 0) {
        fprintf(stderr, "USB error: %s\n", libusb_error_name(ret));
        return 1;
    }
    return check_status(&response, size);
}

static int command_color(libusb_device_handle *device, int argc, char **argv)
{
    if (argc != 4) {
        return 2;
    }

    uint32_t handle = 0;
    uint8_t r = 0, g = 0, b = 0, a = 0;
    if (parse_u32(argv[2], &handle) != 0 ||
        parse_color(argv[3], &r, &g, &b, &a) != 0) {
        return 2;
    }

    gfxlink_set_surface_color_request_t request_payload = {
        .handle = htole32(handle),
        .r = r,
        .g = g,
        .b = b,
        .a = a,
    };
    gfxlink_status_response_t response;
    uint32_t size = 0;
    int ret = request(device, GFXLINK_OP_SET_SURFACE_COLOR,
                      &request_payload, sizeof(request_payload),
                      &response, sizeof(response), &size);
    if (ret != 0) {
        fprintf(stderr, "USB error: %s\n", libusb_error_name(ret));
        return 1;
    }
    return check_status(&response, size);
}

static int command_destroy(libusb_device_handle *device, int argc, char **argv)
{
    if (argc != 3) {
        return 2;
    }

    uint32_t handle = 0;
    if (parse_u32(argv[2], &handle) != 0) {
        return 2;
    }

    gfxlink_destroy_surface_request_t request_payload = {
        .handle = htole32(handle),
    };
    gfxlink_status_response_t response;
    uint32_t size = 0;
    int ret = request(device, GFXLINK_OP_DESTROY_SURFACE,
                      &request_payload, sizeof(request_payload),
                      &response, sizeof(response), &size);
    if (ret != 0) {
        fprintf(stderr, "USB error: %s\n", libusb_error_name(ret));
        return 1;
    }
    return check_status(&response, size);
}

static int dispatch(libusb_device_handle *device, int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }

    if (!strcmp(argv[1], "hello") && argc == 2) {
        return command_hello(device);
    }
    if (!strcmp(argv[1], "info") && argc == 2) {
        return command_info(device);
    }
    if (!strcmp(argv[1], "rect")) {
        return command_rect(device, argc, argv);
    }
    if (!strcmp(argv[1], "move")) {
        return command_move(device, argc, argv);
    }
    if (!strcmp(argv[1], "color")) {
        return command_color(device, argc, argv);
    }
    if (!strcmp(argv[1], "destroy")) {
        return command_destroy(device, argc, argv);
    }
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    libusb_context *usb = NULL;
    int ret = libusb_init(&usb);
    if (ret != 0) {
        fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(ret));
        return 1;
    }

    libusb_device_handle *device = libusb_open_device_with_vid_pid(
        usb,
        GFXLINK_USB_VID,
        GFXLINK_USB_PID
    );
    if (!device) {
        fprintf(stderr, "GRAPE %04x:%04x not found\n",
                GFXLINK_USB_VID, GFXLINK_USB_PID);
        libusb_exit(usb);
        return 1;
    }

    ret = libusb_claim_interface(device, GFXLINK_USB_INTERFACE);
    if (ret != 0) {
        fprintf(stderr, "Unable to claim GFXLINK interface: %s\n",
                libusb_error_name(ret));
        libusb_close(device);
        libusb_exit(usb);
        return 1;
    }

    int result = dispatch(device, argc, argv);
    if (result == 2) {
        usage(argv[0]);
    }

    libusb_release_interface(device, GFXLINK_USB_INTERFACE);
    libusb_close(device);
    libusb_exit(usb);
    return result;
}
