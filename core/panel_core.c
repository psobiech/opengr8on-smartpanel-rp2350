#include "panel_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "font5x7.h"

#define ACK_LEN            4
#define LED_FLASH_US       2000000 // ShowOK / ShowError: flash for 2 s ...
#define LED_FLASH_PHASE_US 250000  // ... with a 500 ms period
#define DRAW_TIMEOUT_US    1000000 // draw ops without DisplayContent -> "! TIMEOUT !" (window guessed)
#define GESTURE_SHOW_US    1000000 // gesture name on the OLED (duration guessed)
#define PROXIMITY_TIMEOUT_MIN_MS 1000 // PANEL ProximityTimeout range 1000-60000 ms
// DISCOVER reply delay: DISCOVER_FIRST_SLOT_US + slot * DISCOVER_SLOT_US, slot 0..DISCOVER_SLOTS-1
// picked from the SN and the attempt number; delays from DISCOVER_GAP_START_US on are moved past
// DISCOVER_GAP_US (20..520 ms, 625..875 ms). The real CLU keeps a DISCOVER open ~1 s and addresses
// every module that answered in it (capture: this panel at 369 ms, a RELAY at 577 ms, both
// assigned), so modules avoid collisions only by answering at different times: the gap keeps the
// panel clear of a real module's ~577 ms reply, and a frame takes only ~1 ms, so 25 ms slots are
// plenty. Another module's reply only delays ours until the bus is idle; a master frame drops it,
// and a following DISCOVER is answered instead.
#define DISCOVER_FIRST_SLOT_US 20000
#define DISCOVER_SLOT_US       25000
#define DISCOVER_SLOTS         32
#define DISCOVER_GAP_START_US  540000
#define DISCOVER_GAP_US        80000

static void render_buttons(panel_t *panel);
static void rx_process(panel_t *panel, uint64_t now_us, bool idle);
static int frame_len(const uint8_t *buffer, size_t buffered_length);
static void resync(panel_t *panel);
static void handle_frame(panel_t *panel, const uint8_t *frame, size_t frame_length, uint64_t now_us);
static void send_status(panel_t *panel, uint64_t now_us);
static void send_state_frame(panel_t *panel, uint16_t changed_buttons, int16_t temperature_deci);
static void send_display_record(panel_t *panel);
static void display_wake(panel_t *panel, uint64_t now_us);
static void service_discover_reply(panel_t *panel, uint64_t now_us);
static uint32_t discover_delay_us(uint32_t serial_number, uint32_t attempt);
static void apply_led(panel_t *panel, uint16_t mask, uint8_t cmd, uint64_t now_us);
static void set_button_mode(panel_t *panel, uint16_t mask, uint8_t mode);
static void tfbus_cmd(panel_t *panel, const uint8_t *data, uint64_t now_us);
static void step_page(panel_t *panel, int direction);
static void show_page(panel_t *panel, int page);
static void show_gesture(panel_t *panel, panel_gesture_t gesture, uint64_t now_us);
static void display_block(panel_t *panel, const uint8_t *block);
static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t max_length);
static void ack(panel_t *panel);
static void send(panel_t *panel, const uint8_t *frame, size_t frame_length);
static uint8_t xor8(const uint8_t *bytes, size_t count);
static void set_button_value(panel_t *panel, int button, bool value);
static void push_change(panel_t *panel, int button, bool value);
static int pop_change(panel_t *panel);
static bool button_held(const panel_t *panel, int button);
static void request_beep(panel_t *panel, uint16_t frequency_hz, uint16_t duration_ms, uint16_t volume);
static int oled_text_width(const char *text, int size);
static void set_oled_pixel(uint8_t *buffer, int x, int y, bool pixel_on);
static void logf_(panel_t *panel, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// ---------------------------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------------------------
void panel_init(panel_t *panel, uint32_t serial_number)
{
    void (*transmit)(const uint8_t *, size_t, void *) = panel->transmit;
    void (*log)(const char *, void *) = panel->log;
    void *context = panel->context;
    memset(panel, 0, sizeof *panel);
    panel->transmit = transmit;
    panel->log = log;
    panel->context = context;
    panel->serial_number = serial_number;
    panel->page = 1;
    panel->buzzer_on = true;
    panel->buttons_led_mode = 1;
    panel->page_control_mode = 1;
    panel->proximity_timeout_ms = 5000;
    panel->display_on = true;
    panel->reported_display_on = true;
    panel->gesture_display_mode = 1;
    panel->temperature_period_ms = 1000; // PANELSENSTEMP Sensitivity default
    render_buttons(panel);
}

// ---------------------------------------------------------------------------------------------
// RX / time
// ---------------------------------------------------------------------------------------------
void panel_rx(panel_t *panel, const uint8_t *data, size_t data_length, uint64_t now_us)
{
    if (panel->rx_length && (int64_t)(now_us - panel->rx_last_us) > PANEL_GAP_US) {
        rx_process(panel, now_us, true);
    }
    for (size_t i = 0; i < data_length; i++) {
        if (panel->rx_length == 0 && data[i] != 0xaa) {
            continue;
        }
        if (panel->rx_length == sizeof panel->rx_buffer) {
            panel->frames_bad++;
            resync(panel);
        }
        panel->rx_buffer[panel->rx_length++] = data[i];
        rx_process(panel, now_us, false);
    }
    panel->rx_last_us = now_us;
}

// now_us may be older than the last RX IRQ stamp (the caller reads the clock before an IRQ lands):
// time differences are signed, so that counts as "no gap" and does not flush a frame mid-way.
void panel_tick(panel_t *panel, uint64_t now_us)
{
    if (panel->rx_length && (int64_t)(now_us - panel->rx_last_us) > PANEL_GAP_US) {
        rx_process(panel, now_us, true);
    }
    service_discover_reply(panel, now_us);
    for (int button = 0; button < PANEL_BUTTONS; button++) {
        if ((panel->led[button] == LEDS_FLASH_GREEN || panel->led[button] == LEDS_FLASH_RED) &&
            now_us >= panel->led_flash_until_us[button]) {
            panel->led[button] = panel->led_after[button];
            panel->version++;
        }
    }
    if (panel->draw_pending && now_us >= panel->draw_deadline_us) {
        panel->draw_pending = false;
        memset(panel->oled_draw, 0, sizeof panel->oled_draw);
        memset(panel->oled_shown, 0, sizeof panel->oled_shown);
        panel_oled_text(panel->oled_shown, 64 - oled_text_width("! TIMEOUT !", 1) / 2, 28, 1, "! TIMEOUT !");
        panel->oled_mode = OLED_TIMEOUT;
        panel->version++;
    }
    if (panel->oled_mode == OLED_GESTURE && now_us >= panel->gesture_shown_until_us) {
        render_buttons(panel);
    }
    uint32_t timeout_ms = panel->proximity_timeout_ms < PROXIMITY_TIMEOUT_MIN_MS ? PROXIMITY_TIMEOUT_MIN_MS
                                                                                 : panel->proximity_timeout_ms;
    if (panel->display_on && (int64_t)(now_us - panel->display_activity_us) >= (int64_t)timeout_ms * 1000) {
        panel->display_on = false;
        panel->version++;
    }
}

// Try to complete frames in the buffer. idle: the line went quiet (unknown-length frames end).
static void rx_process(panel_t *panel, uint64_t now_us, bool idle)
{
    while (panel->rx_length >= 3) {
        if (panel->rx_buffer[0] != 0xaa) {
            resync(panel);
            continue;
        }
        int frame_length = frame_len(panel->rx_buffer, panel->rx_length);
        if (frame_length == 0) {
            return;
        }
        if (frame_length < 0) {
            if (!idle) {
                return;
            }
            frame_length = (int)panel->rx_length; // whole burst
        }
        if (frame_length > 32) {
            panel->frames_bad++;
            resync(panel);
            continue;
        }
        if (panel->rx_length < (size_t)frame_length) {
            if (idle) { // truncated frame
                panel->frames_bad++;
                resync(panel);
                continue;
            }
            return;
        }
        if (xor8(panel->rx_buffer, (size_t)frame_length - 1) != panel->rx_buffer[frame_length - 1]) {
            panel->frames_bad++;
            resync(panel);
            continue;
        }
        handle_frame(panel, panel->rx_buffer, (size_t)frame_length, now_us);
        memmove(panel->rx_buffer, panel->rx_buffer + frame_length, panel->rx_length - (size_t)frame_length);
        panel->rx_length -= (size_t)frame_length;
    }
    if (idle && panel->rx_length) {
        panel->rx_length = 0; // 1-2 stray bytes before the gap
    }
}

// Frame length for CLU -> module frames by type (0 = need more bytes, -1 = unknown: ends on idle).
// Lengths of the class frames 0x13..0x1e follow docs/TFBUS.md (old-path payload sizes) and match
// the real CLU capture for 0x16 / 0x17 / 0x18.
static int frame_len(const uint8_t *buffer, size_t buffered_length)
{
    if (buffer[1] == 0x00) {
        int reply_length = 3 + buffer[2];
        return reply_length < 4 ? 4 : reply_length;
    }
    switch (buffer[2]) {
    case 0x02: return 9;
    case 0x04: case 0x07: case 0x0a: case 0x0b: case 0x1e: case 0x2a: case 0xf9: return 4;
    case 0x08: return buffer[1] == 0xff ? 4 : -1; // broadcast DISCOVER
    case 0x09: return 5;
    case 0x13: return 9;
    case 0x14: case 0x15: case 0x17: case 0x18: case 0x1d: case 0xfd: case 0xfe: return 7;
    case 0x16: return 8;
    case 0x30: case 0x31: return 26;
    case 0xfb: return 25;
    case 0xf0: return buffered_length >= 4 ? 4 + buffer[3] : 0;
    default: return -1;
    }
}

static void resync(panel_t *panel)
{
    // drop the first byte, restart at the next start byte
    const uint8_t *next_start = memchr(panel->rx_buffer + 1, 0xaa, panel->rx_length - 1);
    size_t keep = next_start ? panel->rx_length - (size_t)(next_start - panel->rx_buffer) : 0;
    memmove(panel->rx_buffer, panel->rx_buffer + panel->rx_length - keep, keep);
    panel->rx_length = keep;
}

// ---------------------------------------------------------------------------------------------
// Frame handling
// ---------------------------------------------------------------------------------------------
static void handle_frame(panel_t *panel, const uint8_t *frame, size_t frame_length, uint64_t now_us)
{
    uint8_t dest = frame[1], type = frame[2];
    panel->frames_rx++;
    if (dest != 0x00) {
        panel->discover_reply_pending = false; // the master moved on: answer its next DISCOVER
    }
    if (dest == 0x00) {
        return; // another module's reply (or our own echo)
    }
    bool broadcast = dest == 0xff;
    if (!broadcast && (panel->address == 0 || dest != panel->address)) {
        return;
    }
    if (broadcast) {
        switch (type) {
        case 0x07: // RESET
            if (panel->address) {
                logf_(panel, "RESET: address %u released", panel->address);
            }
            panel->address = 0;
            panel->change_queue_length = 0;
            panel->last_poll_us = 0;
            break;
        case 0x2a: // SEEK: unaddressed modules ACK
            if (!panel->address) {
                ack(panel);
            }
            break;
        case 0x08: // DISCOVER: an unaddressed module reports SN(LE32) hwType fwType fwApiVer, delayed
            if (!panel->address) {
                panel->discover_reply_pending = true;
                panel->discover_reply_due_us =
                    now_us + discover_delay_us(panel->serial_number, panel->discover_requests++);
            }
            break;
        case 0x02: { // ADDR_ASSIGN sn(LE32) addr
            uint32_t assigned_serial = frame[3] | frame[4] << 8 | frame[5] << 16 | (uint32_t)frame[6] << 24;
            if (assigned_serial == panel->serial_number) {
                panel->address = frame[7];
                panel->change_queue_length = 0;
                panel->reported_button_values = 0; // a newly addressed module starts with all Values 0
                panel->last_poll_us = 0;
                panel->state_frame_pending = true;
                logf_(panel, "ADDR_ASSIGN: address %u", panel->address);
                ack(panel);
            }
            break;
        }
        default:
            break;
        }
        return;
    }
    switch (type) {
    case 0x04: // STATUS_POLL
        panel->polls++;
        panel->last_poll_us = now_us;
        send_status(panel, now_us);
        return;
    case 0xf9: // VERSION_QUERY: the real SMART_PANEL_FM_4 does not answer
        return;
    case 0x07: // unicast RESET
        logf_(panel, "unicast RESET: address %u released", panel->address);
        panel->address = 0;
        panel->last_poll_us = 0;
        return;
    case 0x18: // LED_CMD mask_lo mask_hi cmd
        apply_led(panel, (uint16_t)(frame[3] | frame[4] << 8), frame[5], now_us);
        break;
    case 0x31:
        tfbus_cmd(panel, frame + 3, now_us);
        break;
    case 0x30:
        display_block(panel, frame + 3);
        break;
    case 0x17: // PANEL_BUTTON class config mask_lo mask_hi mode
        set_button_mode(panel, (uint16_t)(frame[3] | frame[4] << 8), frame[5]);
        break;
    // 0x16 sensor config [sensor 00 temp / 01 light, Sensitivity / 100 ms, Threshold x 10 LE16] (checked
    // on a real CLU). A real panel samples once per Sensitivity and reports the sample (bench: light with
    // 5000 ms -> a report every ~5.4 s, short flashes between samples lost); Threshold does not limit
    // its reports (light 1.9 -> 2.1 % reported with Threshold 0.5). Light is not measured here.
    case 0x16:
        if (frame[3] == 0x00) {
            panel->temperature_period_ms = (uint16_t)(frame[4] * 100u);
        }
        break;
    case 0xf0: case 0x13: case 0x14: case 0x15: case 0x1d: case 0x1e:
        break; // sensor / class config: accepted, not used
    default:
        panel->frames_unknown++;
        logf_(panel, "frame type 0x%02x (%u bytes) not handled", type, (unsigned)frame_length);
        break;
    }
    ack(panel);
}

// Send the pending DISCOVER reply once due and the bus is idle (no frame being received, no bytes
// within the idle gap): another module's reply in progress only delays it.
static void service_discover_reply(panel_t *panel, uint64_t now_us)
{
    if (!panel->discover_reply_pending || (int64_t)(now_us - panel->discover_reply_due_us) < 0 ||
        panel->rx_length || (int64_t)(now_us - panel->rx_last_us) <= PANEL_GAP_US) {
        return;
    }
    panel->discover_reply_pending = false;
    uint32_t serial_number = panel->serial_number;
    uint8_t reply[11] = {0xaa, 0x00, 0x08, (uint8_t)serial_number, (uint8_t)(serial_number >> 8),
                         (uint8_t)(serial_number >> 16), (uint8_t)(serial_number >> 24), PANEL_HW_TYPE,
                         PANEL_FW_TYPE, PANEL_FW_API_VER, 0};
    reply[10] = xor8(reply, 10);
    panel->discovered++;
    logf_(panel, "DISCOVER: reported SN %08lx", (unsigned long)serial_number);
    send(panel, reply, sizeof reply);
}

// Reply slot for this SN and attempt (murmur3 finalizer): differs per attempt, so two panels
// sharing a slot once do not keep colliding.
static uint32_t discover_delay_us(uint32_t serial_number, uint32_t attempt)
{
    uint32_t mixed = serial_number ^ (attempt * 0x9e3779b9u);
    mixed ^= mixed >> 16;
    mixed *= 0x85ebca6bu;
    mixed ^= mixed >> 13;
    mixed *= 0xc2b2ae35u;
    mixed ^= mixed >> 16;
    uint32_t delay_us = DISCOVER_FIRST_SLOT_US + (mixed % DISCOVER_SLOTS) * DISCOVER_SLOT_US;
    return delay_us < DISCOVER_GAP_START_US ? delay_us : delay_us + DISCOVER_GAP_US;
}

// STATUS_POLL reply. The real CLU (firmware.bin: PANEL_BUTTON state handler 0xb14a2, sensor
// handler 0xb1526, both given frame + 2) reads every SMART_PANEL reply by position, not as TLV:
//   aa 00 0e | changed(LE16) | values(LE16) | temp(LE16, 0.1 C) | light(LE16, 0.1 %) | 00 00 | ext 00 00 | xor
// A button's Value is taken from `values` only while its bit in `changed` is set. Frame byte 13
// (ext) carries one-shot flags, checked against PANEL events on a real CLU: 01 OnProximityDetect,
// 02 OnGestureUp, 04 OnGestureDown, 08 OnGestureLeft, 10 OnGestureRight; 80 = display record
// (all other bytes 0, byte 3 = 1 OnDisplayOn / 0 OnDisplayOff; see send_display_record). There is
// no proximity sensor: the proximity flag goes with a touch that wakes the display
// (panel_display_touch). The CLU derives
// OnSwitchOn / OnSwitchOff / OnChange / OnClick / OnHold ... from Value itself, so the panel sends
// no button events. Like the real panels (capture 2026-10-14) it answers with a bare ACK when there
// is nothing to report.
static void send_status(panel_t *panel, uint64_t now_us)
{
    if (panel->display_on != panel->reported_display_on) {
        send_display_record(panel);
        return;
    }
    int16_t temperature_deci = panel->has_temperature ? (int16_t)(panel->temperature_centi / 10) // same rounding as the screen
                                                      : panel->reported_temperature_deci;
    int change = pop_change(panel);
    if (change < 0 && panel->button_values != panel->reported_button_values) {
        // a change that did not fit the queue (or happened while not polled): report the current Value
        uint16_t differing = panel->button_values ^ panel->reported_button_values;
        int button = __builtin_ctz(differing);
        change = button << 1 | ((panel->button_values >> button) & 1);
    }
    if (change >= 0) {
        uint16_t button_bit = (uint16_t)(1u << (change >> 1));
        panel->reported_button_values = (change & 1) ? (uint16_t)(panel->reported_button_values | button_bit)
                                                     : (uint16_t)(panel->reported_button_values & ~button_bit);
        send_state_frame(panel, button_bit, temperature_deci);
        return;
    }
    if (panel->pending_ext_flags) {
        send_state_frame(panel, 0, temperature_deci);
        return;
    }
    // temperature alone: at most once per Sensitivity (temperature_period_ms)
    if (panel->has_temperature &&
        (panel->state_frame_pending ||
         (temperature_deci != panel->reported_temperature_deci &&
          now_us - panel->last_temperature_report_us >= (uint64_t)panel->temperature_period_ms * 1000))) {
        send_state_frame(panel, 0, temperature_deci);
        panel->last_temperature_report_us = now_us;
        return;
    }
    ack(panel);
}

// State frame of the real SMART_PANEL_FM_4 (see send_status). Real panels, nothing changed:
//   aa 00 0e 00 00 00 00 da 00 19 00 00 00 00 9c 01 xx  (21.8 C; the last pair, 0x176..0x1c2, is
// probably proximity, sent as 0). Light is not measured: sent as 0.
static void send_state_frame(panel_t *panel, uint16_t changed_buttons, int16_t temperature_deci)
{
    uint8_t frame[17] = {0xaa, 0x00, 0x0e,
                         (uint8_t)changed_buttons, (uint8_t)(changed_buttons >> 8),
                         (uint8_t)panel->reported_button_values, (uint8_t)(panel->reported_button_values >> 8),
                         (uint8_t)temperature_deci, (uint8_t)((uint16_t)temperature_deci >> 8)};
    frame[13] = panel->pending_ext_flags;
    panel->pending_ext_flags = 0;
    frame[16] = xor8(frame, 16);
    send(panel, frame, sizeof frame);
    panel->reported_temperature_deci = temperature_deci;
    panel->state_frame_pending = false;
}

// Display record (OnDisplayOn / OnDisplayOff), as the ESP32 CLU driver decodes it: a state frame
// with ext 0x80 and all other bytes 0 except byte 3 (1 on, 0 off). It carries no buttons or
// temperature, so their reported state is left as it was. Only the latest state is sent: a sleep
// and a wake between two polls report nothing.
static void send_display_record(panel_t *panel)
{
    uint8_t frame[17] = {0xaa, 0x00, 0x0e, panel->display_on ? 0x01 : 0x00};
    frame[13] = 0x80;
    frame[16] = xor8(frame, 16);
    send(panel, frame, sizeof frame);
    panel->reported_display_on = panel->display_on;
}

// A real panel wakes on proximity: the display-on record, then a frame with OnProximityDetect (01)
// (ESP32 CLU driver). A touch stands in for the proximity detection, so a waking touch sends both.
bool panel_display_touch(panel_t *panel, uint64_t now_us)
{
    bool was_on = panel->display_on;
    display_wake(panel, now_us);
    if (!was_on) {
        panel->pending_ext_flags |= 0x01; // OnProximityDetect
    }
    return !was_on;
}

// Touch / SwitchOnDisplay: the display is on, the ProximityTimeout countdown starts again.
static void display_wake(panel_t *panel, uint64_t now_us)
{
    panel->display_activity_us = now_us;
    if (!panel->display_on) {
        panel->display_on = true;
        panel->version++;
    }
}

static void apply_led(panel_t *panel, uint16_t mask, uint8_t cmd, uint64_t now_us)
{
    for (int button = 0; button < PANEL_BUTTONS; button++) {
        if (!(mask & (1u << button))) {
            continue;
        }
        led_state_t steady = (panel->led[button] == LEDS_FLASH_GREEN || panel->led[button] == LEDS_FLASH_RED)
                                 ? panel->led_after[button] : panel->led[button];
        switch (cmd) {
        case LED_GREEN: panel->led[button] = LEDS_GREEN; break;
        case LED_RED: panel->led[button] = LEDS_RED; break;
        case LED_OFF: panel->led[button] = LEDS_OFF; break;
        case LED_SHOW_OK:
        case LED_SHOW_ERROR:
            panel->led_after[button] = steady;
            panel->led[button] = cmd == LED_SHOW_OK ? LEDS_FLASH_GREEN : LEDS_FLASH_RED;
            panel->led_flash_until_us[button] = now_us + LED_FLASH_US;
            break;
        default:
            logf_(panel, "LED_CMD unknown cmd 0x%02x mask 0x%04x", cmd, mask);
            return;
        }
    }
    panel->version++;
}

// PANEL_BUTTON Mode for the buttons in mask. Logged on change, so a real-CLU session shows
// whether byte 3 really is the Mode set in OM. A button leaving bistable mode drops its state.
static void set_button_mode(panel_t *panel, uint16_t mask, uint8_t mode)
{
    if (mode > BUTTON_MODE_LOCKED) {
        logf_(panel, "button config 0x17 mask 0x%04x: unknown mode %u, kept", mask, mode);
        return;
    }
    for (int button = 0; button < PANEL_BUTTONS; button++) {
        if (!(mask & (1u << button)) || panel->button_mode[button] == mode) {
            continue;
        }
        logf_(panel, "button %d mode %u -> %u", button, panel->button_mode[button], mode);
        panel->button_mode[button] = mode;
        if (mode == BUTTON_MODE_LOCKED || (mode == BUTTON_MODE_MONOSTABLE && !button_held(panel, button))) {
            set_button_value(panel, button, false); // a bistable ON does not outlive the mode change
        }
    }
}

static void tfbus_cmd(panel_t *panel, const uint8_t *data, uint64_t now_us)
{
    uint8_t cmd = data[0];
    const uint8_t *args = data + 1;
    switch (cmd) {
    case 0x00: display_wake(panel, now_us); break; // SwitchOnDisplay (ESP32 CLU driver)
    case 0x0a: { // PRINT_TEXT x y font text[16]
        char text[17];
        copy_str(text, sizeof text, args + 3, 16);
        panel_oled_text(panel->oled_draw, args[0], args[1], args[2], text);
        if (!panel->draw_pending) {
            panel->draw_deadline_us = now_us + DRAW_TIMEOUT_US;
        }
        panel->draw_pending = true;
        break;
    }
    case 0x04: // ClearScreen: only this blanks the draw buffer; a shown freedraw screen goes blank too
        memset(panel->oled_draw, 0, sizeof panel->oled_draw);
        if (panel->oled_mode == OLED_FREEDRAW) {
            memset(panel->oled_shown, 0, sizeof panel->oled_shown);
            panel->version++;
        }
        break;
    case 0x09: // DisplayContent (ESP32 CLU driver)
    case 0x19: // Draw: show the draw buffer; it is kept, so later texts add to it
        memcpy(panel->oled_shown, panel->oled_draw, sizeof panel->oled_shown);
        panel->draw_pending = false;
        panel->oled_mode = OLED_FREEDRAW;
        panel->version++;
        break;
    case 0x0f: // BUZZER_BEEP freq dur vol (u16 LE)
        request_beep(panel, (uint16_t)(args[0] | args[1] << 8), (uint16_t)(args[2] | args[3] << 8),
                     (uint16_t)(args[4] | args[5] << 8));
        break;
    // Panel settings sent by the CLU after init (values from a real capture match the PANEL
    // feature defaults; command -> feature mapping inferred from that order).
    case 0x02: panel->proximity_timeout_ms = (uint16_t)(args[0] | args[1] << 8); break; // ProximityTimeout
    case 0x0c: panel->buzzer_on = args[0] != 0; break;                                 // BuzzerValue
    case 0x14: panel->buttons_led_mode = args[0]; panel->version++; break;             // ButtonsLEDMode
    case 0x15: panel->page_control_mode = args[0]; break;                              // PageControlMode
    case 0x16: panel->gesture_display_mode = args[0]; break;                           // GestureDisplayMode
    // Page commands (capture 2026-11-26, real CLU): SetPageNr(n) -> 0x10 [n], SetNextPage -> 0x17,
    // SetPrevPage -> 0x18. They work in any PageControlMode. Init pass 2 sends PageNr 0: ignored.
    case 0x10:
        if (args[0] >= 1 && args[0] <= PANEL_PAGES) {
            show_page(panel, args[0]);
        }
        break;
    case 0x17: step_page(panel, +1); break;
    case 0x18: step_page(panel, -1); break;
    case 0x01: case 0x0d: case 0x0e: case 0x11:                         // sens / gesture / page modes
    case 0x12: case 0x1a:                                                // page / button config (undecoded)
        break;
    default:
        panel->frames_unknown++;
        logf_(panel, "TFBUS_CMD 0x%02x %02x %02x %02x %02x not handled", cmd, args[0], args[1], args[2], args[3]);
        break;
    }
}

// DISPLAY_BLOCK (0x30): chan sub data[20]. Button config: chan = button, sub 0x00 / 0x01 / 0x0a
// (IconA / IconB guessed from the init order, Label confirmed). chan 0 sub 2..5 = PANEL gesture
// icons. The button icons are drawn by the board on the button (panel_slot_icon), not on the OLED.
static void display_block(panel_t *panel, const uint8_t *block)
{
    uint8_t channel = block[0], sub = block[1];
    const uint8_t *block_data = block + 2;
    // PANEL GestureIcon*: sub 2 left, 3 right, 4 up, 5 down (the CLU driver's order)
    static const panel_gesture_t k_gesture_by_sub[4] = {PANEL_GESTURE_LEFT, PANEL_GESTURE_RIGHT, PANEL_GESTURE_UP,
                                                        PANEL_GESTURE_DOWN};
    if (channel == 0 && sub >= 0x02 && sub <= 0x05) {
        copy_str(panel->gesture_icon[k_gesture_by_sub[sub - 2]], 17, block_data, 16);
        return;
    }
    if (channel < PANEL_BUTTONS && (sub == 0x00 || sub == 0x01 || sub == 0x0a)) {
        char *dst = sub == 0x0a ? panel->label[channel] : sub == 0x00 ? panel->icon_a[channel] : panel->icon_b[channel];
        char previous[17];
        memcpy(previous, dst, sizeof previous);
        copy_str(dst, 17, block_data, 16);
        if (strcmp(previous, dst) != 0) {
            panel->version++;
            logf_(panel, "button %d %s \"%s\"", channel, sub == 0x0a ? "Label" : sub == 0x00 ? "IconA" : "IconB", dst);
        }
    }
}

static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t max_length)
{
    size_t i = 0;
    for (; i < max_length && i < cap - 1 && src[i]; i++) {
        dst[i] = (char)src[i];
    }
    dst[i] = 0;
}

static void ack(panel_t *panel)
{
    static const uint8_t ack_frame[ACK_LEN] = {0xaa, 0x00, 0x01, 0xab};
    send(panel, ack_frame, sizeof ack_frame);
}

static void send(panel_t *panel, const uint8_t *frame, size_t frame_length)
{
    if (panel->transmit) {
        panel->transmit(frame, frame_length, panel->context);
    }
}

static uint8_t xor8(const uint8_t *bytes, size_t count)
{
    uint8_t checksum = 0;
    while (count--) {
        checksum ^= *bytes++;
    }
    return checksum;
}

// ---------------------------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------------------------
void panel_touch(panel_t *panel, int slot, bool down, uint64_t now_us)
{
    (void)now_us;
    if (slot < 0 || slot >= PANEL_SLOTS || panel->slot_down[slot] == down) {
        return;
    }
    panel->slot_down[slot] = down;
    if (down) {
        int button = panel_slot_button(panel, slot);
        panel->slot_button[slot] = (uint8_t)button;
        request_beep(panel, 2700, 30, 8); // key click (30 ms: 20 ms sounded clipped once the length was exact)
        if (panel->button_mode[button] == BUTTON_MODE_BISTABLE) { // each press toggles, release keeps it
            set_button_value(panel, button, !(panel->button_values & (1u << button)));
        } else if (panel->button_mode[button] == BUTTON_MODE_MONOSTABLE) {
            set_button_value(panel, button, true);
        }
    } else {
        int button = panel->slot_button[slot];
        if (panel->button_mode[button] == BUTTON_MODE_MONOSTABLE && !button_held(panel, button)) {
            set_button_value(panel, button, false);
        }
    }
    panel->version++;
}

void panel_touch_cancel(panel_t *panel, int slot, uint64_t now_us)
{
    panel_touch(panel, slot, false, now_us);
}

void panel_gesture(panel_t *panel, panel_gesture_t gesture, uint64_t now_us)
{
    static const uint8_t k_ext_flag[4] = {0x02, 0x04, 0x08, 0x10}; // per panel_gesture_t
    panel->pending_ext_flags |= k_ext_flag[gesture];
    if (panel->page_control_mode && (gesture == PANEL_GESTURE_LEFT || gesture == PANEL_GESTURE_RIGHT)) {
        step_page(panel, gesture == PANEL_GESTURE_LEFT ? +1 : -1);
    }
    if (panel->gesture_display_mode && (panel->oled_mode == OLED_BUTTONS || panel->oled_mode == OLED_GESTURE)) {
        show_gesture(panel, gesture, now_us);
    }
}

// Next (+1) / previous (-1) page, wrapping around.
static void step_page(panel_t *panel, int direction)
{
    int new_page = panel->page + (direction > 0 ? 1 : -1);
    show_page(panel, new_page < 1 ? PANEL_PAGES : new_page > PANEL_PAGES ? 1 : new_page);
}

static void show_page(panel_t *panel, int page)
{
    panel->page = (uint8_t)page;
    render_buttons(panel);
}

// Gesture icon text (or the direction name) centred on the OLED until GESTURE_SHOW_US has passed.
static void show_gesture(panel_t *panel, panel_gesture_t gesture, uint64_t now_us)
{
    static const char *const k_gesture_names[4] = {"Up", "Down", "Left", "Right"};
    char text[17];
    snprintf(text, sizeof text, "%s", panel->gesture_icon[gesture][0] ? panel->gesture_icon[gesture]
                                                                       : k_gesture_names[gesture]);
    text[10] = 0; // 10 chars fit the width at size 2
    memset(panel->oled_shown, 0, sizeof panel->oled_shown);
    panel_oled_text(panel->oled_shown, 64 - oled_text_width(text, 2) / 2, 24, 2, text);
    panel->oled_mode = OLED_GESTURE;
    panel->gesture_shown_until_us = now_us + GESTURE_SHOW_US;
    panel->version++;
}

// ---------------------------------------------------------------------------------------------
// Button Value
// ---------------------------------------------------------------------------------------------
static void set_button_value(panel_t *panel, int button, bool value)
{
    uint16_t button_bit = (uint16_t)(1u << button);
    if (((panel->button_values & button_bit) != 0) == value) {
        return;
    }
    panel->button_values = value ? (uint16_t)(panel->button_values | button_bit)
                                 : (uint16_t)(panel->button_values & ~button_bit);
    push_change(panel, button, value);
}

// True while another pressed field still holds the button (two fingers on one field).
static bool button_held(const panel_t *panel, int button)
{
    for (int slot = 0; slot < PANEL_SLOTS; slot++) {
        if (panel->slot_down[slot] && panel->slot_button[slot] == button) {
            return true;
        }
    }
    return false;
}

static void push_change(panel_t *panel, int button, bool value)
{
    if (!panel->address || !panel->last_poll_us) {
        return; // no CLU polling us: the current Value goes out once it polls (see send_status)
    }
    if (panel->change_queue_length >= PANEL_CHANGE_QUEUE_SIZE) {
        if (!panel->change_queue_overflow) {
            logf_(panel, "button change queue full (CLU not polling?), reporting current Values only");
        }
        panel->change_queue_overflow = true;
        return;
    }
    panel->change_queue_overflow = false;
    panel->change_queue[panel->change_queue_length++] = (uint8_t)(button << 1 | value);
}

static int pop_change(panel_t *panel)
{
    if (!panel->change_queue_length) {
        return -1;
    }
    int change = panel->change_queue[0];
    memmove(panel->change_queue, panel->change_queue + 1, (size_t)--panel->change_queue_length);
    return change;
}

// ---------------------------------------------------------------------------------------------
// Public state helpers
// ---------------------------------------------------------------------------------------------
void panel_set_temp(panel_t *panel, float celsius)
{
    float centi = celsius * 100.0f;
    panel->temperature_centi = (int16_t)(centi < -32768 ? -32768 : centi > 32767 ? 32767 : centi);
    panel->has_temperature = true;
}

int panel_slot_button(const panel_t *panel, int slot)
{
    // Default page layout (page p, slot s -> button (p-1)*4 + s). Pages can assign other buttons
    // (PANEL_PAGE Object_N_Id); that config upload (TFBUS_CMD 0x12) is not decoded yet.
    return (panel->page - 1) * PANEL_SLOTS + slot;
}

const char *panel_slot_icon(const panel_t *panel, int slot)
{
    int button = panel_slot_button(panel, slot);
    bool on = (panel->button_values >> button) & 1;
    if (panel->button_mode[button] == BUTTON_MODE_BISTABLE && on && panel->icon_b[button][0]) {
        return panel->icon_b[button];
    }
    return panel->icon_a[button];
}

led_state_t panel_slot_led(const panel_t *panel, int slot, uint64_t now_us)
{
    int button = panel_slot_button(panel, slot);
    led_state_t state = panel->led[button];
    if (state == LEDS_FLASH_GREEN || state == LEDS_FLASH_RED) {
        bool lit = ((panel->led_flash_until_us[button] - now_us) / LED_FLASH_PHASE_US) % 2 == 0;
        return lit ? (state == LEDS_FLASH_GREEN ? LEDS_GREEN : LEDS_RED) : LEDS_OFF;
    }
    // The real CLU sends no LED_CMD when a bistable button toggles (capture 2026-11-25): the panel
    // shows the ON state itself, unless the CLU set the LED.
    if (state == LEDS_OFF && panel->button_mode[button] == BUTTON_MODE_BISTABLE &&
        (panel->button_values & (1u << button))) {
        return LEDS_GREEN;
    }
    return state;
}

bool panel_take_beep(panel_t *panel, uint16_t *frequency_hz, uint16_t *duration_ms, uint16_t *volume)
{
    if (!panel->beep) {
        return false;
    }
    panel->beep = false;
    *frequency_hz = panel->beep_frequency_hz;
    *duration_ms = panel->beep_duration_ms;
    *volume = panel->beep_volume;
    return true;
}

static void request_beep(panel_t *panel, uint16_t frequency_hz, uint16_t duration_ms, uint16_t volume)
{
    if (!panel->buzzer_on) {
        return;
    }
    panel->beep = true;
    panel->beep_frequency_hz = frequency_hz;
    panel->beep_duration_ms = duration_ms;
    panel->beep_volume = volume;
}

// ---------------------------------------------------------------------------------------------
// OLED drawing (1 bpp, row-major, MSB = leftmost pixel)
// ---------------------------------------------------------------------------------------------
// "Buttons" display mode: the OLED stays blank. Labels and icons (IconA / IconB) are shown on the
// buttons themselves by the board (panel_slot_icon).
static void render_buttons(panel_t *panel)
{
    memset(panel->oled_shown, 0, sizeof panel->oled_shown);
    panel->oled_mode = OLED_BUTTONS;
    panel->version++;
}

bool panel_oled_pixel(const panel_t *panel, int x, int y)
{
    return (panel->oled_shown[y * (OLED_W / 8) + x / 8] >> (7 - (x & 7))) & 1;
}

size_t panel_to_ascii(char *dst, size_t cap, const char *src)
{
    // Polish letters (U+0104..U+017C) by code point
    static const struct {
        uint16_t code_point;
        char ascii;
    } polish_letters[] = {
        {0x104, 'A'}, {0x105, 'a'}, {0x106, 'C'}, {0x107, 'c'}, {0x118, 'E'}, {0x119, 'e'},
        {0x141, 'L'}, {0x142, 'l'}, {0x143, 'N'}, {0x144, 'n'}, {0xd3, 'O'}, {0xf3, 'o'},
        {0x15a, 'S'}, {0x15b, 's'}, {0x179, 'Z'}, {0x17a, 'z'}, {0x17b, 'Z'}, {0x17c, 'z'},
    };
    size_t out_length = 0;
    const unsigned char *input = (const unsigned char *)src;
    while (*input && out_length + 1 < cap) {
        uint32_t code_point;
        if (*input < 0x80) {
            code_point = *input++;
        } else if ((*input & 0xe0) == 0xc0 && (input[1] & 0xc0) == 0x80) {
            code_point = (uint32_t)(*input & 0x1f) << 6 | (input[1] & 0x3f);
            input += 2;
        } else if ((*input & 0xf0) == 0xe0 && (input[1] & 0xc0) == 0x80 && (input[2] & 0xc0) == 0x80) {
            code_point = (uint32_t)(*input & 0x0f) << 12 | (uint32_t)(input[1] & 0x3f) << 6 | (input[2] & 0x3f);
            input += 3;
        } else {
            code_point = *input++; // not UTF-8: single Latin-1 byte
        }
        char replacement = '?';
        if (code_point >= 0x20 && code_point < 0x7f) {
            replacement = (char)code_point;
        } else if (code_point == 0xb0 || code_point == 0x7f) {
            replacement = 0x7f;
        } else {
            for (size_t i = 0; i < sizeof polish_letters / sizeof polish_letters[0]; i++) {
                if (polish_letters[i].code_point == code_point) {
                    replacement = polish_letters[i].ascii;
                }
            }
        }
        dst[out_length++] = replacement;
    }
    dst[out_length] = 0;
    return out_length;
}

void panel_oled_text(uint8_t *buffer, int x, int y, int size, const char *text)
{
    char ascii[64];
    panel_to_ascii(ascii, sizeof ascii, text);
    if (size < 1) {
        size = 1;
    } else if (size > 3) {
        size = 3;
    }
    for (const char *cursor = ascii; *cursor; cursor++) {
        unsigned char character = (unsigned char)*cursor;
        if (character < 0x20 || character > 0x7f) {
            character = '?';
        }
        const uint8_t *glyph = font5x7[character - 0x20];
        for (int column = 0; column < 6; column++) {
            uint8_t bits = column < 5 ? glyph[column] : 0;
            for (int row = 0; row < 8; row++) {
                bool pixel_on = (bits >> row) & 1;
                for (int repeat_y = 0; repeat_y < size; repeat_y++) {
                    for (int repeat_x = 0; repeat_x < size; repeat_x++) {
                        set_oled_pixel(buffer, x + column * size + repeat_x, y + row * size + repeat_y, pixel_on);
                    }
                }
            }
        }
        x += 6 * size;
    }
}

static int oled_text_width(const char *text, int size)
{
    return (int)strlen(text) * 6 * size;
}

static void set_oled_pixel(uint8_t *buffer, int x, int y, bool pixel_on)
{
    if (x < 0 || y < 0 || x >= OLED_W || y >= OLED_H) {
        return;
    }
    uint8_t mask = (uint8_t)(0x80 >> (x & 7));
    uint8_t *target = &buffer[y * (OLED_W / 8) + x / 8];
    *target = pixel_on ? (*target | mask) : (*target & ~mask);
}

// ---------------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------------
static void logf_(panel_t *panel, const char *fmt, ...)
{
    if (!panel->log) {
        return;
    }
    char message[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    panel->log(message, panel->context);
}
