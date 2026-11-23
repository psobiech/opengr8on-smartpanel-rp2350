// Renders the panel UI on the host into a PPM (make -C test/host preview -> preview.png).
#include <stdio.h>
#include <string.h>

#include "panel_core.h"
#include "ui.h"

static uint16_t s_framebuffer[UI_W * UI_H];

static void dump(const char *path);

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
    memcpy(view.oled, panel.oled_shown, sizeof view.oled);
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
    view.slot[0].led = LEDS_GREEN;
    view.slot[3].led = LEDS_RED;
    view.slot[1].glow = 255; // pressed
    ui_init(s_framebuffer);
    ui_render(&view);
    dump("preview_online.ppm");

    view.link = 0;
    view.has_temperature = false;
    view.slot[1].glow = 0;
    view.page = 2;
    ui_render(&view);
    dump("preview_waiting.ppm");
    return 0;
}

static void dump(const char *path)
{
    FILE *file = fopen(path, "wb");
    fprintf(file, "P6\n%d %d\n255\n", UI_W, UI_H);
    for (int i = 0; i < UI_W * UI_H; i++) {
        uint16_t color = s_framebuffer[i];
        uint8_t pixel[3] = {(uint8_t)((color >> 8 & 0xf8) | (color >> 13)),
                            (uint8_t)((color >> 3 & 0xfc) | (color >> 9 & 3)),
                            (uint8_t)((color << 3 & 0xf8) | (color >> 2 & 7))};
        fwrite(pixel, 1, 3, file);
    }
    fclose(file);
}
