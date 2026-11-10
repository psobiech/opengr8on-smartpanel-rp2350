// SMART_PANEL_FM_4 emulation core: TFBUS module side (RS485 framing, addressing, status replies,
// panel-generated button events), LED and 128x64 "OLED" state. Portable C, no SDK dependencies:
// the board layer feeds bytes/touches/time in and renders the state out. Not thread-safe: call
// all functions from one context (or under one lock).
//
// Wire behaviour follows a real panel on a real CLU (docs/TFBUS_SMART_PANEL_FM_4.md and
// "clu firmware/tfbus_capture_20261014_*.log"):
//   - after RESET: unaddressed; SEEK -> ACK; ADDR_ASSIGN(own SN) -> ACK, take address
//   - STATUS_POLL -> TLV status, always a data frame:  01 | 06 01 01 00   (+ temp / button event)
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
#define PANEL_TEMP_PERIOD_US 1250000 // real panel reports temperature every ~1.25 s
#define PANEL_EVQ          24

// Button events (VAL = btn * 7 + event)
enum {
    PEV_CHANGE = 0, PEV_ON = 1, PEV_OFF = 2, PEV_SHORT = 3, PEV_LONG = 4, PEV_HOLD = 5, PEV_CLICK = 6,
};

// LED_CMD (frame 0x18) commands
enum { LED_SHOW_OK = 0x00, LED_SHOW_ERROR = 0x01, LED_GREEN = 0x02, LED_OFF = 0x04, LED_RED = 0x10 };

typedef enum { LEDS_OFF = 0, LEDS_GREEN, LEDS_RED, LEDS_FLASH_GREEN, LEDS_FLASH_RED } led_state_t;

typedef enum { OLED_BUTTONS = 0, OLED_FREEDRAW, OLED_TIMEOUT } oled_mode_t;

typedef struct {
    uint32_t sn;
    uint8_t addr;                 // 0 = not addressed
    // rx framing
    uint8_t rx[40];
    size_t rxlen;
    uint64_t rx_last_us;
    // stats
    uint32_t polls, frames_rx, frames_bad, frames_unknown, discovered;
    uint64_t last_poll_us;
    // status / events
    uint8_t evq[PANEL_EVQ];
    int evq_n;
    bool evq_overflow;
    int16_t temp_centi;           // 0.01 C
    bool have_temp;
    uint64_t next_temp_us;
    // buttons
    bool slot_down[PANEL_SLOTS];
    uint8_t slot_btn[PANEL_SLOTS];// button index held by each pressed slot
    uint64_t slot_t0[PANEL_SLOTS], slot_next_hold[PANEL_SLOTS];
    uint16_t hold_delay_ms[PANEL_BUTTONS], hold_interval_ms[PANEL_BUTTONS];
    char label[PANEL_BUTTONS][17];
    char icon_a[PANEL_BUTTONS][17];
    // LEDs
    led_state_t led[PANEL_BUTTONS];
    uint64_t led_flash_until[PANEL_BUTTONS];
    led_state_t led_after[PANEL_BUTTONS];
    // panel settings (TFBUS_CMD 0x31 feature commands; see panel_core.c for the mapping)
    uint8_t page;                 // 1..4
    bool buzzer_on;
    uint8_t buttons_led_mode;     // 0 off, 1 location LEDs on, 2 for active
    uint8_t page_control_mode;    // 0 command, 1 gesture/command
    uint16_t prox_timeout_ms;     // stored only: no proximity sensor, the display stays on
    // OLED: draw buffer (commands) and shown buffer (after DRAW / DisplayContent), 1 bpp, row-major
    uint8_t oled_draw[OLED_W * OLED_H / 8];
    uint8_t oled_shown[OLED_W * OLED_H / 8];
    oled_mode_t oled_mode;
    bool draw_pending;
    uint64_t draw_deadline_us;
    uint32_t version;             // bumped on any visible change (LED/OLED/page)
    // beep request for the board (consumed by panel_take_beep)
    bool beep;
    uint16_t beep_freq, beep_ms, beep_vol;
    // board callbacks
    void (*tx)(const uint8_t *frame, size_t len, void *ctx);
    void (*log)(const char *msg, void *ctx);
    void *ctx;
} panel_t;

void panel_init(panel_t *p, uint32_t sn);

// Feed received RS485 bytes (timestamp of arrival of the chunk).
void panel_rx(panel_t *p, const uint8_t *data, size_t len, uint64_t now_us);
// Call periodically (>= every 1 ms): ends frames on line idle, hold events, LED flash, timeouts.
void panel_tick(panel_t *p, uint64_t now_us);

// Physical touch field (0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right).
void panel_touch(panel_t *p, int slot, bool down, uint64_t now_us);
// Release a pressed field without OnClick / OnShortPress / OnLongPress (the touch turned into a
// swipe): only OnSwitchOff + OnChange, so the CLU's switch state stays consistent.
void panel_touch_cancel(panel_t *p, int slot, uint64_t now_us);
// Swipe on the display: dir -1 previous page, +1 next page (local only; wire format of gesture
// events is not decoded, see README).
void panel_swipe(panel_t *p, int dir, uint64_t now_us);

void panel_set_temp(panel_t *p, float celsius);

// Button index shown on a slot of the current page.
int panel_slot_button(const panel_t *p, int slot);
// Effective LED color for a slot right now (flash phases resolved).
led_state_t panel_slot_led(const panel_t *p, int slot, uint64_t now_us);
bool panel_oled_pixel(const panel_t *p, int x, int y);

bool panel_take_beep(panel_t *p, uint16_t *freq, uint16_t *ms, uint16_t *vol);

// UTF-8 (or Latin-1 degree byte) -> ASCII for the panel fonts: Polish letters lose their
// diacritics, the degree sign becomes 0x7f, anything else '?'. Returns the output length.
size_t panel_to_ascii(char *dst, size_t cap, const char *src);

// Exposed for tests: draw text into a 1-bpp OLED buffer (font sizes 1..3).
void panel_oled_text(uint8_t *buf, int x, int y, int size, const char *txt);

#ifdef __cplusplus
}
#endif
