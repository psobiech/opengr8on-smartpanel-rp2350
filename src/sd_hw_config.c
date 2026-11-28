// SD card socket of the Waveshare RP2350-Touch-LCD-4 for the no-OS-FatFS library (it calls
// sd_get_num / sd_get_by_num), as in the vendor's bsp_sd_card.c: 4-bit SDIO, CMD GPIO 11, D0..D3
// GPIO 12..15, CLK GPIO 10 (D0 - 2, fixed by the library's PIO program). pio0 and DMA_IRQ_0: the
// display uses pio1 / pio2 and DMA_IRQ_1.
#include "hw_config.h"

static sd_sdio_if_t s_sdio_interface = {
    .CMD_gpio = 11,
    .D0_gpio = 12,
    .SDIO_PIO = pio0,
    .DMA_IRQ_num = DMA_IRQ_0,
    .baud_rate = 125 * 1000 * 1000 / 6, // 20.8 MHz, the vendor's setting
};

static sd_card_t s_sd_card = {.type = SD_IF_SDIO, .sdio_if_p = &s_sdio_interface};

size_t sd_get_num(void)
{
    return 1;
}

sd_card_t *sd_get_by_num(size_t num)
{
    return num == 0 ? &s_sd_card : NULL;
}
