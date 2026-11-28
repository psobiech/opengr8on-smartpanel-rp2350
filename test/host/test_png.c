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
static void test_crafted_errors(void);
static void test_fuzz_fixtures(void);
static png_icon_result_t decode_crafted(const uint8_t header[13], const uint8_t *palette, size_t palette_length,
                                        const uint8_t *zlib, size_t zlib_length);
static size_t png_chunk(uint8_t *out, const char *type, const uint8_t *data, uint32_t data_length);
static size_t stored_zlib(uint8_t *out, uint8_t compression_method, uint8_t flags, const uint8_t *raw, size_t raw_length);

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

    test_crafted_errors();
    test_fuzz_fixtures();

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

// Hand-made PNGs (stored deflate block) for each header / chunk / zlib / filter error.
static void test_crafted_errors(void)
{
    // IHDR: width, height (BE32), bit depth, colour type, compression, filter, interlace
    static const uint8_t k_gray_2x2[13] = {0, 0, 0, 2, 0, 0, 0, 2, 8, 0, 0, 0, 0};
    static const uint8_t k_rows[6] = {0, 10, 20, 0, 30, 40}; // filter byte + 2 samples, twice
    uint8_t zlib[64];
    size_t zlib_length = stored_zlib(zlib, 0x78, 0x01, k_rows, sizeof k_rows);
    check("crafted gray 2x2: decodes", decode_crafted(k_gray_2x2, NULL, 0, zlib, zlib_length) == PNG_ICON_OK &&
                                          s_icon.width == 2 && s_icon.alphas[3] == 255);

    uint8_t header[13];
    static const struct {
        int byte_index;
        uint8_t value;
        png_icon_result_t expected;
        const char *name;
    } k_header_errors[] = {
        {3, 0, PNG_ICON_CORRUPT, "width 0: corrupt"},
        {7, 0, PNG_ICON_CORRUPT, "height 0: corrupt"},
        {10, 1, PNG_ICON_CORRUPT, "compression method 1: corrupt"},
        {11, 1, PNG_ICON_CORRUPT, "filter method 1: corrupt"},
        {9, 5, PNG_ICON_UNSUPPORTED, "colour type 5: unsupported"},
        {9, 7, PNG_ICON_UNSUPPORTED, "colour type 7: unsupported"},
        {8, 3, PNG_ICON_UNSUPPORTED, "gray, bit depth 3: unsupported"},
    };
    for (size_t i = 0; i < sizeof k_header_errors / sizeof k_header_errors[0]; i++) {
        memcpy(header, k_gray_2x2, sizeof header);
        header[k_header_errors[i].byte_index] = k_header_errors[i].value;
        check(k_header_errors[i].name,
              decode_crafted(header, NULL, 0, zlib, zlib_length) == k_header_errors[i].expected);
    }
    memcpy(header, k_gray_2x2, sizeof header);
    header[8] = 4, header[9] = 2;
    check("RGB, bit depth 4: unsupported", decode_crafted(header, NULL, 0, zlib, zlib_length) == PNG_ICON_UNSUPPORTED);
    header[8] = 16, header[9] = 3;
    check("palette, bit depth 16: unsupported", decode_crafted(header, NULL, 0, zlib, zlib_length) == PNG_ICON_UNSUPPORTED);
    header[8] = 8;
    check("palette without PLTE: corrupt", decode_crafted(header, NULL, 0, zlib, zlib_length) == PNG_ICON_CORRUPT);
    static const uint8_t k_palette[6] = {255, 0, 0, 0, 255, 0};
    check("PLTE length not a multiple of 3: corrupt", decode_crafted(header, k_palette, 4, zlib, zlib_length) ==
                                                           PNG_ICON_CORRUPT);
    check("palette index beyond the palette: transparent",
          decode_crafted(header, k_palette, 6, zlib, zlib_length) == PNG_ICON_OK && s_icon.alphas[0] == 0);
    check("RGB rows shorter than the header says: corrupt",
          (header[9] = 2, decode_crafted(header, NULL, 0, zlib, zlib_length)) == PNG_ICON_CORRUPT);

    uint8_t bad_zlib[64];
    size_t bad_length = stored_zlib(bad_zlib, 0x78, 0x02, k_rows, sizeof k_rows);
    check("zlib header check bits wrong: corrupt", decode_crafted(k_gray_2x2, NULL, 0, bad_zlib, bad_length) ==
                                                       PNG_ICON_CORRUPT);
    bad_length = stored_zlib(bad_zlib, 0x78, 0x20, k_rows, sizeof k_rows);
    check("zlib preset dictionary: corrupt", decode_crafted(k_gray_2x2, NULL, 0, bad_zlib, bad_length) ==
                                                 PNG_ICON_CORRUPT);
    bad_length = stored_zlib(bad_zlib, 0x77, 0x01, k_rows, sizeof k_rows);
    check("zlib method not deflate: corrupt", decode_crafted(k_gray_2x2, NULL, 0, bad_zlib, bad_length) ==
                                                  PNG_ICON_CORRUPT);
    check("IDAT shorter than a zlib stream: corrupt", decode_crafted(k_gray_2x2, NULL, 0, zlib, 5) == PNG_ICON_CORRUPT);
    check("no IDAT: corrupt", decode_crafted(k_gray_2x2, NULL, 0, zlib, 0) == PNG_ICON_CORRUPT);
    static const uint8_t k_bad_filter_rows[6] = {7, 10, 20, 0, 30, 40};
    bad_length = stored_zlib(bad_zlib, 0x78, 0x01, k_bad_filter_rows, sizeof k_bad_filter_rows);
    check("row filter 7: corrupt", decode_crafted(k_gray_2x2, NULL, 0, bad_zlib, bad_length) == PNG_ICON_CORRUPT);
    bad_length = stored_zlib(bad_zlib, 0x78, 0x01, k_rows, 3);
    check("fewer rows than the height: corrupt", decode_crafted(k_gray_2x2, NULL, 0, bad_zlib, bad_length) ==
                                                     PNG_ICON_CORRUPT);
    bad_zlib[2] = 0x07; // final block of the reserved type 3
    check("broken deflate data: corrupt", decode_crafted(k_gray_2x2, NULL, 0, bad_zlib, bad_length) == PNG_ICON_CORRUPT);

    size_t length = 8;
    memcpy(s_work, (const uint8_t[]){137, 'P', 'N', 'G', '\r', '\n', 26, '\n'}, 8);
    length += png_chunk(s_work + length, "IHDR", k_gray_2x2, 12);
    check("IHDR of 12 bytes: corrupt", png_icon_decode(s_work, length, WORK_BYTES, false, &s_icon) == PNG_ICON_CORRUPT);
    length = 8 + png_chunk(s_work + 8, "IHDR", k_gray_2x2, 13);
    s_work[length] = 0x7f; // next chunk length far beyond the file
    memcpy(s_work + length + 4, "IDAT", 4);
    memset(s_work + length + 8, 0, 4);
    check("chunk longer than the file: corrupt", png_icon_decode(s_work, length + 12, WORK_BYTES, false, &s_icon) ==
                                                     PNG_ICON_CORRUPT);
    check("file longer than the work buffer: not a PNG", png_icon_decode(s_work, 100, 99, false, &s_icon) ==
                                                             PNG_ICON_NOT_PNG);

    static const png_icon_result_t k_results[] = {PNG_ICON_OK, PNG_ICON_NOT_PNG, PNG_ICON_UNSUPPORTED,
                                                  PNG_ICON_TOO_LARGE, PNG_ICON_CORRUPT};
    bool texts_distinct = true;
    for (size_t i = 0; i < sizeof k_results / sizeof k_results[0]; i++) {
        texts_distinct &= strcmp(png_icon_result_text(k_results[i]), "?") != 0;
    }
    check("every result has a text, an unknown one '?'",
          texts_distinct && strcmp(png_icon_result_text((png_icon_result_t)99), "?") == 0);
}

// Every truncation and every single-byte corruption of the small fixtures (the 96x96 ones take
// too long under the sanitizers) decodes to some result,
// never reads or writes out of bounds (AddressSanitizer) and never returns a garbage icon size.
static void test_fuzz_fixtures(void)
{
    static const char *const k_fixtures[] = {"rgba8.png", "rgba16.png", "rgb_key.png", "gray_alpha.png", "gray2.png",
                                             "palette4.png", "interlaced.png", "wide96x40.png", "too_large.png"};
    static uint8_t original[WORK_BYTES];
    int decodes = 0;
    bool results_valid = true;
    for (size_t fixture = 0; fixture < sizeof k_fixtures / sizeof k_fixtures[0]; fixture++) {
        size_t file_length = load(k_fixtures[fixture]);
        memcpy(original, s_work, file_length);
        for (size_t cut = 0; cut <= file_length; cut++) {
            memcpy(s_work, original, file_length);
            png_icon_result_t result = png_icon_decode(s_work, cut, WORK_BYTES, false, &s_icon);
            results_valid &= result <= PNG_ICON_CORRUPT &&
                             (result != PNG_ICON_OK || (s_icon.width <= PNG_ICON_SIZE && s_icon.height <= PNG_ICON_SIZE));
            decodes++;
        }
        for (size_t position = 0; position < file_length; position++) {
            static const uint8_t k_flips[] = {0xff, 0x01, 0x80};
            for (size_t flip = 0; flip < sizeof k_flips; flip++) {
                memcpy(s_work, original, file_length);
                s_work[position] ^= k_flips[flip];
                png_icon_result_t result = png_icon_decode(s_work, file_length, WORK_BYTES, flip == 1, &s_icon);
                results_valid &= result <= PNG_ICON_CORRUPT &&
                                 (result != PNG_ICON_OK || (s_icon.width <= PNG_ICON_SIZE && s_icon.height <= PNG_ICON_SIZE));
                decodes++;
            }
        }
    }
    char message[96];
    snprintf(message, sizeof message, "fuzz: %d truncated / corrupted fixtures decode safely", decodes);
    check(message, results_valid);
}

// Signature, IHDR, optional PLTE, one IDAT with the zlib stream, IEND (CRCs are not checked).
static png_icon_result_t decode_crafted(const uint8_t header[13], const uint8_t *palette, size_t palette_length,
                                        const uint8_t *zlib, size_t zlib_length)
{
    static const uint8_t k_signature[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    memcpy(s_work, k_signature, sizeof k_signature);
    size_t length = sizeof k_signature;
    length += png_chunk(s_work + length, "IHDR", header, 13);
    if (palette) {
        length += png_chunk(s_work + length, "PLTE", palette, (uint32_t)palette_length);
    }
    if (zlib_length) {
        length += png_chunk(s_work + length, "IDAT", zlib, (uint32_t)zlib_length);
    }
    length += png_chunk(s_work + length, "IEND", NULL, 0);
    memset(&s_icon, 0, sizeof s_icon);
    return png_icon_decode(s_work, length, WORK_BYTES, false, &s_icon);
}

static size_t png_chunk(uint8_t *out, const char *type, const uint8_t *data, uint32_t data_length)
{
    out[0] = (uint8_t)(data_length >> 24), out[1] = (uint8_t)(data_length >> 16);
    out[2] = (uint8_t)(data_length >> 8), out[3] = (uint8_t)data_length;
    memcpy(out + 4, type, 4);
    if (data_length) {
        memcpy(out + 8, data, data_length);
    }
    memset(out + 8 + data_length, 0, 4); // CRC
    return 12 + data_length;
}

// zlib header, one final stored deflate block, Adler-32 (not checked: zero)
static size_t stored_zlib(uint8_t *out, uint8_t compression_method, uint8_t flags, const uint8_t *raw, size_t raw_length)
{
    out[0] = compression_method;
    out[1] = flags;
    out[2] = 0x01; // final block, stored
    out[3] = (uint8_t)raw_length, out[4] = (uint8_t)(raw_length >> 8);
    out[5] = (uint8_t)~raw_length, out[6] = (uint8_t)(~raw_length >> 8);
    memcpy(out + 7, raw, raw_length);
    memset(out + 7 + raw_length, 0, 4);
    return 11 + raw_length;
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
