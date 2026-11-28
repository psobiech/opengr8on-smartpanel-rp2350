// Replica UI of the SMART_PANEL_FM_4 on the 480x480 RGB565 framebuffer: the screen is split into
// four touch quadrants (the OLED area included), marked only by faint corner brackets with label
// and LED; the 128x64 OLED (3x, dot-matrix look) in the middle, page dots and a status bar.
// Each region is composed row by row straight into the framebuffer and redrawn only when its
// content changes, so the single (continuously scanned) framebuffer shows no flicker.
//
// The OLED dot area (the 128x64 dots, 3x) is not stored: the framebuffer holds every other pixel
// (UI_FRAMEBUFFER_PIXELS) and the rows through the OLED are composed per scan line from the 1-bpp
// OLED buffer (ui_compose_oled_row; on the board core1 does it just before the DMA sends the row).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "panel_core.h"
#include "png_icon.h"

#define UI_W 480
#define UI_H 480

// OLED dot area, composed per scan line, not stored
#define UI_OLED_SCALE 3
#define UI_OLED_W     (OLED_W * UI_OLED_SCALE)  // 384
#define UI_OLED_H     (OLED_H * UI_OLED_SCALE)  // 192
#define UI_OLED_X     ((UI_W - UI_OLED_W) / 2)  // 48
#define UI_OLED_Y     144
#define UI_FRAMEBUFFER_PIXELS (UI_W * UI_H - UI_OLED_W * UI_OLED_H)

typedef struct {
    char label[17];
    const png_icon_t *icon; // IconA / IconB in the middle of the corner, NULL: none
    uint8_t glow;      // press highlight 0..255 (fades out after release)
    led_state_t led;
    bool locate;       // dim "location" LED when the LED is off (ButtonsLEDMode)
} ui_slot_t;

typedef struct {
    ui_slot_t slot[PANEL_SLOTS];
    uint8_t page, pages;
    uint8_t link;      // 0 no CLU, 1 addressed (no polls), 2 online
    uint8_t address;
    uint32_t serial_number;
    int16_t temperature_deci; // 0.1 C
    bool has_temperature;
} ui_view_t;

// framebuffer: UI_FRAMEBUFFER_PIXELS
void ui_init(uint16_t *framebuffer);
// Redraw what differs from the previous call (everything on the first call). The OLED dots are
// not drawn here (ui_compose_oled_row).
void ui_render(const ui_view_t *view);
// Stored screen row y (UI_W pixels), NULL for the rows through the OLED dot area (UI_OLED_Y ..
// UI_OLED_Y + UI_OLED_H - 1). Stored pixels lie before and after every row (a pixel-shifted row
// reads up to SCANOUT_SHIFT_MAX past its ends), the first and last row excepted.
const uint16_t *ui_row(int y);
// Screen row y through the OLED dot area into destination (UI_W pixels): the stored pixels left
// and right of it, the dots from oled (1 bpp, row-major, MSB = leftmost, as panel_t.oled_shown).
// On the board it runs on core1 from RAM.
void ui_compose_oled_row(uint16_t *destination, int y, const uint8_t *oled);
// Touch hit test: quadrant slot 0..3 (anywhere on screen), -2 = outside.
int ui_hit(int x, int y);
