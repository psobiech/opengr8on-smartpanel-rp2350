// Renders the panel UI on the host into a PPM (make -C test/host preview -> preview.png).
#include <stdio.h>
#include <string.h>

#include "panel_core.h"
#include "ui.h"

static uint16_t fb[UI_W * UI_H];

static void dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
    for (int i = 0; i < UI_W * UI_H; i++) {
        uint16_t c = fb[i];
        uint8_t px[3] = {(uint8_t)((c >> 8 & 0xf8) | (c >> 13)), (uint8_t)((c >> 3 & 0xfc) | (c >> 9 & 3)),
                         (uint8_t)((c << 3 & 0xf8) | (c >> 2 & 7))};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main(void)
{
    static panel_t p;
    panel_init(&p, 0x0f05f123);
    // CLU-drawn freedraw content
    panel_oled_text(p.oled_draw, 4, 4, 2, "21.5\xc2\xb0" "C");
    panel_oled_text(p.oled_draw, 4, 30, 1, "Salon  |  50%");
    panel_oled_text(p.oled_draw, 4, 46, 1, "Rolety: otwarte");
    memcpy(p.oled_shown, p.oled_draw, sizeof p.oled_shown);

    static ui_view_t v;
    memcpy(v.oled, p.oled_shown, sizeof v.oled);
    v.page = 1;
    v.pages = 4;
    v.link = 2;
    v.addr = 18;
    v.sn = p.sn;
    v.have_temp = true;
    v.temp_dc = 234;
    const char *labels[4] = {"\xc5\x9awiat\xc5\x82o", "Rolety", "", "Scena TV"};
    for (int i = 0; i < 4; i++) {
        panel_to_ascii(v.slot[i].label, sizeof v.slot[i].label, labels[i]);
        v.slot[i].locate = true;
    }
    v.slot[0].led = LEDS_GREEN;
    v.slot[3].led = LEDS_RED;
    v.slot[1].glow = 255; // pressed
    ui_init(fb);
    ui_render(&v);
    dump("preview_online.ppm");

    v.link = 0;
    v.have_temp = false;
    v.slot[1].glow = 0;
    v.page = 2;
    ui_render(&v);
    dump("preview_waiting.ppm");
    return 0;
}
