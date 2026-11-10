// Grenton SMART_PANEL_FM_4 emulator on the Waveshare RP2350-Touch-LCD-4 (480x480 RGB LCD, GT911
// touch, RS485 on UART1 GPIO8/9 with an auto-direction transceiver, PWM buzzer on GPIO1).
//
// The TFBUS module logic lives in core/panel_core.c (host-tested). This file is the board glue:
//   UART1 RX IRQ -> panel_rx() (replies go out from a timer 800 us after the request: bus turnaround),
//   main loop     -> touch, panel_tick(), buzzer, temperature, rendering of the replica UI.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_gt911.h"
#include "bsp_i2c.h"
#include "bsp_qmi8658.h"
#include "bsp_st7701.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "panel_core.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "pio_rgb.h"

#include "ui.h"

#define LCD_W 480
#define LCD_H 480

#define RS485_UART    uart1
#define RS485_IRQ     UART1_IRQ
#define RS485_TX_PIN  8
#define RS485_RX_PIN  9
#define RS485_BAUD    115200

#define BUZZER_PIN    1
#define TOUCH_STALE_US   150000 // no GT911 report for this long while "touched" = released
#define TOUCH_RELEASE_US 40000  // release debounce

// Serial number: -DPANEL_SN=0x0f05xxxx, else derived from the flash unique ID. Derived SNs use
// 0x0f05f000..0x0f05ffff, away from the range real panels were seen in (0x0f057bxx).
#ifndef PANEL_SN
#define PANEL_SN 0
#endif
#ifndef PANEL_TEMP_OFFSET
#define PANEL_TEMP_OFFSET -13.0f // calibration, added to the measured temperature (board self-heating)
#endif

static panel_t s_panel;
static uint16_t *s_fb;

// ---------------------------------------------------------------------------------------------
// Logging: the core may log from the UART IRQ; lines are printed from the main loop.
// ---------------------------------------------------------------------------------------------
#define LOG_LINES 8
static char s_log[LOG_LINES][120];
static volatile uint32_t s_log_w, s_log_r;

static void on_log(const char *msg, void *ctx)
{
    (void)ctx;
    uint32_t w = s_log_w;
    if (w - s_log_r >= LOG_LINES) {
        return;
    }
    strncpy(s_log[w % LOG_LINES], msg, sizeof s_log[0] - 1);
    s_log[w % LOG_LINES][sizeof s_log[0] - 1] = 0;
    s_log_w = w + 1;
}

static void flush_log(void)
{
    while (s_log_r != s_log_w) {
        printf("[panel] %s\n", s_log[s_log_r % LOG_LINES]);
        s_log_r++;
    }
}

// ---------------------------------------------------------------------------------------------
// RS485
// ---------------------------------------------------------------------------------------------
// RS485 diagnostics (printed on USB every 5 s while something changes)
static volatile uint32_t s_rx_bytes, s_rx_errs, s_tx_frames;
static volatile uint8_t s_rx_last[8];

// Replies leave TX_TURNAROUND_US after the request: the master needs time to release its driver
// (DE). Answering straight from the RX IRQ (~300 us after the last byte) collided with the ESP32
// CLU, which lost the first 1-2 bytes of every reply.
#define TX_TURNAROUND_US 800
static uint8_t s_txq[64];
static volatile size_t s_txq_n;
static volatile bool s_tx_armed;

static int64_t tx_alarm(alarm_id_t id, void *ud)
{
    (void)id, (void)ud;
    uint32_t irq = save_and_disable_interrupts();
    uint8_t f[sizeof s_txq];
    size_t n = s_txq_n;
    memcpy(f, s_txq, n);
    s_txq_n = 0;
    s_tx_armed = false;
    restore_interrupts(irq);
    uart_write_blocking(RS485_UART, f, n); // <= 64 bytes, a few FIFO refills at most
    return 0;
}

static void on_tx(const uint8_t *f, size_t n, void *ctx)
{
    (void)ctx;
    uint32_t irq = save_and_disable_interrupts();
    if (s_txq_n + n <= sizeof s_txq) {
        memcpy(s_txq + s_txq_n, f, n);
        s_txq_n += n;
        s_tx_frames++;
    }
    bool arm = !s_tx_armed;
    s_tx_armed = true;
    restore_interrupts(irq);
    if (arm && add_alarm_in_us(TX_TURNAROUND_US, tx_alarm, NULL, true) < 0) {
        tx_alarm(0, NULL); // no alarm slot: send now
    }
}

static void rs485_irq(void)
{
    uint8_t b[32];
    size_t n = 0;
    while (uart_is_readable(RS485_UART) && n < sizeof b) {
        uint32_t dr = uart_get_hw(RS485_UART)->dr;
        if (dr & 0xf00) { // framing / parity / break / overrun: wrong baud, A/B swapped, noise
            s_rx_errs++;
        }
        b[n++] = (uint8_t)dr;
        s_rx_last[s_rx_bytes++ % sizeof s_rx_last] = (uint8_t)dr;
    }
    if (n) {
        panel_rx(&s_panel, b, n, time_us_64());
    }
}

static void rs485_stats(uint64_t now)
{
    static uint64_t next;
    static uint32_t last_bytes = ~0u;
    if (now < next) {
        return;
    }
    next = now + 5000000;
    uint32_t bytes = s_rx_bytes;
    if (bytes == last_bytes) {
        return;
    }
    last_bytes = bytes;
    uint8_t l[8];
    for (int i = 0; i < 8; i++) {
        l[i] = s_rx_last[(bytes + i) % 8];
    }
    printf("[rs485] rx %lu bytes (%lu errors), frames ok %lu bad %lu, tx %lu, addr %u, last rx "
           "%02x %02x %02x %02x %02x %02x %02x %02x\n",
           (unsigned long)bytes, (unsigned long)s_rx_errs, (unsigned long)s_panel.frames_rx,
           (unsigned long)s_panel.frames_bad, (unsigned long)s_tx_frames, s_panel.addr, l[0], l[1],
           l[2], l[3], l[4], l[5], l[6], l[7]);
}

static void rs485_init(void)
{
    uart_init(RS485_UART, RS485_BAUD);
    gpio_set_function(RS485_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(RS485_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(RS485_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(RS485_UART, true);
    irq_set_exclusive_handler(RS485_IRQ, rs485_irq);
    irq_set_priority(RS485_IRQ, 0x40); // above the display DMA
    irq_set_enabled(RS485_IRQ, true);
    uart_set_irq_enables(RS485_UART, true, false); // RX + RX timeout
}

// Run core calls from the main loop without racing the UART IRQ.
#define LOCKED(stmt)                                     \
    do {                                                 \
        uint32_t _s = save_and_disable_interrupts();     \
        stmt;                                            \
        restore_interrupts(_s);                          \
    } while (0)

// ---------------------------------------------------------------------------------------------
// Buzzer (PWM, frequency + volume, stopped from the main loop)
// ---------------------------------------------------------------------------------------------
static uint64_t s_beep_end;

static void buzzer_init(void)
{
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_chan_level(slice, pwm_gpio_to_channel(BUZZER_PIN), 0);
    pwm_set_enabled(slice, true);
}

static void buzzer_beep(uint16_t freq, uint16_t ms, uint16_t vol)
{
    if (freq < 30) {
        freq = 30;
    } else if (freq > 5700) {
        freq = 5700;
    }
    if (vol > 16) {
        vol = 16;
    }
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    float div = (float)clock_get_hz(clk_sys) / ((float)freq * 1000.0f);
    if (div < 1.0f) {
        div = 1.0f;
    }
    pwm_set_clkdiv(slice, div);
    pwm_set_wrap(slice, 999);
    pwm_set_chan_level(slice, pwm_gpio_to_channel(BUZZER_PIN), (uint16_t)(500 * vol / 16)); // 50% duty = max
    s_beep_end = time_us_64() + (uint64_t)(ms ? ms : 1) * 1000;
}

static void buzzer_poll(uint64_t now)
{
    if (s_beep_end && now >= s_beep_end) {
        pwm_set_chan_level(pwm_gpio_to_slice_num(BUZZER_PIN), pwm_gpio_to_channel(BUZZER_PIN), 0);
        s_beep_end = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Temperature: QMI8658 die sensor (fallback: RP2350 internal sensor)
// ---------------------------------------------------------------------------------------------
static bool s_have_imu;

static float read_temp_raw(void)
{
    if (s_have_imu) {
        return bsp_qmi8658_readTemp(); // NAN on a failed I2C transfer
    }
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    float v = adc_read() * 3.3f / 4096.0f;
    return 27.0f - (v - 0.706f) / 0.001721f;
}

// One sample per second: failed / implausible samples are dropped, the reported value is the
// median of the last TEMP_MEDIAN good samples (a single glitch never reaches the CLU).
// Returns NAN until the first good sample.
#define TEMP_MEDIAN 5
static float read_temp(void)
{
    static float hist[TEMP_MEDIAN];
    static int n, w;
    static uint32_t samples, dropped;
    float t = read_temp_raw();
    bool ok = !isnan(t) && t > -30.0f && t < 85.0f;
    dropped += !ok;
    if (++samples % 30 == 0) {
        printf("[temp] raw %.2f, %lu/%lu samples dropped (I2C error / out of range), offset %.2f\n",
               (double)t, (unsigned long)dropped, (unsigned long)samples, (double)PANEL_TEMP_OFFSET);
    }
    if (ok) {
        hist[w] = t;
        w = (w + 1) % TEMP_MEDIAN;
        if (n < TEMP_MEDIAN) {
            n++;
        }
    }
    if (!n) {
        return NAN;
    }
    float s[TEMP_MEDIAN];
    memcpy(s, hist, sizeof s);
    for (int i = 1; i < n; i++) { // insertion sort of n <= 5
        float v = s[i];
        int j = i - 1;
        while (j >= 0 && s[j] > v) {
            s[j + 1] = s[j];
            j--;
        }
        s[j + 1] = v;
    }
    float c = s[n / 2] + PANEL_TEMP_OFFSET;
    return c < 0.0f ? 0.0f : c; // indoor panel: the offset must not produce negative readings
}

// ---------------------------------------------------------------------------------------------
// View: snapshot of the panel state for the UI (taken with the UART IRQ masked)
// ---------------------------------------------------------------------------------------------
#define GLOW_FADE_US 260000 // press highlight fade-out after release
static uint64_t s_release_us[PANEL_SLOTS];

static void snapshot(ui_view_t *v, uint64_t now)
{
    uint32_t s = save_and_disable_interrupts();
    const panel_t *p = &s_panel;
    memcpy(v->oled, p->oled_shown, sizeof v->oled);
    v->page = p->page;
    v->pages = PANEL_PAGES;
    v->addr = p->addr;
    v->sn = p->sn;
    v->link = !p->addr ? 0 : (p->last_poll_us && (int64_t)(now - p->last_poll_us) < 2000000) ? 2 : 1;
    v->have_temp = p->have_temp;
    v->temp_dc = (int16_t)(p->temp_centi / 10);
    bool down[PANEL_SLOTS];
    for (int i = 0; i < PANEL_SLOTS; i++) {
        int b = panel_slot_button(p, i);
        ui_slot_t *c = &v->slot[i];
        down[i] = p->slot_down[i];
        c->led = panel_slot_led(p, i, now);
        c->locate = p->buttons_led_mode == 1;
        const char *name = p->label[b][0] ? p->label[b] : p->icon_a[b];
        panel_to_ascii(c->label, sizeof c->label, name); // unconfigured button: no label
    }
    restore_interrupts(s);
    for (int i = 0; i < PANEL_SLOTS; i++) {
        if (down[i]) {
            s_release_us[i] = now;
            v->slot[i].glow = 255;
        } else {
            uint64_t dt = now - s_release_us[i];
            int g = dt >= GLOW_FADE_US ? 0 : 255 - (int)(dt * 255 / GLOW_FADE_US);
            v->slot[i].glow = (uint8_t)(g & ~0x1f); // 8 steps: bounded redraws while fading
        }
    }
}

static void set_cpu_clock(uint32_t mhz)
{
    set_sys_clock_khz(mhz * 1000, true);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, mhz * 1000000, mhz * 1000000);
}

static uint32_t panel_sn(void)
{
    if (PANEL_SN) {
        return PANEL_SN;
    }
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    uint32_t h = 2166136261u; // FNV-1a
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
        h = (h ^ id.id[i]) * 16777619u;
    }
    return PANEL_SN_PREFIX | 0xf000 | (h & 0x0fff);
}

// ---------------------------------------------------------------------------------------------
// Touch: up to 5 fingers (GT911), tracked by the controller's track id. The screen is four
// quadrants (slot 0..3, the OLED area included); each finger keeps the quadrant it started on,
// which is pressed while at least one finger holds it. A finger moving SWIPE_PX sideways is a page
// swipe: its press is cancelled (OnSwitchOff without OnClick) and the finger counts no more.
// A finger is gone after TOUCH_RELEASE_US without it in the reports (debounce), or when reports
// stop for TOUCH_STALE_US.
// ---------------------------------------------------------------------------------------------
#define FINGERS GT911_LCD_TOUCH_MAX_POINTS
#define SWIPE_PX 80

typedef struct {
    bool used;
    uint8_t id;
    int field;          // ui_hit() at touch-down; -2 once it became a swipe
    int x0, x;          // start / current x (swipe)
    bool swiped;
    uint64_t up_since;  // 0 = seen in the last report
} finger_t;

static finger_t s_fingers[FINGERS];

static int fingers_on(int field)
{
    int n = 0;
    for (int i = 0; i < FINGERS; i++) {
        n += s_fingers[i].used && s_fingers[i].field == field;
    }
    return n;
}

static void touch_poll(bsp_touch_interface_t *touch, uint64_t now)
{
    bsp_touch_data_t td = {0};
    bool fresh = now - bsp_gt911_last_report_us() < TOUCH_STALE_US;
    int npts = fresh && touch->get_data(&td) ? td.points : 0;
    bool seen[FINGERS] = {false};

    for (int p = 0; p < npts; p++) {
        int x = td.coords[p].x, y = td.coords[p].y, slot = -1;
        for (int i = 0; i < FINGERS; i++) {
            if (s_fingers[i].used && s_fingers[i].id == td.coords[p].id) {
                slot = i;
                break;
            }
        }
        if (slot < 0) { // new finger
            for (int i = 0; i < FINGERS && slot < 0; i++) {
                if (!s_fingers[i].used) {
                    slot = i;
                }
            }
            if (slot < 0) {
                continue;
            }
            finger_t *f = &s_fingers[slot];
            *f = (finger_t){.used = true, .id = td.coords[p].id, .field = ui_hit(x, y), .x0 = x, .x = x};
            if (f->field >= 0 && fingers_on(f->field) == 1) {
                LOCKED(panel_touch(&s_panel, f->field, true, now));
            }
        }
        finger_t *f = &s_fingers[slot];
        seen[slot] = true;
        f->up_since = 0;
        f->x = x;
        if (!f->swiped && s_panel.page_control_mode && abs(x - f->x0) > SWIPE_PX) {
            f->swiped = true; // swipe left = next page
            int field = f->field;
            f->field = -2;
            if (field >= 0 && fingers_on(field) == 0) {
                LOCKED(panel_touch_cancel(&s_panel, field, now));
            }
            LOCKED(panel_swipe(&s_panel, x < f->x0 ? +1 : -1, now));
        }
    }

    for (int i = 0; i < FINGERS; i++) {
        finger_t *f = &s_fingers[i];
        if (!f->used || seen[i]) {
            continue;
        }
        if (!f->up_since) {
            f->up_since = now;
        }
        if (now - f->up_since < TOUCH_RELEASE_US) {
            continue;
        }
        f->used = false;
        if (f->field >= 0 && fingers_on(f->field) == 0) {
            LOCKED(panel_touch(&s_panel, f->field, false, now));
        }
    }
}

int main(void)
{
    stdio_init_all();
    set_cpu_clock(240);
    bsp_i2c_init();

    // display (same setup as the vendor examples: one 480x480 RGB565 framebuffer in SRAM)
    static pio_rgb_info_t rgb;
    rgb.width = LCD_W;
    rgb.height = LCD_H;
    rgb.transfer_size = LCD_W * 80;
    rgb.pclk_freq = BSP_LCD_PCLK_FREQ;
    rgb.mode.double_buffer = false;
    rgb.mode.enabled_transfer = false;
    rgb.mode.enabled_psram = false;
    rgb.framebuffer1 = malloc(LCD_W * LCD_H * sizeof(uint16_t));
    if (!rgb.framebuffer1) { // 460 KB of 520 KB SRAM: fails if .bss/.data grow too much
        panic("framebuffer: out of memory (%u bytes)", (unsigned)(LCD_W * LCD_H * sizeof(uint16_t)));
    }
    rgb.dma_flush_done_cb = NULL;
    s_fb = rgb.framebuffer1;
    memset(s_fb, 0, LCD_W * LCD_H * sizeof(uint16_t));
    bsp_display_interface_t *disp;
    bsp_display_info_t di = {.width = LCD_W, .height = LCD_H, .brightness = 100, .user_data = &rgb};
    bsp_display_new_st7701(&disp, &di);
    disp->init();

    bsp_touch_interface_t *touch;
    bsp_touch_info_t ti = {.width = LCD_W, .height = LCD_H, .rotation = 0};
    bsp_touch_new_gt911(&touch, &ti);
    touch->init();

    s_have_imu = bsp_qmi8658_init() != 0;
    if (!s_have_imu) {
        adc_init();
        adc_set_temp_sensor_enabled(true);
    }
    buzzer_init();

    s_panel.tx = on_tx;
    s_panel.log = on_log;
    panel_init(&s_panel, panel_sn());
    rs485_init();
    printf("SMART_PANEL_FM_4 emulator  SN=0x%08lx (%lu)  hwType 3 fwType 3 fwApiVer 4\n",
           (unsigned long)s_panel.sn, (unsigned long)s_panel.sn);
    printf("OM.LUA: mm_%lu = OBJECT:new(2, 0x%08lx, 0x03, \"%08lx\")\n", (unsigned long)s_panel.sn,
           (unsigned long)s_panel.sn, (unsigned long)s_panel.sn);

    ui_init(s_fb);
    static ui_view_t view;
    uint64_t next_temp = 0, next_touch = 0, next_render = 0;

    for (;;) {
        uint64_t now = time_us_64();
        LOCKED(panel_tick(&s_panel, now));

        if (now >= next_touch) { // GT911 at 100 Hz
            next_touch = now + 10000;
            touch->read();
            now = time_us_64(); // after read(): the report stamp must not be newer than `now`
            touch_poll(touch, now);
        }

        if (now >= next_temp) {
            next_temp = now + 1000000;
            float t = read_temp();
            if (!isnan(t)) {
                LOCKED(panel_set_temp(&s_panel, t));
            }
        }

        uint16_t bf, bms, bvol;
        bool beep;
        LOCKED(beep = panel_take_beep(&s_panel, &bf, &bms, &bvol));
        if (beep) {
            buzzer_beep(bf, bms, bvol);
        }
        buzzer_poll(now);

        if (now >= next_render) { // <= 25 fps; LED flash phases need periodic checks
            next_render = now + 30000;
            snapshot(&view, now);
            ui_render(&view); // redraws only regions that changed
        }
        flush_log();
        rs485_stats(now);
    }
}
