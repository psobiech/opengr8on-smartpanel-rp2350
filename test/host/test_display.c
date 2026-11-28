// Host tests for the display drivers (board/bsp/bsp_dma_channel_irq.c, pio_rgb.c, bsp_st7701.c,
// included: static state). PIO, DMA, PWM and GPIO are fakes that log every call in order. Covered:
// the DMA IRQ dispatch, the ST7701 bring-up over the bit-banged 9-bit SPI (decoded from the GPIO
// log: command / data bit, chip select per command, delays), the backlight PWM, the line table
// scan-out (pixel channel chained to the control channel, the first frame, table swaps, rows
// started) and the RP2350-E5 safe stop / restart order of the scan-out.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../board/bsp/bsp_dma_channel_irq.c"
#include "../../board/bsp/pio_rgb.c"
#include "../../board/bsp/bsp_st7701.c"

static int failures;

pio_hw_t g_fake_pio_blocks[3];
dma_hw_t g_fake_dma_hw;
const pio_program_t hsync_program = {.origin = -1}, vsync_program = {.origin = -1}, rgb_de_program = {.origin = -1},
                    rgb_program = {.origin = -1};

static char g_log[8192];             // every PIO / DMA / IRQ call, in order: "name args; "
static bool g_logging = true;
static int g_next_free_dma_channel = 1;
static int g_next_free_sm[3];
static int g_next_program_offset[3];
static uint32_t g_dma_irq1_status;   // channels with a pending IRQ 1
static irq_handler_t g_dma_irq1_handler;
static int g_irq1_handler_installs;
static dma_channel_config g_configs[16];
static volatile void *g_write_addresses[16];
static const volatile void *g_read_addresses[16];
static uint g_transfer_counts[16];
static float g_sm_clkdiv[3][4];
static uint16_t g_pwm_level;
static uint16_t g_pwm_wrap;
static float g_pwm_clkdiv;
static uint32_t g_slept_ms;
// ST7701 SPI as decoded from the GPIO writes: "c:FF d:77 ... | " (c = command, d = data, | = CS high)
static char g_spi[4096];
static bool g_spi_sda, g_spi_selected;
static uint16_t g_spi_word;
static int g_spi_bits;
static int g_frame_starts;
static int g_callbacks[3];

static void test_dma_irq(void);
static void test_display_init_without_line_table(void);
static void test_display_init(void);
static void test_backlight(void);
static void test_frames(void);
static void test_stop_restart(void);
static void test_lines_started(void);
static void check(const char *name, int condition);
static void check_text(const char *name, const char *actual, const char *expected);
static void log_call(const char *format, ...);
static void on_frame_start(void);
static void callback_0(void);
static void callback_1(void);
static void callback_2(void);

static const uint16_t g_rows[4][480];
static const uint16_t *const g_line_table_a[481] = {g_rows[0], g_rows[1]};
static const uint16_t *const g_line_table_b[481] = {g_rows[2], g_rows[3]};
static pio_rgb_info_t g_rgb_info = {.width = 480, .height = 480, .pclk_freq = 16000000};
static bsp_display_interface_t *g_display;

int main(void)
{
    test_dma_irq();
    test_display_init_without_line_table();
    test_display_init();
    test_backlight();
    test_frames();
    test_stop_restart();
    test_lines_started();
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

// ---- tests -----------------------------------------------------------------------------------
static void test_dma_irq(void)
{
    bsp_dma_channel_irq_add(1, 5, NULL);
    check("DMA IRQ: a NULL callback is not added", g_irq1_info.len == 0);
    bsp_dma_channel_irq_add(1, 5, callback_0);
    bsp_dma_channel_irq_add(1, 6, callback_1);
    bsp_dma_channel_irq_add(0, 7, callback_2);
    check("DMA IRQ 1: handler installed once, IRQ enabled", g_irq1_handler_installs == 1 &&
                                                                strstr(g_log, "irq_set_enabled 11,1; ") != NULL);
    check("DMA IRQ 1: channel IRQs enabled", strstr(g_log, "dma_channel_set_irq1_enabled 5,1; ") != NULL &&
                                                 strstr(g_log, "dma_channel_set_irq1_enabled 6,1; ") != NULL);
    g_log[0] = 0;
    g_dma_irq1_status = 1u << 6;
    g_dma_irq1_handler();
    check("DMA IRQ 1: only the channel that fired, acknowledged before its callback",
          g_callbacks[0] == 0 && g_callbacks[1] == 1 && strcmp(g_log, "dma_channel_acknowledge_irq1 6; ") == 0);
    g_dma_irq1_status = (1u << 5) | (1u << 6);
    g_dma_irq1_handler();
    check("DMA IRQ 1: both channels", g_callbacks[0] == 1 && g_callbacks[1] == 2 && g_callbacks[2] == 0);
    for (int i = 0; i < 20; i++) {
        bsp_dma_channel_irq_add(1, 8, callback_2);
    }
    check("DMA IRQ 1: at most 13 channels (no overflow of the table)", g_irq1_info.len == 13);
    memset(&g_irq1_info, 0, sizeof g_irq1_info);
    g_irq1_info.is_init = true; // the handler stays installed
}

static void test_display_init_without_line_table(void)
{
    static pio_rgb_info_t no_table = {.width = 480, .height = 480};
    bsp_display_info_t info = {.width = 480, .height = 480, .brightness = 70, .user_data = &no_table};
    check("ST7701: no info, no interface", !bsp_display_new_st7701(&g_display, NULL));
    bsp_display_new_st7701(&g_display, &info);
    g_log[0] = 0;
    g_display->init();
    check("ST7701 without a line table: the scan-out is not started", strstr(g_log, "pio_add_program") == NULL);
}

static void test_display_init(void)
{
    g_rgb_info.line_table = g_line_table_a;
    g_rgb_info.dma_flush_done_cb = on_frame_start;
    bsp_display_info_t info = {.width = 480, .height = 480, .brightness = 70, .user_data = &g_rgb_info};
    bsp_display_new_st7701(&g_display, &info);
    g_log[0] = 0;
    g_spi[0] = 0;
    g_slept_ms = 0;
    g_frame_starts = 0;
    g_display->init();

    check("ST7701 SPI: first command FF 77 01 00 00 10 (page select), CS high after it",
          strncmp(g_spi, "c:FF d:77 d:01 d:00 d:00 d:10 | c:C0 d:3B d:00 | ", 48) == 0);
    const char *ending = "c:3A d:60 | c:11 | c:29 | ";
    size_t spi_length = strlen(g_spi);
    check("ST7701 SPI: ends with RGB666, Sleep Out, Display On",
          spi_length > strlen(ending) && strcmp(g_spi + spi_length - strlen(ending), ending) == 0);
    int commands = 0;
    for (const char *cursor = g_spi; (cursor = strstr(cursor, "c:")); cursor++) {
        commands++;
    }
    check("ST7701 SPI: every init command sent", commands == (int)(sizeof init_cmds / sizeof init_cmds[0]));
    check("ST7701: reset 20 + 20 + 200 ms, Sleep Out and Display On 120 ms each", g_slept_ms == 20 + 20 + 200 + 240);
    check("backlight: PWM wrap 1000 at 5 kHz", g_pwm_wrap == PWM_WRAP && g_pwm_clkdiv == 240000000.0f / (5000 * 1000));
    check("backlight: 70% at init (inverted: level 300)", g_pwm_level == 300);

    check("scan-out: 4 PIO programs", strstr(g_log, "pio_add_program 1,0; pio_add_program 1,1; pio_add_program 2,0; "
                                                    "pio_add_program 2,1; ") != NULL);
    check("scan-out: hsync clock divider for a 16 MHz pixel clock", g_sm_clkdiv[1][0] == 240000000.0f / 32000000.0f &&
                                                                       g_sm_clkdiv[1][1] == 1.0f && g_sm_clkdiv[2][1] == 1.0f);
    uint pixel_channel = 0, control_channel = 1;
    check("DMA: pixel channel 0 claimed, control channel from the free ones",
          strstr(g_log, "dma_channel_claim 0; dma_claim_unused_channel 1; ") != NULL && line_control_dma_chan == control_channel);
    dma_channel_config pixel = g_configs[pixel_channel], control = g_configs[control_channel];
    check("DMA pixel channel: 16-bit reads into the RGB PIO FIFO, chained to the control channel, quiet",
          pixel.data_size == DMA_SIZE_16 && pixel.read_increment && !pixel.write_increment &&
              pixel.chain_to == control_channel && pixel.irq_quiet &&
              g_write_addresses[pixel_channel] == &pio2->txf[rgb_sm] && g_transfer_counts[pixel_channel] == 480);
    check("DMA control channel: one 32-bit row address into the pixel channel's READ_ADDR trigger",
          control.data_size == DMA_SIZE_32 && control.read_increment && !control.write_increment &&
              g_write_addresses[control_channel] == &dma_hw->ch[pixel_channel].al3_read_addr_trig &&
              g_read_addresses[control_channel] == g_line_table_a && g_transfer_counts[control_channel] == 1);
    check("DMA: IRQ 1 on the pixel channel (the NULL at the table end)", g_irq1_info.len == 1 &&
                                                                          g_irq1_info.dma_channel[0] == pixel_channel);
    const char *counts = strstr(g_log, "pio_sm_put_blocking 1,0,479; pio_sm_put_blocking 1,1,479; "
                                       "pio_sm_put_blocking 2,0,479; pio_sm_put_blocking 2,1,479; ");
    const char *enable_data = strstr(g_log, "pio_enable_sm_mask_in_sync 2,3; ");
    const char *enable_sync = strstr(g_log, "pio_enable_sm_mask_in_sync 1,3; ");
    check("scan-out: counts loaded, data machines enabled, then the sync machines",
          counts && enable_data && enable_sync && counts < enable_data && enable_data < enable_sync);
    check("first frame started from the line table, callback run", g_frame_starts == 1 &&
                                                                     strstr(g_log, "dma_channel_set_read_addr 1,a,1; ") > enable_sync);
}

static void test_backlight(void)
{
    g_display->set_brightness(0);
    check("backlight 0%: PWM level 1000", g_pwm_level == 1000 && g_display_info->brightness == 0);
    g_display->set_brightness(150);
    check("backlight above 100%: clamped (level 0)", g_pwm_level == 0 && g_display_info->brightness == 100);
    g_display->set_brightness(5);
    check("backlight 5%: level 950", g_pwm_level == 950);
}

static void test_frames(void)
{
    g_log[0] = 0;
    g_dma_irq1_status = 1u << 0;
    g_dma_irq1_handler();
    check("frame end IRQ: next frame from the same table", g_frame_starts == 2 &&
                                                            strstr(g_log, "dma_channel_set_read_addr 1,a,1; ") != NULL);
    pio_rgb_set_line_table(g_line_table_b);
    g_log[0] = 0;
    g_dma_irq1_handler();
    check("table swapped: the next frame runs from it", strstr(g_log, "dma_channel_set_read_addr 1,b,1; ") != NULL &&
                                                            line_table_started == g_line_table_b);
}

static void test_stop_restart(void)
{
    g_log[0] = 0;
    g_fake_dma_hw.ch[0].al1_ctrl = g_fake_dma_hw.ch[1].al1_ctrl = DMA_CH0_CTRL_TRIG_EN_BITS;
    pio_rgb_set_scanout_enabled(false);
    check_text("stop: state machines, IRQ masked, both channels' TRIG_EN cleared before the aborts (RP2350-E5)", g_log,
               "pio_set_sm_mask_enabled 1,3,0; pio_set_sm_mask_enabled 2,3,0; dma_irqn_set_channel_enabled 1,0,0; "
               "hw_clear_bits ch1.ctrl; hw_clear_bits ch0.ctrl; dma_channel_abort 1; dma_channel_abort 0; "
               "dma_irqn_acknowledge_channel 1,0; ");
    check("stop: channels not triggerable", !(g_fake_dma_hw.ch[0].al1_ctrl & DMA_CH0_CTRL_TRIG_EN_BITS) &&
                                                !(g_fake_dma_hw.ch[1].al1_ctrl & DMA_CH0_CTRL_TRIG_EN_BITS));

    g_log[0] = 0;
    int frames_before = g_frame_starts;
    pio_rgb_set_scanout_enabled(true);
    check("restart: every machine back to its program start",
          strstr(g_log, "pio_sm_clear_fifos 1,0; pio_sm_restart 1,0; pio_sm_exec 1,0,jmp0; "
                        "pio_sm_clear_fifos 1,1; pio_sm_restart 1,1; pio_sm_exec 1,1,jmp1; "
                        "pio_sm_clear_fifos 2,0; pio_sm_restart 2,0; pio_sm_exec 2,0,jmp0; "
                        "pio_sm_clear_fifos 2,1; pio_sm_restart 2,1; pio_sm_exec 2,1,jmp1; ") == g_log);
    check("restart: PIO IRQ flags 0..2 cleared on both blocks",
          strstr(g_log, "pio_interrupt_clear 1,0; pio_interrupt_clear 2,0; pio_interrupt_clear 1,1; "
                        "pio_interrupt_clear 2,1; pio_interrupt_clear 1,2; pio_interrupt_clear 2,2; ") != NULL);
    const char *trigger_enabled = strstr(g_log, "hw_set_bits ch0.ctrl; hw_set_bits ch1.ctrl; ");
    const char *irq_enabled = strstr(g_log, "dma_irqn_acknowledge_channel 1,0; dma_irqn_set_channel_enabled 1,0,1; ");
    const char *machines_enabled = strstr(g_log, "pio_enable_sm_mask_in_sync 2,3; pio_enable_sm_mask_in_sync 1,3; ");
    const char *frame = strstr(g_log, "dma_channel_set_read_addr 1,b,1; ");
    check("restart: TRIG_EN back, IRQ re-armed, machines in sync, then a frame from the current table",
          trigger_enabled && irq_enabled && machines_enabled && frame && trigger_enabled < irq_enabled &&
              irq_enabled < machines_enabled && machines_enabled < frame && g_frame_starts == frames_before + 1);
    check("restart: channels triggerable again", (g_fake_dma_hw.ch[0].al1_ctrl & DMA_CH0_CTRL_TRIG_EN_BITS) &&
                                                     (g_fake_dma_hw.ch[1].al1_ctrl & DMA_CH0_CTRL_TRIG_EN_BITS));
}

static void test_lines_started(void)
{
    dma_channel_hw_t *control = &g_fake_dma_hw.ch[line_control_dma_chan];
    control->read_addr = (uintptr_t)(g_line_table_b + 37);
    check("rows started: from the control channel's read address", pio_rgb_lines_started() == 37);
    control->read_addr = (uintptr_t)(g_line_table_b + 481);
    check("rows started: at most the height", pio_rgb_lines_started() == 480);
    control->read_addr = (uintptr_t)g_line_table_b - 64;
    check("rows started: 0 while the control channel still points before the table", pio_rgb_lines_started() == 0);
}

// ---- helpers ---------------------------------------------------------------------------------
static void check(const char *name, int condition)
{
    printf("%s %s\n", condition ? "ok  " : "FAIL", name);
    failures += !condition;
}

static void check_text(const char *name, const char *actual, const char *expected)
{
    int matches = strcmp(actual, expected) == 0;
    check(name, matches);
    if (!matches) {
        printf("  got  \"%s\"\n  want \"%s\"\n", actual, expected);
    }
}

static void log_call(const char *format, ...)
{
    if (!g_logging) {
        return;
    }
    size_t length = strlen(g_log);
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(g_log + length, sizeof g_log - length, format, arguments);
    va_end(arguments);
    length = strlen(g_log);
    snprintf(g_log + length, sizeof g_log - length, "; ");
}

static void on_frame_start(void)
{
    g_frame_starts++;
}

static void callback_0(void)
{
    g_callbacks[0]++;
}

static void callback_1(void)
{
    g_callbacks[1]++;
}

static void callback_2(void)
{
    g_callbacks[2]++;
}

static int pio_index(PIO pio)
{
    return (int)(pio - g_fake_pio_blocks);
}

static const char *table_name(const volatile void *address)
{
    return address == g_line_table_a ? "a" : address == g_line_table_b ? "b" : "?";
}

static const char *register_name(volatile uint32_t *address)
{
    static char name[24];
    for (int channel = 0; channel < 16; channel++) {
        if (address == &g_fake_dma_hw.ch[channel].al1_ctrl) {
            snprintf(name, sizeof name, "ch%d.ctrl", channel);
            return name;
        }
    }
    return "?";
}

// ---- SDK fakes -------------------------------------------------------------------------------
uint64_t time_us_64(void)
{
    return 0;
}

void sleep_ms(uint32_t delay_ms)
{
    g_slept_ms += delay_ms;
}

void sleep_us(uint64_t delay_us)
{
    (void)delay_us;
}

uint32_t clock_get_hz(enum clock_index clock)
{
    (void)clock;
    return 240000000;
}

void gpio_init(uint gpio)
{
    (void)gpio;
}

void gpio_set_dir(uint gpio, bool out)
{
    (void)gpio, (void)out;
}

void gpio_pull_up(uint gpio)
{
    (void)gpio;
}

void gpio_set_function(uint gpio, enum gpio_function function)
{
    (void)gpio, (void)function;
}

// The ST7701 3-wire SPI: CS low starts a command, SDA is sampled on each SCK rising edge, 9 bits
// per word (bit 8: 0 command, 1 data), CS high ends the command.
void gpio_put(uint gpio, bool value)
{
    if (gpio == BSP_LCD_CS_PIN) {
        if (value && g_spi_selected) {
            strcat(g_spi, "| ");
        }
        g_spi_selected = !value;
        g_spi_bits = 0;
        g_spi_word = 0;
    } else if (gpio == BSP_LCD_SDA_PIN) {
        g_spi_sda = value;
    } else if (gpio == BSP_LCD_SCK_PIN && value && g_spi_selected) {
        g_spi_word = (uint16_t)(g_spi_word << 1 | g_spi_sda);
        if (++g_spi_bits == 9) {
            size_t length = strlen(g_spi);
            snprintf(g_spi + length, sizeof g_spi - length, "%c:%02X ", g_spi_word & 0x100 ? 'd' : 'c',
                     g_spi_word & 0xff);
            g_spi_bits = 0;
            g_spi_word = 0;
        }
    }
}

uint pwm_gpio_to_slice_num(uint gpio)
{
    return gpio >> 1 & 7;
}

uint pwm_gpio_to_channel(uint gpio)
{
    return gpio & 1;
}

void pwm_set_chan_level(uint slice, uint channel, uint16_t level)
{
    (void)slice, (void)channel;
    g_pwm_level = level;
}

void pwm_set_enabled(uint slice, bool enabled)
{
    (void)slice, (void)enabled;
}

void pwm_set_clkdiv(uint slice, float divider)
{
    (void)slice;
    g_pwm_clkdiv = divider;
}

void pwm_set_wrap(uint slice, uint16_t wrap)
{
    (void)slice;
    g_pwm_wrap = wrap;
}

void irq_set_exclusive_handler(uint irq, irq_handler_t handler)
{
    if (irq == DMA_IRQ_1) {
        g_dma_irq1_handler = handler;
        g_irq1_handler_installs++;
    }
    log_call("irq_set_exclusive_handler %u", irq);
}

void irq_set_enabled(uint irq, bool enabled)
{
    log_call("irq_set_enabled %u,%d", irq, enabled);
}

int pio_set_gpio_base(PIO pio, uint gpio_base)
{
    log_call("pio_set_gpio_base %d,%u", pio_index(pio), gpio_base);
    return 0;
}

int pio_claim_unused_sm(PIO pio, bool required)
{
    (void)required;
    return g_next_free_sm[pio_index(pio)]++;
}

int pio_add_program(PIO pio, const pio_program_t *program)
{
    (void)program;
    int offset = g_next_program_offset[pio_index(pio)]++;
    log_call("pio_add_program %d,%d", pio_index(pio), offset);
    return offset;
}

void pio_gpio_init(PIO pio, uint pin)
{
    (void)pio, (void)pin;
}

int pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin_base, uint pin_count, bool is_out)
{
    (void)pio, (void)sm, (void)pin_base, (void)pin_count, (void)is_out;
    return 0;
}

int pio_sm_init(PIO pio, uint sm, uint initial_pc, const pio_sm_config *config)
{
    g_sm_clkdiv[pio_index(pio)][sm] = (float)config->clkdiv_milli / 1000.0f;
    log_call("pio_sm_init %d,%u,%u", pio_index(pio), sm, initial_pc);
    return 0;
}

void pio_sm_set_enabled(PIO pio, uint sm, bool enabled)
{
    log_call("pio_sm_set_enabled %d,%u,%d", pio_index(pio), sm, enabled);
}

void pio_set_sm_mask_enabled(PIO pio, uint32_t mask, bool enabled)
{
    log_call("pio_set_sm_mask_enabled %d,%u,%d", pio_index(pio), (unsigned)mask, enabled);
}

void pio_enable_sm_mask_in_sync(PIO pio, uint32_t mask)
{
    log_call("pio_enable_sm_mask_in_sync %d,%u", pio_index(pio), (unsigned)mask);
}

void pio_sm_put_blocking(PIO pio, uint sm, uint32_t data)
{
    log_call("pio_sm_put_blocking %d,%u,%u", pio_index(pio), sm, (unsigned)data);
}

void pio_sm_clear_fifos(PIO pio, uint sm)
{
    log_call("pio_sm_clear_fifos %d,%u", pio_index(pio), sm);
}

void pio_sm_restart(PIO pio, uint sm)
{
    log_call("pio_sm_restart %d,%u", pio_index(pio), sm);
}

void pio_sm_exec(PIO pio, uint sm, uint instruction)
{
    log_call("pio_sm_exec %d,%u,jmp%u", pio_index(pio), sm, instruction);
}

void pio_interrupt_clear(PIO pio, uint pio_interrupt_num)
{
    log_call("pio_interrupt_clear %d,%u", pio_index(pio), pio_interrupt_num);
}

uint pio_get_dreq(PIO pio, uint sm, bool is_tx)
{
    (void)is_tx;
    return (uint)(pio_index(pio) * 8 + (int)sm);
}

uint pio_encode_jmp(uint address)
{
    return address;
}

pio_sm_config hsync_program_get_default_config(uint offset)
{
    (void)offset;
    return (pio_sm_config){0};
}

pio_sm_config vsync_program_get_default_config(uint offset)
{
    (void)offset;
    return (pio_sm_config){0};
}

pio_sm_config rgb_de_program_get_default_config(uint offset)
{
    (void)offset;
    return (pio_sm_config){0};
}

pio_sm_config rgb_program_get_default_config(uint offset)
{
    (void)offset;
    return (pio_sm_config){0};
}

void sm_config_set_sideset_pins(pio_sm_config *config, uint sideset_base)
{
    config->sideset_base = sideset_base;
}

void sm_config_set_set_pins(pio_sm_config *config, uint set_base, uint set_count)
{
    (void)set_count;
    config->set_base = set_base;
}

void sm_config_set_out_pins(pio_sm_config *config, uint out_base, uint out_count)
{
    config->out_base = out_base;
    config->out_count = out_count;
}

void sm_config_set_out_shift(pio_sm_config *config, bool shift_right, bool autopull, uint pull_threshold)
{
    (void)config, (void)shift_right, (void)autopull, (void)pull_threshold;
}

void sm_config_set_clkdiv(pio_sm_config *config, float divider)
{
    config->clkdiv_milli = (uint32_t)(divider * 1000.0f + 0.5f);
}

void sm_config_set_fifo_join(pio_sm_config *config, enum pio_fifo_join join)
{
    (void)config, (void)join;
}

dma_channel_config dma_channel_get_default_config(uint channel)
{
    return (dma_channel_config){.data_size = DMA_SIZE_32, .read_increment = true, .chain_to = channel};
}

void channel_config_set_transfer_data_size(dma_channel_config *config, enum dma_channel_transfer_size size)
{
    config->data_size = size;
}

void channel_config_set_read_increment(dma_channel_config *config, bool increment)
{
    config->read_increment = increment;
}

void channel_config_set_write_increment(dma_channel_config *config, bool increment)
{
    config->write_increment = increment;
}

void channel_config_set_dreq(dma_channel_config *config, uint dreq)
{
    config->dreq = dreq;
}

void channel_config_set_chain_to(dma_channel_config *config, uint chain_to)
{
    config->chain_to = chain_to;
}

void channel_config_set_irq_quiet(dma_channel_config *config, bool irq_quiet)
{
    config->irq_quiet = irq_quiet;
}

void dma_channel_configure(uint channel, const dma_channel_config *config, volatile void *write_address,
                           const volatile void *read_address, uint transfer_count, bool trigger)
{
    g_configs[channel] = *config;
    g_write_addresses[channel] = write_address;
    g_read_addresses[channel] = read_address;
    g_transfer_counts[channel] = transfer_count;
    log_call("dma_channel_configure %u,%d", channel, trigger);
}

void dma_channel_claim(uint channel)
{
    log_call("dma_channel_claim %u", channel);
}

int dma_claim_unused_channel(bool required)
{
    (void)required;
    log_call("dma_claim_unused_channel %d", g_next_free_dma_channel);
    return g_next_free_dma_channel++;
}

void dma_channel_set_read_addr(uint channel, const volatile void *read_address, bool trigger)
{
    g_fake_dma_hw.ch[channel].read_addr = (uintptr_t)read_address;
    log_call("dma_channel_set_read_addr %u,%s,%d", channel, table_name(read_address), trigger);
}

void dma_channel_abort(uint channel)
{
    log_call("dma_channel_abort %u", channel);
}

void dma_channel_set_irq1_enabled(uint channel, bool enabled)
{
    log_call("dma_channel_set_irq1_enabled %u,%d", channel, enabled);
}

bool dma_channel_get_irq1_status(uint channel)
{
    return g_dma_irq1_status & (1u << channel);
}

void dma_channel_acknowledge_irq1(uint channel)
{
    log_call("dma_channel_acknowledge_irq1 %u", channel);
}

void dma_irqn_set_channel_enabled(uint irq_index, uint channel, bool enabled)
{
    log_call("dma_irqn_set_channel_enabled %u,%u,%d", irq_index, channel, enabled);
}

void dma_irqn_acknowledge_channel(uint irq_index, uint channel)
{
    log_call("dma_irqn_acknowledge_channel %u,%u", irq_index, channel);
}

void hw_set_bits(volatile uint32_t *address, uint32_t mask)
{
    *address |= mask;
    log_call("hw_set_bits %s", register_name(address));
}

void hw_clear_bits(volatile uint32_t *address, uint32_t mask)
{
    *address &= ~mask;
    log_call("hw_clear_bits %s", register_name(address));
}
