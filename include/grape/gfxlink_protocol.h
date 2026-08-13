#pragma once

#include <stdint.h>

#define GFXLINK_MAGIC 0x50415247u
#define GFXLINK_PROTOCOL_VERSION 2u
#define GFXLINK_MAX_PAYLOAD (16u * 1024u)
#define GFXLINK_MAX_RESOURCE_SIZE (16u * 1024u * 1024u)

#define GFXLINK_USB_VID 0xCAFEu
#define GFXLINK_USB_PID 0x4750u
#define GFXLINK_USB_INTERFACE 0u
#define GFXLINK_USB_EP_OUT 0x01u
#define GFXLINK_USB_EP_IN 0x81u

#define GFXLINK_FLAG_RESPONSE 0x0001u

#define GFXLINK_CAP_SOLID_SURFACE (1u << 0)
#define GFXLINK_CAP_SURFACE_POSITION (1u << 1)
#define GFXLINK_CAP_SURFACE_COLOR (1u << 2)
#define GFXLINK_CAP_SURFACE_DESTROY (1u << 3)
#define GFXLINK_CAP_EXPLICIT_PRESENT (1u << 4)
#define GFXLINK_CAP_RESOURCE_STREAM (1u << 5)

typedef enum {
    GFXLINK_OP_HELLO = 0x01,
    GFXLINK_OP_GET_INFO = 0x02,
    GFXLINK_OP_PRESENT = 0x03,

    GFXLINK_OP_CREATE_SOLID_SURFACE = 0x10,
    GFXLINK_OP_SET_SURFACE_POSITION = 0x11,
    GFXLINK_OP_SET_SURFACE_COLOR = 0x12,
    GFXLINK_OP_DESTROY_SURFACE = 0x13,

    GFXLINK_OP_RESOURCE_CREATE = 0x20,
    GFXLINK_OP_RESOURCE_WRITE = 0x21,
    GFXLINK_OP_RESOURCE_COMMIT = 0x22,
    GFXLINK_OP_RESOURCE_DESTROY = 0x23,
} gfxlink_opcode_t;

typedef enum {
    GFXLINK_STATUS_OK = 0,
    GFXLINK_STATUS_INVALID_PACKET = -1,
    GFXLINK_STATUS_UNSUPPORTED = -2,
    GFXLINK_STATUS_INVALID_ARGUMENT = -3,
    GFXLINK_STATUS_NO_MEMORY = -4,
    GFXLINK_STATUS_NOT_FOUND = -5,
    GFXLINK_STATUS_BUSY = -6,
    GFXLINK_STATUS_INTERNAL = -7,
    GFXLINK_STATUS_INCOMPLETE = -8,
} gfxlink_status_t;

typedef enum {
    GFXLINK_RESOURCE_GENERIC = 0,
    GFXLINK_RESOURCE_TEXTURE = 1,
    GFXLINK_RESOURCE_VECTOR = 2,
    GFXLINK_RESOURCE_FONT = 3,
    GFXLINK_RESOURCE_SVG = 4,
} gfxlink_resource_kind_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t opcode;
    uint16_t flags;
    uint32_t sequence;
    uint32_t payload_size;
} gfxlink_header_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint8_t protocol_version;
    uint8_t reserved[3];
    uint32_t capabilities;
    uint32_t max_payload;
    uint32_t max_resource_size;
} gfxlink_hello_response_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t pixel_format;
    uint32_t max_surfaces;
    uint32_t max_resources;
} gfxlink_info_response_t;

typedef struct __attribute__((packed)) {
    uint32_t width;
    uint32_t height;
    uint32_t x_bits;
    uint32_t y_bits;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} gfxlink_create_solid_surface_request_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t handle;
} gfxlink_create_surface_response_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint32_t x_bits;
    uint32_t y_bits;
} gfxlink_set_surface_position_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} gfxlink_set_surface_color_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
} gfxlink_destroy_surface_request_t;

typedef struct __attribute__((packed)) {
    uint32_t kind;
    uint32_t total_size;
    uint32_t flags;
} gfxlink_resource_create_request_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t handle;
} gfxlink_resource_create_response_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint32_t offset;
} gfxlink_resource_write_request_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
} gfxlink_resource_handle_request_t;

typedef struct __attribute__((packed)) {
    int32_t status;
} gfxlink_status_response_t;

_Static_assert(sizeof(gfxlink_header_t) == 16, "GFXLINK header must be 16 bytes");
_Static_assert(sizeof(gfxlink_resource_write_request_t) == 8,
               "GFXLINK resource write header must be 8 bytes");
