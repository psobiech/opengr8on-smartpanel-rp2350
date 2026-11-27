// Grenton SMART_PANEL_FM_4 emulator on the Waveshare RP2350-Touch-LCD-4 (480x480 RGB LCD, GT911
// touch, RS485 on UART1 GPIO8/9 with an auto-direction transceiver, PWM buzzer on GPIO1).
//
// The TFBUS module logic lives in core/panel_core.c (host-tested). This file is the board glue:
//   UART1 RX IRQ -> panel_rx() (replies go out from a timer 800 us after the request: bus turnaround),
//   main loop     -> touch, panel_tick(), display sleep, pixel shift, buzzer, temperature, rendering
//                    of the replica UI.
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
// Display sleep (see display_poll). CMake options: PANEL_DISPLAY_BRIGHTNESS = backlight while awake
// (percent, limits LED aging and heat); PANEL_DISPLAY_DIM_BRIGHTNESS = backlight while dimmed
// (percent); PANEL_DISPLAY_BLANK 1 = blank DISPLAY_BLANK_AFTER_US after the dimming starts,
// 0 = stay dimmed.
#ifndef PANEL_DISPLAY_BRIGHTNESS
#define PANEL_DISPLAY_BRIGHTNESS 70
#endif
#ifndef PANEL_DISPLAY_DIM_BRIGHTNESS
#define PANEL_DISPLAY_DIM_BRIGHTNESS 5
#endif
#ifndef PANEL_DISPLAY_BLANK
#define PANEL_DISPLAY_BLANK 1
#endif
#define DISPLAY_FADE_US        1000000  // awake -> dimmed
#define DISPLAY_BLANK_AFTER_US 10000000 // dimmed -> blank, counted from the start of the fade
_Static_assert(PANEL_DISPLAY_DIM_BRIGHTNESS >= 0 && PANEL_DISPLAY_DIM_BRIGHTNESS <= PANEL_DISPLAY_BRIGHTNESS &&
                   PANEL_DISPLAY_BRIGHTNESS <= 100,
               "0 <= PANEL_DISPLAY_DIM_BRIGHTNESS <= PANEL_DISPLAY_BRIGHTNESS <= 100 (percent)");

// Run core calls from the main loop without racing the UART IRQ.
#define LOCKED(stmt)                                                 \
    do {                                                             \
        uint32_t _saved_interrupts = save_and_disable_interrupts();  \
        stmt;                                                        \
        restore_interrupts(_saved_interrupts);                       \
    } while (0)

static panel_t s_panel;
static uint16_t *s_framebuffer;

// Logging: the core may log from the UART IRQ; lines are printed from the main loop.
#define LOG_LINES 8
static char s_log[LOG_LINES][120];
static volatile uint32_t s_log_write, s_log_read;

// RS485 diagnostics (printed on USB every 5 s while something changes)
static volatile uint32_t s_rx_bytes, s_rx_errs, s_tx_frames;
static volatile uint8_t s_rx_last[8];

// Replies leave TX_TURNAROUND_US after the request: the master needs time to release its driver
// (DE). Answering straight from the RX IRQ (~300 us after the last byte) collided with the ESP32
// CLU, which lost the first 1-2 bytes of every reply.
#define TX_TURNAROUND_US 800
static uint8_t s_tx_queue[64];
static volatile size_t s_tx_queue_length;
static volatile bool s_tx_armed;

// Buzzer (PWM, frequency + volume, stopped from the main loop)
static uint64_t s_beep_end_us;

// Temperature: QMI8658 die sensor (fallback: RP2350 internal sensor)
static bool s_have_imu;

// Display sleep (see display_poll)
static bsp_display_interface_t *s_display;
static uint8_t s_backlight_percent = PANEL_DISPLAY_BRIGHTNESS;
static bool s_display_asleep;     // the panel's display is off: dimming / blank
static uint64_t s_asleep_since_us;
static bool s_display_blank;      // backlight off and ST7701 Display Off
static bool s_wake_touch;         // the touch that woke a blank display: not passed to the panel
static uint64_t s_last_touch_us;

// Pixel shift (see pixel_shift_poll): the framebuffer has FRAMEBUFFER_MARGIN pixels of black
// before and after it, so the shifted scan-out never reads outside the allocation.
#define PIXEL_SHIFT_PX        2
#define PIXEL_SHIFT_PERIOD_US 120000000ull // at most one 1 px step every 2 min, only while dimmed / blank
#define FRAMEBUFFER_MARGIN    (PIXEL_SHIFT_PX * LCD_W + PIXEL_SHIFT_PX)
static pio_rgb_info_t *s_rgb_info;

// View: press highlight fade-out after release
#define GLOW_FADE_US 260000
static uint64_t s_release_us[PANEL_SLOTS];

// ---------------------------------------------------------------------------------------------
// Touch: up to 5 fingers (GT911), tracked by the controller's track id. The screen is four
// quadrants (slot 0..3, the OLED area included); each finger keeps the quadrant it started on,
// which is pressed while at least one finger holds it. A press is committed only once the finger
// has stayed within DRAG_PX of its start for PRESS_DELAY_US (or lifted earlier: a tap), with the
// touch-down time as its start, so a drag never reaches the CLU. A finger moving DRAG_PX is a drag
// and counts no more; if it was already pressed, the press is released (an ordinary release for
// the CLU). A drag of SWIPE_PX is a gesture (PANEL OnGesture*, by its main axis); sideways it also
// flips the page in page control mode.
// A finger is gone after TOUCH_RELEASE_US without it in the reports (debounce), or when reports
// stop for TOUCH_STALE_US.
// ---------------------------------------------------------------------------------------------
#define FINGERS GT911_LCD_TOUCH_MAX_POINTS
#define DRAG_PX 24
#define SWIPE_PX 80
#define PRESS_DELAY_US 80000

typedef struct {
    bool used;
    uint8_t track_id;
    int field;            // ui_hit() at touch-down; -2 once it became a drag
    bool pressed;         // press committed to the panel
    bool swiped;
    int start_x, start_y; // start position
    uint64_t down_us;     // touch-down time
    uint64_t up_since_us; // 0 = seen in the last report
} finger_t;

static finger_t s_fingers[FINGERS];

static void set_cpu_clock(uint32_t mhz);
static void buzzer_init(void);
static uint32_t panel_sn(void);
static void rs485_init(void);
static void touch_poll(bsp_touch_interface_t *touch, uint64_t now);
static bool display_touch_filter(bool touched, uint64_t now);
static void display_poll(uint64_t now);
static void display_set_blank(bool blank);
static void backlight_set(uint8_t percent);
static void pixel_shift_poll(uint64_t now);
static void finger_press(finger_t *finger);
static int fingers_on(int field);
static float read_temp(void);
static float read_temp_raw(void);
static void buzzer_beep(uint16_t frequency_hz, uint16_t duration_ms, uint16_t volume);
static void buzzer_poll(uint64_t now);
static void snapshot(ui_view_t *view, uint64_t now);
static void flush_log(void);
static void rs485_stats(uint64_t now);
static void rs485_irq(void);
static void on_tx(const uint8_t *frame, size_t frame_length, void *context);
static int64_t tx_alarm(alarm_id_t id, void *user_data);
static void on_log(const char *message, void *context);

int main(void)
{
    stdio_init_all();
    set_cpu_clock(240);
    bsp_i2c_init();

    // display (same setup as the vendor examples: one 480x480 RGB565 framebuffer in SRAM)
    static pio_rgb_info_t rgb_info;
    rgb_info.width = LCD_W;
    rgb_info.height = LCD_H;
    rgb_info.transfer_size = LCD_W * 80;
    rgb_info.pclk_freq = BSP_LCD_PCLK_FREQ;
    rgb_info.mode.double_buffer = false;
    rgb_info.mode.enabled_transfer = false;
    rgb_info.mode.enabled_psram = false;
    size_t framebuffer_allocation_bytes = (LCD_W * LCD_H + 2 * FRAMEBUFFER_MARGIN) * sizeof(uint16_t);
    uint16_t *framebuffer_allocation = malloc(framebuffer_allocation_bytes);
    if (!framebuffer_allocation) { // 464 KB of 520 KB SRAM: fails if .bss/.data grow too much
        panic("framebuffer: out of memory (%u bytes)", (unsigned)framebuffer_allocation_bytes);
    }
    memset(framebuffer_allocation, 0, framebuffer_allocation_bytes); // the margins stay black
    rgb_info.framebuffer1 = framebuffer_allocation + FRAMEBUFFER_MARGIN;
    rgb_info.dma_flush_done_cb = NULL;
    s_framebuffer = rgb_info.framebuffer1;
    s_rgb_info = &rgb_info;
    bsp_display_info_t display_info = {.width = LCD_W, .height = LCD_H, .brightness = PANEL_DISPLAY_BRIGHTNESS,
                                       .user_data = &rgb_info};
    bsp_display_new_st7701(&s_display, &display_info);
    s_display->init();

    bsp_touch_interface_t *touch;
    bsp_touch_info_t touch_info = {.width = LCD_W, .height = LCD_H, .rotation = 0};
    bsp_touch_new_gt911(&touch, &touch_info);
    touch->init();

    s_have_imu = bsp_qmi8658_init() != 0;
    if (!s_have_imu) {
        adc_init();
        adc_set_temp_sensor_enabled(true);
    }
    buzzer_init();

    s_panel.transmit = on_tx;
    s_panel.log = on_log;
    panel_init(&s_panel, panel_sn());
    rs485_init();
    printf("SMART_PANEL_FM_4 emulator  SN=0x%08lx (%lu)  hwType 3 fwType 3 fwApiVer 4\n",
           (unsigned long)s_panel.serial_number, (unsigned long)s_panel.serial_number);
    printf("OM.LUA: mm_%lu = OBJECT:new(2, 0x%08lx, 0x03, \"%08lx\")\n", (unsigned long)s_panel.serial_number,
           (unsigned long)s_panel.serial_number, (unsigned long)s_panel.serial_number);

    ui_init(s_framebuffer);
    static ui_view_t view;
    uint64_t next_temperature = 0, next_touch = 0, next_render = 0;

    for (;;) {
        uint64_t now = time_us_64();
        LOCKED(panel_tick(&s_panel, now));
        display_poll(now);
        pixel_shift_poll(now);

        if (now >= next_touch) { // GT911 at 100 Hz
            next_touch = now + 10000;
            touch->read();
            now = time_us_64(); // after read(): the report stamp must not be newer than `now`
            touch_poll(touch, now);
        }

        if (now >= next_temperature) {
            next_temperature = now + 1000000;
            float temperature = read_temp();
            if (!isnan(temperature)) {
                LOCKED(panel_set_temp(&s_panel, temperature));
            }
        }

        uint16_t beep_frequency_hz, beep_duration_ms, beep_volume;
        bool beep;
        LOCKED(beep = panel_take_beep(&s_panel, &beep_frequency_hz, &beep_duration_ms, &beep_volume));
        if (beep) {
            buzzer_beep(beep_frequency_hz, beep_duration_ms, beep_volume);
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

// ---------------------------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------------------------
static void set_cpu_clock(uint32_t mhz)
{
    set_sys_clock_khz(mhz * 1000, true);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, mhz * 1000000, mhz * 1000000);
}

static void buzzer_init(void)
{
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_chan_level(slice, pwm_gpio_to_channel(BUZZER_PIN), 0);
    pwm_set_enabled(slice, true);
}

static uint32_t panel_sn(void)
{
    if (PANEL_SN) {
        return PANEL_SN;
    }
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    uint32_t hash = 2166136261u; // FNV-1a
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
        hash = (hash ^ board_id.id[i]) * 16777619u;
    }
    return PANEL_SN_PREFIX | 0xf000 | (hash & 0x0fff);
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

// ---------------------------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------------------------
static void touch_poll(bsp_touch_interface_t *touch, uint64_t now)
{
    bsp_touch_data_t touch_data = {0};
    bool fresh = now - bsp_gt911_last_report_us() < TOUCH_STALE_US;
    int point_count = fresh && touch->get_data(&touch_data) ? touch_data.points : 0;
    if (display_touch_filter(point_count > 0, now)) {
        return;
    }
    bool seen[FINGERS] = {false};

    for (int point = 0; point < point_count; point++) {
        int x = touch_data.coords[point].x, y = touch_data.coords[point].y, finger_index = -1;
        for (int i = 0; i < FINGERS; i++) {
            if (s_fingers[i].used && s_fingers[i].track_id == touch_data.coords[point].id) {
                finger_index = i;
                break;
            }
        }
        if (finger_index < 0) { // new finger
            for (int i = 0; i < FINGERS && finger_index < 0; i++) {
                if (!s_fingers[i].used) {
                    finger_index = i;
                }
            }
            if (finger_index < 0) {
                continue;
            }
            s_fingers[finger_index] = (finger_t){.used = true, .track_id = touch_data.coords[point].id,
                                                 .field = ui_hit(x, y), .start_x = x, .start_y = y, .down_us = now};
        }
        finger_t *finger = &s_fingers[finger_index];
        seen[finger_index] = true;
        finger->up_since_us = 0;
        int move_x = x - finger->start_x, move_y = y - finger->start_y;
        if (finger->field != -2 && move_x * move_x + move_y * move_y > DRAG_PX * DRAG_PX) {
            int dragged_field = finger->field;
            bool was_pressed = finger->pressed;
            finger->field = -2;
            finger->pressed = false;
            if (was_pressed && fingers_on(dragged_field) == 0) {
                LOCKED(panel_touch_cancel(&s_panel, dragged_field, now));
            }
        }
        if (finger->field >= 0 && !finger->pressed && now - finger->down_us >= PRESS_DELAY_US) {
            finger_press(finger);
        }
        if (!finger->swiped && (abs(move_x) > SWIPE_PX || abs(move_y) > SWIPE_PX)) {
            finger->swiped = true;
            panel_gesture_t gesture = abs(move_x) >= abs(move_y)
                                          ? (move_x < 0 ? PANEL_GESTURE_LEFT : PANEL_GESTURE_RIGHT)
                                          : (move_y < 0 ? PANEL_GESTURE_UP : PANEL_GESTURE_DOWN);
            LOCKED(panel_gesture(&s_panel, gesture, now));
        }
    }

    for (int i = 0; i < FINGERS; i++) {
        finger_t *finger = &s_fingers[i];
        if (!finger->used || seen[i]) {
            continue;
        }
        if (!finger->up_since_us) {
            finger->up_since_us = now;
        }
        if (now - finger->up_since_us < TOUCH_RELEASE_US) {
            continue;
        }
        if (finger->field >= 0 && !finger->pressed) {
            finger_press(finger); // tap shorter than PRESS_DELAY_US
        }
        bool was_pressed = finger->pressed;
        finger->used = false;
        if (was_pressed && fingers_on(finger->field) == 0) {
            LOCKED(panel_touch(&s_panel, finger->field, false, now));
            s_release_us[finger->field] = now; // glow fade even if no frame saw the press (short tap)
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Display sleep (LCD image retention). The core puts the display to sleep after ProximityTimeout
// without a touch (OnDisplayOff) and wakes it on a touch or SwitchOnDisplay (OnDisplayOn; a touch
// also sends OnProximityDetect: the real panel counts from the last proximity detection, this
// board has only touch). The board shows the sleep in steps:
//   - the backlight fades from PANEL_DISPLAY_BRIGHTNESS to PANEL_DISPLAY_DIM_BRIGHTNESS in DISPLAY_FADE_US;
//     the display stays readable and the buttons keep working;
//   - PANEL_DISPLAY_BLANK 1: DISPLAY_BLANK_AFTER_US after the fade started, backlight off, then
//     ST7701 Display Off (the LCD stops driving the pixels, so the liquid crystal is not biased by
//     a static image either).
// A wake is immediate, at full brightness.
// ---------------------------------------------------------------------------------------------

// Any finger on the display: restarts the countdown and wakes the display. On a blank display the
// waking touch is swallowed until all fingers lift (TOUCH_RELEASE_US debounce): the user could not
// see what they tapped. Returns true while the touch must not reach the panel.
static bool display_touch_filter(bool touched, uint64_t now)
{
    if (touched) {
        bool woke;
        LOCKED(woke = panel_display_touch(&s_panel, now));
        if (woke && s_display_blank) {
            s_wake_touch = true;
        }
        s_last_touch_us = now;
    } else if (s_wake_touch && now - s_last_touch_us >= TOUCH_RELEASE_US) {
        s_wake_touch = false;
    }
    return s_wake_touch;
}

static void display_poll(uint64_t now)
{
    bool display_on;
    LOCKED(display_on = s_panel.display_on);
    if (display_on) {
        s_display_asleep = false;
        display_set_blank(false);
        backlight_set(PANEL_DISPLAY_BRIGHTNESS);
        return;
    }
    if (!s_display_asleep) {
        s_display_asleep = true;
        s_asleep_since_us = now;
    }
    uint64_t asleep_us = now - s_asleep_since_us;
    if (PANEL_DISPLAY_BLANK && asleep_us >= DISPLAY_BLANK_AFTER_US) {
        display_set_blank(true);
        return;
    }
    uint64_t fade_us = asleep_us < DISPLAY_FADE_US ? asleep_us : DISPLAY_FADE_US;
    backlight_set((uint8_t)(PANEL_DISPLAY_BRIGHTNESS -
                            (PANEL_DISPLAY_BRIGHTNESS - PANEL_DISPLAY_DIM_BRIGHTNESS) * fade_us / DISPLAY_FADE_US));
}

// Vendor function (board/bsp/bsp_st7701.c), not declared in bsp_st7701.h. Bit-banged 3-wire SPI:
// ~2 ms per command byte.
void bsp_st7701_spi_tx_cmd(bsp_st7701_cmd_t *cmds, size_t cmd_len);

// Blank: backlight off first, then Display Off; unblank: Display On (the caller sets the backlight).
static void display_set_blank(bool blank)
{
    if (blank == s_display_blank) {
        return;
    }
    s_display_blank = blank;
    if (blank) {
        backlight_set(0);
    }
    bsp_st7701_cmd_t display_command = {.reg = blank ? 0x28 : 0x29}; // Display Off / Display On
    bsp_st7701_spi_tx_cmd(&display_command, 1);
}

static void backlight_set(uint8_t percent)
{
    if (percent != s_backlight_percent) {
        s_backlight_percent = percent;
        s_display->set_brightness(percent);
    }
}

// ---------------------------------------------------------------------------------------------
// Pixel shift (LCD image retention of static edges: corner brackets, status and page dots). The
// scan-out DMA re-reads its start address (_framebuffer) at the end of every frame, so starting it
// dy rows + dx pixels away from the framebuffer moves the whole picture on the glass, without a
// redraw and in whole frames (no tearing). The rendering, the touch mapping and the core are not
// affected. A step is taken at most every PIXEL_SHIFT_PERIOD_US and only while the display is
// dimmed or blank (display_poll), never while someone looks at it at full brightness: a step due
// while awake waits for the next dimming. A panel kept awake all the time does not move.
// The start walks the perimeter of a +-PIXEL_SHIFT_PX square, 1 px per step (invisible at
// ~0.15 mm per pixel). A vertical shift shows up to PIXEL_SHIFT_PX black rows from the margin at
// the top or bottom; a horizontal one wraps the neighbouring row's edge pixels (background).
// ---------------------------------------------------------------------------------------------
static void pixel_shift_poll(uint64_t now)
{
    static const int8_t k_shift_path[][2] = { // {dx, dy}
        {-2, -2}, {-1, -2}, {0, -2}, {1, -2}, {2, -2}, {2, -1}, {2, 0},   {2, 1},
        {2, 2},   {1, 2},   {0, 2},  {-1, 2}, {-2, 2}, {-2, 1}, {-2, 0}, {-2, -1},
    };
    static uint64_t next_shift_us = PIXEL_SHIFT_PERIOD_US;
    static size_t path_index;
    if (now < next_shift_us || s_backlight_percent > PANEL_DISPLAY_DIM_BRIGHTNESS) { // awake or still fading
        return;
    }
    next_shift_us = now + PIXEL_SHIFT_PERIOD_US;
    const int8_t *shift = k_shift_path[path_index];
    path_index = (path_index + 1) % (sizeof k_shift_path / sizeof k_shift_path[0]);
    s_rgb_info->_framebuffer = s_framebuffer + shift[1] * LCD_W + shift[0]; // a pointer store: atomic
}

static void finger_press(finger_t *finger)
{
    finger->pressed = true;
    if (fingers_on(finger->field) == 1) {
        LOCKED(panel_touch(&s_panel, finger->field, true, finger->down_us));
    }
}

static int fingers_on(int field)
{
    int count = 0;
    for (int i = 0; i < FINGERS; i++) {
        count += s_fingers[i].used && s_fingers[i].pressed && s_fingers[i].field == field;
    }
    return count;
}

// ---------------------------------------------------------------------------------------------
// Temperature
// ---------------------------------------------------------------------------------------------
// One sample per second: failed / implausible samples are dropped, the reported value is the
// median of the last TEMP_MEDIAN good samples (a single glitch never reaches the CLU).
// Returns NAN until the first good sample.
#define TEMP_MEDIAN 5
static float read_temp(void)
{
    static float history[TEMP_MEDIAN];
    static int history_count, history_write;
    static uint32_t samples, dropped;
    float raw = read_temp_raw();
    bool valid = !isnan(raw) && raw > -30.0f && raw < 85.0f;
    dropped += !valid;
    if (++samples % 30 == 0) {
        printf("[temp] raw %.2f, %lu/%lu samples dropped (I2C error / out of range), offset %.2f\n",
               (double)raw, (unsigned long)dropped, (unsigned long)samples, (double)PANEL_TEMP_OFFSET);
    }
    if (valid) {
        history[history_write] = raw;
        history_write = (history_write + 1) % TEMP_MEDIAN;
        if (history_count < TEMP_MEDIAN) {
            history_count++;
        }
    }
    if (!history_count) {
        return NAN;
    }
    float sorted[TEMP_MEDIAN];
    memcpy(sorted, history, sizeof sorted);
    for (int i = 1; i < history_count; i++) { // insertion sort of history_count <= 5
        float value = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > value) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = value;
    }
    float celsius = sorted[history_count / 2] + PANEL_TEMP_OFFSET;
    return celsius < 0.0f ? 0.0f : celsius; // indoor panel: the offset must not produce negative readings
}

static float read_temp_raw(void)
{
    if (s_have_imu) {
        return bsp_qmi8658_readTemp(); // NAN on a failed I2C transfer
    }
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    float voltage = adc_read() * 3.3f / 4096.0f;
    return 27.0f - (voltage - 0.706f) / 0.001721f;
}

// ---------------------------------------------------------------------------------------------
// Buzzer
// ---------------------------------------------------------------------------------------------
static void buzzer_beep(uint16_t frequency_hz, uint16_t duration_ms, uint16_t volume)
{
    if (frequency_hz < 30) {
        frequency_hz = 30;
    } else if (frequency_hz > 5700) {
        frequency_hz = 5700;
    }
    if (volume > 16) {
        volume = 16;
    }
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    float clock_divider = (float)clock_get_hz(clk_sys) / ((float)frequency_hz * 1000.0f);
    if (clock_divider < 1.0f) {
        clock_divider = 1.0f;
    }
    pwm_set_clkdiv(slice, clock_divider);
    pwm_set_wrap(slice, 999);
    pwm_set_chan_level(slice, pwm_gpio_to_channel(BUZZER_PIN), (uint16_t)(500 * volume / 16)); // 50% duty = max
    s_beep_end_us = time_us_64() + (uint64_t)(duration_ms ? duration_ms : 1) * 1000;
}

static void buzzer_poll(uint64_t now)
{
    if (s_beep_end_us && now >= s_beep_end_us) {
        pwm_set_chan_level(pwm_gpio_to_slice_num(BUZZER_PIN), pwm_gpio_to_channel(BUZZER_PIN), 0);
        s_beep_end_us = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// View: snapshot of the panel state for the UI (taken with the UART IRQ masked)
// ---------------------------------------------------------------------------------------------
static void snapshot(ui_view_t *view, uint64_t now)
{
    uint32_t saved_interrupts = save_and_disable_interrupts();
    const panel_t *panel = &s_panel;
    memcpy(view->oled, panel->oled_shown, sizeof view->oled);
    view->page = panel->page;
    view->pages = PANEL_PAGES;
    view->address = panel->address;
    view->serial_number = panel->serial_number;
    view->link = !panel->address ? 0
                 : (panel->last_poll_us && (int64_t)(now - panel->last_poll_us) < 2000000) ? 2 : 1;
    view->has_temperature = panel->has_temperature;
    view->temperature_deci = (int16_t)(panel->temperature_centi / 10);
    bool down[PANEL_SLOTS];
    for (int i = 0; i < PANEL_SLOTS; i++) {
        int button = panel_slot_button(panel, i);
        ui_slot_t *slot_view = &view->slot[i];
        down[i] = panel->slot_down[i];
        slot_view->led = panel_slot_led(panel, i, now);
        slot_view->locate = panel->buttons_led_mode == 1;
        const char *name = panel->label[button][0] ? panel->label[button] : panel->icon_a[button];
        panel_to_ascii(slot_view->label, sizeof slot_view->label, name); // unconfigured button: no label
    }
    restore_interrupts(saved_interrupts);
    for (int i = 0; i < PANEL_SLOTS; i++) {
        if (down[i]) {
            s_release_us[i] = now;
            view->slot[i].glow = 255;
        } else {
            uint64_t since_release_us = now - s_release_us[i];
            int glow = since_release_us >= GLOW_FADE_US ? 0 : 255 - (int)(since_release_us * 255 / GLOW_FADE_US);
            view->slot[i].glow = (uint8_t)(glow & ~0x1f); // 8 steps: bounded redraws while fading
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------------
static void flush_log(void)
{
    while (s_log_read != s_log_write) {
        printf("[panel] %s\n", s_log[s_log_read % LOG_LINES]);
        s_log_read++;
    }
}

static void rs485_stats(uint64_t now)
{
    static uint64_t next_us;
    static uint32_t last_bytes = ~0u;
    if (now < next_us) {
        return;
    }
    next_us = now + 5000000;
    uint32_t bytes = s_rx_bytes;
    if (bytes == last_bytes) {
        return;
    }
    last_bytes = bytes;
    uint8_t last_rx[8];
    for (int i = 0; i < 8; i++) {
        last_rx[i] = s_rx_last[(bytes + i) % 8];
    }
    printf("[rs485] rx %lu bytes (%lu errors), frames ok %lu bad %lu, polls %lu, tx %lu (queued %u, armed %d), "
           "addr %u, last rx %02x %02x %02x %02x %02x %02x %02x %02x\n",
           (unsigned long)bytes, (unsigned long)s_rx_errs, (unsigned long)s_panel.frames_rx,
           (unsigned long)s_panel.frames_bad, (unsigned long)s_panel.polls, (unsigned long)s_tx_frames,
           (unsigned)s_tx_queue_length, s_tx_armed, s_panel.address, last_rx[0], last_rx[1], last_rx[2], last_rx[3],
           last_rx[4], last_rx[5], last_rx[6], last_rx[7]);
}

// ---------------------------------------------------------------------------------------------
// IRQ / alarm / core callbacks
// ---------------------------------------------------------------------------------------------
static void rs485_irq(void)
{
    uint8_t bytes[32];
    size_t count = 0;
    while (uart_is_readable(RS485_UART) && count < sizeof bytes) {
        uint32_t data_register = uart_get_hw(RS485_UART)->dr;
        if (data_register & 0xf00) { // framing / parity / break / overrun: wrong baud, A/B swapped, noise
            s_rx_errs++;
        }
        bytes[count++] = (uint8_t)data_register;
        s_rx_last[s_rx_bytes++ % sizeof s_rx_last] = (uint8_t)data_register;
    }
    if (count) {
        panel_rx(&s_panel, bytes, count, time_us_64());
    }
}

static void on_tx(const uint8_t *frame, size_t frame_length, void *context)
{
    (void)context;
    uint32_t saved_interrupts = save_and_disable_interrupts();
    if (s_tx_queue_length + frame_length <= sizeof s_tx_queue) {
        memcpy(s_tx_queue + s_tx_queue_length, frame, frame_length);
        s_tx_queue_length += frame_length;
        s_tx_frames++;
    }
    bool arm_alarm = !s_tx_armed;
    s_tx_armed = true;
    restore_interrupts(saved_interrupts);
    if (arm_alarm && add_alarm_in_us(TX_TURNAROUND_US, tx_alarm, NULL, true) < 0) {
        tx_alarm(0, NULL); // no alarm slot: send now
    }
}

static int64_t tx_alarm(alarm_id_t id, void *user_data)
{
    (void)id, (void)user_data;
    uint32_t saved_interrupts = save_and_disable_interrupts();
    uint8_t frames[sizeof s_tx_queue];
    size_t frames_length = s_tx_queue_length;
    memcpy(frames, s_tx_queue, frames_length);
    s_tx_queue_length = 0;
    s_tx_armed = false;
    restore_interrupts(saved_interrupts);
    uart_write_blocking(RS485_UART, frames, frames_length); // <= 64 bytes, a few FIFO refills at most
    return 0;
}

static void on_log(const char *message, void *context)
{
    (void)context;
    uint32_t write_index = s_log_write;
    if (write_index - s_log_read >= LOG_LINES) {
        return;
    }
    strncpy(s_log[write_index % LOG_LINES], message, sizeof s_log[0] - 1);
    s_log[write_index % LOG_LINES][sizeof s_log[0] - 1] = 0;
    s_log_write = write_index + 1;
}
