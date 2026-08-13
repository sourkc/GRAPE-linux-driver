#pragma once

static int grapectl_rect(libusb_device_handle *device, int argc, char **argv)
{
    if (argc != 7) return 2;
    char *end = NULL;
    float x = strtof(argv[2], &end); if (*end) return 2;
    float y = strtof(argv[3], &end); if (*end) return 2;
    uint32_t width = (uint32_t)strtoul(argv[4], &end, 0); if (*end) return 2;
    uint32_t height = (uint32_t)strtoul(argv[5], &end, 0); if (*end) return 2;
    const char *text = argv[6][0] == '#' ? argv[6] + 1 : argv[6];
    size_t length = strlen(text); if (length != 6 && length != 8) return 2;
    unsigned long color = strtoul(text, &end, 16); if (*end) return 2;
    uint8_t r, g, b, a;
    if (length == 6) { r=color>>16; g=color>>8; b=color; a=255; }
    else { r=color>>24; g=color>>16; b=color>>8; a=color; }
    gfxlink_create_solid_surface_request_t request = {
        .width=htole32(width), .height=htole32(height),
        .x_bits=grapectl_float_bits(x), .y_bits=grapectl_float_bits(y),
        .r=r, .g=g, .b=b, .a=a,
    };
    uint8_t payload[32]; uint32_t size=0;
    int ret=grapectl_request(device,GFXLINK_OP_CREATE_SOLID_SURFACE,&request,sizeof(request),payload,sizeof(payload),&size);
    if (ret || size != sizeof(gfxlink_create_surface_response_t)) return 1;
    gfxlink_create_surface_response_t response; memcpy(&response,payload,sizeof(response));
    if (grapectl_status(&response) != GFXLINK_STATUS_OK) return 1;
    printf("handle=%" PRIu32 "\n", le32toh(response.handle));
    return 0;
}
