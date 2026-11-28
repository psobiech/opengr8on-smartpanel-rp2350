// Host stand-in for hardware/pio.h as board/bsp/pio_rgb.c uses it; test_display.c fakes the functions.
#pragma once
#include "pico_host.h"

typedef struct {
    volatile uint32_t txf[4];
} pio_hw_t;
typedef pio_hw_t *PIO;
extern pio_hw_t g_fake_pio_blocks[3];
#define pio0 (&g_fake_pio_blocks[0])
#define pio1 (&g_fake_pio_blocks[1])
#define pio2 (&g_fake_pio_blocks[2])

typedef struct {
    uint32_t clkdiv_milli;
    uint sideset_base, set_base, out_base, out_count;
} pio_sm_config;
typedef struct {
    const uint16_t *instructions;
    uint8_t length;
    int8_t origin;
} pio_program_t;
enum pio_fifo_join { PIO_FIFO_JOIN_NONE = 0, PIO_FIFO_JOIN_TX = 1, PIO_FIFO_JOIN_RX = 2 };

int pio_set_gpio_base(PIO pio, uint gpio_base);
int pio_claim_unused_sm(PIO pio, bool required);
int pio_add_program(PIO pio, const pio_program_t *program);
void pio_gpio_init(PIO pio, uint pin);
int pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin_base, uint pin_count, bool is_out);
int pio_sm_init(PIO pio, uint sm, uint initial_pc, const pio_sm_config *config);
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled);
void pio_set_sm_mask_enabled(PIO pio, uint32_t mask, bool enabled);
void pio_enable_sm_mask_in_sync(PIO pio, uint32_t mask);
void pio_sm_put_blocking(PIO pio, uint sm, uint32_t data);
void pio_sm_clear_fifos(PIO pio, uint sm);
void pio_sm_restart(PIO pio, uint sm);
void pio_sm_exec(PIO pio, uint sm, uint instruction);
void pio_interrupt_clear(PIO pio, uint pio_interrupt_num);
uint pio_get_dreq(PIO pio, uint sm, bool is_tx);
uint pio_encode_jmp(uint address);
void sm_config_set_sideset_pins(pio_sm_config *config, uint sideset_base);
void sm_config_set_set_pins(pio_sm_config *config, uint set_base, uint set_count);
void sm_config_set_out_pins(pio_sm_config *config, uint out_base, uint out_count);
void sm_config_set_out_shift(pio_sm_config *config, bool shift_right, bool autopull, uint pull_threshold);
void sm_config_set_clkdiv(pio_sm_config *config, float divider);
void sm_config_set_fifo_join(pio_sm_config *config, enum pio_fifo_join join);
