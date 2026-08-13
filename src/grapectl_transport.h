#pragma once

#define GFXLINK_TIMEOUT_MS 1500

static uint32_t s_sequence = 1;

static uint32_t grapectl_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return htole32(bits);
}

static int32_t grapectl_status(const void *payload)
{
    int32_t value;
    memcpy(&value, payload, sizeof(value));
    return (int32_t)le32toh((uint32_t)value);
}

static int grapectl_request(libusb_device_handle *device, uint8_t opcode,
                            const void *payload, uint32_t payload_size,
                            void *response, uint32_t response_capacity,
                            uint32_t *response_size)
{
    uint8_t tx[sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD];
    uint8_t rx[sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD];
    if (payload_size > GFXLINK_MAX_PAYLOAD) return LIBUSB_ERROR_INVALID_PARAM;

    gfxlink_header_t header = {
        .magic = htole32(GFXLINK_MAGIC),
        .version = GFXLINK_PROTOCOL_VERSION,
        .opcode = opcode,
        .flags = 0,
        .sequence = htole32(s_sequence),
        .payload_size = htole32(payload_size),
    };
    memcpy(tx, &header, sizeof(header));
    if (payload_size) memcpy(tx + sizeof(header), payload, payload_size);

    int transferred = 0;
    int ret = libusb_bulk_transfer(device, GFXLINK_USB_EP_OUT, tx,
                                   (int)(sizeof(header) + payload_size),
                                   &transferred, GFXLINK_TIMEOUT_MS);
    if (ret != 0) return ret;
    if (transferred != (int)(sizeof(header) + payload_size)) return LIBUSB_ERROR_IO;

    size_t received = 0;
    size_t expected = 0;
    while (expected == 0 || received < expected) {
        ret = libusb_bulk_transfer(device, GFXLINK_USB_EP_IN, rx + received,
                                   (int)(sizeof(rx) - received), &transferred,
                                   GFXLINK_TIMEOUT_MS);
        if (ret != 0) return ret;
        if (transferred <= 0) return LIBUSB_ERROR_IO;
        received += (size_t)transferred;

        if (expected == 0 && received >= sizeof(gfxlink_header_t)) {
            gfxlink_header_t incoming;
            memcpy(&incoming, rx, sizeof(incoming));
            if (le32toh(incoming.magic) != GFXLINK_MAGIC ||
                incoming.version != GFXLINK_PROTOCOL_VERSION ||
                incoming.opcode != opcode ||
                !(le16toh(incoming.flags) & GFXLINK_FLAG_RESPONSE) ||
                le32toh(incoming.sequence) != s_sequence) return LIBUSB_ERROR_IO;
            uint32_t size = le32toh(incoming.payload_size);
            if (size > GFXLINK_MAX_PAYLOAD) return LIBUSB_ERROR_OVERFLOW;
            expected = sizeof(gfxlink_header_t) + size;
        }
    }

    uint32_t size = (uint32_t)(expected - sizeof(gfxlink_header_t));
    if (size > response_capacity) return LIBUSB_ERROR_OVERFLOW;
    if (size) memcpy(response, rx + sizeof(gfxlink_header_t), size);
    *response_size = size;
    s_sequence++;
    return 0;
}

static int grapectl_check_status(const void *payload, uint32_t size)
{
    if (size < sizeof(gfxlink_status_response_t)) {
        fprintf(stderr, "Malformed GFXLINK response\n");
        return 1;
    }
    int32_t value = grapectl_status(payload);
    if (value != GFXLINK_STATUS_OK) {
        fprintf(stderr, "GRAPE returned GFXLINK status %" PRId32 "\n", value);
        return 1;
    }
    return 0;
}
