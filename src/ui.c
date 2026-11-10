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
    int r, g, b;
} rgb_t;

#define RGB(h) ((rgb_t){((h) >> 16) & 0xff, ((h) >> 8) & 0xff, (h) & 0xff})

static const rgb_t BG_TOP = RGB(0x0e1626), BG_BOT = RGB(0x06080e);
static const rgb_t ACCENT = RGB(0x38c6ff);
static const rgb_t TXT = RGB(0xe3ebff), TXT_DIM = RGB(0x6b7a99);
static const rgb_t LED_G = RGB(0x3cf27e), LED_R = RGB(0xff4f5e), LED_LOC = RGB(0x27324a);
static const rgb_t BEZEL = RGB(0x04060a), BEZEL_EDGE = RGB(0x1e2a40);
static const rgb_t PIX_ON = RGB(0xeef7ff), PIX_HALO = RGB(0x5d8fc4), PIX_OFF = RGB(0x0b1019);
static const rgb_t AMBER = RGB(0xffb547), OFF = RGB(0x46516a);

static inline rgb_t mix(rgb_t a, rgb_t b, int t) // t 0..255
{
    return (rgb_t){a.r + (((b.r - a.r) * t) >> 8), a.g + (((b.g - a.g) * t) >> 8), a.b + (((b.b - a.b) * t) >> 8)};
}

// RGB565 with 4x4 ordered dithering: smooth gradients without banding.
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static inline uint16_t pack_d(rgb_t c, int x, int y)
{
    int t = BAYER[y & 3][x & 3];
    int r = c.r + (t >> 1), g = c.g + (t >> 2), b = c.b + (t >> 1);
    r = r > 255 ? 255 : r;
    g = g > 255 ? 255 : g;
    b = b > 255 ? 255 : b;
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static inline int clamp255(float v)
{
    return v <= 0 ? 0 : v >= 255 ? 255 : (int)v;
}

static uint16_t *s_fb;
typedef struct {
    uint8_t r, g, b;
} rgb8_t;
static rgb8_t s_bg8[UI_H]; // background gradient per row
#define BG(y) ((rgb_t){s_bg8[y].r, s_bg8[y].g, s_bg8[y].b})

// Coverage (0..255) of a rounded rectangle at pixel (x, y) relative to its top-left corner.
static int rrect_cov(float x, float y, float w, float h, float r)
{
    float px = x + 0.5f, py = y + 0.5f;
    float dx = px < r ? r - px : px > w - r ? px - (w - r) : 0;
    float dy = py < r ? r - py : py > h - r ? py - (h - r) : 0;
    if (px < 0 || py < 0 || px > w || py > h) {
        return 0;
    }
    float d = (dx > 0 && dy > 0) ? sqrtf(dx * dx + dy * dy) : (dx > dy ? dx : dy);
    if (dx == 0 && dy == 0) { // straight edges
        float e = fminf(fminf(px, w - px), fminf(py, h - py));
        return clamp255(e * 255.0f + 0.5f * 255.0f);
    }
    return clamp255((r - d + 0.5f) * 255.0f);
}

// ---------------------------------------------------------------------------------------------
// Text into an 8-bit alpha canvas
// ---------------------------------------------------------------------------------------------
// Shared text canvas, sized for the largest user: the status bar (460 x 24). A corner label
// (240 x 24) fits in the same bytes.
#define MASK_BYTES ((UI_W - 20) * STAT_H)
static uint8_t s_mask[MASK_BYTES];
static int s_mask_w, s_mask_h;

static void mask_clear(int w, int h)
{
    if (w * h > MASK_BYTES) { // never overrun the canvas
        h = MASK_BYTES / w;
    }
    s_mask_w = w;
    s_mask_h = h;
    memset(s_mask, 0, (size_t)(w * h));
}

static const aa_glyph_t *glyph(const aa_font_t *f, unsigned char c)
{
    if (c < 0x20 || c > 0x7f) {
        c = '?';
    }
    return &f->glyphs[c - 0x20];
}

static int text_width(const aa_font_t *f, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        w += glyph(f, (unsigned char)*s)->adv;
    }
    return w;
}

static void mask_text(const aa_font_t *f, int x, int y, const char *s)
{
    for (; *s; s++) {
        const aa_glyph_t *g = glyph(f, (unsigned char)*s);
        for (int j = 0; j < g->h; j++) {
            for (int i = 0; i < g->w; i++) {
                int px = x + g->x + i, py = y + g->y + j;
                if (px < 0 || py < 0 || px >= s_mask_w || py >= s_mask_h) {
                    continue;
                }
                uint32_t n = (uint32_t)(j * g->w + i);
                uint8_t b = f->data[g->off + n / 2];
                int a = ((n & 1) ? b & 0x0f : b >> 4) * 17;
                uint8_t *m = &s_mask[py * s_mask_w + px];
                if (a > *m) {
                    *m = (uint8_t)a;
                }
            }
        }
        x += g->adv;
    }
}

// Fit a string into max_w pixels (ellipsis).
static void fit(const aa_font_t *f, char *s, int max_w)
{
    size_t n = strlen(s);
    while (n > 1 && text_width(f, s) > max_w) {
        s[--n] = 0;
        if (n > 2) {
            s[n - 1] = '.';
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------------------------
// Distance from (px, py) to the segment (ax, ay)-(bx, by).
static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = (wx * vx + wy * vy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf(dx * dx + dy * dy);
}

// One corner band: background, faint quadrant divider, L bracket, label and LED. Low opacity at
// rest; the bracket, label and a soft glow from the corner light up while the quadrant is held.
static void draw_corner(int slot, const ui_slot_t *c)
{
    bool right = slot & 1, bottom = slot & 2;
    int ox = right ? CORN_W : 0, oy = bottom ? CORN_Y1 : 0;
    // local coordinates mirrored so that (0, 0) is the screen corner of this quadrant
#define MX(x) (right ? CORN_W - 1 - (x) : (x))
#define MY(y) (bottom ? CORN_H - 1 - (y) : (y))
    const float in = CORN_IN, arm = CORN_ARM;
    const float led_x = in + 11.0f, led_y = in + 11.0f; // in the crook of the bracket

    char label[17];
    snprintf(label, sizeof label, "%s", c->label);
    fit(&font_small, label, CORN_W - 70);
    mask_clear(CORN_W, font_small.height);
    int lw = text_width(&font_small, label);
    int lx = (int)(in + arm + 10); // after the horizontal arm, aligned with it
    mask_text(&font_small, right ? CORN_W - lx - lw : lx, 0, label);
    int label_y = bottom ? CORN_H - (int)in - font_small.height + 4 : (int)in - 4;

    rgb_t led = c->led == LEDS_GREEN ? LED_G : c->led == LEDS_RED ? LED_R : LED_LOC;
    bool led_on = c->led == LEDS_GREEN || c->led == LEDS_RED;
    int g = c->glow;
    int a_br = 70 + ((255 - 70) * g >> 8), a_tx = 105 + ((255 - 105) * g >> 8);
    rgb_t br_col = mix(TXT_DIM, ACCENT, g), tx_col = mix(TXT_DIM, TXT, g);

    for (int y = 0; y < CORN_H; y++) {
        int sy = oy + y;
        uint16_t *row = s_fb + sy * UI_W + ox;
        rgb_t bg = BG(sy);
        float ly = MY(y) + 0.5f;
        for (int x = 0; x < CORN_W; x++) {
            float lx_ = MX(x) + 0.5f;
            rgb_t col = bg;
            // press glow from the screen corner
            if (g) {
                float d = sqrtf(lx_ * lx_ + ly * ly);
                if (d < 150.0f) {
                    float h = 1.0f - d / 150.0f;
                    col = mix(col, ACCENT, clamp255(h * h * 70.0f) * g >> 8);
                }
            }
            // divider between the left and right quadrants, fading out toward the screen edge
            if (MX(x) == CORN_W - 1) {
                col = mix(col, TXT_DIM, 10 + 30 * (CORN_H - MY(y)) / CORN_H);
            }
            // L bracket, 3 px, round caps
            float d = fminf(seg_dist(lx_, ly, in, in, in + arm, in), seg_dist(lx_, ly, in, in, in, in + arm));
            int a = clamp255((1.5f - d + 0.5f) * 255.0f);
            if (a) {
                col = mix(col, br_col, a * a_br >> 8);
            }
            // label
            int my = y - label_y;
            if (my >= 0 && my < s_mask_h) {
                uint8_t t = s_mask[my * CORN_W + x];
                if (t) {
                    col = mix(col, tx_col, t * a_tx >> 8);
                }
            }
            // LED core + halo
            float dx = lx_ - led_x, dy = ly - led_y, dl = sqrtf(dx * dx + dy * dy);
            if (led_on && dl < 24.0f) {
                float h = 1.0f - dl / 24.0f;
                col = mix(col, led, clamp255(h * h * 150.0f));
            }
            if (led_on || c->locate) {
                float rc = led_on ? 4.5f : 3.0f;
                int la = clamp255((rc - dl + 0.5f) * 255.0f);
                if (la) {
                    col = mix(col, led_on ? mix(led, RGB(0xffffff), 70) : led, la);
                }
            }
            row[x] = pack_d(col, ox + x, sy);
        }
    }
#undef MX
#undef MY
}

static void draw_oled(const ui_view_t *v)
{
    for (int y = BEZEL_Y - 2; y < BEZEL_Y + BEZEL_H + 1; y++) {
        uint16_t *row = s_fb + y * UI_W;
        rgb_t bg = BG(y);
        int ry = y - BEZEL_Y;
        int oy = y - OLED_PY, cy = oy / OLED_S, sy = oy % OLED_S;
        for (int x = BEZEL_X - 2; x < BEZEL_X + BEZEL_W + 2; x++) {
            int rx = x - BEZEL_X;
            int cov = rrect_cov((float)rx, (float)ry, BEZEL_W, BEZEL_H, 10.0f);
            rgb_t col = bg;
            if (cov) {
                int inner = rrect_cov(rx - 1.0f, ry - 1.0f, BEZEL_W - 2.0f, BEZEL_H - 2.0f, 9.0f);
                rgb_t f = mix(BEZEL_EDGE, BEZEL, inner);
                int ox = x - OLED_PX;
                if (oy >= 0 && oy < OLED_H * OLED_S && ox >= 0 && ox < OLED_W * OLED_S) {
                    int cx = ox / OLED_S, sx = ox % OLED_S;
                    bool on = (v->oled[cy * (OLED_W / 8) + cx / 8] & (0x80 >> (cx & 7)));
                    bool core = sx < 2 && sy < 2;
                    if (on) {
                        f = core ? PIX_ON : mix(BEZEL, PIX_HALO, 110);
                    } else if (core) {
                        f = PIX_OFF;
                    }
                }
                col = mix(bg, f, cov);
            }
            row[x] = pack_d(col, x, y);
        }
    }
}

static void draw_dots(const ui_view_t *v)
{
    const int pill_w = 22, dot = 7, gap = 10;
    int total = (v->pages - 1) * (dot + gap) + pill_w;
    int x0 = (UI_W - total) / 2, cy = DOTS_Y + DOTS_H / 2;
    for (int y = DOTS_Y; y < DOTS_Y + DOTS_H; y++) {
        uint16_t *row = s_fb + y * UI_W;
        for (int x = x0 - 4; x < x0 + total + 4; x++) {
            rgb_t col = BG(y);
            int px = x0;
            for (int p = 1; p <= v->pages; p++) {
                int w = p == v->page ? pill_w : dot;
                int a = rrect_cov((float)(x - px), (float)(y - (cy - dot / 2)), (float)w, (float)dot, dot / 2.0f);
                if (a) {
                    col = mix(col, p == v->page ? ACCENT : OFF, a);
                }
                px += w + gap;
            }
            row[x] = pack_d(col, x, y);
        }
    }
}

static void draw_status(const ui_view_t *v)
{
    char left[40], right[24];
    if (v->link == 2) {
        snprintf(left, sizeof left, "CLU  |  address %u", v->addr);
    } else if (v->link == 1) {
        snprintf(left, sizeof left, "address %u, no polls", v->addr);
    } else {
        snprintf(left, sizeof left, "waiting for CLU  |  SN %08lx", (unsigned long)v->sn);
    }
    if (v->have_temp) {
        snprintf(right, sizeof right, "%d.%d\x7f" "C", v->temp_dc / 10, (v->temp_dc < 0 ? -v->temp_dc : v->temp_dc) % 10);
    } else {
        right[0] = 0;
    }
    mask_clear(UI_W - 20, STAT_H);
    int ty = (STAT_H - font_small.height) / 2 + 1;
    mask_text(&font_small, 22, ty, left);
    mask_text(&font_small, UI_W - 20 - 8 - text_width(&font_small, right), ty, right);
    rgb_t dotc = v->link == 2 ? LED_G : v->link == 1 ? AMBER : OFF;
    float dcy = STAT_H / 2.0f + 0.5f;
    for (int y = 0; y < STAT_H; y++) {
        uint16_t *row = s_fb + (STAT_Y + y) * UI_W + 10;
        rgb_t bg = BG(STAT_Y + y);
        for (int x = 0; x < UI_W - 20; x++) {
            rgb_t col = bg;
            uint8_t a = s_mask[y * s_mask_w + x];
            if (a) {
                col = mix(col, TXT_DIM, a);
            }
            float dx = x + 0.5f - 12.0f, dy = y + 0.5f - dcy, d = sqrtf(dx * dx + dy * dy);
            int c = clamp255((4.0f - d + 0.5f) * 255.0f);
            if (c) {
                col = mix(col, dotc, c);
            }
            if (v->link == 2 && d < 10.0f) { // soft glow when online
                float h = 1.0f - d / 10.0f;
                col = mix(col, dotc, clamp255(h * h * 90.0f));
            }
            row[x] = pack_d(col, x + 10, STAT_Y + y);
        }
    }
}

// ---------------------------------------------------------------------------------------------
static ui_view_t s_prev;
static bool s_first = true;

void ui_init(uint16_t *fb)
{
    s_fb = fb;
    for (int y = 0; y < UI_H; y++) {
        // vertical gradient with a slight lift around the display
        float t = (float)y / (UI_H - 1);
        rgb_t c = mix(BG_TOP, BG_BOT, (int)(t * 255));
        float lift = 1.0f - fabsf(y - 240.0f) / 240.0f;
        c = mix(c, RGB(0x16213a), (int)(lift * lift * 70));
        s_bg8[y] = (rgb8_t){(uint8_t)c.r, (uint8_t)c.g, (uint8_t)c.b};
        for (int x = 0; x < UI_W; x++) {
            fb[y * UI_W + x] = pack_d(c, x, y);
        }
    }
    // faint divider between the top and bottom quadrants, beside the OLED bezel
    for (int x = 0; x < UI_W; x++) {
        if (x < BEZEL_X - 8 || x >= BEZEL_X + BEZEL_W + 8) {
            int e = x < UI_W / 2 ? x : UI_W - 1 - x; // fade in from the screen edge
            fb[(UI_H / 2) * UI_W + x] = pack_d(mix(BG(UI_H / 2), TXT_DIM, 40 - e), x, UI_H / 2);
        }
    }
    s_first = true;
}

void ui_render(const ui_view_t *v)
{
    for (int s = 0; s < PANEL_SLOTS; s++) {
        if (s_first || memcmp(&v->slot[s], &s_prev.slot[s], sizeof v->slot[s]) != 0) {
            draw_corner(s, &v->slot[s]);
        }
    }
    if (s_first || memcmp(v->oled, s_prev.oled, sizeof v->oled) != 0) {
        draw_oled(v);
    }
    if (s_first || v->page != s_prev.page || v->pages != s_prev.pages) {
        draw_dots(v);
    }
    if (s_first || v->link != s_prev.link || v->addr != s_prev.addr || v->temp_dc != s_prev.temp_dc ||
        v->have_temp != s_prev.have_temp || v->sn != s_prev.sn) {
        draw_status(v);
    }
    s_prev = *v;
    s_first = false;
}

int ui_hit(int x, int y)
{
    if (x < 0 || y < 0 || x >= UI_W || y >= UI_H) {
        return -2;
    }
    return (x >= UI_W / 2) | (y >= UI_H / 2) << 1; // whole quadrant, the OLED area included
}
