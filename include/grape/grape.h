#pragma once

#include <stdbool.h>
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

typedef enum {
    GRAPE_PIXEL_FORMAT_RGB565 = GFXLINK_PIXEL_FORMAT_RGB565,
    GRAPE_PIXEL_FORMAT_RGB888 = GFXLINK_PIXEL_FORMAT_RGB888,
    GRAPE_PIXEL_FORMAT_A8 = GFXLINK_PIXEL_FORMAT_A8,
} grape_pixel_format_t;

typedef struct {
    float x;
    float y;
    float scale_x;
    float scale_y;
    float rotation;
    float origin_x;
    float origin_y;
} grape_transform_t;

#define GRAPE_TRANSFORM_DEFAULT() \
    { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f }

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
    uint32_t max_textures;
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

typedef struct {
    grape_handle_t handle;
    uint32_t width;
    uint32_t height;
    grape_pixel_format_t format;
    uint32_t stride;
    uint32_t size;
} grape_texture_info_t;

int grape_open(grape_device_t **out_device);
void grape_close(grape_device_t *device);
const char *grape_error_name(int result);

int grape_hello(grape_device_t *device, grape_hello_info_t *out_info);
int grape_get_info(grape_device_t *device, grape_device_info_t *out_info);
int grape_present(grape_device_t *device);

/* M1 convenience surface: owns an internal A8 texture. */
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

/* M2.1 textures. Pixel buffers are tightly packed. */
int grape_texture_create(grape_device_t *device,
                         uint32_t width,
                         uint32_t height,
                         grape_pixel_format_t format,
                         const void *pixels,
                         uint32_t size,
                         grape_texture_info_t *out_texture);
int grape_texture_update_rect(grape_device_t *device,
                              grape_handle_t texture,
                              uint32_t x,
                              uint32_t y,
                              uint32_t width,
                              uint32_t height,
                              const void *pixels,
                              uint32_t size);
int grape_texture_update(grape_device_t *device,
                         grape_handle_t texture,
                         uint32_t width,
                         uint32_t height,
                         const void *pixels,
                         uint32_t size);
int grape_texture_destroy(grape_device_t *device, grape_handle_t texture);

/* M2.1 persistent surface API. */
int grape_surface_create(grape_device_t *device,
                         grape_handle_t texture,
                         grape_handle_t *out_surface);
int grape_surface_set_texture(grape_device_t *device,
                              grape_handle_t surface,
                              grape_handle_t texture);
int grape_surface_set_transform(grape_device_t *device,
                                grape_handle_t surface,
                                const grape_transform_t *transform);
int grape_surface_set_position(grape_device_t *device,
                               grape_handle_t handle,
                               float x,
                               float y);
int grape_surface_set_scale(grape_device_t *device,
                            grape_handle_t surface,
                            float scale_x,
                            float scale_y);
int grape_surface_set_rotation(grape_device_t *device,
                               grape_handle_t surface,
                               float radians);
int grape_surface_set_origin(grape_device_t *device,
                             grape_handle_t surface,
                             float origin_x,
                             float origin_y);
int grape_surface_set_z(grape_device_t *device,
                        grape_handle_t surface,
                        int32_t z);
int grape_surface_set_opacity(grape_device_t *device,
                              grape_handle_t surface,
                              uint8_t opacity);
int grape_surface_set_color(grape_device_t *device,
                            grape_handle_t handle,
                            uint8_t r,
                            uint8_t g,
                            uint8_t b,
                            uint8_t a);
int grape_surface_set_visible(grape_device_t *device,
                              grape_handle_t surface,
                              bool visible);
int grape_surface_destroy(grape_device_t *device, grape_handle_t handle);

/* Generic reliable resource transport used by higher-level resource APIs. */
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
