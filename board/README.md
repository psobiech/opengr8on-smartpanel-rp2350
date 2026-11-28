# Board support (vendored)

Copied unmodified from Waveshare's RP2350-Touch-LCD-4 demo package (December 2025 download):

- `waveshare_rp2350_touch_lcd_4.h` — Pico SDK board definition (`boards/`; BSD-3-Clause, Raspberry Pi)
- `bsp/` — from `libraries/bsp/`: ST7701 LCD init + PIO RGB scanout (`bsp_st7701`, `pio_rgb`,
  `bsp_dma_channel_irq`), GT911 touch, I2C, QMI8658 IMU (temperature). Waveshare demo code; no
  licence statement in the files. Local changes are marked `opengr8on:` (QMI8658: temperature read
  error, tap detection; `pio_rgb`: scan-out stop / restart, line table mode — the DMA sends every row
  from its own address, fed by a second, control DMA channel; `bsp_st7701`: no framebuffer in line
  table mode).

Not copied: the SD card / FatFS library (the vendor BSP CMake links it; nothing here uses it), LVGL,
CST820 touch, RTC, CAN, battery and buzzer helpers (the buzzer is driven directly in `src/main.c`).
