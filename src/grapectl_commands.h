#pragma once
#include "grapectl_surface.h"

static void grapectl_usage(const char *program)
{
    fprintf(stderr, "Usage: %s hello | info | rect <x> <y> <width> <height> <RRGGBB|RRGGBBAA>\n", program);
}

static int grapectl_dispatch(libusb_device_handle *device, int argc, char **argv)
{
    if (!strcmp(argv[1], "rect")) return grapectl_rect(device, argc, argv);
    if (argc != 2) {
        grapectl_usage(argv[0]);
        return 2;
    }

    uint8_t opcode = !strcmp(argv[1], "hello") ? GFXLINK_OP_HELLO :
                     !strcmp(argv[1], "info") ? GFXLINK_OP_GET_INFO : 0;
    if (!opcode) {
        grapectl_usage(argv[0]);
        return 2;
    }

    uint8_t payload[64];
    uint32_t size = 0;
    int ret = grapectl_request(device, opcode, NULL, 0, payload, sizeof(payload), &size);
    if (ret != 0) {
        fprintf(stderr, "USB error: %s\n", libusb_error_name(ret));
        return 1;
    }
    if (size < sizeof(gfxlink_status_response_t) || grapectl_status(payload) != GFXLINK_STATUS_OK) return 1;

    if (opcode == GFXLINK_OP_HELLO && size == sizeof(gfxlink_hello_response_t)) {
        gfxlink_hello_response_t response;
        memcpy(&response, payload, sizeof(response));
        printf("GFXLINK v%u capabilities=0x%08" PRIx32 "\n", response.protocol_version, le32toh(response.capabilities));
    } else if (opcode == GFXLINK_OP_GET_INFO && size == sizeof(gfxlink_info_response_t)) {
        gfxlink_info_response_t response;
        memcpy(&response, payload, sizeof(response));
        printf("display=%" PRIu32 "x%" PRIu32 "\n", le32toh(response.display_width), le32toh(response.display_height));
    }
    return 0;
}
