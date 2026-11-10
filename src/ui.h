// Replica UI of the SMART_PANEL_FM_4 on the 480x480 RGB565 framebuffer: the screen is split into
// four touch quadrants (the OLED area included), marked only by faint corner brackets with label
// and LED; the 128x64 OLED (3x, dot-matrix look) in the middle, page dots and a status bar.
// Each region is composed row by row straight into the framebuffer and redrawn only when its
// content changes, so the single (continuously scanned) framebuffer shows no flicker.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "panel_core.h"

#define UI_W 480
#define UI_H 480

typedef struct {
    char label[17];
    uint8_t glow;      // press highlight 0..255 (fades out after release)
    led_state_t led;
    bool locate;       // dim "location" LED when the LED is off (ButtonsLEDMode)
} ui_slot_t;

typedef struct {
    ui_slot_t slot[PANEL_SLOTS];
    uint8_t oled[OLED_W * OLED_H / 8];
    uint8_t page, pages;
    uint8_t link;      // 0 no CLU, 1 addressed (no polls), 2 online
    uint8_t addr;
    uint32_t sn;
    int16_t temp_dc;   // 0.1 C
    bool have_temp;
} ui_view_t;

void ui_init(uint16_t *fb);
// Redraw what differs from the previous call (everything on the first call).
void ui_render(const ui_view_t *v);
// Touch hit test: quadrant slot 0..3 (anywhere on screen), -2 = outside.
int ui_hit(int x, int y);
