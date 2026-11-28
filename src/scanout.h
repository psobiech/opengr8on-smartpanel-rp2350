// Display scan-out with the OLED dot area composed per scan line (saves its 147 KB of framebuffer).
//
// The RGB DMA runs from a line table (pio_rgb line table mode): one read address per screen row.
// Rows outside the OLED dot area point straight into the UI framebuffer (ui_row). The rows through
// it point into a small ring of line buffers that core1 fills (ui_compose_oled_row) just before
// the DMA reaches them, from a per-frame copy of the 1-bpp OLED (so an OLED change never tears).
// Pixel shift: rebuilt line tables, swapped at a frame start.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pio_rgb.h"

// Largest pixel shift in each direction (the framebuffer needs this many pixels of margin before
// and after it: a shifted row reads past its ends).
#define SCANOUT_SHIFT_MAX 2

// After ui_init (the table points into the UI framebuffer), before the display init: sets the line
// table and the frame-start callback in rgb_info.
void scanout_init(pio_rgb_info_t *rgb_info);
// Start core1 (after the display init).
void scanout_start(void);
// Stop (display blank: core1 sleeps too) / restart the scan-out; a restart begins a new frame.
void scanout_set_enabled(bool enabled);
// New OLED content (1 bpp, as panel_t.oled_shown), shown from the next frame.
void scanout_set_oled(const uint8_t *oled);
// Show stored row y + dy, pixel x + dx at screen (x, y) from the next frame (|dx|, |dy| <=
// SCANOUT_SHIFT_MAX); rows outside the screen are black.
void scanout_set_shift(int dx, int dy);
// OLED rows composed too late since boot (each one showed stale content for a frame).
uint32_t scanout_late_lines(void);
