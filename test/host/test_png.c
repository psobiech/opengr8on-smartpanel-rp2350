// Host tests for the PNG icon decoder (core/png_icon.c). The fixtures come from
// tools/gen_png_fixtures.py; the pixel patterns below must match the ones there.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png_icon.h"

#define WORK_BYTES (48 * 1024)

static int failures;
static uint8_t s_work[WORK_BYTES];
static png_icon_t s_icon;

static void check(const char *name, int condition);
static png_icon_result_t decode_file(const char *name, bool negative);
static size_t load(const char *name);
static void pattern_rgba(int x, int y, uint8_t rgba[4]);
static uint16_t rgb565(int red, int green, int blue);
static int pixels_match(int width, int height, void (*expected)(int x, int y, uint8_t rgba[4]), bool negative);
static void expected_gray_alpha(int x, int y, uint8_t rgba[4]);
static void expected_rgb_key(int x, int y, uint8_t rgba[4]);
static void expected_gray2(int x, int y, uint8_t rgba[4]);
static void expected_palette4(int x, int y, uint8_t rgba[4]);
static void expected_scaled(int x, int y, uint8_t rgba[4]);

int main(void)
{
    check("RGBA 8-bit (all five row filters, split IDAT)",
          decode_file("rgba8.png", false) == PNG_ICON_OK && s_icon.width == 16 && s_icon.height == 12 &&
              pixels_match(16, 12, pattern_rgba, false));
    check("RGBA 8-bit, negative: colours inverted, alpha kept",
          decode_file("rgba8.png", true) == PNG_ICON_OK && pixels_match(16, 12, pattern_rgba, true));
    check("RGBA 16-bit: high byte", decode_file("rgba16.png", false) == PNG_ICON_OK && pixels_match(16, 12, pattern_rgba, false));
    check("RGB with a tRNS colour key", decode_file("rgb_key.png", false) == PNG_ICON_OK && pixels_match(8, 8, expected_rgb_key, false));
    check("gray + alpha", decode_file("gray_alpha.png", false) == PNG_ICON_OK && pixels_match(16, 12, expected_gray_alpha, false));
    check("gray 2-bit (odd width) with a tRNS gray key",
          decode_file("gray2.png", false) == PNG_ICON_OK && s_icon.width == 9 && pixels_match(9, 5, expected_gray2, false));
    check("palette 4-bit with tRNS alphas",
          decode_file("palette4.png", false) == PNG_ICON_OK && pixels_match(7, 6, expected_palette4, false));
    check("96x96 scaled to 64x64: area averages, transparent pixels add no colour",
          decode_file("scale96.png", false) == PNG_ICON_OK && s_icon.width == 64 && s_icon.height == 64 &&
              pixels_match(64, 64, expected_scaled, false));
    check("96x40 scaled to 64x27 (aspect kept)",
          decode_file("wide96x40.png", false) == PNG_ICON_OK && s_icon.width == 64 && s_icon.height == 27);
    check("interlaced: unsupported", decode_file("interlaced.png", false) == PNG_ICON_UNSUPPORTED);
    check("97 px wide: too large", decode_file("too_large.png", false) == PNG_ICON_TOO_LARGE);

    size_t file_length = load("rgba8.png");
    check("truncated: corrupt", png_icon_decode(s_work, file_length / 2, WORK_BYTES, false, &s_icon) == PNG_ICON_CORRUPT);
    file_length = load("rgba8.png");
    check("rows do not fit the work buffer: too large",
          png_icon_decode(s_work, file_length, file_length + 100, false, &s_icon) == PNG_ICON_TOO_LARGE);
    memcpy(s_work, "GIF89a..", 8);
    check("not a PNG", png_icon_decode(s_work, 8, WORK_BYTES, false, &s_icon) == PNG_ICON_NOT_PNG);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void check(const char *name, int condition)
{
    printf("%s  %s\n", condition ? "ok  " : "FAIL", name);
    failures += !condition;
}

static png_icon_result_t decode_file(const char *name, bool negative)
{
    size_t file_length = load(name);
    memset(&s_icon, 0, sizeof s_icon);
    return png_icon_decode(s_work, file_length, WORK_BYTES, negative, &s_icon);
}

static size_t load(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "fixtures/%s", name);
    FILE *file = fopen(path, "rb");
    if (!file) {
        printf("FAIL  cannot open %s (run python3 tools/gen_png_fixtures.py)\n", path);
        failures++;
        return 0;
    }
    size_t file_length = fread(s_work, 1, sizeof s_work, file);
    fclose(file);
    return file_length;
}

// Same as pattern_rgba() in tools/gen_png_fixtures.py.
static void pattern_rgba(int x, int y, uint8_t rgba[4])
{
    rgba[0] = (uint8_t)((x * 37 + y * 11) & 255);
    rgba[1] = (uint8_t)((x * 5 + y * 53) & 255);
    rgba[2] = (uint8_t)((x * y * 3) & 255);
    rgba[3] = (uint8_t)((x * 29 + y * 7) & 255);
}

static uint16_t rgb565(int red, int green, int blue)
{
    return (uint16_t)(((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3));
}

// Every icon pixel against expected(); colours only where the pixel is not fully transparent.
static int pixels_match(int width, int height, void (*expected)(int x, int y, uint8_t rgba[4]), bool negative)
{
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t rgba[4];
            expected(x, y, rgba);
            if (negative) {
                for (int i = 0; i < 3; i++) {
                    rgba[i] = (uint8_t)(255 - rgba[i]);
                }
            }
            int index = y * width + x;
            bool alpha_ok = s_icon.alphas[index] == rgba[3];
            bool color_ok = !rgba[3] || s_icon.colors[index] == rgb565(rgba[0], rgba[1], rgba[2]);
            if (!alpha_ok || !color_ok) {
                printf("      pixel (%d, %d): got %04x a%u, expected %04x a%u\n", x, y, s_icon.colors[index],
                       s_icon.alphas[index], rgb565(rgba[0], rgba[1], rgba[2]), rgba[3]);
                return 0;
            }
        }
    }
    return 1;
}

static void expected_gray_alpha(int x, int y, uint8_t rgba[4])
{
    pattern_rgba(x, y, rgba);
    rgba[1] = rgba[2] = rgba[0];
}

static void expected_rgb_key(int x, int y, uint8_t rgba[4])
{
    static const uint8_t k_key[3] = {10, 20, 30};
    if ((x + y) % 3 == 0) {
        memcpy(rgba, k_key, 3);
        rgba[3] = 0;
        return;
    }
    pattern_rgba(x, y, rgba);
    rgba[3] = (rgba[0] == k_key[0] && rgba[1] == k_key[1] && rgba[2] == k_key[2]) ? 0 : 255;
}

static void expected_gray2(int x, int y, uint8_t rgba[4])
{
    int value = (x + y) % 4;
    rgba[0] = rgba[1] = rgba[2] = (uint8_t)(value * 85);
    rgba[3] = value == 1 ? 0 : 255; // tRNS gray key 1
}

static void expected_palette4(int x, int y, uint8_t rgba[4])
{
    int index = (x * 3 + y) % 10;
    rgba[0] = (uint8_t)(index * 20);
    rgba[1] = (uint8_t)(255 - index * 20);
    rgba[2] = (uint8_t)(index * 7);
    rgba[3] = index < 5 ? (uint8_t)(index * 50) : 255;
}

// scale_pattern() in tools/gen_png_fixtures.py: each 3x3 source block becomes 2x2 icon pixels, each
// covering 1.5x1.5 source pixels of that block.
static void expected_scaled(int x, int y, uint8_t rgba[4])
{
    int block_x = x / 2, block_y = y / 2;
    if ((block_x + block_y) % 7 == 0) { // opaque red row over two transparent blue rows
        rgba[0] = 250;
        rgba[1] = 10;
        rgba[2] = 10;
        rgba[3] = y % 2 == 0 ? 170 : 0; // upper icon row: 1 opaque of 1.5 rows; lower: none
        return;
    }
    pattern_rgba(block_x, block_y, rgba);
    rgba[3] = 255;
}
