#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "font_aa.h"

// ---------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------
// The screen is split into four touch quadrants (slots 0..3) covering everything, the OLED included.
// Only the outer bands show them: a faint corner bracket, label and LED in each screen corner.
#define CORN_W   (UI_W / 2)
#define CORN_H   110                               // top band 0..110, bottom band 370..480
#define CORN_Y1  (UI_H - CORN_H)
#define CORN_IN  14.0f                             // bracket inset from the screen edge
#define CORN_ARM 26.0f                             // bracket arm length

#define OLED_S   3
#define OLED_PX  ((UI_W - OLED_W * OLED_S) / 2)   // 48
#define OLED_PY  144
#define BEZEL_X  (OLED_PX - 8)
#define BEZEL_Y  (OLED_PY - 4)
#define BEZEL_W  (OLED_W * OLED_S + 16)
#define BEZEL_H  (OLED_H * OLED_S + 8)

#define DOTS_Y   118                               // region 118..138
#define DOTS_H   20
#define STAT_Y   (BEZEL_Y + BEZEL_H + 1)           // region 341..365
#define STAT_H   24

// ---------------------------------------------------------------------------------------------
// Colours (8-bit RGB, packed to RGB565 at the end)
// ---------------------------------------------------------------------------------------------
typedef struct {
    int red, green, blue;
} rgb_t;

#define RGB(hex) ((rgb_t){((hex) >> 16) & 0xff, ((hex) >> 8) & 0xff, (hex) & 0xff})

static const rgb_t BG_TOP = RGB(0x0e1626), BG_BOT = RGB(0x06080e);
static const rgb_t ACCENT = RGB(0x38c6ff);
static const rgb_t TXT = RGB(0xe3ebff), TXT_DIM = RGB(0x6b7a99);
static const rgb_t LED_G = RGB(0x3cf27e), LED_R = RGB(0xff4f5e), LED_LOC = RGB(0x27324a);
static const rgb_t BEZEL = RGB(0x04060a), BEZEL_EDGE = RGB(0x1e2a40);
static const rgb_t PIX_ON = RGB(0xeef7ff), PIX_HALO = RGB(0x5d8fc4), PIX_OFF = RGB(0x0b1019);
static const rgb_t AMBER = RGB(0xffb547), OFF = RGB(0x46516a);

// RGB565 with 4x4 ordered dithering: smooth gradients without banding.
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static uint16_t *s_framebuffer;
typedef struct {
    uint8_t red, green, blue;
} rgb8_t;
static rgb8_t s_bg8[UI_H]; // background gradient per row
#define BG(y) ((rgb_t){s_bg8[y].red, s_bg8[y].green, s_bg8[y].blue})

// Shared text canvas, sized for the largest user: the status bar (460 x 24). A corner label
// (240 x 24) fits in the same bytes.
#define MASK_BYTES ((UI_W - 20) * STAT_H)
static uint8_t s_mask[MASK_BYTES];
static int s_mask_w, s_mask_h;

static ui_view_t s_prev;
static bool s_first = true;

static void draw_corner(int slot, const ui_slot_t *corner);
static void draw_oled(const ui_view_t *view);
static void draw_dots(const ui_view_t *view);
static void draw_status(const ui_view_t *view);
static void fit(const aa_font_t *font, char *text, int max_width);
static void mask_clear(int width, int height);
static void mask_text(const aa_font_t *font, int x, int y, const char *text);
static int text_width(const aa_font_t *font, const char *text);
static const aa_glyph_t *font_glyph(const aa_font_t *font, unsigned char character);
static float seg_dist(float point_x, float point_y, float start_x, float start_y, float end_x, float end_y);
static int rrect_cov(float x, float y, float width, float height, float radius);
static inline rgb_t mix(rgb_t from, rgb_t target, int amount);
static inline uint16_t pack_d(rgb_t color, int x, int y);
static inline int clamp255(float value);

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------
void ui_init(uint16_t *framebuffer)
{
    s_framebuffer = framebuffer;
    for (int y = 0; y < UI_H; y++) {
        // vertical gradient with a slight lift around the display
        float gradient_position = (float)y / (UI_H - 1);
        rgb_t color = mix(BG_TOP, BG_BOT, (int)(gradient_position * 255));
        float lift = 1.0f - fabsf(y - 240.0f) / 240.0f;
        color = mix(color, RGB(0x16213a), (int)(lift * lift * 70));
        s_bg8[y] = (rgb8_t){(uint8_t)color.red, (uint8_t)color.green, (uint8_t)color.blue};
        for (int x = 0; x < UI_W; x++) {
            framebuffer[y * UI_W + x] = pack_d(color, x, y);
        }
    }
    // faint divider between the top and bottom quadrants, beside the OLED bezel
    for (int x = 0; x < UI_W; x++) {
        if (x < BEZEL_X - 8 || x >= BEZEL_X + BEZEL_W + 8) {
            int edge_distance = x < UI_W / 2 ? x : UI_W - 1 - x; // fade in from the screen edge
            framebuffer[(UI_H / 2) * UI_W + x] = pack_d(mix(BG(UI_H / 2), TXT_DIM, 40 - edge_distance), x, UI_H / 2);
        }
    }
    s_first = true;
}

void ui_render(const ui_view_t *view)
{
    for (int slot = 0; slot < PANEL_SLOTS; slot++) {
        if (s_first || memcmp(&view->slot[slot], &s_prev.slot[slot], sizeof view->slot[slot]) != 0) {
            draw_corner(slot, &view->slot[slot]);
        }
    }
    if (s_first || memcmp(view->oled, s_prev.oled, sizeof view->oled) != 0) {
        draw_oled(view);
    }
    if (s_first || view->page != s_prev.page || view->pages != s_prev.pages) {
        draw_dots(view);
    }
    if (s_first || view->link != s_prev.link || view->address != s_prev.address ||
        view->temperature_deci != s_prev.temperature_deci || view->has_temperature != s_prev.has_temperature ||
        view->serial_number != s_prev.serial_number) {
        draw_status(view);
    }
    s_prev = *view;
    s_first = false;
}

int ui_hit(int x, int y)
{
    if (x < 0 || y < 0 || x >= UI_W || y >= UI_H) {
        return -2;
    }
    return (x >= UI_W / 2) | (y >= UI_H / 2) << 1; // whole quadrant, the OLED area included
}

// ---------------------------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------------------------
// One corner band: background, faint quadrant divider, L bracket, label and LED. Low opacity at
// rest; the bracket, label and a soft glow from the corner light up while the quadrant is held.
static void draw_corner(int slot, const ui_slot_t *corner)
{
    bool right = slot & 1, bottom = slot & 2;
    int origin_x = right ? CORN_W : 0, origin_y = bottom ? CORN_Y1 : 0;
    // local coordinates mirrored so that (0, 0) is the screen corner of this quadrant
#define MX(x) (right ? CORN_W - 1 - (x) : (x))
#define MY(y) (bottom ? CORN_H - 1 - (y) : (y))
    const float inset = CORN_IN, arm = CORN_ARM;
    const float led_x = inset + 11.0f, led_y = inset + 11.0f; // in the crook of the bracket

    char label[17];
    snprintf(label, sizeof label, "%s", corner->label);
    fit(&font_small, label, CORN_W - 70);
    mask_clear(CORN_W, font_small.height);
    int label_width = text_width(&font_small, label);
    int label_x = (int)(inset + arm + 10); // after the horizontal arm, aligned with it
    mask_text(&font_small, right ? CORN_W - label_x - label_width : label_x, 0, label);
    int label_y = bottom ? CORN_H - (int)inset - font_small.height + 4 : (int)inset - 4;

    rgb_t led_color = corner->led == LEDS_GREEN ? LED_G : corner->led == LEDS_RED ? LED_R : LED_LOC;
    bool led_on = corner->led == LEDS_GREEN || corner->led == LEDS_RED;
    int glow = corner->glow;
    int bracket_alpha = 70 + ((255 - 70) * glow >> 8), text_alpha = 105 + ((255 - 105) * glow >> 8);
    rgb_t bracket_color = mix(TXT_DIM, ACCENT, glow), text_color = mix(TXT_DIM, TXT, glow);

    for (int y = 0; y < CORN_H; y++) {
        int screen_y = origin_y + y;
        uint16_t *row = s_framebuffer + screen_y * UI_W + origin_x;
        rgb_t background = BG(screen_y);
        float local_y = MY(y) + 0.5f;
        for (int x = 0; x < CORN_W; x++) {
            float local_x = MX(x) + 0.5f;
            rgb_t color = background;
            // press glow from the screen corner
            if (glow) {
                float corner_distance = sqrtf(local_x * local_x + local_y * local_y);
                if (corner_distance < 150.0f) {
                    float falloff = 1.0f - corner_distance / 150.0f;
                    color = mix(color, ACCENT, clamp255(falloff * falloff * 70.0f) * glow >> 8);
                }
            }
            // divider between the left and right quadrants, fading out toward the screen edge
            if (MX(x) == CORN_W - 1) {
                color = mix(color, TXT_DIM, 10 + 30 * (CORN_H - MY(y)) / CORN_H);
            }
            // L bracket, 3 px, round caps
            float bracket_distance = fminf(seg_dist(local_x, local_y, inset, inset, inset + arm, inset),
                                           seg_dist(local_x, local_y, inset, inset, inset, inset + arm));
            int bracket_coverage = clamp255((1.5f - bracket_distance + 0.5f) * 255.0f);
            if (bracket_coverage) {
                color = mix(color, bracket_color, bracket_coverage * bracket_alpha >> 8);
            }
            // label
            int mask_row = y - label_y;
            if (mask_row >= 0 && mask_row < s_mask_h) {
                uint8_t text_coverage = s_mask[mask_row * CORN_W + x];
                if (text_coverage) {
                    color = mix(color, text_color, text_coverage * text_alpha >> 8);
                }
            }
            // LED core + halo
            float led_offset_x = local_x - led_x, led_offset_y = local_y - led_y;
            float led_distance = sqrtf(led_offset_x * led_offset_x + led_offset_y * led_offset_y);
            if (led_on && led_distance < 24.0f) {
                float falloff = 1.0f - led_distance / 24.0f;
                color = mix(color, led_color, clamp255(falloff * falloff * 150.0f));
            }
            if (led_on || corner->locate) {
                float led_radius = led_on ? 4.5f : 3.0f;
                int led_coverage = clamp255((led_radius - led_distance + 0.5f) * 255.0f);
                if (led_coverage) {
                    color = mix(color, led_on ? mix(led_color, RGB(0xffffff), 70) : led_color, led_coverage);
                }
            }
            row[x] = pack_d(color, origin_x + x, screen_y);
        }
    }
#undef MX
#undef MY
}

static void draw_oled(const ui_view_t *view)
{
    for (int y = BEZEL_Y - 2; y < BEZEL_Y + BEZEL_H + 1; y++) {
        uint16_t *row = s_framebuffer + y * UI_W;
        rgb_t background = BG(y);
        int bezel_offset_y = y - BEZEL_Y;
        int oled_offset_y = y - OLED_PY, oled_row = oled_offset_y / OLED_S, subpixel_y = oled_offset_y % OLED_S;
        for (int x = BEZEL_X - 2; x < BEZEL_X + BEZEL_W + 2; x++) {
            int bezel_offset_x = x - BEZEL_X;
            int coverage = rrect_cov((float)bezel_offset_x, (float)bezel_offset_y, BEZEL_W, BEZEL_H, 10.0f);
            rgb_t color = background;
            if (coverage) {
                int inner_coverage = rrect_cov(bezel_offset_x - 1.0f, bezel_offset_y - 1.0f, BEZEL_W - 2.0f,
                                               BEZEL_H - 2.0f, 9.0f);
                rgb_t fill = mix(BEZEL_EDGE, BEZEL, inner_coverage);
                int oled_offset_x = x - OLED_PX;
                if (oled_offset_y >= 0 && oled_offset_y < OLED_H * OLED_S && oled_offset_x >= 0 &&
                    oled_offset_x < OLED_W * OLED_S) {
                    int oled_column = oled_offset_x / OLED_S, subpixel_x = oled_offset_x % OLED_S;
                    bool lit = (view->oled[oled_row * (OLED_W / 8) + oled_column / 8] & (0x80 >> (oled_column & 7)));
                    bool core = subpixel_x < 2 && subpixel_y < 2;
                    if (lit) {
                        fill = core ? PIX_ON : mix(BEZEL, PIX_HALO, 110);
                    } else if (core) {
                        fill = PIX_OFF;
                    }
                }
                color = mix(background, fill, coverage);
            }
            row[x] = pack_d(color, x, y);
        }
    }
}

static void draw_dots(const ui_view_t *view)
{
    const int pill_width = 22, dot_size = 7, gap = 10;
    int total_width = (view->pages - 1) * (dot_size + gap) + pill_width;
    int start_x = (UI_W - total_width) / 2, center_y = DOTS_Y + DOTS_H / 2;
    for (int y = DOTS_Y; y < DOTS_Y + DOTS_H; y++) {
        uint16_t *row = s_framebuffer + y * UI_W;
        for (int x = start_x - 4; x < start_x + total_width + 4; x++) {
            rgb_t color = BG(y);
            int dot_x = start_x;
            for (int page = 1; page <= view->pages; page++) {
                int width = page == view->page ? pill_width : dot_size;
                int coverage = rrect_cov((float)(x - dot_x), (float)(y - (center_y - dot_size / 2)), (float)width,
                                         (float)dot_size, dot_size / 2.0f);
                if (coverage) {
                    color = mix(color, page == view->page ? ACCENT : OFF, coverage);
                }
                dot_x += width + gap;
            }
            row[x] = pack_d(color, x, y);
        }
    }
}

static void draw_status(const ui_view_t *view)
{
    char left[40], right[24];
    if (view->link == 2) {
        snprintf(left, sizeof left, "CLU  |  address %u", view->address);
    } else if (view->link == 1) {
        snprintf(left, sizeof left, "address %u, no polls", view->address);
    } else {
        snprintf(left, sizeof left, "waiting for CLU  |  SN %lu", (unsigned long)view->serial_number);
    }
    if (view->has_temperature) {
        int temperature = view->temperature_deci;
        snprintf(right, sizeof right, "%d.%d\x7f" "C", temperature / 10, (temperature < 0 ? -temperature : temperature) % 10);
    } else {
        right[0] = 0;
    }
    mask_clear(UI_W - 20, STAT_H);
    int text_y = (STAT_H - font_small.height) / 2 + 1;
    mask_text(&font_small, 22, text_y, left);
    mask_text(&font_small, UI_W - 20 - 8 - text_width(&font_small, right), text_y, right);
    rgb_t dot_color = view->link == 2 ? LED_G : view->link == 1 ? AMBER : OFF;
    float dot_center_y = STAT_H / 2.0f + 0.5f;
    for (int y = 0; y < STAT_H; y++) {
        uint16_t *row = s_framebuffer + (STAT_Y + y) * UI_W + 10;
        rgb_t background = BG(STAT_Y + y);
        for (int x = 0; x < UI_W - 20; x++) {
            rgb_t color = background;
            uint8_t alpha = s_mask[y * s_mask_w + x];
            if (alpha) {
                color = mix(color, TXT_DIM, alpha);
            }
            float offset_x = x + 0.5f - 12.0f, offset_y = y + 0.5f - dot_center_y;
            float distance = sqrtf(offset_x * offset_x + offset_y * offset_y);
            int dot_coverage = clamp255((4.0f - distance + 0.5f) * 255.0f);
            if (dot_coverage) {
                color = mix(color, dot_color, dot_coverage);
            }
            if (view->link == 2 && distance < 10.0f) { // soft glow when online
                float falloff = 1.0f - distance / 10.0f;
                color = mix(color, dot_color, clamp255(falloff * falloff * 90.0f));
            }
            row[x] = pack_d(color, x + 10, STAT_Y + y);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Text into an 8-bit alpha canvas
// ---------------------------------------------------------------------------------------------
// Fit a string into max_width pixels (ellipsis).
static void fit(const aa_font_t *font, char *text, int max_width)
{
    size_t length = strlen(text);
    while (length > 1 && text_width(font, text) > max_width) {
        text[--length] = 0;
        if (length > 2) {
            text[length - 1] = '.';
        }
    }
}

static void mask_clear(int width, int height)
{
    if (width * height > MASK_BYTES) { // never overrun the canvas
        height = MASK_BYTES / width;
    }
    s_mask_w = width;
    s_mask_h = height;
    memset(s_mask, 0, (size_t)(width * height));
}

static void mask_text(const aa_font_t *font, int x, int y, const char *text)
{
    for (; *text; text++) {
        const aa_glyph_t *glyph = font_glyph(font, (unsigned char)*text);
        for (int j = 0; j < glyph->height; j++) {
            for (int i = 0; i < glyph->width; i++) {
                int mask_x = x + glyph->offset_x + i, mask_y = y + glyph->offset_y + j;
                if (mask_x < 0 || mask_y < 0 || mask_x >= s_mask_w || mask_y >= s_mask_h) {
                    continue;
                }
                uint32_t pixel_index = (uint32_t)(j * glyph->width + i);
                uint8_t packed = font->data[glyph->data_offset + pixel_index / 2];
                int alpha = ((pixel_index & 1) ? packed & 0x0f : packed >> 4) * 17;
                uint8_t *mask_pixel = &s_mask[mask_y * s_mask_w + mask_x];
                if (alpha > *mask_pixel) {
                    *mask_pixel = (uint8_t)alpha;
                }
            }
        }
        x += glyph->advance;
    }
}

static int text_width(const aa_font_t *font, const char *text)
{
    int width = 0;
    for (; *text; text++) {
        width += font_glyph(font, (unsigned char)*text)->advance;
    }
    return width;
}

static const aa_glyph_t *font_glyph(const aa_font_t *font, unsigned char character)
{
    if (character < 0x20 || character > 0x7f) {
        character = '?';
    }
    return &font->glyphs[character - 0x20];
}

// ---------------------------------------------------------------------------------------------
// Geometry and colour helpers
// ---------------------------------------------------------------------------------------------
// Distance from (point_x, point_y) to the segment (start_x, start_y)-(end_x, end_y).
static float seg_dist(float point_x, float point_y, float start_x, float start_y, float end_x, float end_y)
{
    float segment_x = end_x - start_x, segment_y = end_y - start_y;
    float offset_x = point_x - start_x, offset_y = point_y - start_y;
    float projection = (offset_x * segment_x + offset_y * segment_y) / (segment_x * segment_x + segment_y * segment_y);
    projection = projection < 0 ? 0 : projection > 1 ? 1 : projection;
    float delta_x = offset_x - projection * segment_x, delta_y = offset_y - projection * segment_y;
    return sqrtf(delta_x * delta_x + delta_y * delta_y);
}

// Coverage (0..255) of a rounded rectangle at pixel (x, y) relative to its top-left corner.
static int rrect_cov(float x, float y, float width, float height, float radius)
{
    float center_x = x + 0.5f, center_y = y + 0.5f;
    float corner_x = center_x < radius ? radius - center_x : center_x > width - radius ? center_x - (width - radius) : 0;
    float corner_y = center_y < radius ? radius - center_y : center_y > height - radius ? center_y - (height - radius) : 0;
    if (center_x < 0 || center_y < 0 || center_x > width || center_y > height) {
        return 0;
    }
    float distance = (corner_x > 0 && corner_y > 0) ? sqrtf(corner_x * corner_x + corner_y * corner_y)
                                                    : (corner_x > corner_y ? corner_x : corner_y);
    if (corner_x == 0 && corner_y == 0) { // straight edges
        float edge_distance = fminf(fminf(center_x, width - center_x), fminf(center_y, height - center_y));
        return clamp255(edge_distance * 255.0f + 0.5f * 255.0f);
    }
    return clamp255((radius - distance + 0.5f) * 255.0f);
}

static inline rgb_t mix(rgb_t from, rgb_t target, int amount) // amount 0..255
{
    return (rgb_t){from.red + (((target.red - from.red) * amount) >> 8), from.green + (((target.green - from.green) * amount) >> 8),
                   from.blue + (((target.blue - from.blue) * amount) >> 8)};
}

static inline uint16_t pack_d(rgb_t color, int x, int y)
{
    int threshold = BAYER[y & 3][x & 3];
    int red = color.red + (threshold >> 1), green = color.green + (threshold >> 2), blue = color.blue + (threshold >> 1);
    red = red > 255 ? 255 : red;
    green = green > 255 ? 255 : green;
    blue = blue > 255 ? 255 : blue;
    return (uint16_t)(((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3));
}

static inline int clamp255(float value)
{
    return value <= 0 ? 0 : value >= 255 ? 255 : (int)value;
}
