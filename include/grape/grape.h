#pragma once

#include <stddef.h>
#include <stdint.h>

#include "grape/gfxlink_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_device grape_device_t;
typedef uint32_t grape_handle_t;

typedef enum {
    GRAPE_OK = 0,
    GRAPE_ERROR_INVALID_ARGUMENT = -1000,
    GRAPE_ERROR_NO_MEMORY = -1001,
    GRAPE_ERROR_PROTOCOL = -1002,
    GRAPE_ERROR_DEVICE_NOT_FOUND = -1003,
    GRAPE_ERROR_PERMISSION = -1004,
    GRAPE_ERROR_USB = -1005,
    GRAPE_ERROR_RETRY_LIMIT = -1006,
    GRAPE_ERROR_REMOTE_BASE = -2000,
} grape_result_t;

typedef struct {
    uint8_t protocol_version;
    uint32_t capabilities;
    uint32_t max_payload;
    uint32_t max_resource_size;
} grape_hello_info_t;

typedef struct {
    uint32_t display_width;
    uint32_t display_height;
    uint32_t pixel_format;
    uint32_t max_surfaces;
    uint32_t max_resources;
} grape_device_info_t;

typedef struct {
    grape_handle_t handle;
    uint32_t chunk_size;
    uint32_t chunk_count;
} grape_resource_info_t;

typedef struct {
    uint32_t chunk_count;
    uint32_t resource_crc32;
    uint8_t missing_bitmap[GFXLINK_RESOURCE_BITMAP_BYTES];
    uint8_t corrupt_bitmap[GFXLINK_RESOURCE_BITMAP_BYTES];
} grape_resource_commit_report_t;

typedef struct {
    uint32_t chunks_sent;
    uint32_t chunks_retransmitted;
    uint32_t commit_attempts;
} grape_resource_upload_stats_t;

int grape_open(grape_device_t **out_device);
void grape_close(grape_device_t *device);
const char *grape_error_name(int result);

int grape_hello(grape_device_t *device, grape_hello_info_t *out_info);
int grape_get_info(grape_device_t *device, grape_device_info_t *out_info);
int grape_present(grape_device_t *device);

int grape_create_solid_surface(grape_device_t *device,
                               float x,
                               float y,
                               uint32_t width,
                               uint32_t height,
                               uint8_t r,
                               uint8_t g,
                               uint8_t b,
                               uint8_t a,
                               grape_handle_t *out_handle);
int grape_surface_set_position(grape_device_t *device,
                               grape_handle_t handle,
                               float x,
                               float y);
int grape_surface_set_color(grape_device_t *device,
                            grape_handle_t handle,
                            uint8_t r,
                            uint8_t g,
                            uint8_t b,
                            uint8_t a);
int grape_surface_destroy(grape_device_t *device, grape_handle_t handle);

int grape_resource_create(grape_device_t *device,
                          gfxlink_resource_kind_t kind,
                          uint32_t total_size,
                          grape_resource_info_t *out_info);
int grape_resource_write(grape_device_t *device,
                         grape_handle_t handle,
                         uint32_t offset,
                         const void *data,
                         uint32_t size);
int grape_resource_commit(grape_device_t *device,
                          grape_handle_t handle,
                          uint32_t expected_crc32,
                          grape_resource_commit_report_t *out_report);
int grape_resource_destroy(grape_device_t *device, grape_handle_t handle);
int grape_resource_upload_ex(grape_device_t *device,
                             gfxlink_resource_kind_t kind,
                             const void *data,
                             uint32_t size,
                             grape_handle_t *out_handle,
                             grape_resource_upload_stats_t *out_stats);
int grape_resource_upload(grape_device_t *device,
                          gfxlink_resource_kind_t kind,
                          const void *data,
                          uint32_t size,
                          grape_handle_t *out_handle);

#ifdef __cplusplus
}
#endif
