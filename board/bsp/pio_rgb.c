#include "pio_rgb.h"
#include "pio_rgb.pio.h"

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"

#define RGB_SYNC_PIO pio1
#define RGB_COLOR_DATA_PIO pio2
#define RGB_PIO_BASE_PIN 16

static uint vsync_sm;
static uint hsync_sm;

static uint rgb_de_sm;
static uint rgb_sm;

static int rgb_dma_chan;

// opengr8on: program offsets, kept for pio_rgb_set_scanout_enabled()
static uint hsync_offset;
static uint vsync_offset;
static uint rgb_de_offset;
static uint rgb_offset;

static pio_rgb_info_t *g_pio_rgb_info;

// opengr8on: line table scan-out (pio_rgb_info_t.line_table; the vendor framebuffer / transfer /
// PSRAM modes are removed): the control channel writes the next row's
// address to the pixel channel's READ_ADDR trigger after every row; NULL ends the frame (IRQ).
static uint line_control_dma_chan;
static const uint16_t *const *volatile line_table_started; // the table the current frame runs from

// Frame start (DMA IRQ at the NULL that ends the line table, and from init / restart): the next
// frame runs from the table set last (pio_rgb_set_line_table), from its first row address.
void __no_inline_not_in_flash_func(dma_complete_handler)(void)
{
    line_table_started = g_pio_rgb_info->line_table;
    dma_channel_set_read_addr(line_control_dma_chan, line_table_started, true);
    if (g_pio_rgb_info->dma_flush_done_cb)
    {
        g_pio_rgb_info->dma_flush_done_cb();
    }
}

static inline void hsync_program_init(PIO pio, uint sm, uint offset, uint pin, float div)
{
    // creates state machine configuration object c, sets
    // to default configurations. I believe this function is auto-generated
    // and gets a name of <program name>_program_get_default_config
    pio_sm_config c = hsync_program_get_default_config(offset);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    // Map the state machine's SET pin group to one pin, namely the `pin`
    // parameter to this function.
    sm_config_set_sideset_pins(&c, pin);

    // Set clock division (div by 5 for 25 MHz state machine)
    sm_config_set_clkdiv(&c, div);

    // Set this pin's GPIO function (connect PIO to the pad)
    pio_gpio_init(pio, pin);
    pio_gpio_init(pio, pin + 1);

    // Set the pin direction to output at the PIO
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 2, true);

    // Load our configuration, and jump to the start of the program
    pio_sm_init(pio, sm, offset, &c);

    // Set the state machine running (commented out so can be synchronized w/ vsync)
    // pio_sm_set_enabled(pio, sm, true);
}

static inline void vsync_program_init(PIO pio, uint sm, uint offset, uint pin, float div)
{

    // creates state machine configuration object c, sets
    // to default configurations. I believe this function is auto-generated
    // and gets a name of <program name>_program_get_default_config
    pio_sm_config c = vsync_program_get_default_config(offset);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    // Map the state machine's SET pin group to one pin, namely the `pin`
    // parameter to this function.
    sm_config_set_sideset_pins(&c, pin);

    // Set clock division (div by 5 for 25 MHz state machine)
    sm_config_set_clkdiv(&c, div);

    // Set this pin's GPIO function (connect PIO to the pad)
    pio_gpio_init(pio, pin);

    // Set the pin direction to output at the PIO
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);

    // Load our configuration, and jump to the start of the program
    pio_sm_init(pio, sm, offset, &c);

    // Set the state machine running (commented out so can be synchronized with hsync)
    // pio_sm_set_enabled(pio, sm, true);
}

static inline void rgb_de_program_init(PIO pio, uint sm, uint offset, uint pin, float div)
{

    // creates state machine configuration object c, sets
    // to default configurations. I believe this function is auto-generated
    // and gets a name of <program name>_program_get_default_config
    pio_sm_config c = rgb_de_program_get_default_config(offset);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    // Map the state machine's SET pin group to one pin, namely the `pin`
    // parameter to this function.
    sm_config_set_sideset_pins(&c, pin);

    // Set clock division (div by 5 for 25 MHz state machine)
    sm_config_set_clkdiv(&c, div);

    // Set this pin's GPIO function (connect PIO to the pad)
    pio_gpio_init(pio, pin);

    // Set the pin direction to output at the PIO
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);

    // Load our configuration, and jump to the start of the program
    pio_sm_init(pio, sm, offset, &c);

    // Set the state machine running (commented out so can be synchronized with hsync)
    // pio_sm_set_enabled(pio, sm, true);
}

static inline void rgb_program_init(PIO pio, uint sm, uint offset, uint pin, float div)
{

    // creates state machine configuration object c, sets
    // to default configurations. I believe this function is auto-generated
    // and gets a name of <program name>_program_get_default_config
    pio_sm_config c = rgb_program_get_default_config(offset);
    // sm_config_set_out_shift(&c, true, false, 32);
    // Map the state machine's SET and OUT pin group to 16 pins, the `pin`
    // parameter to this function is the lowest one. These groups overlap.
    // sm_config_set_set_pins(&c, pin, 16);
    // 中文：设置out的引脚
    // English: Set the out pin
    sm_config_set_out_pins(&c, pin, 16);
    // 中文：设置sideset的引脚
    // English: Set the sideset pin
    // sm_config_set_sideset_pins(&c, pin + 16);

    // Set clock division (Commented out, this one runs at full speed)
    // 中文：设置分频系数
    // English: Set the frequency division coefficient
    sm_config_set_clkdiv(&c, div);

    // Set this pin's GPIO function (connect PIO to the pad)
    // 中文：设置引脚为pio引脚
    // English: Set the pin to pio pin
    for (int i = 0; i < 16; i++)
    {
        pio_gpio_init(pio, pin + i);
        gpio_pull_up(pin + i);
    }

    // Set the pin direction to output at the PIO (4 pins)
    // 中文：设置输出的引脚
    // English: Set the output pin
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 16, true);

    // Load our configuration, and jump to the start of the program
    // 中文：初始化状态机
    // English: Initialize state machine
    pio_sm_init(pio, sm, offset, &c);

    // Set the state machine running (commented out, I'll start this in the C)
    // pio_sm_set_enabled(pio, sm, true);
}

void pio_rgb_dma_init(pio_rgb_info_t *info)
{
    dma_channel_config c0 = dma_channel_get_default_config(rgb_dma_chan);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_16); // 16-bit transfers
    channel_config_set_read_increment(&c0, true);
    channel_config_set_write_increment(&c0, false);
    channel_config_set_dreq(&c0, pio_get_dreq(RGB_COLOR_DATA_PIO, rgb_sm, true)); // set for pio2 sm 0
    // channel_config_set_chain_to(&c0, rgb_chan_1);

    // opengr8on: one row per pixel-channel run, chained to the control channel that loads the
    // next row address; IRQ only on the NULL that ends the table (IRQ_QUIET).
    dma_channel_claim(rgb_dma_chan); // channel 0, otherwise never claimed
    line_control_dma_chan = dma_claim_unused_channel(true);
    channel_config_set_chain_to(&c0, line_control_dma_chan);
    channel_config_set_irq_quiet(&c0, true);
    dma_channel_configure(rgb_dma_chan, &c0, &RGB_COLOR_DATA_PIO->txf[rgb_sm], NULL, info->width, false);

    dma_channel_config control = dma_channel_get_default_config(line_control_dma_chan);
    channel_config_set_transfer_data_size(&control, DMA_SIZE_32);
    channel_config_set_read_increment(&control, true);
    channel_config_set_write_increment(&control, false);
    dma_channel_configure(line_control_dma_chan, &control, &dma_hw->ch[rgb_dma_chan].al3_read_addr_trig,
                          info->line_table, 1, false);
    bsp_dma_channel_irq_add(1, rgb_dma_chan, dma_complete_handler);
}

void pio_rgb_init(pio_rgb_info_t *info, pio_rgb_pin_t *pin)
{
    static pio_rgb_info_t pio_rgb_info;
    memcpy(&pio_rgb_info, info, sizeof(pio_rgb_info_t));
    g_pio_rgb_info = &pio_rgb_info;

    // 中文：获取系统时钟
    // English: Get the system clock
    float sys_clk = clock_get_hz(clk_sys);
    float pio_freq = sys_clk / ((float)(info->pclk_freq * 2));

    pio_set_gpio_base(RGB_SYNC_PIO, RGB_PIO_BASE_PIN);
    pio_set_gpio_base(RGB_COLOR_DATA_PIO, RGB_PIO_BASE_PIN);
    hsync_sm = pio_claim_unused_sm(RGB_SYNC_PIO, true);
    vsync_sm = pio_claim_unused_sm(RGB_SYNC_PIO, true);
    rgb_de_sm = pio_claim_unused_sm(RGB_COLOR_DATA_PIO, true);
    rgb_sm = pio_claim_unused_sm(RGB_COLOR_DATA_PIO, true);

    // 中文：添加pio 程序
    // English: Add pio program
    hsync_offset = pio_add_program(RGB_SYNC_PIO, &hsync_program);
    vsync_offset = pio_add_program(RGB_SYNC_PIO, &vsync_program);
    rgb_de_offset = pio_add_program(RGB_COLOR_DATA_PIO, &rgb_de_program);
    rgb_offset = pio_add_program(RGB_COLOR_DATA_PIO, &rgb_program);

    // 中文：初始化pio 程序
    // English: Initialize pio program
    hsync_program_init(RGB_SYNC_PIO, hsync_sm, hsync_offset, pin->hsync_pin, pio_freq);
    vsync_program_init(RGB_SYNC_PIO, vsync_sm, vsync_offset, pin->vsync_pin, 1.0f);
    rgb_de_program_init(RGB_COLOR_DATA_PIO, rgb_de_sm, rgb_de_offset, pin->de_pin, 1.0f);
    rgb_program_init(RGB_COLOR_DATA_PIO, rgb_sm, rgb_offset, pin->data0_pin, 1.0f);

    pio_rgb_dma_init(info);
    pio_sm_put_blocking(RGB_SYNC_PIO, hsync_sm, info->width - 1);

    pio_sm_put_blocking(RGB_SYNC_PIO, vsync_sm, info->height - 1);
    
    pio_sm_put_blocking(RGB_COLOR_DATA_PIO, rgb_de_sm, info->height - 1);
    pio_sm_put_blocking(RGB_COLOR_DATA_PIO, rgb_sm, info->width - 1);

    pio_enable_sm_mask_in_sync(RGB_COLOR_DATA_PIO, ((1u << rgb_de_sm) | (1u << rgb_sm)));
    pio_enable_sm_mask_in_sync(RGB_SYNC_PIO, ((1u << hsync_sm) | (1u << vsync_sm)));

    dma_complete_handler();
}

// opengr8on: stop / restart the scan-out (display blank: the ST7701 sleeps, no RGB signals needed).
// Stop: the state machines halt, the DMA channel is aborted (its completion IRQ, which re-arms the
// next frame, is masked first). Start: every state machine is reset to the beginning of its program
// with the same counts as pio_rgb_init(), and the DMA restarts at the beginning of a frame
// (from the current line table, so the pixel shift is kept), so the sync and data machines start in step.
void pio_rgb_set_scanout_enabled(bool enabled)
{
    uint sync_mask = (1u << hsync_sm) | (1u << vsync_sm);
    uint color_data_mask = (1u << rgb_de_sm) | (1u << rgb_sm);
    if (!enabled)
    {
        pio_set_sm_mask_enabled(RGB_SYNC_PIO, sync_mask, false);
        pio_set_sm_mask_enabled(RGB_COLOR_DATA_PIO, color_data_mask, false);
        dma_irqn_set_channel_enabled(1, rgb_dma_chan, false);
        // RP2350-E5: an aborted channel or one chained to it can re-trigger (the abort then never
        // finishes) unless both are disabled first; re-enabled by the restart below
        hw_clear_bits(&dma_hw->ch[line_control_dma_chan].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
        hw_clear_bits(&dma_hw->ch[rgb_dma_chan].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
        dma_channel_abort(line_control_dma_chan);
        dma_channel_abort(rgb_dma_chan);
        dma_irqn_acknowledge_channel(1, rgb_dma_chan);
        return;
    }

    const struct
    {
        PIO pio;
        uint sm;
        uint offset;
        uint32_t count;
    } machines[] = {
        {RGB_SYNC_PIO, hsync_sm, hsync_offset, g_pio_rgb_info->width - 1u},
        {RGB_SYNC_PIO, vsync_sm, vsync_offset, g_pio_rgb_info->height - 1u},
        {RGB_COLOR_DATA_PIO, rgb_de_sm, rgb_de_offset, g_pio_rgb_info->height - 1u},
        {RGB_COLOR_DATA_PIO, rgb_sm, rgb_offset, g_pio_rgb_info->width - 1u},
    };
    for (size_t i = 0; i < sizeof machines / sizeof machines[0]; i++)
    {
        pio_sm_clear_fifos(machines[i].pio, machines[i].sm);
        pio_sm_restart(machines[i].pio, machines[i].sm);
        pio_sm_exec(machines[i].pio, machines[i].sm, pio_encode_jmp(machines[i].offset));
    }
    for (uint irq_flag = 0; irq_flag <= 2; irq_flag++) // rgb -> rgb_de (0), vsync (1), hsync -> vsync (2)
    {
        pio_interrupt_clear(RGB_SYNC_PIO, irq_flag);
        pio_interrupt_clear(RGB_COLOR_DATA_PIO, irq_flag);
    }
    for (size_t i = 0; i < sizeof machines / sizeof machines[0]; i++)
    {
        pio_sm_put_blocking(machines[i].pio, machines[i].sm, machines[i].count);
    }
    hw_set_bits(&dma_hw->ch[rgb_dma_chan].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_set_bits(&dma_hw->ch[line_control_dma_chan].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_irqn_acknowledge_channel(1, rgb_dma_chan);
    dma_irqn_set_channel_enabled(1, rgb_dma_chan, true);

    pio_enable_sm_mask_in_sync(RGB_COLOR_DATA_PIO, color_data_mask);
    pio_enable_sm_mask_in_sync(RGB_SYNC_PIO, sync_mask);

    dma_complete_handler();
}

void pio_rgb_set_line_table(const uint16_t *const *line_table)
{
    g_pio_rgb_info->line_table = line_table;
}

int __not_in_flash_func(pio_rgb_lines_started)(void)
{
    const uint16_t *const *table = line_table_started;
    intptr_t started = (intptr_t)(dma_hw->ch[line_control_dma_chan].read_addr - (uintptr_t)table) /
                       (intptr_t)sizeof(table[0]);
    // just after a frame start the control channel can still point into the previous table
    return started < 0 ? 0 : started > g_pio_rgb_info->height ? g_pio_rgb_info->height : (int)started;
}
