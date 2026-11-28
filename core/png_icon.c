#include "png_icon.h"

#include <string.h>

#include "puff.h"

typedef struct {
    uint32_t width, height;
    uint8_t bit_depth, color_type, channels;
    size_t row_bytes;          // without the filter byte
    uint8_t palette[256][4];   // RGBA
    int palette_size;
    bool has_color_key;        // tRNS for colour types 0 / 2: this exact sample value is transparent
    uint16_t color_key[3];     // gray, or red green blue, at the image's bit depth
    const uint8_t *rows;       // unfiltered rows, each after its filter byte
} png_image_t;

static png_image_t s_image; // ~1 KB palette: static rather than on the (small) main stack

static png_icon_result_t parse_chunks(png_image_t *image, uint8_t *work, size_t file_length, size_t *image_data_length);
static png_icon_result_t check_header(png_image_t *image);
static png_icon_result_t inflate_rows(png_image_t *image, uint8_t *work, size_t image_data_length, size_t work_size);
static png_icon_result_t unfilter_rows(png_image_t *image, uint8_t *raw);
static void resample(const png_image_t *image, bool negative, png_icon_t *icon);
static void read_pixel(const png_image_t *image, uint32_t x, uint32_t y, uint8_t rgba[4]);
static uint16_t read_sample(const png_image_t *image, const uint8_t *row, uint32_t sample_index);
static uint8_t paeth(uint8_t left, uint8_t above, uint8_t above_left);
static uint32_t read_be32(const uint8_t *bytes);

png_icon_result_t png_icon_decode(uint8_t *work, size_t file_length, size_t work_size, bool negative,
                                  png_icon_t *icon)
{
    static const uint8_t k_signature[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    if (file_length < sizeof k_signature || file_length > work_size || memcmp(work, k_signature, sizeof k_signature)) {
        return PNG_ICON_NOT_PNG;
    }
    png_image_t *image = &s_image;
    memset(image, 0, sizeof *image);
    size_t image_data_length = 0;
    png_icon_result_t result = parse_chunks(image, work, file_length, &image_data_length);
    if (result == PNG_ICON_OK) {
        result = check_header(image);
    }
    if (result == PNG_ICON_OK) {
        result = inflate_rows(image, work, image_data_length, work_size);
    }
    if (result == PNG_ICON_OK) {
        resample(image, negative, icon);
    }
    return result;
}

const char *png_icon_result_text(png_icon_result_t result)
{
    switch (result) {
    case PNG_ICON_OK: return "ok";
    case PNG_ICON_NOT_PNG: return "not a PNG file";
    case PNG_ICON_UNSUPPORTED: return "unsupported PNG (interlaced?)";
    case PNG_ICON_TOO_LARGE: return "too large";
    case PNG_ICON_CORRUPT: return "corrupt PNG";
    }
    return "?";
}

// Header, palette and transparency into image; the IDAT data moved together to the start of work
// (each chunk's data only moves down, over chunks already parsed).
static png_icon_result_t parse_chunks(png_image_t *image, uint8_t *work, size_t file_length, size_t *image_data_length)
{
    bool have_header = false, have_end = false;
    size_t position = 8;
    while (!have_end && position + 12 <= file_length) {
        uint32_t chunk_length = read_be32(work + position);
        if (chunk_length > file_length - position - 12) {
            return PNG_ICON_CORRUPT;
        }
        const uint8_t *type = work + position + 4;
        const uint8_t *data = work + position + 8;
        if (!memcmp(type, "IHDR", 4)) {
            if (chunk_length != 13) {
                return PNG_ICON_CORRUPT;
            }
            image->width = read_be32(data);
            image->height = read_be32(data + 4);
            image->bit_depth = data[8];
            image->color_type = data[9];
            if (data[10] != 0 || data[11] != 0) { // compression, filter method
                return PNG_ICON_CORRUPT;
            }
            if (data[12] != 0) {
                return PNG_ICON_UNSUPPORTED; // interlaced
            }
            have_header = true;
        } else if (!memcmp(type, "PLTE", 4)) {
            if (chunk_length % 3 || chunk_length > 3 * 256) {
                return PNG_ICON_CORRUPT;
            }
            image->palette_size = (int)(chunk_length / 3);
            for (int i = 0; i < image->palette_size; i++) {
                memcpy(image->palette[i], data + 3 * i, 3);
                image->palette[i][3] = 255;
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            if (image->color_type == 3) {
                for (uint32_t i = 0; i < chunk_length && i < 256; i++) {
                    image->palette[i][3] = data[i];
                }
            } else if (image->color_type == 0 && chunk_length >= 2) {
                image->has_color_key = true;
                image->color_key[0] = (uint16_t)(data[0] << 8 | data[1]);
            } else if (image->color_type == 2 && chunk_length >= 6) {
                image->has_color_key = true;
                for (int i = 0; i < 3; i++) {
                    image->color_key[i] = (uint16_t)(data[2 * i] << 8 | data[2 * i + 1]);
                }
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            memmove(work + *image_data_length, data, chunk_length);
            *image_data_length += chunk_length;
        } else if (!memcmp(type, "IEND", 4)) {
            have_end = true;
        }
        position += 12 + (size_t)chunk_length;
    }
    return have_header && *image_data_length ? PNG_ICON_OK : PNG_ICON_CORRUPT;
}

static png_icon_result_t check_header(png_image_t *image)
{
    static const uint8_t k_channels[7] = {1, 0, 3, 1, 2, 0, 4}; // per colour type, 0: invalid
    uint8_t depth = image->bit_depth, color_type = image->color_type;
    if (color_type > 6 || !k_channels[color_type]) {
        return PNG_ICON_UNSUPPORTED;
    }
    bool depth_valid = color_type == 0   ? (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)
                       : color_type == 3 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8)
                                         : (depth == 8 || depth == 16);
    if (!depth_valid) {
        return PNG_ICON_UNSUPPORTED;
    }
    if (color_type == 3 && !image->palette_size) {
        return PNG_ICON_CORRUPT;
    }
    if (!image->width || !image->height) {
        return PNG_ICON_CORRUPT;
    }
    if (image->width > PNG_ICON_SOURCE_MAX || image->height > PNG_ICON_SOURCE_MAX) {
        return PNG_ICON_TOO_LARGE;
    }
    image->channels = k_channels[color_type];
    image->row_bytes = (image->width * image->channels * depth + 7) / 8;
    return PNG_ICON_OK;
}

// zlib stream (2-byte header, deflate data, Adler-32 not checked) -> filtered rows after the
// compacted image data, then unfiltered in place.
static png_icon_result_t inflate_rows(png_image_t *image, uint8_t *work, size_t image_data_length, size_t work_size)
{
    size_t raw_length = (1 + image->row_bytes) * image->height;
    if (raw_length > work_size - image_data_length) {
        return PNG_ICON_TOO_LARGE;
    }
    uint8_t compression_method = work[0], flags = work[1];
    if (image_data_length < 6 || (compression_method & 0x0f) != 8 || ((compression_method << 8) | flags) % 31 ||
        (flags & 0x20)) {
        return PNG_ICON_CORRUPT;
    }
    uint8_t *raw = work + image_data_length;
    unsigned long inflated_length = raw_length, deflate_length = image_data_length - 2;
    if (puff(raw, &inflated_length, work + 2, &deflate_length) != 0 || inflated_length != raw_length) {
        return PNG_ICON_CORRUPT;
    }
    return unfilter_rows(image, raw);
}

static png_icon_result_t unfilter_rows(png_image_t *image, uint8_t *raw)
{
    size_t stride = 1 + image->row_bytes;
    size_t pixel_bytes = (image->channels * image->bit_depth + 7) / 8; // the filters' "bpp", at least 1
    for (uint32_t y = 0; y < image->height; y++) {
        uint8_t filter = raw[y * stride];
        uint8_t *row = raw + y * stride + 1;
        const uint8_t *above_row = y ? row - stride : NULL;
        for (size_t i = 0; i < image->row_bytes; i++) {
            uint8_t left = i >= pixel_bytes ? row[i - pixel_bytes] : 0;
            uint8_t above = above_row ? above_row[i] : 0;
            uint8_t above_left = above_row && i >= pixel_bytes ? above_row[i - pixel_bytes] : 0;
            switch (filter) {
            case 0: break;
            case 1: row[i] = (uint8_t)(row[i] + left); break;
            case 2: row[i] = (uint8_t)(row[i] + above); break;
            case 3: row[i] = (uint8_t)(row[i] + ((left + above) >> 1)); break;
            case 4: row[i] = (uint8_t)(row[i] + paeth(left, above, above_left)); break;
            default: return PNG_ICON_CORRUPT;
            }
        }
    }
    image->rows = raw + 1;
    return PNG_ICON_OK;
}

// Into the icon at the source size, or scaled down to fit PNG_ICON_SIZE: each icon pixel averages
// the source area it covers (fractional overlap), colours weighted by alpha so transparent pixels do
// not darken the edges.
static void resample(const png_image_t *image, bool negative, png_icon_t *icon)
{
    uint32_t largest = image->width > image->height ? image->width : image->height;
    uint32_t icon_width = image->width, icon_height = image->height;
    if (largest > PNG_ICON_SIZE) {
        icon_width = (image->width * PNG_ICON_SIZE + largest / 2) / largest;
        icon_height = (image->height * PNG_ICON_SIZE + largest / 2) / largest;
        icon_width = icon_width ? icon_width : 1;
        icon_height = icon_height ? icon_height : 1;
    }
    icon->width = (uint8_t)icon_width;
    icon->height = (uint8_t)icon_height;
    float scale_x = (float)image->width / (float)icon_width, scale_y = (float)image->height / (float)icon_height;
    for (uint32_t icon_y = 0; icon_y < icon_height; icon_y++) {
        float top = icon_y * scale_y, bottom = top + scale_y;
        for (uint32_t icon_x = 0; icon_x < icon_width; icon_x++) {
            float left = icon_x * scale_x, right = left + scale_x;
            float red = 0, green = 0, blue = 0, alpha = 0, area = 0;
            for (uint32_t source_y = (uint32_t)top; source_y < image->height && (float)source_y < bottom; source_y++) {
                float overlap_y = ((float)(source_y + 1) < bottom ? (float)(source_y + 1) : bottom) -
                                  ((float)source_y > top ? (float)source_y : top);
                for (uint32_t source_x = (uint32_t)left; source_x < image->width && (float)source_x < right; source_x++) {
                    float overlap_x = ((float)(source_x + 1) < right ? (float)(source_x + 1) : right) -
                                      ((float)source_x > left ? (float)source_x : left);
                    float weight = overlap_x * overlap_y;
                    uint8_t rgba[4];
                    read_pixel(image, source_x, source_y, rgba);
                    float weighted_alpha = rgba[3] * weight;
                    red += rgba[0] * weighted_alpha;
                    green += rgba[1] * weighted_alpha;
                    blue += rgba[2] * weighted_alpha;
                    alpha += weighted_alpha;
                    area += weight;
                }
            }
            uint8_t color[3] = {0, 0, 0};
            if (alpha > 0) {
                color[0] = (uint8_t)(red / alpha + 0.5f);
                color[1] = (uint8_t)(green / alpha + 0.5f);
                color[2] = (uint8_t)(blue / alpha + 0.5f);
            }
            if (negative) {
                for (int i = 0; i < 3; i++) {
                    color[i] = (uint8_t)(255 - color[i]);
                }
            }
            size_t index = icon_y * icon_width + icon_x;
            icon->colors[index] = (uint16_t)(((color[0] & 0xf8) << 8) | ((color[1] & 0xfc) << 3) | (color[2] >> 3));
            icon->alphas[index] = (uint8_t)(alpha / area + 0.5f);
        }
    }
}

static void read_pixel(const png_image_t *image, uint32_t x, uint32_t y, uint8_t rgba[4])
{
    const uint8_t *row = image->rows + y * (1 + image->row_bytes);
    uint32_t first_sample = x * image->channels;
    uint8_t depth = image->bit_depth;
    // samples scaled to 8 bits (16-bit: the high byte; 1/2/4-bit gray: stretched to 0..255)
    #define SAMPLE_8(value) (uint8_t)(depth == 16 ? (value) >> 8 : depth == 8 ? (value) : (value) * 255 / ((1u << depth) - 1))
    switch (image->color_type) {
    case 0: { // gray
        uint16_t gray = read_sample(image, row, first_sample);
        rgba[0] = rgba[1] = rgba[2] = SAMPLE_8(gray);
        rgba[3] = image->has_color_key && gray == image->color_key[0] ? 0 : 255;
        break;
    }
    case 2: { // RGB
        uint16_t samples[3];
        for (int i = 0; i < 3; i++) {
            samples[i] = read_sample(image, row, first_sample + i);
            rgba[i] = SAMPLE_8(samples[i]);
        }
        bool keyed = image->has_color_key && samples[0] == image->color_key[0] && samples[1] == image->color_key[1] &&
                     samples[2] == image->color_key[2];
        rgba[3] = keyed ? 0 : 255;
        break;
    }
    case 3: { // palette
        uint16_t index = read_sample(image, row, first_sample);
        if (index < image->palette_size) {
            memcpy(rgba, image->palette[index], 4);
        } else {
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
        }
        break;
    }
    case 4: // gray + alpha
        rgba[0] = rgba[1] = rgba[2] = SAMPLE_8(read_sample(image, row, first_sample));
        rgba[3] = SAMPLE_8(read_sample(image, row, first_sample + 1));
        break;
    default: // 6: RGBA
        for (int i = 0; i < 4; i++) {
            rgba[i] = SAMPLE_8(read_sample(image, row, first_sample + i));
        }
        break;
    }
    #undef SAMPLE_8
}

// Sample sample_index of a row at the image's bit depth (1/2/4 bits: MSB first in each byte).
static uint16_t read_sample(const png_image_t *image, const uint8_t *row, uint32_t sample_index)
{
    switch (image->bit_depth) {
    case 16: return (uint16_t)(row[2 * sample_index] << 8 | row[2 * sample_index + 1]);
    case 8: return row[sample_index];
    default: {
        uint32_t bit_position = sample_index * image->bit_depth;
        uint8_t shift = (uint8_t)(8 - image->bit_depth - bit_position % 8);
        return (uint16_t)((row[bit_position / 8] >> shift) & ((1u << image->bit_depth) - 1));
    }
    }
}

static uint8_t paeth(uint8_t left, uint8_t above, uint8_t above_left)
{
    int estimate = left + above - above_left;
    int distance_left = estimate > left ? estimate - left : left - estimate;
    int distance_above = estimate > above ? estimate - above : above - estimate;
    int distance_above_left = estimate > above_left ? estimate - above_left : above_left - estimate;
    if (distance_left <= distance_above && distance_left <= distance_above_left) {
        return left;
    }
    return distance_above <= distance_above_left ? above : above_left;
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}
