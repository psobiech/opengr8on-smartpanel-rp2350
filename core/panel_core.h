// SMART_PANEL_FM_4 emulation core: TFBUS module side (RS485 framing, addressing, status replies,
// panel-generated button events), LED and 128x64 "OLED" state. Portable C, no SDK dependencies:
// the board layer feeds bytes/touches/time in and renders the state out. Not thread-safe: call
// all functions from one context (or under one lock).
//
// Wire behaviour follows a real panel on a real CLU (docs/TFBUS_SMART_PANEL_FM_4.md and
// "clu firmware/tfbus_capture_20261014_*.log"):
//   - after RESET: unaddressed; SEEK -> ACK; ADDR_ASSIGN(own SN) -> ACK, take address
//   - DISCOVER while unaddressed -> SN hwType fwType fwApiVer, after a per-SN/per-attempt slot
//     delay (a real module answered ~576 ms late) and only on an idle bus; dropped if the master
//     sends a frame first
//   - STATUS_POLL -> bare ACK, or the 14-byte state frame (button Value bitmask + temperature) on
//     a change; the CLU derives OnSwitchOn / OnClick / OnHold ... from the Value bits itself
//   - any other unicast frame -> ACK (AA 00 01 AB); VERSION_QUERY -> no reply (like the real panel)
//   - broadcasts other than SEEK / own ADDR_ASSIGN -> no reply
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_HW_TYPE      0x03
#define PANEL_FW_TYPE      0x03
#define PANEL_FW_API_VER   0x04
#define PANEL_SN_PREFIX    0x0f050000u

#define PANEL_BUTTONS      16 // PANEL_BUTTON objects (4 per page)
#define PANEL_SLOTS        4  // physical touch fields
#define PANEL_PAGES        4
#define OLED_W             128
#define OLED_H             64

// Idle gap that ends an incomplete/unknown frame. Measured between RX IRQs, not bytes: the SDK sets
// the RX FIFO IRQ threshold to 4 bytes, so a 7-byte frame arrives as 4 bytes + 3 bytes ~540 us later
// (3 bytes + 32-bit RX timeout). 400 us dropped every frame > 4 bytes; known-length frames complete
// as soon as their last byte arrives, so a long gap only delays flushing garbage.
#define PANEL_GAP_US       1500
#define PANEL_CHANGE_QUEUE_SIZE 24

// PANEL_BUTTON feature 1 Mode, from the class frame 0x17 [mask16 LE, mode] (byte 3 = Mode is
// inferred: every captured button was monostable, i.e. 00)
enum { BUTTON_MODE_MONOSTABLE = 0, BUTTON_MODE_BISTABLE = 1, BUTTON_MODE_LOCKED = 2 };

// LED_CMD (frame 0x18) commands
enum { LED_SHOW_OK = 0x00, LED_SHOW_ERROR = 0x01, LED_GREEN = 0x02, LED_OFF = 0x04, LED_RED = 0x10 };

typedef enum { LEDS_OFF = 0, LEDS_GREEN, LEDS_RED, LEDS_FLASH_GREEN, LEDS_FLASH_RED } led_state_t;

typedef enum { OLED_BUTTONS = 0, OLED_FREEDRAW, OLED_TIMEOUT } oled_mode_t;

typedef struct {
    uint32_t serial_number;
    uint8_t address;              // 0 = not addressed
    // rx framing
    uint8_t rx_buffer[40];
    size_t rx_length;
    uint64_t rx_last_us;
    // stats
    uint32_t polls, frames_rx, frames_bad, frames_unknown, discovered;
    uint64_t last_poll_us;
    // delayed DISCOVER reply (collision avoidance between unaddressed modules)
    bool discover_reply_pending;
    uint64_t discover_reply_due_us;
    uint32_t discover_requests;   // DISCOVERs seen while unaddressed: varies the reply slot
    // status: button Value changes not yet reported (button << 1 | value), one per poll, so a
    // tap shorter than the poll interval still reaches the CLU as 1 then 0
    uint8_t change_queue[PANEL_CHANGE_QUEUE_SIZE];
    int change_queue_length;
    bool change_queue_overflow;
    uint16_t reported_button_values; // Value bits as last sent to the CLU
    int16_t temperature_centi;    // 0.01 C
    bool has_temperature;
    bool state_frame_pending;     // send the state frame on the next poll even without a change
    int16_t reported_temperature_deci; // 0.1 C, last value sent in a state frame
    // buttons
    bool slot_down[PANEL_SLOTS];
    uint8_t slot_button[PANEL_SLOTS]; // button index held by each pressed slot
    uint8_t button_mode[PANEL_BUTTONS]; // BUTTON_MODE_*
    uint16_t button_values;       // PANEL_BUTTON Value, bit per button
    char label[PANEL_BUTTONS][17];
    char icon_a[PANEL_BUTTONS][17];
    // LEDs
    led_state_t led[PANEL_BUTTONS];
    uint64_t led_flash_until_us[PANEL_BUTTONS];
    led_state_t led_after[PANEL_BUTTONS];
    // panel settings (TFBUS_CMD 0x31 feature commands; see panel_core.c for the mapping)
    uint8_t page;                 // 1..4
    bool buzzer_on;
    uint8_t buttons_led_mode;     // 0 off, 1 location LEDs on, 2 for active
    uint8_t page_control_mode;    // 0 command, 1 gesture/command
    uint16_t proximity_timeout_ms; // stored only: no proximity sensor, the display stays on
    // OLED: draw buffer (commands) and shown buffer (after DRAW / DisplayContent), 1 bpp, row-major
    uint8_t oled_draw[OLED_W * OLED_H / 8];
    uint8_t oled_shown[OLED_W * OLED_H / 8];
    oled_mode_t oled_mode;
    bool draw_pending;
    uint64_t draw_deadline_us;
    uint32_t version;             // bumped on any visible change (LED/OLED/page)
    // beep request for the board (consumed by panel_take_beep)
    bool beep;
    uint16_t beep_frequency_hz, beep_duration_ms, beep_volume;
    // board callbacks
    void (*transmit)(const uint8_t *frame, size_t frame_length, void *context);
    void (*log)(const char *message, void *context);
    void *context;
} panel_t;

void panel_init(panel_t *panel, uint32_t serial_number);

// Feed received RS485 bytes (timestamp of arrival of the chunk).
void panel_rx(panel_t *panel, const uint8_t *data, size_t data_length, uint64_t now_us);
// Call periodically (>= every 1 ms): ends frames on line idle, hold events, LED flash, timeouts.
void panel_tick(panel_t *panel, uint64_t now_us);

// Physical touch field (0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right).
void panel_touch(panel_t *panel, int slot, bool down, uint64_t now_us);
// Release a pressed field because the touch turned into a swipe. The CLU derives its events from
// Value, so for it this is an ordinary release.
void panel_touch_cancel(panel_t *panel, int slot, uint64_t now_us);
// Swipe on the display: direction -1 previous page, +1 next page (local only; wire format of
// gesture events is not decoded, see README).
void panel_swipe(panel_t *panel, int direction, uint64_t now_us);

void panel_set_temp(panel_t *panel, float celsius);

// Button index shown on a slot of the current page.
int panel_slot_button(const panel_t *panel, int slot);
// Effective LED color for a slot right now (flash phases resolved).
led_state_t panel_slot_led(const panel_t *panel, int slot, uint64_t now_us);
bool panel_oled_pixel(const panel_t *panel, int x, int y);

bool panel_take_beep(panel_t *panel, uint16_t *frequency_hz, uint16_t *duration_ms, uint16_t *volume);

// UTF-8 (or Latin-1 degree byte) -> ASCII for the panel fonts: Polish letters lose their
// diacritics, the degree sign becomes 0x7f, anything else '?'. Returns the output length.
size_t panel_to_ascii(char *dst, size_t cap, const char *src);

// Exposed for tests: draw text into a 1-bpp OLED buffer (font sizes 1..3).
void panel_oled_text(uint8_t *buffer, int x, int y, int size, const char *text);

#ifdef __cplusplus
}
#endif
