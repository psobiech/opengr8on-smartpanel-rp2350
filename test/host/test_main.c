// Host tests for the board wiring (src/main.c, included: its functions are static). The Pico SDK,
// the board drivers (GT911, QMI8658, ST7701), the scan-out and the icon cache are fakes defined
// below; the panel core (core/panel_core.c) and the UI (src/ui.c) are the real ones. Covered: the
// touch state machine (tap / hold / drag / swipe / several fingers / debounce / stale reports),
// display sleep (dimming, blank, wake-touch swallowing, tap wake, pixel shift), the temperature
// median, the buzzer, RS485 RX / TX turnaround, the log ring, the UI snapshot and the boot + main
// loop.
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main firmware_main
#include "../../src/main.c"
#undef main

static int failures;

// ---- fake hardware state ---------------------------------------------------------------------
static uint64_t g_now_us;
static uint64_t g_slept_us;
static int g_interrupts_disabled_depth;
static char g_calls[2048]; // boot call log: "name arg,arg; "

// GT911
static bsp_touch_data_t g_touch_report;
static uint64_t g_gt911_report_us;
static uint8_t g_touch_level, g_touch_leave_level;
// QMI8658
static unsigned char g_imu_present = 1;
static bool g_tap_accepted = true, g_tap_pending;
static float g_imu_temperature = NAN;
static float g_tap_peak_g2, g_tap_quiet_g2;
// ADC (RP2350 temperature sensor)
static uint16_t g_adc_value;
// display
static uint8_t g_brightness_percent = 255;
static int g_brightness_changes;
static char g_st7701_commands[256];
// scan-out
static bool g_scanout_enabled = true;
static int g_scanout_shift_x = 99, g_scanout_shift_y = 99, g_scanout_shifts;
static uint32_t g_scanout_late_lines;
static const uint8_t *g_scanout_oled;
// icons
static png_icon_t g_bulb_icon;
static char g_icons_found[256];
static int g_main_loop_iterations_left;
static jmp_buf g_main_loop_exit;
// buzzer PWM
static uint16_t g_buzzer_level;
static float g_buzzer_clock_divider;
// alarms
typedef struct {
    alarm_id_t id;
    alarm_callback_t callback;
    uint64_t due_us;
} fake_alarm_t;
static fake_alarm_t g_alarms[8];
static int g_alarm_count;
static alarm_id_t g_next_alarm_id = 1;
static bool g_alarm_slots_free = true;
static int g_alarms_cancelled;
// UART
static uint32_t g_uart_rx[128]; // data register values (byte | error bits 8..11)
static size_t g_uart_rx_length, g_uart_rx_read;
static uart_hw_t g_uart_hw;
static char g_uart_written[512];
static int g_uart_writes;
static int g_fake_uart_storage;
uart_inst_t *const uart1 = (uart_inst_t *)&g_fake_uart_storage;

static bsp_display_interface_t g_display_interface;
static bsp_touch_interface_t g_touch_interface;

static void test_read_temp_before_any_sample(void);
static void test_boot_and_main_loop(void);
static void test_boot_without_imu(void);
static void test_touch_tap(void);
static void test_touch_hold(void);
static void test_touch_drag(void);
static void test_touch_swipes(void);
static void test_touch_swipe_only_middle(void);
static void test_touch_several_fingers(void);
static void test_touch_debounce_and_stale(void);
static void test_touch_page_control(void);
static void test_display_wake_filter(void);
static void test_tap_wake(void);
static void test_display_sleep(void);
static void test_pixel_shift(void);
static void test_read_temp(void);
static void test_buzzer(void);
static void test_rs485(void);
static void test_log_ring(void);
static void test_snapshot(void);
static void test_stats(void);
static void check(const char *name, int condition);
static void check_text(const char *name, const char *actual, const char *expected);
static void free_framebuffer(void);
static void reset_touch(void);
static void touch_report(uint64_t at_us, int point_count, ...);
static void hold(uint8_t track_id, int x, int y, uint64_t from_us, uint64_t to_us);
static void lift_all(uint64_t from_us, uint64_t to_us);
static const char *take_changes(void);
static void fire_alarms(void);
static void uart_receive(const char *hex, uint32_t error_bits);
static void record_call(const char *format, ...);
static void fake_display_set_brightness(uint8_t brightness);
static void fake_touch_noop(void);
static bool fake_touch_get_data(bsp_touch_data_t *data);

int main(void)
{
    test_read_temp_before_any_sample();
    test_boot_and_main_loop();
    test_boot_without_imu();
    test_touch_tap();
    test_touch_hold();
    test_touch_drag();
    test_touch_swipes();
    test_touch_swipe_only_middle();
    test_touch_several_fingers();
    test_touch_debounce_and_stale();
    test_touch_page_control();
    test_display_wake_filter();
    test_tap_wake();
    test_display_sleep();
    test_pixel_shift();
    test_read_temp();
    test_buzzer();
    test_rs485();
    test_log_ring();
    test_snapshot();
    test_stats();
    check("interrupts restored after every LOCKED section", g_interrupts_disabled_depth == 0);
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

// ---- tests -----------------------------------------------------------------------------------
static void test_read_temp_before_any_sample(void)
{
    s_have_imu = true;
    g_imu_temperature = NAN; // failed I2C transfer
    check("temperature: NAN until the first good sample", isnan(read_temp()));
    g_imu_temperature = 120.0f; // implausible
    check("temperature: out-of-range sample dropped", isnan(read_temp()));
    s_have_imu = false;
}

static void test_boot_and_main_loop(void)
{
    g_now_us = 1000000;
    g_imu_present = 1;
    g_imu_temperature = 25.0f;
    g_calls[0] = 0;
    g_main_loop_iterations_left = 4;
    if (!setjmp(g_main_loop_exit)) {
        firmware_main();
    }
    free_framebuffer();
    static const char k_boot_start[] = "gpio_init 8; gpio_put 8,1; gpio_set_dir 8,1; stdio_init_all; ";
    check("boot: RS485 TX held high (idle) before anything else",
          strncmp(g_calls, k_boot_start, sizeof k_boot_start - 1) == 0);
    check("boot: CPU and peripheral clock 240 MHz", strstr(g_calls, "set_sys_clock_khz 240000; ") != NULL &&
                                                        strstr(g_calls, "clock_configure 3,240000000; ") != NULL);
    check("boot: RS485 TX pin handed to the UART after the panel init",
          strstr(g_calls, "uart_init 115200; gpio_set_function 8,2; gpio_set_function 9,2; ") != NULL);
    check("boot: UART RX IRQ installed", strstr(g_calls, "irq_set_exclusive_handler 34; irq_set_priority 34,64; "
                                                         "irq_set_enabled 34,1; uart_set_irq_enables 1,0; ") != NULL);
    check("boot: icons after the display init", strstr(g_calls, "scanout_start; ") < strstr(g_calls, "icons_init; "));
    check("boot: GT911 touch / leave levels 60 / 40", g_touch_level == 60 && g_touch_leave_level == 40);
    check("boot: QMI8658 tap engine 0.5 / 0.2 g^2", s_have_imu && s_have_tap && g_tap_peak_g2 == 0.5f &&
                                                       g_tap_quiet_g2 == 0.2f);
    check("boot: no ADC temperature sensor with the IMU", strstr(g_calls, "adc_init") == NULL);
    check("boot: buzzer PWM silent", strstr(g_calls, "pwm_set_enabled 0,1; ") != NULL && g_buzzer_level == 0);
    uint32_t board_hash = 2166136261u;
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
        board_hash = (board_hash ^ (uint8_t)(0x11 * (i + 1))) * 16777619u;
    }
    check("boot: SN from the flash unique ID, in 0x0f05f000..0x0f05ffff",
          s_panel.serial_number == (0x0f05f000u | (board_hash & 0x0fff)));
    check("boot: panel callbacks are the board's", s_panel.transmit == on_tx && s_panel.log == on_log);
    check("main loop: temperature read (25.0 + offset -6.8)", s_panel.has_temperature &&
                                                                  s_panel.temperature_centi == 1820);
    check("main loop: the OLED handed to the scan-out", g_scanout_oled == s_panel.oled_shown);
    check("main loop: display awake at full brightness", !s_display_blank && g_brightness_percent == 255);
}

static void test_boot_without_imu(void)
{
    g_imu_present = 0;
    g_calls[0] = 0;
    g_main_loop_iterations_left = 1;
    if (!setjmp(g_main_loop_exit)) {
        firmware_main();
    }
    free_framebuffer();
    check("boot without IMU: ADC temperature sensor enabled",
          strstr(g_calls, "adc_init; adc_set_temp_sensor_enabled 1; ") != NULL && !s_have_imu);
    g_adc_value = 876; // 0.706 V = 27 C
    float raw = read_temp_raw();
    check("ADC temperature: 0.706 V is 27 C", fabsf(raw - 27.0f) < 0.3f);
    g_imu_present = 1;
    s_have_imu = true;
}

static void test_touch_tap(void)
{
    reset_touch();
    hold(1, 100, 100, 0, 30000); // 30 ms tap, shorter than PRESS_DELAY_US
    check_text("tap: nothing before the release debounce", take_changes(), "");
    lift_all(40000, 60000);
    check_text("tap: still nothing within TOUCH_RELEASE_US", take_changes(), "");
    lift_all(70000, 80000);
    check_text("tap: press + release once the finger is gone", take_changes(), "0+ 0- ");
    check("tap: glow fade starts at the release", s_release_us[0] == 80000);
    check("tap: finger slot free again", !s_fingers[0].used);
}

static void test_touch_hold(void)
{
    reset_touch();
    hold(1, 400, 100, 0, 70000);
    check_text("hold: no press before PRESS_DELAY_US", take_changes(), "");
    hold(1, 402, 103, 80000, 500000); // small jitter, within DRAG_PX
    check_text("hold: press after PRESS_DELAY_US (top-right = button 1)", take_changes(), "1+ ");
    lift_all(510000, 560000);
    check_text("hold: release after the debounce", take_changes(), "1- ");
}

static void test_touch_drag(void)
{
    reset_touch();
    hold(1, 100, 400, 0, 30000);
    hold(1, 130, 400, 40000, 300000); // 30 px > DRAG_PX before the press delay
    lift_all(310000, 400000);
    check_text("drag before the press delay: never reaches the CLU", take_changes(), "");
    check("drag of 30 px: no gesture", s_panel.pending_ext_flags == 0);

    reset_touch();
    hold(1, 100, 400, 0, 200000);
    check_text("drag after the press: pressed first (bottom-left = button 2)", take_changes(), "2+ ");
    hold(1, 100, 430, 210000, 300000);
    check_text("drag after the press: released at once", take_changes(), "2- ");
    lift_all(310000, 400000);
    check_text("drag after the press: no second release", take_changes(), "");
}

static void test_touch_swipes(void)
{
    static const struct {
        const char *name;
        int to_x, to_y;
        uint8_t ext_flag;
    } k_swipes[] = {
        {"swipe up", 240, 20, 0x02},
        {"swipe down", 240, 460, 0x04},
        {"swipe left", 20, 240, 0x08},
        {"swipe right", 460, 240, 0x10},
        {"diagonal, mostly sideways: left", 100, 180, 0x08},
    };
    for (size_t i = 0; i < sizeof k_swipes / sizeof k_swipes[0]; i++) {
        reset_touch();
        hold(1, 240, 240, 0, 10000);
        hold(1, k_swipes[i].to_x, k_swipes[i].to_y, 20000, 30000);
        check(k_swipes[i].name, s_panel.pending_ext_flags == k_swipes[i].ext_flag);
    }
    reset_touch();
    hold(1, 240, 240, 0, 10000);
    hold(1, 100, 240, 20000, 30000);
    s_panel.pending_ext_flags = 0;
    hold(1, 240, 460, 40000, 50000); // the same finger moves on: no second gesture
    check("one gesture per finger", s_panel.pending_ext_flags == 0);
    lift_all(60000, 120000);
    check_text("swipe from a quadrant: no button press", take_changes(), "");
}

static void test_touch_swipe_only_middle(void)
{
    reset_touch();
    hold(1, 240, 240, 0, 300000); // middle of the OLED
    lift_all(310000, 400000);
    check_text("middle of the OLED: held, no press", take_changes(), "");
    reset_touch();
    hold(1, 60, 240, 0, 300000); // outer half of the OLED, left: bottom-left quadrant (y >= 240)
    lift_all(310000, 400000);
    check_text("outer half of the OLED: a button", take_changes(), "2+ 2- ");
    reset_touch();
    hold(1, 600, 100, 0, 300000); // outside the screen (bogus GT911 report)
    lift_all(310000, 400000);
    check_text("outside the screen: nothing", take_changes(), "");
}

static void test_touch_several_fingers(void)
{
    reset_touch();
    for (uint64_t at_us = 0; at_us <= 200000; at_us += 10000) { // two fingers on the top-left quadrant
        touch_report(at_us, 2, 1, 50, 50, 2, 150, 150);
    }
    check_text("two fingers, one quadrant: one press", take_changes(), "0+ ");
    for (uint64_t at_us = 210000; at_us <= 300000; at_us += 10000) { // the first lifts
        touch_report(at_us, 1, 2, 150, 150);
    }
    check_text("one of two fingers lifts: still held", take_changes(), "");
    lift_all(310000, 400000);
    check_text("the last finger lifts: released", take_changes(), "0- ");

    reset_touch();
    for (uint64_t at_us = 0; at_us <= 200000; at_us += 10000) { // top-left and bottom-right
        touch_report(at_us, 2, 1, 50, 50, 2, 430, 430);
    }
    lift_all(210000, 300000);
    check_text("two quadrants: both pressed and released", take_changes(), "0+ 3+ 0- 3- ");

    reset_touch();
    for (uint64_t at_us = 0; at_us <= 200000; at_us += 10000) { // 5 fingers: the GT911 maximum
        touch_report(at_us, 5, 1, 50, 50, 2, 430, 50, 3, 50, 430, 4, 430, 430, 5, 60, 60);
    }
    check_text("five fingers: four quadrants pressed", take_changes(), "0+ 1+ 2+ 3+ ");
    s_fingers[4].used = true; // a sixth finger with every slot taken is ignored
    s_fingers[4].track_id = 9;
    touch_report(210000, 1, 7, 240, 20);
    check("a finger beyond FINGERS is ignored", s_fingers[4].track_id == 9);
}

static void test_touch_debounce_and_stale(void)
{
    reset_touch();
    hold(1, 50, 50, 0, 200000);
    take_changes();
    lift_all(210000, 230000); // a 20 ms gap in the reports
    hold(1, 50, 50, 240000, 300000);
    check_text("debounce: a short gap in the reports is not a release", take_changes(), "");
    g_now_us = 300000 + TOUCH_STALE_US; // the GT911 stops reporting while touched
    touch_poll(&g_touch_interface, g_now_us);
    g_now_us += TOUCH_RELEASE_US;
    touch_poll(&g_touch_interface, g_now_us);
    check_text("stale reports: released", take_changes(), "0- ");
}

static void test_touch_page_control(void)
{
    reset_touch();
    s_panel.page_control_mode = 1;
    hold(1, 300, 240, 0, 10000);
    hold(1, 100, 240, 20000, 30000); // left: next page
    lift_all(40000, 100000);
    check("page control: swipe left = page 2", s_panel.page == 2);
    hold(2, 50, 50, 110000, 300000);
    lift_all(310000, 400000);
    check_text("page 2: top-left is button 4", take_changes(), "4+ 4- ");
}

static void test_display_wake_filter(void)
{
    reset_touch();
    s_panel.display_on = false; // dimmed, not blank: the touch works
    hold(1, 50, 50, 0, 200000);
    lift_all(210000, 300000);
    check("dimmed display: a touch wakes it", s_panel.display_on);
    check_text("dimmed display: the waking touch reaches the CLU", take_changes(), "0+ 0- ");
    check("waking touch: OnProximityDetect", s_panel.pending_ext_flags & 0x01);

    reset_touch();
    s_panel.display_on = false;
    s_display_blank = true;
    hold(1, 50, 50, 0, 300000);
    check("blank display: the waking touch is swallowed", s_wake_touch && !s_fingers[0].used);
    s_display_blank = false; // unblanked by display_poll meanwhile
    hold(1, 50, 50, 310000, 400000);
    lift_all(410000, 430000);
    check("blank display: still swallowed within TOUCH_RELEASE_US of the lift", s_wake_touch);
    lift_all(440000, 460000);
    check("blank display: released after the lift", !s_wake_touch);
    check_text("blank display: no button press", take_changes(), "");
    hold(1, 50, 50, 470000, 700000);
    lift_all(710000, 800000);
    check_text("next touch after the wake works", take_changes(), "0+ 0- ");
    check("touch restarts the ProximityTimeout countdown", s_panel.display_activity_us == 700000);
}

static void test_tap_wake(void)
{
    reset_touch();
    g_now_us = 1000;
    tap_poll(g_now_us);
    check("no tap: nothing", s_panel.display_activity_us == 0);
    g_tap_pending = true;
    tap_poll(g_now_us);
    check("tap on an awake display: countdown restarted, no swallowing",
          s_panel.display_activity_us == 1000 && !s_wake_touch);
    s_panel.display_on = false;
    s_display_blank = true;
    g_tap_pending = true;
    tap_poll(2000);
    check("tap on a blank display: woken, the following touch swallowed",
          s_panel.display_on && s_wake_touch && s_last_touch_us == 2000);
    s_display_blank = false;
    s_wake_touch = false;
}

static void test_display_sleep(void)
{
    reset_touch();
    s_display = &g_display_interface;
    s_backlight_percent = PANEL_DISPLAY_BRIGHTNESS;
    g_brightness_percent = PANEL_DISPLAY_BRIGHTNESS;
    g_brightness_changes = 0;
    s_display_asleep = false;
    g_now_us = 10000000;
    display_poll(g_now_us);
    check("awake: full brightness, nothing sent", g_brightness_changes == 0);
    s_panel.display_on = false;
    display_poll(g_now_us);
    check("asleep: the fade starts at full brightness", g_brightness_percent == PANEL_DISPLAY_BRIGHTNESS);
    g_now_us += DISPLAY_FADE_US / 2;
    display_poll(g_now_us);
    check("fade: half way", g_brightness_percent == 38);
    g_now_us += DISPLAY_FADE_US;
    display_poll(g_now_us);
    check("fade: dimmed after DISPLAY_FADE_US", g_brightness_percent == PANEL_DISPLAY_DIM_BRIGHTNESS);
    g_st7701_commands[0] = 0;
    g_now_us = 10000000 + DISPLAY_BLANK_AFTER_US;
    display_poll(g_now_us);
    check("blank: backlight off", g_brightness_percent == 0 && s_display_blank);
    check_text("blank: Display Off, Sleep In", g_st7701_commands, "28 10 ");
    check("blank: scan-out runs until the ST7701 sleeps", g_scanout_enabled);
    g_now_us += ST7701_SLEEP_US - 1;
    display_poll(g_now_us);
    check("blank: scan-out still on before ST7701_SLEEP_US", g_scanout_enabled);
    g_now_us += 1;
    display_poll(g_now_us);
    check("blank: scan-out stopped ST7701_SLEEP_US after Sleep In", !g_scanout_enabled && s_scanout_stopped);
    display_poll(g_now_us + 1000);
    check_text("blank: commands sent once", g_st7701_commands, "28 10 ");

    g_st7701_commands[0] = 0;
    g_slept_us = 0;
    s_panel.display_on = true;
    display_poll(g_now_us);
    check("wake: scan-out running again", g_scanout_enabled && !s_scanout_stopped);
    check_text("wake: Sleep Out, Display On", g_st7701_commands, "11/120 29 ");
    check("wake: no extra wait long after Sleep In", g_slept_us == 0);
    check("wake: full brightness", g_brightness_percent == PANEL_DISPLAY_BRIGHTNESS && !s_display_blank);

    // a wake right after Sleep In waits out ST7701_SLEEP_US first
    display_set_blank(true);
    g_now_us += 50000;
    display_set_blank(false);
    check("wake right after Sleep In: waits the rest of ST7701_SLEEP_US", g_slept_us == ST7701_SLEEP_US - 50000);
    backlight_set(PANEL_DISPLAY_BRIGHTNESS);
}

static void test_pixel_shift(void)
{
    g_scanout_shifts = 0;
    s_backlight_percent = PANEL_DISPLAY_BRIGHTNESS;
    pixel_shift_poll(PIXEL_SHIFT_PERIOD_US + 1);
    check("pixel shift: never while awake", g_scanout_shifts == 0);
    s_backlight_percent = PANEL_DISPLAY_DIM_BRIGHTNESS;
    pixel_shift_poll(PIXEL_SHIFT_PERIOD_US - 1);
    check("pixel shift: not before PIXEL_SHIFT_PERIOD_US", g_scanout_shifts == 0);
    uint64_t first_shift_us = PIXEL_SHIFT_PERIOD_US + 2;
    pixel_shift_poll(first_shift_us);
    check("pixel shift: first step while dimmed", g_scanout_shifts == 1 && g_scanout_shift_x == -2 &&
                                                      g_scanout_shift_y == -2);
    pixel_shift_poll(first_shift_us + PIXEL_SHIFT_PERIOD_US - 1);
    check("pixel shift: at most one step per period", g_scanout_shifts == 1);
    uint64_t at_us = first_shift_us;
    bool within_square = true;
    for (int step = 1; step <= 16; step++) {
        at_us += PIXEL_SHIFT_PERIOD_US;
        int previous_x = g_scanout_shift_x, previous_y = g_scanout_shift_y;
        pixel_shift_poll(at_us);
        within_square &= abs(g_scanout_shift_x) <= SCANOUT_SHIFT_MAX && abs(g_scanout_shift_y) <= SCANOUT_SHIFT_MAX &&
                         abs(g_scanout_shift_x - previous_x) + abs(g_scanout_shift_y - previous_y) == 1;
    }
    check("pixel shift: 1 px steps around the +-SCANOUT_SHIFT_MAX square, back to the start",
          within_square && g_scanout_shift_x == -2 && g_scanout_shift_y == -2);
    s_backlight_percent = PANEL_DISPLAY_BRIGHTNESS;
}

static void test_read_temp(void)
{
    s_have_imu = true;
    static const float k_samples[] = {25.0f, 25.2f, 24.9f, 25.1f, 25.0f};
    float celsius = 0;
    for (size_t i = 0; i < sizeof k_samples / sizeof k_samples[0]; i++) {
        g_imu_temperature = k_samples[i];
        celsius = read_temp();
    }
    check("temperature: median of 5 + offset", fabsf(celsius - (25.0f + PANEL_TEMP_OFFSET)) < 0.001f);
    g_imu_temperature = 60.0f; // a single glitch replaces the oldest 25.0: 24.9 25.0 25.1 25.2 60
    celsius = read_temp();
    check("temperature: one glitch does not reach the CLU", fabsf(celsius - (25.1f + PANEL_TEMP_OFFSET)) < 0.001f);
    g_imu_temperature = NAN;
    celsius = read_temp();
    check("temperature: failed sample dropped, median kept", fabsf(celsius - (25.1f + PANEL_TEMP_OFFSET)) < 0.001f);
    g_imu_temperature = -40.0f;
    celsius = read_temp();
    check("temperature: below -30 C dropped", fabsf(celsius - (25.1f + PANEL_TEMP_OFFSET)) < 0.001f);
    for (int i = 0; i < 40; i++) { // also passes the every-30-samples report
        g_imu_temperature = 3.0f;
        celsius = read_temp();
    }
    check("temperature: never negative after the offset", celsius == 0.0f);
}

static void test_buzzer(void)
{
    g_alarm_count = 0;
    g_alarms_cancelled = 0;
    s_beep_alarm = 0;
    buzzer_beep(2700, 30, 8);
    check("beep: 50% duty at volume 16, half at 8", g_buzzer_level == 250);
    check("beep: PWM divider for 2700 Hz (wrap 1000)", fabsf(g_buzzer_clock_divider - 240000000.0f / 2700000.0f) < 0.01f);
    check("beep: an alarm ends it after 30 ms", g_alarm_count == 1 && g_alarms[0].due_us == g_now_us + 30000 &&
                                                    s_beep_end_us == 0);
    buzzer_beep(10, 0, 40); // clamped: 30 Hz, volume 16, at least 1 ms
    check("beep: volume clamped to 16", g_buzzer_level == 500);
    check("beep: frequency clamped to 30 Hz", fabsf(g_buzzer_clock_divider - 240000000.0f / 30000.0f) < 0.01f);
    check("beep: the previous alarm cancelled first", g_alarms_cancelled == 1 && g_alarm_count == 1 &&
                                                          g_alarms[0].due_us == g_now_us + 1000);
    fire_alarms();
    check("beep: the alarm silences the buzzer", g_buzzer_level == 0);
    buzzer_beep(9000, 20, 16);
    check("beep: frequency clamped to 5700 Hz", fabsf(g_buzzer_clock_divider - 240000000.0f / 5700000.0f) < 0.01f);
    fire_alarms();
    g_alarm_slots_free = false;
    s_beep_alarm = 0;
    buzzer_beep(2700, 20, 16);
    check("beep without an alarm slot: the main loop ends it", s_beep_end_us == g_now_us + 20000);
    buzzer_poll(g_now_us + 19999);
    check("beep without an alarm slot: still on before its end", g_buzzer_level == 500);
    buzzer_poll(g_now_us + 20000);
    check("beep without an alarm slot: off at its end", g_buzzer_level == 0 && s_beep_end_us == 0);
    g_alarm_slots_free = true;
}

static void test_rs485(void)
{
    panel_init(&s_panel, 0x0f057b79);
    s_panel.transmit = on_tx;
    s_panel.log = on_log;
    g_alarm_count = 0;
    g_uart_written[0] = 0;
    g_uart_writes = 0;
    s_tx_queue_length = 0;
    s_tx_armed = false;
    uint32_t errors_before = s_rx_errs;
    g_now_us += 10000;
    uart_receive("aaff2a7f", 0); // SEEK
    rs485_irq();
    check("RX: bytes reach the panel", s_panel.frames_rx == 1);
    check("TX: the ACK waits for the turnaround alarm", g_uart_writes == 0 && g_alarm_count == 1 &&
                                                             g_alarms[0].due_us == g_now_us + TX_TURNAROUND_US);
    fire_alarms();
    check_text("TX: ACK sent by the alarm", g_uart_written, "aa0001ab ");
    check("TX: queue empty, alarm disarmed", s_tx_queue_length == 0 && !s_tx_armed);

    g_uart_written[0] = 0;
    on_tx((const uint8_t[]){0xaa, 0x00, 0x01, 0xab}, 4, NULL);
    on_tx((const uint8_t[]){0xaa, 0x00, 0x01, 0xab}, 4, NULL);
    check("TX: frames queued before the alarm share it", g_alarm_count == 1);
    fire_alarms();
    check_text("TX: queued frames in one write", g_uart_written, "aa0001abaa0001ab ");

    g_uart_written[0] = 0;
    uint8_t long_frame[40] = {0xaa};
    on_tx(long_frame, sizeof long_frame, NULL);
    uint32_t frames_before = s_tx_frames;
    on_tx(long_frame, sizeof long_frame, NULL); // 80 bytes > the 64-byte queue
    check("TX: a frame that does not fit is dropped", s_tx_frames == frames_before && s_tx_queue_length == 40);
    fire_alarms();

    g_alarm_slots_free = false;
    g_uart_written[0] = 0;
    on_tx((const uint8_t[]){0xaa, 0x00, 0x01, 0xab}, 4, NULL);
    check_text("TX without an alarm slot: sent at once", g_uart_written, "aa0001ab ");
    g_alarm_slots_free = true;

    uart_receive("aaff2a7f", 0x100); // framing error on every byte
    rs485_irq();
    check("RX: UART errors counted", s_rx_errs == errors_before + 4);
    fire_alarms();
    uint32_t rx_bytes_before = s_rx_bytes;
    char forty_bytes_hex[81];
    memset(forty_bytes_hex, '0', 80);
    forty_bytes_hex[80] = 0;
    uart_receive(forty_bytes_hex, 0);
    rs485_irq();
    check("RX: at most 32 bytes per IRQ", s_rx_bytes == rx_bytes_before + 32);
    rs485_irq();
    rs485_irq(); // FIFO empty: nothing passed on
    check("RX: the rest on the next IRQ", s_rx_bytes == rx_bytes_before + 40);
}

static void test_log_ring(void)
{
    flush_log();
    for (int i = 0; i < LOG_LINES + 3; i++) {
        char message[200];
        snprintf(message, sizeof message, "line %d %0150d", i, 0); // longer than a ring line
        on_log(message, NULL);
    }
    check("log ring: full after LOG_LINES lines, the rest dropped", s_log_write - s_log_read == LOG_LINES);
    check("log ring: long lines cut and terminated", strlen(s_log[s_log_read % LOG_LINES]) == sizeof s_log[0] - 1);
    flush_log();
    check("log ring: emptied by flush_log", s_log_write == s_log_read);
}

static void test_snapshot(void)
{
    static ui_view_t view;
    panel_init(&s_panel, 0x0f05f123);
    g_now_us = 50000000;
    memset(s_release_us, 0, sizeof s_release_us);
    snapshot(&view, g_now_us);
    check("snapshot: no address = no CLU", view.link == 0 && view.page == 1 && view.pages == PANEL_PAGES);
    s_panel.address = 0x12;
    snapshot(&view, g_now_us);
    check("snapshot: addressed, never polled", view.link == 1 && view.address == 0x12);
    s_panel.last_poll_us = g_now_us - 1000000;
    snapshot(&view, g_now_us);
    check("snapshot: polled within 2 s = online", view.link == 2);
    s_panel.last_poll_us = g_now_us - 2000000;
    snapshot(&view, g_now_us);
    check("snapshot: no poll for 2 s = addressed only", view.link == 1);

    snprintf(s_panel.label[0], sizeof s_panel.label[0], "%s", "Łazienka");
    snprintf(s_panel.icon_a[0], sizeof s_panel.icon_a[0], "%s", "bulb");
    snprintf(s_panel.icon_a[1], sizeof s_panel.icon_a[1], "%s", "~fan");
    s_panel.buttons_led_mode = 1;
    panel_set_temp(&s_panel, 21.57f);
    g_icons_found[0] = 0;
    snapshot(&view, g_now_us);
    check_text("snapshot: label in ASCII for the UI font", view.slot[0].label, "Lazienka");
    check_text("snapshot: the slot icons looked up", g_icons_found, "bulb ~fan ");
    check("snapshot: icon pointers from the cache", view.slot[0].icon == &g_bulb_icon && view.slot[1].icon == NULL);
    check("snapshot: location LEDs", view.slot[0].locate && view.slot[3].locate);
    check("snapshot: temperature in 0.1 C", view.has_temperature && view.temperature_deci == 215);
    check("snapshot: SN", view.serial_number == 0x0f05f123);

    panel_touch(&s_panel, 2, true, g_now_us);
    snapshot(&view, g_now_us);
    check("snapshot: held slot glows fully", view.slot[2].glow == 255 && view.slot[0].glow == 0);
    panel_touch(&s_panel, 2, false, g_now_us);
    snapshot(&view, g_now_us + GLOW_FADE_US / 2);
    check("snapshot: glow fades in 8 steps (255 - 127, multiple of 32)", view.slot[2].glow == 128);
    snapshot(&view, g_now_us + GLOW_FADE_US);
    check("snapshot: glow gone after GLOW_FADE_US", view.slot[2].glow == 0);
}

static void test_stats(void)
{
    uint64_t at_us = 1000000000;
    s_rx_bytes++;
    rs485_stats(at_us);
    check("rs485 stats: printed when bytes arrived", true);
    rs485_stats(at_us + 5000000); // nothing new: quiet
    g_scanout_late_lines = 3;
    scanout_stats(at_us);
    scanout_stats(at_us + 5000000);
    g_now_us = 0;
    loop_step_done("slow", 0);
    g_now_us = 25000;
    loop_step_done("slow", 0);
    check("loop stats: the longest step kept", s_longest_step_us == 25000 && !strcmp(s_longest_step_name, "slow"));
    loop_stats(at_us);
    check("loop stats: reset after the report", s_longest_step_us == 0);
    loop_stats(at_us + 1);
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

// The framebuffer firmware_main allocated (it never returns on the board, so it never frees it).
static void free_framebuffer(void)
{
    free((uint16_t *)ui_row(0) - FRAMEBUFFER_MARGIN);
}

// Fresh panel (addressed and polled, so button changes are queued) and no fingers.
static void reset_touch(void)
{
    panel_init(&s_panel, 0x0f057b79);
    s_panel.address = 0x12;
    s_panel.last_poll_us = 1;
    s_panel.display_activity_us = 0;
    memset(s_fingers, 0, sizeof s_fingers);
    memset(s_release_us, 0, sizeof s_release_us);
    s_wake_touch = false;
    s_display_blank = false;
    s_last_touch_us = 0;
}

// One GT911 report at at_us: point_count (track id, x, y) triples.
static void touch_report(uint64_t at_us, int point_count, ...)
{
    va_list points;
    va_start(points, point_count);
    memset(&g_touch_report, 0, sizeof g_touch_report);
    g_touch_report.points = (uint8_t)point_count;
    for (int point = 0; point < point_count; point++) {
        g_touch_report.coords[point].id = (uint8_t)va_arg(points, int);
        g_touch_report.coords[point].x = (uint16_t)va_arg(points, int);
        g_touch_report.coords[point].y = (uint16_t)va_arg(points, int);
    }
    va_end(points);
    g_now_us = at_us;
    g_gt911_report_us = at_us;
    touch_poll(&g_touch_interface, at_us);
}

// One finger at (x, y), reported every 10 ms from from_us to to_us.
static void hold(uint8_t track_id, int x, int y, uint64_t from_us, uint64_t to_us)
{
    for (uint64_t at_us = from_us; at_us <= to_us; at_us += 10000) {
        touch_report(at_us, 1, track_id, x, y);
    }
}

// Reports without fingers every 10 ms from from_us to to_us.
static void lift_all(uint64_t from_us, uint64_t to_us)
{
    for (uint64_t at_us = from_us; at_us <= to_us; at_us += 10000) {
        touch_report(at_us, 0);
    }
}

// Button Value changes queued for the CLU since the last call: "<button>+ " / "<button>- ".
static const char *take_changes(void)
{
    static char changes[128];
    size_t length = 0;
    changes[0] = 0;
    for (int i = 0; i < s_panel.change_queue_length; i++) {
        length += (size_t)snprintf(changes + length, sizeof changes - length, "%d%c ", s_panel.change_queue[i] >> 1,
                                   s_panel.change_queue[i] & 1 ? '+' : '-');
    }
    s_panel.change_queue_length = 0;
    return changes;
}

static void fire_alarms(void)
{
    while (g_alarm_count) {
        fake_alarm_t alarm = g_alarms[0];
        memmove(g_alarms, g_alarms + 1, (size_t)--g_alarm_count * sizeof g_alarms[0]);
        alarm.callback(alarm.id, NULL);
    }
}

static void uart_receive(const char *hex, uint32_t error_bits)
{
    g_uart_rx_length = g_uart_rx_read = 0;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        unsigned value;
        sscanf(hex + i, "%2x", &value);
        g_uart_rx[g_uart_rx_length++] = value | error_bits;
    }
}

static void record_call(const char *format, ...)
{
    size_t length = strlen(g_calls);
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(g_calls + length, sizeof g_calls - length, format, arguments);
    va_end(arguments);
    length = strlen(g_calls);
    snprintf(g_calls + length, sizeof g_calls - length, "; ");
}

static void fake_display_set_brightness(uint8_t brightness)
{
    g_brightness_percent = brightness;
    g_brightness_changes++;
}

static void fake_touch_noop(void)
{
}

static bool fake_touch_get_data(bsp_touch_data_t *data)
{
    if (!g_touch_report.points) {
        return false;
    }
    *data = g_touch_report;
    return true;
}

// ---- Pico SDK fakes --------------------------------------------------------------------------
uint64_t time_us_64(void)
{
    return g_now_us;
}

void sleep_us(uint64_t delay_us)
{
    g_slept_us += delay_us;
    g_now_us += delay_us;
}

void sleep_ms(uint32_t delay_ms)
{
    sleep_us((uint64_t)delay_ms * 1000);
}

alarm_id_t add_alarm_in_us(uint64_t delay_us, alarm_callback_t callback, void *user_data, bool fire_if_past)
{
    (void)user_data, (void)fire_if_past;
    if (!g_alarm_slots_free || g_alarm_count == (int)(sizeof g_alarms / sizeof g_alarms[0])) {
        return -1;
    }
    g_alarms[g_alarm_count++] = (fake_alarm_t){.id = g_next_alarm_id, .callback = callback,
                                               .due_us = g_now_us + delay_us};
    return g_next_alarm_id++;
}

bool cancel_alarm(alarm_id_t alarm_id)
{
    for (int i = 0; i < g_alarm_count; i++) {
        if (g_alarms[i].id == alarm_id) {
            memmove(g_alarms + i, g_alarms + i + 1, (size_t)(g_alarm_count - i - 1) * sizeof g_alarms[0]);
            g_alarm_count--;
            g_alarms_cancelled++;
            return true;
        }
    }
    return false;
}

absolute_time_t make_timeout_time_us(uint64_t delay_us)
{
    return (absolute_time_t){g_now_us + delay_us};
}

bool best_effort_wfe_or_timeout(absolute_time_t timeout_timestamp)
{
    g_now_us = timeout_timestamp._private_us_since_boot;
    return true;
}

bool stdio_init_all(void)
{
    record_call("stdio_init_all");
    return true;
}

void panic(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    abort();
}

void __sev(void)
{
}

void __wfe(void)
{
}

bool set_sys_clock_khz(uint32_t frequency_khz, bool required)
{
    (void)required;
    record_call("set_sys_clock_khz %u", (unsigned)frequency_khz);
    return true;
}

bool clock_configure(enum clock_index clock, uint32_t source, uint32_t auxiliary_source, uint32_t source_frequency,
                     uint32_t frequency)
{
    (void)source, (void)auxiliary_source, (void)source_frequency;
    record_call("clock_configure %d,%u", clock, (unsigned)frequency);
    return true;
}

uint32_t clock_get_hz(enum clock_index clock)
{
    (void)clock;
    return 240000000;
}

void gpio_init(uint gpio)
{
    record_call("gpio_init %u", gpio);
}

void gpio_put(uint gpio, bool value)
{
    record_call("gpio_put %u,%d", gpio, value);
}

void gpio_set_dir(uint gpio, bool out)
{
    record_call("gpio_set_dir %u,%d", gpio, out);
}

void gpio_set_function(uint gpio, enum gpio_function function)
{
    record_call("gpio_set_function %u,%d", gpio, function);
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
    g_buzzer_level = level;
}

void pwm_set_enabled(uint slice, bool enabled)
{
    record_call("pwm_set_enabled %u,%d", slice, enabled);
}

void pwm_set_clkdiv(uint slice, float divider)
{
    (void)slice;
    g_buzzer_clock_divider = divider;
}

void pwm_set_wrap(uint slice, uint16_t wrap)
{
    (void)slice, (void)wrap;
}

uint uart_init(uart_inst_t *uart, uint baudrate)
{
    (void)uart;
    record_call("uart_init %u", baudrate);
    return baudrate;
}

void uart_set_format(uart_inst_t *uart, uint data_bits, uint stop_bits, enum uart_parity parity)
{
    (void)uart, (void)data_bits, (void)stop_bits, (void)parity;
}

void uart_set_fifo_enabled(uart_inst_t *uart, bool enabled)
{
    (void)uart, (void)enabled;
}

void uart_set_irq_enables(uart_inst_t *uart, bool rx_has_data, bool tx_needs_data)
{
    (void)uart;
    record_call("uart_set_irq_enables %d,%d", rx_has_data, tx_needs_data);
}

bool uart_is_readable(uart_inst_t *uart)
{
    (void)uart;
    return g_uart_rx_read < g_uart_rx_length;
}

// rs485_irq reads the data register once per uart_get_hw call: the next received byte.
uart_hw_t *uart_get_hw(uart_inst_t *uart)
{
    (void)uart;
    g_uart_hw.dr = g_uart_rx_read < g_uart_rx_length ? g_uart_rx[g_uart_rx_read++] : 0;
    return &g_uart_hw;
}

void uart_write_blocking(uart_inst_t *uart, const uint8_t *source, size_t length)
{
    (void)uart;
    size_t written = strlen(g_uart_written);
    for (size_t i = 0; i < length && written + 3 < sizeof g_uart_written; i++) {
        written += (size_t)snprintf(g_uart_written + written, sizeof g_uart_written - written, "%02x", source[i]);
    }
    snprintf(g_uart_written + written, sizeof g_uart_written - written, " ");
    g_uart_writes++;
}

void irq_set_exclusive_handler(uint irq, irq_handler_t handler)
{
    record_call("irq_set_exclusive_handler %u", irq);
    check("UART IRQ handler is rs485_irq", handler == rs485_irq);
}

void irq_set_priority(uint irq, uint8_t priority)
{
    record_call("irq_set_priority %u,%u", irq, priority);
}

void irq_set_enabled(uint irq, bool enabled)
{
    record_call("irq_set_enabled %u,%d", irq, enabled);
}

uint32_t save_and_disable_interrupts(void)
{
    return (uint32_t)g_interrupts_disabled_depth++;
}

void restore_interrupts(uint32_t status)
{
    g_interrupts_disabled_depth = (int)status;
}

void adc_init(void)
{
    record_call("adc_init");
}

void adc_set_temp_sensor_enabled(bool enable)
{
    record_call("adc_set_temp_sensor_enabled %d", enable);
}

void adc_select_input(uint input)
{
    check("ADC: temperature channel", input == ADC_TEMPERATURE_CHANNEL_NUM);
}

uint16_t adc_read(void)
{
    return g_adc_value;
}

void pico_get_unique_board_id(pico_unique_board_id_t *board_id)
{
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
        board_id->id[i] = (uint8_t)(0x11 * (i + 1));
    }
}

// ---- board driver fakes ----------------------------------------------------------------------
void bsp_i2c_init(void)
{
    record_call("bsp_i2c_init");
}

bool bsp_display_new_st7701(bsp_display_interface_t **interface, bsp_display_info_t *info)
{
    record_call("bsp_display_new_st7701 %ux%u,%u", info->width, info->height, info->brightness);
    g_display_interface.init = fake_touch_noop;
    g_display_interface.set_brightness = fake_display_set_brightness;
    *interface = &g_display_interface;
    return true;
}

void bsp_st7701_spi_tx_cmd(bsp_st7701_cmd_t *cmds, size_t cmd_len)
{
    for (size_t i = 0; i < cmd_len; i++) {
        size_t length = strlen(g_st7701_commands);
        if (cmds[i].delay_ms) {
            snprintf(g_st7701_commands + length, sizeof g_st7701_commands - length, "%02x/%u ", cmds[i].reg,
                     cmds[i].delay_ms);
        } else {
            snprintf(g_st7701_commands + length, sizeof g_st7701_commands - length, "%02x ", cmds[i].reg);
        }
    }
}

bool bsp_touch_new_gt911(bsp_touch_interface_t **interface, bsp_touch_info_t *info)
{
    record_call("bsp_touch_new_gt911 %ux%u", info->width, info->height);
    g_touch_interface.init = fake_touch_noop;
    g_touch_interface.read = fake_touch_noop;
    g_touch_interface.get_data = fake_touch_get_data;
    *interface = &g_touch_interface;
    return true;
}

uint64_t bsp_gt911_last_report_us(void)
{
    return g_gt911_report_us;
}

bool bsp_gt911_set_touch_levels(uint8_t touch_level, uint8_t leave_level)
{
    g_touch_level = touch_level;
    g_touch_leave_level = leave_level;
    return true;
}

unsigned char bsp_qmi8658_init(void)
{
    return g_imu_present;
}

bool bsp_qmi8658_enable_tap(float peak_threshold_g2, float quiet_threshold_g2)
{
    g_tap_peak_g2 = peak_threshold_g2;
    g_tap_quiet_g2 = quiet_threshold_g2;
    return g_tap_accepted;
}

bool bsp_qmi8658_tap_detected(void)
{
    bool tap = g_tap_pending;
    g_tap_pending = false;
    return tap;
}

float bsp_qmi8658_readTemp(void)
{
    return g_imu_temperature;
}

// ---- scan-out and icon cache fakes -----------------------------------------------------------
void scanout_init(pio_rgb_info_t *rgb_info)
{
    record_call("scanout_init %ux%u", rgb_info->width, rgb_info->height);
}

void scanout_start(void)
{
    record_call("scanout_start");
}

void scanout_set_enabled(bool enabled)
{
    g_scanout_enabled = enabled;
}

void scanout_set_oled(const uint8_t *oled)
{
    g_scanout_oled = oled;
}

void scanout_set_shift(int dx, int dy)
{
    g_scanout_shift_x = dx;
    g_scanout_shift_y = dy;
    g_scanout_shifts++;
}

uint32_t scanout_late_lines(void)
{
    return g_scanout_late_lines;
}

void icons_init(void)
{
    record_call("icons_init");
}

const png_icon_t *icons_find(const char *name)
{
    if (!name[0]) {
        return NULL;
    }
    size_t length = strlen(g_icons_found);
    snprintf(g_icons_found + length, sizeof g_icons_found - length, "%s ", name);
    return strcmp(name, "bulb") == 0 ? &g_bulb_icon : NULL;
}

// Called once per main loop iteration: the test leaves the (endless) loop from here.
void icons_poll(void)
{
    g_now_us += 10000;
    if (--g_main_loop_iterations_left <= 0) {
        longjmp(g_main_loop_exit, 1);
    }
}
