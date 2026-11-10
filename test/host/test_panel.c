// Host tests for the SMART_PANEL_FM_4 emulation core. Every frame the panel sends is also parsed
// with the CLU-side parser of the ESP32 CLU (external/clu-esp32/components/clu_proto), and the expected
// replies come from a real panel on a real CLU ("clu firmware/tfbus_capture_20261014_*.log").
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panel_core.h"
#include "tfbus_frame.h"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---- captured panel output -------------------------------------------------------------------
static char s_tx[4096];
static size_t s_txlen;
static int s_tx_frames, s_tx_unparsable;

static void on_tx(const uint8_t *f, size_t n, void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < n; i++) {
        s_txlen += sprintf(s_tx + s_txlen, "%02x", f[i]);
    }
    s_tx[s_txlen++] = ' ';
    s_tx[s_txlen] = 0;
    s_tx_frames++;
    // what the CLU sees: its streaming parser + response validation
    const uint8_t *pl;
    size_t plen;
    tfbus_ident_t id;
    if (!tfbus_parse_response(f, n, &pl, &plen) ||
        (plen && tfbus_parse_tlv(pl, plen, (tfbus_tlv_t[8]){0}, 8) < 0 &&
         !(tfbus_parse_discover(pl, plen, &id) && id.hw_type == PANEL_HW_TYPE))) {
        s_tx_unparsable++;
    }
}

static void on_log(const char *m, void *ctx)
{
    (void)ctx;
    printf("     log: %s\n", m);
}

static void tx_reset(void)
{
    s_txlen = 0;
    s_tx[0] = 0;
    s_tx_frames = 0;
}

static void check_tx(const char *name, const char *want)
{
    int ok = strcmp(s_tx, want) == 0;
    check(name, ok);
    if (!ok) {
        printf("  got  %s\n  want %s\n", s_tx, want);
    }
}

static size_t unhex(uint8_t *out, const char *h)
{
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) {
        if (*h == ' ') {
            h--;
            continue;
        }
        unsigned b;
        sscanf(h, "%2x", &b);
        out[n++] = (uint8_t)b;
    }
    return n;
}

static uint64_t now;

static void frame(panel_t *p, uint8_t dest, uint8_t type, const uint8_t *pl, size_t n)
{
    uint8_t f[32];
    size_t l = tfbus_build(f, sizeof f, dest, type, pl, n);
    panel_rx(p, f, l, now);
    now += 1000;
    panel_tick(p, now);
}

// CLU frame on the bus, followed by an idle gap
static void bus(panel_t *p, const char *hex)
{
    uint8_t b[64];
    size_t n = unhex(b, hex);
    panel_rx(p, b, n, now);
    now += 1000;
    panel_tick(p, now);
}

static void poll(panel_t *p, uint8_t addr)
{
    uint8_t f[4] = {0xaa, addr, 0x04, 0};
    f[3] = tfbus_xor(f, 3);
    panel_rx(p, f, 4, now);
    now += 1000;
    panel_tick(p, now);
}

// Replay a whole real bus capture (CLU frames + every module's replies) into a panel with the
// SN of the capture's panel at 0x12: it must answer exactly the frames addressed to it.
static int replay(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("cannot open %s\n", path);
        return 1;
    }
    static panel_t R = {.tx = on_tx};
    panel_init(&R, 0x0f057b79);
    char line[512];
    uint64_t t = 1000000;
    int to_me = 0, polls_to_me = 0;
    tx_reset();
    int frames = 0;
    while (fgets(line, sizeof line, f)) {
        char *r = strstr(line, "raw: ");
        if (!r) {
            continue;
        }
        uint8_t b[64];
        size_t n = unhex(b, r + 5);
        if (n >= 4 && b[1] == R.addr && R.addr) {
            to_me++;
            polls_to_me += b[2] == 0x04;
        }
        panel_rx(&R, b, n, t);
        t += 600; // inter-frame gap
        panel_tick(&R, t);
        frames++;
        if (s_txlen > sizeof s_tx - 256) {
            s_txlen = 0;
        }
    }
    fclose(f);
    printf("     replayed %d frames: %d to the panel (%d polls), panel sent %d, bad %u, unknown %u\n", frames,
           to_me, polls_to_me, s_tx_frames, R.frames_bad, R.frames_unknown);
    check("replay: addressed by the capture's ADDR_ASSIGN", R.addr == 0x12 || to_me > 0);
    check("replay: one reply per frame addressed to the panel (+ SEEK/ADDR_ASSIGN acks)",
          s_tx_frames >= to_me && s_tx_frames <= to_me + 4);
    check("replay: no unknown frame types", R.frames_unknown == 0);
    check("replay: every reply parses on the CLU side", s_tx_unparsable == 0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        replay(argv[1]);
        printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    static panel_t P = {.tx = on_tx, .log = on_log};
    panel_t *p = &P;
    panel_init(p, 0x0f057b79); // SN of the panel at 0x12 in the real capture
    now = 1000000;

    // ---- discovery (real capture sequence) ----
    tx_reset();
    bus(p, "aaff0752");                  // RESET
    bus(p, "aaff2a7f");                  // SEEK -> ACK
    check_tx("RESET silent, SEEK ack", "aa0001ab ");
    tx_reset();
    bus(p, "aaff085d");                  // DISCOVER while unaddressed -> SN hwType fwType fwApiVer
    check_tx("DISCOVER reply: SN LE, 03 03 04", "aa0008797b050f030304ae ");
    {
        const uint8_t *pl;
        size_t plen;
        tfbus_ident_t id = {0};
        const uint8_t r[] = {0xaa, 0x00, 0x08, 0x79, 0x7b, 0x05, 0x0f, 0x03, 0x03, 0x04, 0xae};
        check("CLU parses DISCOVER reply", tfbus_parse_response(r, sizeof r, &pl, &plen) &&
                                               tfbus_parse_discover(pl, plen, &id) && id.sn == 0x0f057b79 &&
                                               id.hw_type == 3 && id.fw_type == 3 && id.fw_api_ver == 4);
    }
    tx_reset();
    bus(p, "aaff02b3a9ab1302f7");        // ADDR_ASSIGN other module -> silent
    bus(p, "aaff02797b050f124d");        // ADDR_ASSIGN 0x0f057b79 -> 0x12, ACK
    check_tx("own ADDR_ASSIGN ack", "aa0001ab ");
    check("address 0x12 taken", p->addr == 0x12);
    tx_reset();
    bus(p, "aaff085d");                  // DISCOVER once addressed: silent
    check_tx("DISCOVER ignored once addressed", "");

    // RX IRQ chunking on the board: FIFO threshold 4 bytes, the tail arrives with the RX timeout
    // ~540 us later. The frame must still be assembled (was dropped as truncated with a 400 us gap).
    tx_reset();
    {
        uint8_t f[7] = {0xaa, 0x12, 0x17, 0x01, 0x00, 0x00, 0};
        f[6] = tfbus_xor(f, 6);
        panel_rx(p, f, 4, now);
        now += 540;
        panel_rx(p, f + 4, 3, now);
        now += 1000;
        panel_tick(p, now);
    }
    check_tx("7-byte frame split 4 + 3 bytes (540 us apart) -> ACK", "aa0001ab ");

    // Main loop reads the clock, then an RX IRQ lands before panel_tick(): the tick's `now` is older
    // than the RX stamp. Must not count as an idle gap (unsigned wrap flushed half frames).
    tx_reset();
    {
        uint8_t f[7] = {0xaa, 0x12, 0x17, 0x01, 0x00, 0x00, 0};
        f[6] = tfbus_xor(f, 6);
        uint64_t loop_now = now;
        panel_rx(p, f, 4, now + 50);
        panel_tick(p, loop_now); // older than rx_last_us
        panel_rx(p, f + 4, 3, now + 540);
        now += 2000;
        panel_tick(p, now);
    }
    check_tx("panel_tick with a clock older than the last RX keeps the partial frame", "aa0001ab ");

    tx_reset();
    bus(p, "aa12f9e1");                  // VERSION_QUERY: real panel does not answer
    bus(p, "aa1304bd");                  // poll of another panel: silent
    check_tx("VERSION_QUERY and foreign poll unanswered", "");

    // ---- status: baseline / temperature exactly as the real panel ----
    panel_set_temp(p, 23.20f);
    tx_reset();
    poll(p, 0x12);
    poll(p, 0x12);
    check_tx("poll: temp frame then baseline (== real capture bytes)",
             "aa000b02060101000202101009ac aa00060106010100ab ");
    tx_reset();
    now += PANEL_TEMP_PERIOD_US;
    poll(p, 0x12);
    check("temperature again after 1.25 s", strstr(s_tx, "02021010 09") || strncmp(s_tx, "aa000b02", 8) == 0);

    // ---- real CLU init sequence for this panel: everything ACKed, nothing unknown ----
    static const char *init[] = {
        "aa1217010000ae", "aa1230000a000000000000000000000000000000000000000082",
        "aa12300000000000000000000000000000000000000000000088", "aa12300001000000000000000000000000000000000000000089",
        "aa123000037269676874000000000075700000000000000000ee", "aa12300005646f776e0000000000000500000000000000881301",
        "aa1231010500000000000000000000000000000000000000008d", "aa12310288130000000000000000000000000000000000000010",
        "aa12310c01000000000000000000000000000000000000000084", "aa12310d03000000000000000000000000000000000000000087",
        "aa12310e02000000000000000000000000000000000000000085", "aa12311001000000000000000000000000000000000000000098",
        "aa12311100000000000000000000000000000000000000000098", "aa1231140100000000000000000000000000000000000000009c",
        "aa1231150100000000000000000000000000000000000000009d", "aa1231160100000000000000000000000000000000000000009e",
        "aa121600010200ad", "aa1216000a0200a6", "aa121601010a00a4", "aa1216010a0a00af",
        "aa1231120103020000000000000000000000000000000000009b", "aa12311a0c02000000000000000000000000000000000000009d",
        "aa1218000002a2",
    };
    tx_reset();
    uint32_t unknown0 = p->frames_unknown, bad0 = p->frames_bad;
    for (size_t i = 0; i < sizeof init / sizeof init[0]; i++) {
        bus(p, init[i]);
    }
    int acks = 0;
    for (const char *s = s_tx; (s = strstr(s, "aa0001ab ")); s++) {
        acks++;
    }
    check("real CLU init frames: all ACKed", acks == (int)(sizeof init / sizeof init[0]));
    check("real CLU init frames: none unknown / malformed", p->frames_unknown == unknown0 && p->frames_bad == bad0);
    check("ProximityTimeout 5000 / buzzer on from init", p->prox_timeout_ms == 5000 && p->buzzer_on);
    // init's LED_CMD 18 00 00 02 has mask 0x0000 (LED_CMD byte 0 is a button mask, docs: btn4 = 0x08)
    check("LED_CMD from init with empty mask: no LED change", p->led[0] == LEDS_OFF);

    // ---- framing: back-to-back frames, split bytes, foreign traffic, garbage ----
    tx_reset();
    {
        uint8_t s[128];
        size_t n = unhex(s, "13 37 aa0204ac aa000f0300010100010101000202106709d9 aa1204bc aa05f0080116030000032969 aa1204bc");
        for (size_t i = 0; i < n; i++) { // byte by byte, no gaps
            panel_rx(p, s + i, 1, now);
            now += 87;
        }
        now += 1000;
        panel_tick(p, now);
    }
    check("byte stream with foreign frames: two polls answered", s_tx_frames == 2);
    tx_reset();
    bus(p, "aa1204bd");                  // bad XOR
    check_tx("bad XOR ignored", "");

    // ---- touch: events drained one per poll, VAL = btn*7 + ev ----
    tx_reset();
    panel_touch(p, 2, true, now);        // bottom-left = button 2
    now += 200000;
    panel_touch(p, 2, false, now);       // 200 ms -> click
    int vals[8], nv = 0;
    for (int i = 0; i < 6; i++) {
        tx_reset();
        poll(p, 0x12);
        const char *e = strstr(s_tx, "080210");
        if (e) {
            unsigned v;
            sscanf(e + 6, "%2x", &v);
            vals[nv++] = (int)v;
        }
    }
    check("press/release btn 2 -> On, Change, Off, Change, Click",
          nv == 5 && vals[0] == 15 && vals[1] == 14 && vals[2] == 16 && vals[3] == 14 && vals[4] == 20);

    tx_reset();
    panel_touch(p, 0, true, now);
    now += 1000000;
    panel_tick(p, now);                  // HoldDelay 1000 ms
    now += 100000;
    panel_tick(p, now);                  // HoldInterval: not queued twice
    int holds = 0;
    for (int i = 0; i < p->evq_n; i++) {
        holds += p->evq[i] == 0 * 7 + PEV_HOLD;
    }
    check("hold: OnHold after HoldDelay, not duplicated while queued", holds == 1);
    now += 1500000;
    panel_touch(p, 0, false, now);       // 2.6 s -> long press
    check("long press queued", p->evq[p->evq_n - 1] == PEV_LONG);
    for (int i = 0; i < 10; i++) {
        poll(p, 0x12);
    }

    // ---- press turned into a swipe: Off + Change, no click ----
    tx_reset();
    panel_touch(p, 1, true, now);
    now += 100000;
    panel_touch_cancel(p, 1, now);
    check("cancel: On, Change, Off, Change (no click)",
          p->evq_n == 4 && p->evq[2] == 1 * 7 + PEV_OFF && p->evq[3] == 1 * 7 + PEV_CHANGE && !p->slot_down[1]);
    for (int i = 0; i < 6; i++) {
        poll(p, 0x12);
    }

    // ---- pages: swipe -> page 2, slot 0 = button 4 ----
    panel_swipe(p, +1, now);
    check("swipe next page", p->page == 2 && panel_slot_button(p, 0) == 4);
    panel_touch(p, 0, true, now);
    tx_reset();
    poll(p, 0x12);
    check("page 2 slot 0 -> VAL 29 (btn 4 OnSwitchOn)", strstr(s_tx, "0802101d00") != NULL);
    panel_touch(p, 0, false, now);
    for (int i = 0; i < 5; i++) {
        poll(p, 0x12);
    }
    panel_swipe(p, -1, now);

    // ---- LEDs ----
    frame(p, 0x12, 0x18, (const uint8_t[]){0x08, 0x00, 0x10}, 3);            // RedLedSwitchOn btn 3 (mask 0x08)
    check("RedLedSwitchOn", panel_slot_led(p, 3, now) == LEDS_RED);
    frame(p, 0x12, 0x18, (const uint8_t[]){0x08, 0x00, 0x00}, 3);            // ShowOK btn 3: flash green, then back to red
    check("ShowOK flashes green", panel_slot_led(p, 3, now) == LEDS_GREEN || panel_slot_led(p, 3, now) == LEDS_OFF);
    now += 2100000;
    panel_tick(p, now);
    check("ShowOK ends, previous state restored", panel_slot_led(p, 3, now) == LEDS_RED);
    frame(p, 0x12, 0x18, (const uint8_t[]){0x08, 0x00, 0x04}, 3);
    check("LedSwitchOff", panel_slot_led(p, 3, now) == LEDS_OFF);

    // ---- display: PrintText + DRAW (ESP32 CLU driver frames) ----
    {
        uint8_t d[22] = {0x0a, 10, 20, 1, 'H', 'i'}, f[32];
        size_t n = tfbus_cmd(f, sizeof f, 0x12, d[0], d + 1, 21);
        panel_rx(p, f, n, now);
        now += 1000;
        n = tfbus_cmd(f, sizeof f, 0x12, 0x19, NULL, 0);
        panel_rx(p, f, n, now);
        now += 1000;
        panel_tick(p, now);
        // 'H' column 0 = 0x7F: pixels (10, 20..26) on, (10, 27) off
        check("PrintText + DRAW -> pixels shown", p->oled_mode == OLED_FREEDRAW && panel_oled_pixel(p, 10, 20) &&
                                                     panel_oled_pixel(p, 10, 26) && !panel_oled_pixel(p, 10, 27));
        n = tfbus_cmd(f, sizeof f, 0x12, 0x0a, (const uint8_t[]){0, 0, 2, 'X'}, 4);
        panel_rx(p, f, n, now);
        now += 1100000;
        panel_tick(p, now);
        check("draw without DisplayContent -> ! TIMEOUT !", p->oled_mode == OLED_TIMEOUT);

        // BUZZER_BEEP 1000 Hz 100 ms vol 8 (ESP32 driver SetBeep frame)
        uint16_t fq, ms, vol;
        panel_take_beep(p, &fq, &ms, &vol);
        n = tfbus_cmd(f, sizeof f, 0x12, 0x0f, (const uint8_t[]){0xe8, 0x03, 100, 0, 8, 0, 0}, 7);
        panel_rx(p, f, n, now);
        now += 1000;
        panel_tick(p, now);
        check("SetBeep", panel_take_beep(p, &fq, &ms, &vol) && fq == 1000 && ms == 100 && vol == 8);
        uint8_t bz[22] = {0x0c, 0};
        frame(p, 0x12, 0x31, bz, sizeof bz); // BuzzerValue 0
        panel_rx(p, f, n, now);
        now += 1000;
        panel_tick(p, now);
        check("BuzzerValue 0 mutes", !panel_take_beep(p, &fq, &ms, &vol));
    }

    // ---- RESET drops the address ----
    bus(p, "aaff0752");
    tx_reset();
    poll(p, 0x12);
    check("after RESET: no address, poll unanswered", p->addr == 0 && s_tx_frames == 0);

    check("every frame sent parses on the CLU side", s_tx_unparsable == 0);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
