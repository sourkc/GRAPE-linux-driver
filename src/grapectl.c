#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "grape/grape.h"

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
        "  %s present\n"
        "  %s rect <x> <y> <width> <height> <RRGGBB|RRGGBBAA>\n"
        "  %s move <handle> <x> <y>\n"
        "  %s color <handle> <RRGGBB|RRGGBBAA>\n"
        "  %s destroy <handle>\n"
        "  %s upload-test <bytes>\n",
        program, program, program, program,
        program, program, program, program);
}

static int print_result(const char *operation, int result)
{
    if (result == GRAPE_OK) {
        return 0;
    }
    fprintf(stderr, "%s failed: %s (%d)\n",
            operation, grape_error_name(result), result);
    return 1;
}

static int command_hello(grape_device_t *device)
{
    grape_hello_info_t info;
    int result = grape_hello(device, &info);
    if (result != GRAPE_OK) {
        return print_result("hello", result);
    }

    printf("GFXLINK v%u capabilities=0x%08" PRIx32
           " max_payload=%" PRIu32
           " max_resource=%" PRIu32 "\n",
           info.protocol_version,
           info.capabilities,
           info.max_payload,
           info.max_resource_size);
    return 0;
}

static int command_info(grape_device_t *device)
{
    grape_device_info_t info;
    int result = grape_get_info(device, &info);
    if (result != GRAPE_OK) {
        return print_result("info", result);
    }

    printf("display=%" PRIu32 "x%" PRIu32
           " format=%" PRIu32
           " max_surfaces=%" PRIu32
           " max_resources=%" PRIu32 "\n",
           info.display_width,
           info.display_height,
           info.pixel_format,
           info.max_surfaces,
           info.max_resources);
    return 0;
}

static int command_present(grape_device_t *device)
{
    return print_result("present", grape_present(device));
}

static int command_rect(grape_device_t *device, int argc, char **argv)
{
    if (argc != 7) {
        return 2;
    }

    float x = 0.0f;
    float y = 0.0f;
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint8_t r = 0U, g = 0U, b = 0U, a = 0U;

    if (parse_float(argv[2], &x) != 0 ||
        parse_float(argv[3], &y) != 0 ||
        parse_u32(argv[4], &width) != 0 ||
        parse_u32(argv[5], &height) != 0 ||
        width == 0U || height == 0U ||
        parse_color(argv[6], &r, &g, &b, &a) != 0) {
        return 2;
    }

    grape_handle_t handle = 0U;
    int result = grape_create_solid_surface(
        device, x, y, width, height, r, g, b, a, &handle
    );
    if (result != GRAPE_OK) {
        return print_result("rect", result);
    }

    result = grape_present(device);
    if (result != GRAPE_OK) {
        return print_result("present", result);
    }

    printf("handle=%" PRIu32 "\n", handle);
    return 0;
}

static int command_move(grape_device_t *device, int argc, char **argv)
{
    if (argc != 5) {
        return 2;
    }

    uint32_t handle = 0U;
    float x = 0.0f;
    float y = 0.0f;
    if (parse_u32(argv[2], &handle) != 0 ||
        parse_float(argv[3], &x) != 0 ||
        parse_float(argv[4], &y) != 0) {
        return 2;
    }

    int result = grape_surface_set_position(device, handle, x, y);
    if (result != GRAPE_OK) {
        return print_result("move", result);
    }
    return print_result("present", grape_present(device));
}

static int command_color(grape_device_t *device, int argc, char **argv)
{
    if (argc != 4) {
        return 2;
    }

    uint32_t handle = 0U;
    uint8_t r = 0U, g = 0U, b = 0U, a = 0U;
    if (parse_u32(argv[2], &handle) != 0 ||
        parse_color(argv[3], &r, &g, &b, &a) != 0) {
        return 2;
    }

    int result = grape_surface_set_color(device, handle, r, g, b, a);
    if (result != GRAPE_OK) {
        return print_result("color", result);
    }
    return print_result("present", grape_present(device));
}

static int command_destroy(grape_device_t *device, int argc, char **argv)
{
    if (argc != 3) {
        return 2;
    }

    uint32_t handle = 0U;
    if (parse_u32(argv[2], &handle) != 0) {
        return 2;
    }

    int result = grape_surface_destroy(device, handle);
    if (result != GRAPE_OK) {
        return print_result("destroy", result);
    }
    return print_result("present", grape_present(device));
}

static double elapsed_seconds(const struct timespec *start, const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static int command_upload_test(grape_device_t *device, int argc, char **argv)
{
    if (argc != 3) {
        return 2;
    }

    uint32_t size = 0U;
    if (parse_u32(argv[2], &size) != 0 ||
        size == 0U || size > GFXLINK_MAX_RESOURCE_SIZE) {
        return 2;
    }

    uint8_t *data = malloc(size);
    if (!data) {
        fprintf(stderr, "Unable to allocate %" PRIu32 " bytes\n", size);
        return 1;
    }

    for (uint32_t i = 0U; i < size; ++i) {
        data[i] = (uint8_t)((i * 131U + 17U) & 0xffU);
    }

    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    grape_handle_t handle = 0U;
    grape_resource_upload_stats_t stats;
    int result = grape_resource_upload_ex(
        device,
        GFXLINK_RESOURCE_GENERIC,
        data,
        size,
        &handle,
        &stats
    );

    clock_gettime(CLOCK_MONOTONIC, &end);
    free(data);

    if (result != GRAPE_OK) {
        return print_result("upload-test", result);
    }

    double seconds = elapsed_seconds(&start, &end);
    double mib = (double)size / (1024.0 * 1024.0);
    double throughput = seconds > 0.0 ? mib / seconds : 0.0;

    printf("resource=%" PRIu32
           " uploaded=%" PRIu32
           " bytes time=%.3f s throughput=%.2f MiB/s"
           " chunks=%" PRIu32
           " retransmits=%" PRIu32
           " commits=%" PRIu32 "\n",
           handle, size, seconds, throughput,
           stats.chunks_sent,
           stats.chunks_retransmitted,
           stats.commit_attempts);

    result = grape_resource_destroy(device, handle);
    if (result != GRAPE_OK) {
        return print_result("resource destroy", result);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    grape_device_t *device = NULL;
    int result = grape_open(&device);
    if (result != GRAPE_OK) {
        fprintf(stderr, "%s\n", grape_error_name(result));
        return 1;
    }

    int status = 2;
    if (strcmp(argv[1], "hello") == 0 && argc == 2) {
        status = command_hello(device);
    } else if (strcmp(argv[1], "info") == 0 && argc == 2) {
        status = command_info(device);
    } else if (strcmp(argv[1], "present") == 0 && argc == 2) {
        status = command_present(device);
    } else if (strcmp(argv[1], "rect") == 0) {
        status = command_rect(device, argc, argv);
    } else if (strcmp(argv[1], "move") == 0) {
        status = command_move(device, argc, argv);
    } else if (strcmp(argv[1], "color") == 0) {
        status = command_color(device, argc, argv);
    } else if (strcmp(argv[1], "destroy") == 0) {
        status = command_destroy(device, argc, argv);
    } else if (strcmp(argv[1], "upload-test") == 0) {
        status = command_upload_test(device, argc, argv);
    }

    grape_close(device);

    if (status == 2) {
        usage(argv[0]);
    }
    return status;
}
