#include <errno.h>
#include <inttypes.h>
#include <png.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
        "  %s upload-test <bytes>\n"
        "  %s texture-demo\n"
        "  %s vector-demo\n"
        "  %s svg-demo\n"
        "  %s svg <file.svg>\n"
        "  %s text-demo <font.ttf> [text]\n"
        "  %s stream-raw-screenshots [auto|0|90|180|270]\n",
        program, program, program, program,
        program, program, program, program,
        program, program, program, program, program, program);
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
           " max_resources=%" PRIu32
           " max_textures=%" PRIu32
           " max_paths=%" PRIu32
           " max_svg=%" PRIu32
           " max_fonts=%" PRIu32 "\n",
           info.display_width,
           info.display_height,
           info.pixel_format,
           info.max_surfaces,
           info.max_resources,
           info.max_textures,
           info.max_paths,
           info.max_svg_documents,
           info.max_fonts);
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


static int command_texture_demo(grape_device_t *device)
{
    const uint32_t width = 320U;
    const uint32_t height = 240U;
    const uint32_t size = width * height * 2U;
    uint8_t *pixels = malloc(size);
    if (!pixels) return print_result("texture-demo allocation", GRAPE_ERROR_NO_MEMORY);

    for (uint32_t y = 0U; y < height; ++y) {
        for (uint32_t x = 0U; x < width; ++x) {
            uint8_t r = (uint8_t)((x * 255U) / (width - 1U));
            uint8_t g = (uint8_t)((y * 255U) / (height - 1U));
            uint8_t b = ((x / 32U) ^ (y / 32U)) & 1U ? 255U : 32U;
            uint16_t rgb565 = (uint16_t)(((uint16_t)(r >> 3U) << 11U) |
                                        ((uint16_t)(g >> 2U) << 5U) |
                                        (uint16_t)(b >> 3U));
            size_t i = ((size_t)y * width + x) * 2U;
            pixels[i] = (uint8_t)(rgb565 & 0xffU);
            pixels[i + 1U] = (uint8_t)(rgb565 >> 8U);
        }
    }

    grape_texture_info_t texture;
    int result = grape_texture_create(device, width, height,
                                      GRAPE_PIXEL_FORMAT_RGB565,
                                      pixels, size, &texture);
    free(pixels);
    if (result != GRAPE_OK) return print_result("texture create", result);

    grape_handle_t surface = 0U;
    result = grape_surface_create(device, texture.handle, &surface);
    if (result == GRAPE_OK) result = grape_surface_set_position(device, surface, 80.0f, 120.0f);
    if (result == GRAPE_OK) result = grape_surface_set_origin(device, surface, 160.0f, 120.0f);
    if (result == GRAPE_OK) result = grape_surface_set_rotation(device, surface, 0.12f);
    if (result == GRAPE_OK) result = grape_surface_set_scale(device, surface, 1.35f, 1.35f);
    if (result == GRAPE_OK) result = grape_present(device);
    if (result != GRAPE_OK) {
        if (surface) grape_surface_destroy(device, surface);
        grape_texture_destroy(device, texture.handle);
        return print_result("texture-demo", result);
    }

    printf("texture=%" PRIu32 " surface=%" PRIu32
           " (left allocated so you can manipulate it)\n",
           texture.handle, surface);
    return 0;
}


static int read_file_bytes(const char *path, uint8_t **out_data, uint32_t *out_size)
{
    if (!path || !out_data || !out_size) return -1;
    *out_data = NULL;
    *out_size = 0U;

    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long length = ftell(file);
    if (length <= 0 || (unsigned long)length > UINT32_MAX ||
        (uint32_t)length > GFXLINK_MAX_RESOURCE_SIZE) {
        fclose(file);
        return -1;
    }
    rewind(file);

    uint8_t *data = malloc((size_t)length);
    if (!data) {
        fclose(file);
        return -1;
    }
    size_t got = fread(data, 1U, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) {
        free(data);
        return -1;
    }

    *out_data = data;
    *out_size = (uint32_t)length;
    return 0;
}

static int command_vector_demo(grape_device_t *device)
{
    grape_path_command_t commands[] = {
        { .type = GRAPE_PATH_MOVE_TO, .values = {150.0f, 270.0f} },
        { .type = GRAPE_PATH_CUBIC_TO,
          .values = {40.0f, 205.0f, 15.0f, 120.0f, 70.0f, 62.0f} },
        { .type = GRAPE_PATH_CUBIC_TO,
          .values = {112.0f, 18.0f, 150.0f, 52.0f, 150.0f, 92.0f} },
        { .type = GRAPE_PATH_CUBIC_TO,
          .values = {150.0f, 52.0f, 188.0f, 18.0f, 230.0f, 62.0f} },
        { .type = GRAPE_PATH_CUBIC_TO,
          .values = {285.0f, 120.0f, 260.0f, 205.0f, 150.0f, 270.0f} },
        { .type = GRAPE_PATH_CLOSE },
    };

    grape_handle_t path = 0U;
    int result = grape_path_create(
        device, commands,
        (uint32_t)(sizeof(commands) / sizeof(commands[0])),
        &path);
    if (result != GRAPE_OK) return print_result("path create", result);

    grape_path_raster_info_t raster;
    result = grape_path_rasterize(device, path, 1.0f, 4U, 2U, &raster);
    int path_cleanup = grape_path_destroy(device, path);
    if (result != GRAPE_OK) return print_result("path rasterize", result);
    if (path_cleanup != GRAPE_OK) return print_result("path destroy", path_cleanup);

    grape_handle_t surface = 0U;
    result = grape_surface_create(device, raster.texture_handle, &surface);
    if (result == GRAPE_OK) {
        result = grape_surface_set_position(
            device, surface,
            100.0f + raster.origin_x * raster.pixels_per_unit,
            160.0f + raster.origin_y * raster.pixels_per_unit);
    }
    if (result == GRAPE_OK) {
        result = grape_surface_set_color(device, surface, 245U, 80U, 160U, 255U);
    }
    if (result == GRAPE_OK) result = grape_present(device);
    if (result != GRAPE_OK) {
        if (surface) grape_surface_destroy(device, surface);
        grape_texture_destroy(device, raster.texture_handle);
        return print_result("vector-demo", result);
    }

    printf("vector texture=%" PRIu32 " surface=%" PRIu32
           " raster=%" PRIu32 "x%" PRIu32
           " origin=(%.2f,%.2f)\\n",
           raster.texture_handle, surface,
           raster.width, raster.height,
           raster.origin_x, raster.origin_y);
    return 0;
}

static int create_svg_from_text(grape_device_t *device,
                                const char *svg,
                                uint32_t svg_size,
                                const char *label)
{
    grape_device_info_t display;
    int result = grape_get_info(device, &display);
    if (result != GRAPE_OK) return print_result("info", result);

    grape_svg_config_t config = GRAPE_SVG_CONFIG_DEFAULT();
    config.x = 40.0f;
    config.y = 80.0f;
    config.width = display.display_width > 80U
                 ? (float)(display.display_width - 80U)
                 : (float)display.display_width;
    config.height = 520.0f;
    config.samples_per_axis = 4U;
    config.padding_pixels = 2U;
    config.z_base = 10;

    grape_svg_info_t info;
    result = grape_svg_create(device, svg, svg_size, &config, &info);
    if (result == GRAPE_OK) result = grape_present(device);
    if (result != GRAPE_OK) return print_result(label, result);

    printf("svg=%" PRIu32 " layers=%" PRIu32
           " viewBox=(%.2f %.2f %.2f %.2f)"
           " (left allocated; destroy with a future API client call)\\n",
           info.handle, info.layer_count,
           info.view_box_min_x, info.view_box_min_y,
           info.view_box_width, info.view_box_height);
    return 0;
}

static int command_svg_demo(grape_device_t *device)
{
    static const char svg[] =
        "<svg viewBox=\"0 0 320 240\">"
        "<path fill=\"#6f55ff\" d=\"M 20 20 H 300 V 220 H 20 Z\"/>"
        "<path fill=\"#ff4f9a\" d=\"M 55 170 C 90 65 230 65 265 170 C 210 130 110 130 55 170 Z\"/>"
        "<path fill=\"#ffffff\" d=\"M 95 90 A 22 22 0 1 0 139 90 A 22 22 0 1 0 95 90 Z M 181 90 A 22 22 0 1 0 225 90 A 22 22 0 1 0 181 90 Z\"/>"
        "</svg>";
    return create_svg_from_text(device, svg, (uint32_t)strlen(svg), "svg-demo");
}

static int command_svg_file(grape_device_t *device, int argc, char **argv)
{
    if (argc != 3) return 2;
    uint8_t *data = NULL;
    uint32_t size = 0U;
    if (read_file_bytes(argv[2], &data, &size) != 0) {
        fprintf(stderr, "unable to read SVG: %s\\n", argv[2]);
        return 1;
    }
    int result = create_svg_from_text(device, (const char *)data, size, "svg");
    free(data);
    return result;
}

static int command_text_demo(grape_device_t *device, int argc, char **argv)
{
    if (argc < 3 || argc > 4) return 2;
    const char *text = argc == 4 ? argv[3] : "Hello from Linux!";

    uint8_t *font_data = NULL;
    uint32_t font_size = 0U;
    if (read_file_bytes(argv[2], &font_data, &font_size) != 0) {
        fprintf(stderr, "unable to read font: %s\\n", argv[2]);
        return 1;
    }

    grape_font_info_t font;
    int result = grape_font_create(device, font_data, font_size, &font);
    free(font_data);
    if (result != GRAPE_OK) return print_result("font create", result);
    if (font.units_per_em == 0U) {
        grape_font_destroy(device, font.handle);
        return print_result("font metadata", GRAPE_ERROR_PROTOCOL);
    }

    const float ppem = 72.0f;
    grape_text_raster_info_t raster;
    result = grape_text_rasterize_utf8(
        device, font.handle, text,
        ppem / (float)font.units_per_em,
        4U, 2U, &raster);
    if (result != GRAPE_OK) {
        grape_font_destroy(device, font.handle);
        return print_result("text rasterize", result);
    }

    grape_handle_t surface = 0U;
    result = grape_surface_create(device, raster.texture_handle, &surface);
    if (result == GRAPE_OK) result = grape_surface_set_position(device, surface, 40.0f, 320.0f);
    if (result == GRAPE_OK) result = grape_surface_set_color(device, surface, 248U, 244U, 255U, 255U);
    if (result == GRAPE_OK) result = grape_present(device);
    if (result != GRAPE_OK) {
        if (surface) grape_surface_destroy(device, surface);
        grape_texture_destroy(device, raster.texture_handle);
        grape_font_destroy(device, font.handle);
        return print_result("text-demo", result);
    }

    printf("font=%" PRIu32 " glyphs=%" PRIu32 " units/em=%" PRIu32
           " text_texture=%" PRIu32 " surface=%" PRIu32
           " raster=%" PRIu32 "x%" PRIu32
           " advance=%.2f font-units\\n",
           font.handle, raster.glyph_count, font.units_per_em,
           raster.texture_handle, surface,
           raster.width, raster.height, raster.advance_width);
    return 0;
}

static volatile sig_atomic_t s_stream_stop;

static void stream_signal_handler(int signal_number)
{
    (void)signal_number;
    s_stream_stop = 1;
}

static int capture_plasma_wayland(const char *path)
{
    char command[1024];
    unlink(path);
    int length = snprintf(command, sizeof(command),
                          "spectacle -b -n -f -o '%s' >/dev/null 2>&1", path);
    if (length < 0 || (size_t)length >= sizeof(command)) return -1;
    int status = system(command);
    if (status == 0) return 0;

    unlink(path);
    length = snprintf(command, sizeof(command),
                      "spectacle --nonotify fullscreen --output '%s' >/dev/null 2>&1", path);
    if (length < 0 || (size_t)length >= sizeof(command)) return -1;
    return system(command) == 0 ? 0 : -1;
}

static int png_to_rgb565_scaled(const char *path,
                                uint32_t dst_width,
                                uint32_t dst_height,
                                uint8_t *dst,
                                int requested_rotation,
                                int *out_applied_rotation)
{
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path)) return -1;
    image.format = PNG_FORMAT_RGB;

    size_t source_size = PNG_IMAGE_SIZE(image);
    uint8_t *source = malloc(source_size);
    if (!source) {
        png_image_free(&image);
        return -1;
    }
    if (!png_image_finish_read(&image, NULL, source, 0, NULL)) {
        free(source);
        png_image_free(&image);
        return -1;
    }

    uint32_t src_width = image.width;
    uint32_t src_height = image.height;
    if (src_width == 0U || src_height == 0U) {
        free(source);
        png_image_free(&image);
        return -1;
    }

    int rotation = requested_rotation;
    if (rotation < 0) {
        bool src_landscape = src_width >= src_height;
        bool dst_landscape = dst_width >= dst_height;
        rotation = src_landscape == dst_landscape ? 0 : 90;
    }
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
        free(source);
        png_image_free(&image);
        return -1;
    }

    uint32_t rotated_width = (rotation == 90 || rotation == 270)
                           ? src_height : src_width;
    uint32_t rotated_height = (rotation == 90 || rotation == 270)
                            ? src_width : src_height;

    uint32_t scaled_width = dst_width;
    uint32_t scaled_height = dst_height;
    if ((uint64_t)rotated_width * dst_height >
        (uint64_t)dst_width * rotated_height) {
        scaled_height = (uint32_t)(((uint64_t)rotated_height * dst_width) /
                                   rotated_width);
        if (scaled_height == 0U) scaled_height = 1U;
    } else {
        scaled_width = (uint32_t)(((uint64_t)rotated_width * dst_height) /
                                  rotated_height);
        if (scaled_width == 0U) scaled_width = 1U;
    }

    uint32_t offset_x = (dst_width - scaled_width) / 2U;
    uint32_t offset_y = (dst_height - scaled_height) / 2U;
    memset(dst, 0, (size_t)dst_width * dst_height * 2U);

    for (uint32_t y = 0U; y < scaled_height; ++y) {
        uint32_t ry = (uint32_t)(((uint64_t)y * rotated_height) / scaled_height);
        if (ry >= rotated_height) ry = rotated_height - 1U;

        for (uint32_t x = 0U; x < scaled_width; ++x) {
            uint32_t rx = (uint32_t)(((uint64_t)x * rotated_width) / scaled_width);
            if (rx >= rotated_width) rx = rotated_width - 1U;

            uint32_t sx = 0U;
            uint32_t sy = 0U;
            switch (rotation) {
                case 0:
                    sx = rx;
                    sy = ry;
                    break;
                case 90:
                    sx = ry;
                    sy = src_height - 1U - rx;
                    break;
                case 180:
                    sx = src_width - 1U - rx;
                    sy = src_height - 1U - ry;
                    break;
                case 270:
                    sx = src_width - 1U - ry;
                    sy = rx;
                    break;
            }

            const uint8_t *src = source + ((size_t)sy * src_width + sx) * 3U;
            uint16_t rgb565 = (uint16_t)(((uint16_t)(src[0] >> 3U) << 11U) |
                                        ((uint16_t)(src[1] >> 2U) << 5U) |
                                        (uint16_t)(src[2] >> 3U));
            size_t i = ((size_t)(offset_y + y) * dst_width + offset_x + x) * 2U;
            dst[i] = (uint8_t)(rgb565 & 0xffU);
            dst[i + 1U] = (uint8_t)(rgb565 >> 8U);
        }
    }

    if (out_applied_rotation) *out_applied_rotation = rotation;
    free(source);
    png_image_free(&image);
    return 0;
}

static int command_stream_raw_screenshots(grape_device_t *device, int argc, char **argv)
{
    if (argc < 2 || argc > 3) return 2;

    int rotation = -1;
    if (argc == 3) {
        if (strcmp(argv[2], "auto") == 0) rotation = -1;
        else if (strcmp(argv[2], "0") == 0) rotation = 0;
        else if (strcmp(argv[2], "90") == 0) rotation = 90;
        else if (strcmp(argv[2], "180") == 0) rotation = 180;
        else if (strcmp(argv[2], "270") == 0) rotation = 270;
        else return 2;
    }
    if (system("command -v spectacle >/dev/null 2>&1") != 0) {
        fprintf(stderr, "stream-raw-screenshots requires KDE Spectacle in the Wayland session\n");
        return 1;
    }

    grape_device_info_t display;
    int result = grape_get_info(device, &display);
    if (result != GRAPE_OK) return print_result("info", result);
    if (display.display_width == 0U || display.display_height == 0U ||
        display.display_width > SIZE_MAX / display.display_height ||
        (size_t)display.display_width * display.display_height > SIZE_MAX / 2U) {
        fprintf(stderr, "invalid display dimensions\n");
        return 1;
    }

    size_t frame_size = (size_t)display.display_width * display.display_height * 2U;
    if (frame_size > UINT32_MAX || frame_size > GFXLINK_MAX_RESOURCE_SIZE) {
        fprintf(stderr, "display frame is too large for current GFXLINK resource limit\n");
        return 1;
    }
    uint8_t *frame = malloc(frame_size);
    if (!frame) return print_result("frame allocation", GRAPE_ERROR_NO_MEMORY);

    char path[] = "/tmp/grape-screenshot-XXXXXX.png";
    int fd = mkstemps(path, 4);
    if (fd < 0) {
        free(frame);
        perror("mkstemps");
        return 1;
    }
    close(fd);
    unlink(path);

    int applied_rotation = 0;
    if (capture_plasma_wayland(path) != 0 ||
        png_to_rgb565_scaled(path, display.display_width, display.display_height,
                             frame, rotation, &applied_rotation) != 0) {
        fprintf(stderr, "failed to capture/decode a Plasma Wayland screenshot with Spectacle\n");
        unlink(path);
        free(frame);
        return 1;
    }

    grape_texture_info_t texture;
    result = grape_texture_create(device,
                                  display.display_width,
                                  display.display_height,
                                  GRAPE_PIXEL_FORMAT_RGB565,
                                  frame, (uint32_t)frame_size,
                                  &texture);
    if (result != GRAPE_OK) {
        unlink(path);
        free(frame);
        return print_result("texture create", result);
    }

    grape_handle_t surface = 0U;
    result = grape_surface_create(device, texture.handle, &surface);
    if (result == GRAPE_OK) result = grape_present(device);
    if (result != GRAPE_OK) {
        if (surface) grape_surface_destroy(device, surface);
        grape_texture_destroy(device, texture.handle);
        unlink(path);
        free(frame);
        return print_result("stream setup", result);
    }

    printf("Streaming Plasma Wayland screenshots -> %" PRIu32 "x%" PRIu32
           " RGB565 rotation=%d. Ctrl+C to stop.\n",
           display.display_width, display.display_height, applied_rotation);
    printf("texture=%" PRIu32 " surface=%" PRIu32 "\n", texture.handle, surface);
    fflush(stdout);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stream_signal_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    s_stream_stop = 0;

    uint64_t frames = 1U;
    struct timespec report_start;
    clock_gettime(CLOCK_MONOTONIC, &report_start);
    uint64_t report_frames = 0U;

    while (!s_stream_stop) {
        if (capture_plasma_wayland(path) != 0 ||
            png_to_rgb565_scaled(path, display.display_width, display.display_height,
                                 frame, rotation, NULL) != 0) {
            fprintf(stderr, "screenshot capture failed; stopping stream\n");
            break;
        }
        result = grape_texture_update(device, texture.handle,
                                      display.display_width, display.display_height,
                                      frame, (uint32_t)frame_size);
        if (result == GRAPE_OK) result = grape_present(device);
        if (result != GRAPE_OK) {
            print_result("frame upload", result);
            break;
        }
        ++frames;
        ++report_frames;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = elapsed_seconds(&report_start, &now);
        if (elapsed >= 1.0) {
            printf("frames=%" PRIu64 " current=%.2f fps\n",
                   frames, (double)report_frames / elapsed);
            fflush(stdout);
            report_start = now;
            report_frames = 0U;
        }
    }

    unlink(path);
    int cleanup_surface = grape_surface_destroy(device, surface);
    int cleanup_texture = grape_texture_destroy(device, texture.handle);
    if (cleanup_surface == GRAPE_OK) grape_present(device);
    free(frame);

    if (result != GRAPE_OK) return 1;
    if (cleanup_surface != GRAPE_OK) return print_result("surface cleanup", cleanup_surface);
    if (cleanup_texture != GRAPE_OK) return print_result("texture cleanup", cleanup_texture);
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
    } else if (strcmp(argv[1], "texture-demo") == 0 && argc == 2) {
        status = command_texture_demo(device);
    } else if (strcmp(argv[1], "vector-demo") == 0 && argc == 2) {
        status = command_vector_demo(device);
    } else if (strcmp(argv[1], "svg-demo") == 0 && argc == 2) {
        status = command_svg_demo(device);
    } else if (strcmp(argv[1], "svg") == 0) {
        status = command_svg_file(device, argc, argv);
    } else if (strcmp(argv[1], "text-demo") == 0) {
        status = command_text_demo(device, argc, argv);
    } else if (strcmp(argv[1], "stream-raw-screenshots") == 0) {
        status = command_stream_raw_screenshots(device, argc, argv);
    }

    grape_close(device);

    if (status == 2) {
        usage(argv[0]);
    }
    return status;
}
