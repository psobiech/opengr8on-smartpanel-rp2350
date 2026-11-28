// Renders the panel UI on the host into a PPM (make -C test/host preview -> preview.png).
#include <stdio.h>
#include <string.h>

#include "panel_core.h"
#include "ui.h"

static uint16_t s_framebuffer[UI_FRAMEBUFFER_PIXELS];
static png_icon_t s_bulb_on, s_bulb_off;

static void dump(const char *path, const uint8_t *oled);
static void load_icon(const char *path, png_icon_t *icon);

int main(void)
{
    static panel_t panel;
    panel_init(&panel, 0x0f05f123);
    // CLU-drawn freedraw content
    panel_oled_text(panel.oled_draw, 4, 4, 2, "21.5\xc2\xb0" "C");
    panel_oled_text(panel.oled_draw, 4, 30, 1, "Salon  |  50%");
    panel_oled_text(panel.oled_draw, 4, 46, 1, "Rolety: otwarte");
    memcpy(panel.oled_shown, panel.oled_draw, sizeof panel.oled_shown);

    static ui_view_t view;
    view.page = 1;
    view.pages = 4;
    view.link = 2;
    view.address = 18;
    view.serial_number = panel.serial_number;
    view.has_temperature = true;
    view.temperature_deci = 234;
    const char *labels[4] = {"\xc5\x9awiat\xc5\x82o", "Rolety", "", "Scena TV"};
    for (int i = 0; i < 4; i++) {
        panel_to_ascii(view.slot[i].label, sizeof view.slot[i].label, labels[i]);
        view.slot[i].locate = true;
    }
    load_icon("fixtures/bulb_on.png", &s_bulb_on);
    load_icon("fixtures/bulb_off.png", &s_bulb_off);
    view.slot[0].icon = &s_bulb_on;  // bistable ON: IconB
    view.slot[1].icon = &s_bulb_off; // IconA
    view.slot[3].icon = &s_bulb_on;
    view.slot[0].led = LEDS_GREEN;
    view.slot[3].led = LEDS_RED;
    view.slot[1].glow = 255; // pressed
    ui_init(s_framebuffer);
    ui_render(&view);
    dump("preview_online.ppm", panel.oled_shown);

    view.link = 0;
    view.has_temperature = false;
    view.slot[1].glow = 0;
    view.page = 2;
    ui_render(&view);
    dump("preview_waiting.ppm", panel.oled_shown);
    return 0;
}

// Button icon through the firmware's decoder (fixtures: tools/gen_png_fixtures.py).
static void load_icon(const char *path, png_icon_t *icon)
{
    static uint8_t work[48 * 1024];
    FILE *file = fopen(path, "rb");
    size_t file_length = file ? fread(work, 1, sizeof work, file) : 0;
    if (file) {
        fclose(file);
    }
    png_icon_result_t result = png_icon_decode(work, file_length, sizeof work, false, icon);
    if (result != PNG_ICON_OK) {
        fprintf(stderr, "%s: %s\n", path, png_icon_result_text(result));
    }
}

// The screen as the scan-out sends it: stored rows, the rows through the OLED composed from oled.
static void dump(const char *path, const uint8_t *oled)
{
    FILE *file = fopen(path, "wb");
    fprintf(file, "P6\n%d %d\n255\n", UI_W, UI_H);
    for (int y = 0; y < UI_H; y++) {
        uint16_t composed_row[UI_W];
        const uint16_t *row = ui_row(y);
        if (!row) {
            ui_compose_oled_row(composed_row, y, oled);
            row = composed_row;
        }
        for (int x = 0; x < UI_W; x++) {
            uint16_t color = row[x];
            uint8_t pixel[3] = {(uint8_t)((color >> 8 & 0xf8) | (color >> 13)),
                                (uint8_t)((color >> 3 & 0xfc) | (color >> 9 & 3)),
                                (uint8_t)((color << 3 & 0xf8) | (color >> 2 & 7))};
            fwrite(pixel, 1, 3, file);
        }
    }
    fclose(file);
}
