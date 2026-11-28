// PNG -> button icon: RGB565 + 8-bit alpha, at most PNG_ICON_SIZE x PNG_ICON_SIZE (larger images
// are scaled down, aspect kept). Portable C, no allocation: the caller lends a work buffer.
// Supported: every PNG colour type and bit depth, palette and colour-key transparency (tRNS).
// Not supported: interlaced (Adam7) images.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PNG_ICON_SIZE       64 // decoded icons fit in this square
#define PNG_ICON_SOURCE_MAX 96 // largest accepted PNG width / height

typedef struct {
    uint8_t width, height;
    uint16_t colors[PNG_ICON_SIZE * PNG_ICON_SIZE]; // RGB565, row-major, `width` per row
    uint8_t alphas[PNG_ICON_SIZE * PNG_ICON_SIZE];  // 0 transparent .. 255 opaque
} png_icon_t;

typedef enum {
    PNG_ICON_OK = 0,
    PNG_ICON_NOT_PNG,
    PNG_ICON_UNSUPPORTED, // interlaced, or an invalid colour type / bit depth combination
    PNG_ICON_TOO_LARGE,   // wider or taller than PNG_ICON_SOURCE_MAX, or the rows do not fit the work buffer
    PNG_ICON_CORRUPT,
} png_icon_result_t;

// Decode the PNG file held in the first file_length bytes of work (work_size bytes in all) into
// icon. The rest of work holds the decoded rows: (1 + bytes per row) * height bytes, e.g. 37 KB for
// a 96x96 RGBA image. work is overwritten. negative: inverted colours (alpha kept). Not reentrant.
png_icon_result_t png_icon_decode(uint8_t *work, size_t file_length, size_t work_size, bool negative,
                                  png_icon_t *icon);

const char *png_icon_result_text(png_icon_result_t result);

#ifdef __cplusplus
}
#endif
