#include "panel_core.h"

#include <stdio.h>
#include <string.h>

#include "font5x7.h"

#define ACK_LEN            4
#define LED_FLASH_US       2000000 // ShowOK / ShowError: flash for 2 s ...
#define LED_FLASH_PHASE_US 250000  // ... with a 500 ms period
#define DRAW_TIMEOUT_US    1000000 // draw ops without DisplayContent -> "! TIMEOUT !" (window guessed)

static void logf_(panel_t *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static void logf_(panel_t *p, const char *fmt, ...)
{
    if (!p->log) {
        return;
    }
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    p->log(b, p->ctx);
}

static uint8_t xor8(const uint8_t *b, size_t n)
{
    uint8_t x = 0;
    while (n--) {
        x ^= *b++;
    }
    return x;
}

static void send(panel_t *p, const uint8_t *f, size_t n)
{
    if (p->tx) {
        p->tx(f, n, p->ctx);
    }
}

static void ack(panel_t *p)
{
    static const uint8_t a[ACK_LEN] = {0xaa, 0x00, 0x01, 0xab};
    send(p, a, sizeof a);
}

// ---------------------------------------------------------------------------------------------
// OLED drawing (1 bpp, row-major, MSB = leftmost pixel)
// ---------------------------------------------------------------------------------------------
static void px(uint8_t *buf, int x, int y, bool on)
{
    if (x < 0 || y < 0 || x >= OLED_W || y >= OLED_H) {
        return;
    }
    uint8_t m = (uint8_t)(0x80 >> (x & 7));
    uint8_t *b = &buf[y * (OLED_W / 8) + x / 8];
    *b = on ? (*b | m) : (*b & ~m);
}

size_t panel_to_ascii(char *dst, size_t cap, const char *src)
{
    // Polish letters (U+0104..U+017C) by code point
    static const struct { uint16_t cp; char c; } pl[] = {
        {0x104, 'A'}, {0x105, 'a'}, {0x106, 'C'}, {0x107, 'c'}, {0x118, 'E'}, {0x119, 'e'},
        {0x141, 'L'}, {0x142, 'l'}, {0x143, 'N'}, {0x144, 'n'}, {0xd3, 'O'}, {0xf3, 'o'},
        {0x15a, 'S'}, {0x15b, 's'}, {0x179, 'Z'}, {0x17a, 'z'}, {0x17b, 'Z'}, {0x17c, 'z'},
    };
    size_t o = 0;
    const unsigned char *s = (const unsigned char *)src;
    while (*s && o + 1 < cap) {
        uint32_t cp;
        if (*s < 0x80) {
            cp = *s++;
        } else if ((*s & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
            cp = (uint32_t)(*s & 0x1f) << 6 | (s[1] & 0x3f);
            s += 2;
        } else if ((*s & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
            cp = (uint32_t)(*s & 0x0f) << 12 | (uint32_t)(s[1] & 0x3f) << 6 | (s[2] & 0x3f);
            s += 3;
        } else {
            cp = *s++; // not UTF-8: single Latin-1 byte
        }
        char c = '?';
        if (cp >= 0x20 && cp < 0x7f) {
            c = (char)cp;
        } else if (cp == 0xb0 || cp == 0x7f) {
            c = 0x7f;
        } else {
            for (size_t i = 0; i < sizeof pl / sizeof pl[0]; i++) {
                if (pl[i].cp == cp) {
                    c = pl[i].c;
                }
            }
        }
        dst[o++] = c;
    }
    dst[o] = 0;
    return o;
}

void panel_oled_text(uint8_t *buf, int x, int y, int size, const char *utf8)
{
    char ascii[64];
    panel_to_ascii(ascii, sizeof ascii, utf8);
    const char *txt = ascii;
    if (size < 1) {
        size = 1;
    } else if (size > 3) {
        size = 3;
    }
    for (; *txt; txt++) {
        unsigned char c = (unsigned char)*txt;
        if (c < 0x20 || c > 0x7f) {
            c = '?';
        }
        const uint8_t *g = font5x7[c - 0x20];
        for (int col = 0; col < 6; col++) {
            uint8_t bits = col < 5 ? g[col] : 0;
            for (int row = 0; row < 8; row++) {
                bool on = (bits >> row) & 1;
                for (int dy = 0; dy < size; dy++) {
                    for (int dx = 0; dx < size; dx++) {
                        px(buf, x + col * size + dx, y + row * size + dy, on);
                    }
                }
            }
        }
        x += 6 * size;
    }
}

static int text_w(const char *s, int size) { return (int)strlen(s) * 6 * size; }

bool panel_oled_pixel(const panel_t *p, int x, int y)
{
    return (p->oled_shown[y * (OLED_W / 8) + x / 8] >> (7 - (x & 7))) & 1;
}

// "Buttons" display mode: configured label / icon name of the current page's buttons.
// Unconfigured buttons draw nothing, so the screen stays clear by default.
static void render_buttons(panel_t *p)
{
    memset(p->oled_shown, 0, sizeof p->oled_shown);
    for (int s = 0; s < PANEL_SLOTS; s++) {
        int b = panel_slot_button(p, s);
        char t[17];
        snprintf(t, sizeof t, "%s", p->label[b][0] ? p->label[b] : p->icon_a[b]);
        if (!t[0]) {
            continue;
        }
        t[10] = 0; // 10 chars fit a 64-px cell
        int cx = (s & 1) ? 96 : 32, cy = (s & 2) ? 44 : 12;
        panel_oled_text(p->oled_shown, cx - text_w(t, 1) / 2, cy, 1, t);
    }
    p->oled_mode = OLED_BUTTONS;
    p->version++;
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------
static bool evq_has(const panel_t *p, uint8_t val)
{
    for (int i = 0; i < p->evq_n; i++) {
        if (p->evq[i] == val) {
            return true;
        }
    }
    return false;
}

static void push_event(panel_t *p, int btn, int ev)
{
    if (!p->addr || !p->last_poll_us) {
        return; // no CLU polling us (unaddressed, or addressed but not in OM.LUA): nobody to tell
    }
    if (p->evq_n >= PANEL_EVQ) {
        if (!p->evq_overflow) {
            logf_(p, "event queue full (CLU not polling?), dropping button events");
        }
        p->evq_overflow = true;
        return;
    }
    p->evq_overflow = false;
    p->evq[p->evq_n++] = (uint8_t)(btn * 7 + ev);
}

static int pop_event(panel_t *p)
{
    if (!p->evq_n) {
        return -1;
    }
    int v = p->evq[0];
    memmove(p->evq, p->evq + 1, (size_t)--p->evq_n);
    return v;
}

// ---------------------------------------------------------------------------------------------
// Public state helpers
// ---------------------------------------------------------------------------------------------
int panel_slot_button(const panel_t *p, int slot)
{
    // Default page layout (page p, slot s -> button (p-1)*4 + s). Pages can assign other buttons
    // (PANEL_PAGE Object_N_Id); that config upload (TFBUS_CMD 0x12) is not decoded yet.
    return (p->page - 1) * PANEL_SLOTS + slot;
}

led_state_t panel_slot_led(const panel_t *p, int slot, uint64_t now_us)
{
    int b = panel_slot_button(p, slot);
    led_state_t s = p->led[b];
    if (s == LEDS_FLASH_GREEN || s == LEDS_FLASH_RED) {
        bool on = ((p->led_flash_until[b] - now_us) / LED_FLASH_PHASE_US) % 2 == 0;
        return on ? (s == LEDS_FLASH_GREEN ? LEDS_GREEN : LEDS_RED) : LEDS_OFF;
    }
    return s;
}

bool panel_take_beep(panel_t *p, uint16_t *freq, uint16_t *ms, uint16_t *vol)
{
    if (!p->beep) {
        return false;
    }
    p->beep = false;
    *freq = p->beep_freq;
    *ms = p->beep_ms;
    *vol = p->beep_vol;
    return true;
}

static void request_beep(panel_t *p, uint16_t f, uint16_t ms, uint16_t vol)
{
    if (!p->buzzer_on) {
        return;
    }
    p->beep = true;
    p->beep_freq = f;
    p->beep_ms = ms;
    p->beep_vol = vol;
}

void panel_set_temp(panel_t *p, float c)
{
    float v = c * 100.0f;
    p->temp_centi = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    p->have_temp = true;
}

// ---------------------------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------------------------
void panel_init(panel_t *p, uint32_t sn)
{
    void (*tx)(const uint8_t *, size_t, void *) = p->tx;
    void (*log)(const char *, void *) = p->log;
    void *ctx = p->ctx;
    memset(p, 0, sizeof *p);
    p->tx = tx;
    p->log = log;
    p->ctx = ctx;
    p->sn = sn;
    p->page = 1;
    p->buzzer_on = true;
    p->buttons_led_mode = 1;
    p->page_control_mode = 1;
    p->prox_timeout_ms = 5000;
    for (int b = 0; b < PANEL_BUTTONS; b++) {
        p->hold_delay_ms[b] = 1000;
        p->hold_interval_ms[b] = 100;
    }
    render_buttons(p);
}

// ---------------------------------------------------------------------------------------------
// Frame handling
// ---------------------------------------------------------------------------------------------
static void send_status(panel_t *p, uint64_t now_us)
{
    uint8_t f[32] = {0xaa, 0x00};
    size_t o = 4; // f[2] = len, f[3] = count
    uint8_t n = 0;
    f[o++] = 0x06; f[o++] = 0x01; f[o++] = 0x01; f[o++] = 0x00; n++; // baseline, always present
    if (p->have_temp && now_us >= p->next_temp_us) {
        f[o++] = 0x02; f[o++] = 0x02; f[o++] = 0x10;
        f[o++] = (uint8_t)p->temp_centi; f[o++] = (uint8_t)((uint16_t)p->temp_centi >> 8);
        n++;
        p->next_temp_us = now_us + PANEL_TEMP_PERIOD_US;
    }
    int ev = pop_event(p); // one button event per poll, like the real panel
    if (ev >= 0) {
        f[o++] = 0x08; f[o++] = 0x02; f[o++] = 0x10; f[o++] = (uint8_t)ev; f[o++] = 0x00;
        n++;
    }
    f[3] = n;
    f[2] = (uint8_t)(o - 3 + 1); // payload length + 1
    f[o] = xor8(f, o);
    send(p, f, o + 1);
}

static void apply_led(panel_t *p, uint16_t mask, uint8_t cmd, uint64_t now_us)
{
    for (int b = 0; b < PANEL_BUTTONS; b++) {
        if (!(mask & (1u << b))) {
            continue;
        }
        led_state_t steady = (p->led[b] == LEDS_FLASH_GREEN || p->led[b] == LEDS_FLASH_RED)
                                 ? p->led_after[b] : p->led[b];
        switch (cmd) {
        case LED_GREEN: p->led[b] = LEDS_GREEN; break;
        case LED_RED: p->led[b] = LEDS_RED; break;
        case LED_OFF: p->led[b] = LEDS_OFF; break;
        case LED_SHOW_OK:
        case LED_SHOW_ERROR:
            p->led_after[b] = steady;
            p->led[b] = cmd == LED_SHOW_OK ? LEDS_FLASH_GREEN : LEDS_FLASH_RED;
            p->led_flash_until[b] = now_us + LED_FLASH_US;
            break;
        default:
            logf_(p, "LED_CMD unknown cmd 0x%02x mask 0x%04x", cmd, mask);
            return;
        }
    }
    p->version++;
}

static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t i = 0;
    for (; i < n && i < cap - 1 && src[i]; i++) {
        dst[i] = (char)src[i];
    }
    dst[i] = 0;
}

// DISPLAY_BLOCK (0x30): chan sub data[20]. Button config: chan = button, sub 0x00 / 0x01 / 0x0a
// (meaning per sub guessed: IconA / IconB / Label). chan 0 sub 2..5 = gesture icons left/right/up/down.
static void display_block(panel_t *p, const uint8_t *d)
{
    uint8_t ch = d[0], sub = d[1];
    const uint8_t *data = d + 2;
    if (ch < PANEL_BUTTONS && (sub == 0x00 || sub == 0x0a)) {
        char *dst = sub == 0x0a ? p->label[ch] : p->icon_a[ch];
        char old[17];
        memcpy(old, dst, sizeof old);
        copy_str(dst, 17, data, 16);
        if (strcmp(old, dst) != 0 && p->oled_mode == OLED_BUTTONS) {
            render_buttons(p);
        }
    }
}

static void tfbus_cmd(panel_t *p, const uint8_t *d, uint64_t now_us)
{
    uint8_t cmd = d[0];
    const uint8_t *a = d + 1;
    switch (cmd) {
    case 0x0a: { // PRINT_TEXT x y font text[16]
        char t[17];
        copy_str(t, sizeof t, a + 3, 16);
        panel_oled_text(p->oled_draw, a[0], a[1], a[2], t);
        if (!p->draw_pending) {
            p->draw_deadline_us = now_us + DRAW_TIMEOUT_US;
        }
        p->draw_pending = true;
        break;
    }
    case 0x19: // DRAW / DisplayContent: show the draw buffer
        memcpy(p->oled_shown, p->oled_draw, sizeof p->oled_shown);
        memset(p->oled_draw, 0, sizeof p->oled_draw);
        p->draw_pending = false;
        p->oled_mode = OLED_FREEDRAW;
        p->version++;
        break;
    case 0x0f: // BUZZER_BEEP freq dur vol (u16 LE)
        request_beep(p, (uint16_t)(a[0] | a[1] << 8), (uint16_t)(a[2] | a[3] << 8), (uint16_t)(a[4] | a[5] << 8));
        break;
    // Panel settings sent by the CLU after init (values from a real capture match the PANEL
    // feature defaults; command -> feature mapping inferred from that order).
    case 0x02: p->prox_timeout_ms = (uint16_t)(a[0] | a[1] << 8); break; // ProximityTimeout
    case 0x0c: p->buzzer_on = a[0] != 0; break;                          // BuzzerValue
    case 0x14: p->buttons_led_mode = a[0]; p->version++; break;          // ButtonsLEDMode
    case 0x15: p->page_control_mode = a[0]; break;                       // PageControlMode
    case 0x01: case 0x0d: case 0x0e: case 0x10: case 0x11: case 0x16:   // sens / gesture / page modes
    case 0x12: case 0x1a:                                                // page / button config (undecoded)
        break;
    default:
        p->frames_unknown++;
        logf_(p, "TFBUS_CMD 0x%02x %02x %02x %02x %02x not handled", cmd, a[0], a[1], a[2], a[3]);
        break;
    }
}

static void handle_frame(panel_t *p, const uint8_t *f, size_t n, uint64_t now_us)
{
    uint8_t dest = f[1], type = f[2];
    p->frames_rx++;
    if (dest == 0x00) {
        return; // another module's reply (or our own echo)
    }
    bool bcast = dest == 0xff;
    if (!bcast && (p->addr == 0 || dest != p->addr)) {
        return;
    }
    if (bcast) {
        switch (type) {
        case 0x07: // RESET
            if (p->addr) {
                logf_(p, "RESET: address %u released", p->addr);
            }
            p->addr = 0;
            p->evq_n = 0;
            p->last_poll_us = 0;
            break;
        case 0x2a: // SEEK: unaddressed modules ACK
            if (!p->addr) {
                ack(p);
            }
            break;
        case 0x08: // DISCOVER: an unaddressed module reports SN(LE32) hwType fwType fwApiVer
            if (!p->addr) {
                uint8_t r[11] = {0xaa, 0x00, 0x08, (uint8_t)p->sn, (uint8_t)(p->sn >> 8),
                                 (uint8_t)(p->sn >> 16), (uint8_t)(p->sn >> 24), PANEL_HW_TYPE,
                                 PANEL_FW_TYPE, PANEL_FW_API_VER, 0};
                r[10] = xor8(r, 10);
                p->discovered++;
                logf_(p, "DISCOVER: reported SN %08lx", (unsigned long)p->sn);
                send(p, r, sizeof r);
            }
            break;
        case 0x02: { // ADDR_ASSIGN sn(LE32) addr
            uint32_t sn = f[3] | f[4] << 8 | f[5] << 16 | (uint32_t)f[6] << 24;
            if (sn == p->sn) {
                p->addr = f[7];
                p->evq_n = 0;
                p->last_poll_us = 0;
                p->next_temp_us = now_us;
                logf_(p, "ADDR_ASSIGN: address %u", p->addr);
                ack(p);
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
        p->polls++;
        p->last_poll_us = now_us;
        send_status(p, now_us);
        return;
    case 0xf9: // VERSION_QUERY: the real SMART_PANEL_FM_4 does not answer
        return;
    case 0x07: // unicast RESET
        p->addr = 0;
        p->last_poll_us = 0;
        return;
    case 0x18: // LED_CMD mask_lo mask_hi cmd
        apply_led(p, (uint16_t)(f[3] | f[4] << 8), f[5], now_us);
        break;
    case 0x31:
        tfbus_cmd(p, f + 3, now_us);
        break;
    case 0x30:
        display_block(p, f + 3);
        break;
    case 0x16: case 0x17: case 0xf0: case 0x13: case 0x14: case 0x15: case 0x1d: case 0x1e:
        break; // sensor / button / class config: accepted, not used
    default:
        p->frames_unknown++;
        logf_(p, "frame type 0x%02x (%u bytes) not handled", type, (unsigned)n);
        break;
    }
    ack(p);
}

// Frame length for CLU -> module frames by type (0 = need more bytes, -1 = unknown: ends on idle).
// Lengths of the class frames 0x13..0x1e follow docs/TFBUS.md (old-path payload sizes) and match
// the real CLU capture for 0x16 / 0x17 / 0x18.
static int frame_len(const uint8_t *b, size_t n)
{
    if (b[1] == 0x00) {
        int l = 3 + b[2];
        return l < 4 ? 4 : l;
    }
    switch (b[2]) {
    case 0x02: return 9;
    case 0x04: case 0x07: case 0x0a: case 0x0b: case 0x1e: case 0x2a: case 0xf9: return 4;
    case 0x08: return b[1] == 0xff ? 4 : -1; // broadcast DISCOVER
    case 0x09: return 5;
    case 0x13: return 9;
    case 0x14: case 0x15: case 0x17: case 0x18: case 0x1d: case 0xfd: case 0xfe: return 7;
    case 0x16: return 8;
    case 0x30: case 0x31: return 26;
    case 0xfb: return 25;
    case 0xf0: return n >= 4 ? 4 + b[3] : 0;
    default: return -1;
    }
}

static void resync(panel_t *p)
{
    // drop the first byte, restart at the next start byte
    const uint8_t *s = memchr(p->rx + 1, 0xaa, p->rxlen - 1);
    size_t keep = s ? p->rxlen - (size_t)(s - p->rx) : 0;
    memmove(p->rx, p->rx + p->rxlen - keep, keep);
    p->rxlen = keep;
}

// Try to complete frames in the buffer. idle: the line went quiet (unknown-length frames end).
static void rx_process(panel_t *p, uint64_t now_us, bool idle)
{
    while (p->rxlen >= 3) {
        if (p->rx[0] != 0xaa) {
            resync(p);
            continue;
        }
        int l = frame_len(p->rx, p->rxlen);
        if (l == 0) {
            return;
        }
        if (l < 0) {
            if (!idle) {
                return;
            }
            l = (int)p->rxlen; // whole burst
        }
        if (l > 32) {
            p->frames_bad++;
            resync(p);
            continue;
        }
        if (p->rxlen < (size_t)l) {
            if (idle) { // truncated frame
                p->frames_bad++;
                resync(p);
                continue;
            }
            return;
        }
        if (xor8(p->rx, (size_t)l - 1) != p->rx[l - 1]) {
            p->frames_bad++;
            resync(p);
            continue;
        }
        handle_frame(p, p->rx, (size_t)l, now_us);
        memmove(p->rx, p->rx + l, p->rxlen - (size_t)l);
        p->rxlen -= (size_t)l;
    }
    if (idle && p->rxlen) {
        p->rxlen = 0; // 1-2 stray bytes before the gap
    }
}

void panel_rx(panel_t *p, const uint8_t *data, size_t len, uint64_t now_us)
{
    if (p->rxlen && (int64_t)(now_us - p->rx_last_us) > PANEL_GAP_US) {
        rx_process(p, now_us, true);
    }
    for (size_t i = 0; i < len; i++) {
        if (p->rxlen == 0 && data[i] != 0xaa) {
            continue;
        }
        if (p->rxlen == sizeof p->rx) {
            p->frames_bad++;
            resync(p);
        }
        p->rx[p->rxlen++] = data[i];
        rx_process(p, now_us, false);
    }
    p->rx_last_us = now_us;
}

// ---------------------------------------------------------------------------------------------
// Touch / time
// ---------------------------------------------------------------------------------------------
void panel_touch(panel_t *p, int slot, bool down, uint64_t now_us)
{
    if (slot < 0 || slot >= PANEL_SLOTS || p->slot_down[slot] == down) {
        return;
    }
    p->slot_down[slot] = down;
    if (down) {
        int b = panel_slot_button(p, slot);
        p->slot_btn[slot] = (uint8_t)b;
        p->slot_t0[slot] = now_us;
        p->slot_next_hold[slot] = now_us + (uint64_t)p->hold_delay_ms[b] * 1000;
        push_event(p, b, PEV_ON);
        push_event(p, b, PEV_CHANGE);
        request_beep(p, 2700, 20, 8);
    } else {
        int b = p->slot_btn[slot];
        uint64_t ms = (now_us - p->slot_t0[slot]) / 1000;
        push_event(p, b, PEV_OFF);
        push_event(p, b, PEV_CHANGE);
        push_event(p, b, ms < 500 ? PEV_CLICK : ms < 2000 ? PEV_SHORT : PEV_LONG);
    }
    p->version++;
}

void panel_touch_cancel(panel_t *p, int slot, uint64_t now_us)
{
    (void)now_us;
    if (slot < 0 || slot >= PANEL_SLOTS || !p->slot_down[slot]) {
        return;
    }
    p->slot_down[slot] = false;
    int b = p->slot_btn[slot];
    push_event(p, b, PEV_OFF);
    push_event(p, b, PEV_CHANGE);
    p->version++;
}

void panel_swipe(panel_t *p, int dir, uint64_t now_us)
{
    (void)now_us;
    if (!p->page_control_mode) {
        return;
    }
    int pg = p->page + (dir > 0 ? 1 : -1);
    p->page = (uint8_t)(pg < 1 ? PANEL_PAGES : pg > PANEL_PAGES ? 1 : pg);
    render_buttons(p);
}

// now_us may be older than the last RX IRQ stamp (the caller reads the clock before an IRQ lands):
// time differences are signed, so that counts as "no gap" and does not flush a frame mid-way.
void panel_tick(panel_t *p, uint64_t now_us)
{
    if (p->rxlen && (int64_t)(now_us - p->rx_last_us) > PANEL_GAP_US) {
        rx_process(p, now_us, true);
    }
    for (int s = 0; s < PANEL_SLOTS; s++) {
        if (!p->slot_down[s] || now_us < p->slot_next_hold[s]) {
            continue;
        }
        int b = p->slot_btn[s];
        if (!evq_has(p, (uint8_t)(b * 7 + PEV_HOLD))) { // polls drain slower than 100 ms holds
            push_event(p, b, PEV_HOLD);
        }
        p->slot_next_hold[s] = now_us + (uint64_t)p->hold_interval_ms[b] * 1000;
    }
    for (int b = 0; b < PANEL_BUTTONS; b++) {
        if ((p->led[b] == LEDS_FLASH_GREEN || p->led[b] == LEDS_FLASH_RED) && now_us >= p->led_flash_until[b]) {
            p->led[b] = p->led_after[b];
            p->version++;
        }
    }
    if (p->draw_pending && now_us >= p->draw_deadline_us) {
        p->draw_pending = false;
        memset(p->oled_draw, 0, sizeof p->oled_draw);
        memset(p->oled_shown, 0, sizeof p->oled_shown);
        panel_oled_text(p->oled_shown, 64 - text_w("! TIMEOUT !", 1) / 2, 28, 1, "! TIMEOUT !");
        p->oled_mode = OLED_TIMEOUT;
        p->version++;
    }
}
