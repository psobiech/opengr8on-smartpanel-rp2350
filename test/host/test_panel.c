// Host tests for the SMART_PANEL_FM_4 emulation core. Every frame the panel sends is also parsed
// with the CLU-side parser of the ESP32 CLU (external/clu-esp32/components/clu_proto), and the expected
// replies come from a real panel on a real CLU ("clu firmware/tfbus_capture_20261014_*.log").
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panel_core.h"
#include "tfbus_frame.h"

static int failures;

// ---- captured panel output -------------------------------------------------------------------
static char s_tx_hex[4096];
static size_t s_tx_hex_length;
static int s_tx_frames, s_tx_unparsable;

static uint64_t now;

static int replay(const char *path);
static void check(const char *name, int condition);
static void check_tx(const char *name, const char *expected);
static void tx_reset(void);
static void on_tx(const uint8_t *frame, size_t frame_length, void *context);
static void on_log(const char *message, void *context);
static void frame(panel_t *panel, uint8_t dest, uint8_t type, const uint8_t *payload, size_t payload_length);
static void bus(panel_t *panel, const char *hex);
static void idle_bus(panel_t *panel, uint64_t duration_us);
static void poll(panel_t *panel, uint8_t address);
static size_t unhex(uint8_t *out, const char *hex);

int main(int argc, char **argv)
{
    if (argc > 1) {
        replay(argv[1]);
        printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    static panel_t test_panel = {.transmit = on_tx, .log = on_log};
    panel_t *panel = &test_panel;
    panel_init(panel, 0x0f057b79); // SN of the panel at 0x12 in the real capture
    now = 1000000;

    // ---- discovery (real capture sequence) ----
    tx_reset();
    bus(panel, "aaff0752");              // RESET
    bus(panel, "aaff2a7f");              // SEEK -> ACK
    check_tx("RESET silent, SEEK ack", "aa0001ab ");
    tx_reset();
    bus(panel, "aaff085d");              // DISCOVER while unaddressed -> SN hwType fwType fwApiVer, delayed
    check_tx("DISCOVER: no immediate reply", "");
    idle_bus(panel, 435000);
    check_tx("DISCOVER: still waiting 436 ms later", "");
    idle_bus(panel, 10000);
    check_tx("DISCOVER reply after this SN's 1st-attempt slot (445 ms): SN LE, 03 03 04", "aa0008797b050f030304ae ");
    {
        const uint8_t *payload;
        size_t payload_length;
        tfbus_ident_t ident = {0};
        const uint8_t reply[] = {0xaa, 0x00, 0x08, 0x79, 0x7b, 0x05, 0x0f, 0x03, 0x03, 0x04, 0xae};
        check("CLU parses DISCOVER reply", tfbus_parse_response(reply, sizeof reply, &payload, &payload_length) &&
                                               tfbus_parse_discover(payload, payload_length, &ident) &&
                                               ident.sn == 0x0f057b79 && ident.hw_type == 3 && ident.fw_type == 3 &&
                                               ident.fw_api_ver == 4);
    }
    // 2nd DISCOVER (195 ms slot): another module answers at 100 ms -> ours still goes out at 195 ms
    // (the real CLU addresses every module that answered within its ~1 s window)
    tx_reset();
    bus(panel, "aaff085d");
    idle_bus(panel, 100000);
    frame(panel, 0x00, 0x08, (const uint8_t[]){0x0b, 0xfb, 0x5e, 0x87, 0x15, 0x02, 0x02}, 7);
    idle_bus(panel, 85000);
    check_tx("DISCOVER: another module's reply does not drop ours", "");
    idle_bus(panel, 10000);
    check_tx("DISCOVER: reply in its slot after another module's reply", "aa0008797b050f030304ae ");
    // 3rd DISCOVER (420 ms slot): a frame is arriving when the slot comes -> sent once the bus is idle
    tx_reset();
    bus(panel, "aaff085d");
    idle_bus(panel, 418000);
    panel_rx(panel, (const uint8_t[]){0xaa, 0x00, 0x08, 0x0b}, 4, now);
    idle_bus(panel, 1000);
    check_tx("DISCOVER: no reply while the bus is busy", "");
    panel_rx(panel, (const uint8_t[]){0xfb, 0x5e, 0x87, 0x15, 0x02, 0x02, 0x9e}, 7, now);
    idle_bus(panel, 1000);
    check_tx("DISCOVER: still none within the idle gap", "");
    idle_bus(panel, 2000);
    check_tx("DISCOVER: reply once the bus is idle", "aa0008797b050f030304ae ");
    // 4th DISCOVER (420 ms slot): the master's ADDR_ASSIGN to another module comes first -> dropped
    tx_reset();
    bus(panel, "aaff085d");
    idle_bus(panel, 5000);
    bus(panel, "aaff02b3a9ab1302f7");
    idle_bus(panel, 1000000);
    check_tx("DISCOVER reply dropped: the master sent a frame first", "");
    tx_reset();
    bus(panel, "aaff02b3a9ab1302f7");    // ADDR_ASSIGN other module -> silent
    bus(panel, "aaff02797b050f124d");    // ADDR_ASSIGN 0x0f057b79 -> 0x12, ACK
    check_tx("own ADDR_ASSIGN ack", "aa0001ab ");
    check("address 0x12 taken", panel->address == 0x12);
    tx_reset();
    bus(panel, "aaff085d");              // DISCOVER once addressed: silent
    check_tx("DISCOVER ignored once addressed", "");

    // DISCOVER slots stay clear of a real module's reply (~577 ms) and inside the real CLU's ~1 s window
    {
        static panel_t slot_panel = {.transmit = on_tx};
        panel_init(&slot_panel, 0x0f057b79);
        int misplaced_replies = 0;
        for (int attempt = 0; attempt < 64; attempt++) {
            tx_reset();
            uint64_t discover_us = now;
            bus(&slot_panel, "aaff085d");
            while (s_tx_frames == 0 && now - discover_us < 1000000) {
                idle_bus(&slot_panel, 1000);
            }
            uint64_t reply_delay_ms = (now - discover_us) / 1000;
            if (s_tx_frames == 0 || (reply_delay_ms > 520 && reply_delay_ms < 625) || reply_delay_ms > 880) {
                printf("  attempt %d: reply after %llu ms\n", attempt, (unsigned long long)reply_delay_ms);
                misplaced_replies++;
            }
        }
        check("DISCOVER slots: 20..520 ms or 625..875 ms", misplaced_replies == 0);
    }

    // RX IRQ chunking on the board: FIFO threshold 4 bytes, the tail arrives with the RX timeout
    // ~540 us later. The frame must still be assembled (was dropped as truncated with a 400 us gap).
    tx_reset();
    {
        uint8_t split_frame[7] = {0xaa, 0x12, 0x17, 0x01, 0x00, 0x00, 0};
        split_frame[6] = tfbus_xor(split_frame, 6);
        panel_rx(panel, split_frame, 4, now);
        now += 540;
        panel_rx(panel, split_frame + 4, 3, now);
        now += 1000;
        panel_tick(panel, now);
    }
    check_tx("7-byte frame split 4 + 3 bytes (540 us apart) -> ACK", "aa0001ab ");

    // Main loop reads the clock, then an RX IRQ lands before panel_tick(): the tick's `now` is older
    // than the RX stamp. Must not count as an idle gap (unsigned wrap flushed half frames).
    tx_reset();
    {
        uint8_t split_frame[7] = {0xaa, 0x12, 0x17, 0x01, 0x00, 0x00, 0};
        split_frame[6] = tfbus_xor(split_frame, 6);
        uint64_t loop_now = now;
        panel_rx(panel, split_frame, 4, now + 50);
        panel_tick(panel, loop_now); // older than rx_last_us
        panel_rx(panel, split_frame + 4, 3, now + 540);
        now += 2000;
        panel_tick(panel, now);
    }
    check_tx("panel_tick with a clock older than the last RX keeps the partial frame", "aa0001ab ");

    tx_reset();
    bus(panel, "aa12f9e1");              // VERSION_QUERY: real panel does not answer
    bus(panel, "aa1304bd");              // poll of another panel: silent
    check_tx("VERSION_QUERY and foreign poll unanswered", "");

    // ---- status: sensor frame (temperature in 0.1 C at bytes 7-8) on change, bare ACK otherwise ----
    panel_set_temp(panel, 23.20f);
    tx_reset();
    poll(panel, 0x12);
    poll(panel, 0x12);
    check_tx("poll after ADDR_ASSIGN: sensor frame 23.2 C, then bare ACK",
             "aa000e00000000e800000000000000004c aa0001ab ");
    tx_reset();
    panel_set_temp(panel, 23.25f);       // same 0.1 C step as shown on the screen
    poll(panel, 0x12);
    panel_set_temp(panel, 23.31f);
    poll(panel, 0x12);
    check_tx("temperature: no frame within the same 0.1 C, sensor frame on change",
             "aa0001ab aa000e00000000e900000000000000004d ");

    // ---- real CLU init sequence for this panel: everything ACKed, nothing unknown ----
    static const char *init_frames[] = {
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
    uint32_t unknown_before = panel->frames_unknown, bad_before = panel->frames_bad;
    for (size_t i = 0; i < sizeof init_frames / sizeof init_frames[0]; i++) {
        bus(panel, init_frames[i]);
    }
    int acks = 0;
    for (const char *cursor = s_tx_hex; (cursor = strstr(cursor, "aa0001ab ")); cursor++) {
        acks++;
    }
    check("real CLU init frames: all ACKed", acks == (int)(sizeof init_frames / sizeof init_frames[0]));
    check("real CLU init frames: none unknown / malformed",
          panel->frames_unknown == unknown_before && panel->frames_bad == bad_before);
    check("ProximityTimeout 5000 / buzzer on from init", panel->proximity_timeout_ms == 5000 && panel->buzzer_on);
    // init's LED_CMD 18 00 00 02 has mask 0x0000 (LED_CMD byte 0 is a button mask, docs: btn4 = 0x08)
    check("LED_CMD from init with empty mask: no LED change", panel->led[0] == LEDS_OFF);

    // ---- framing: back-to-back frames, split bytes, foreign traffic, garbage ----
    tx_reset();
    {
        uint8_t stream[128];
        size_t stream_length =
            unhex(stream, "13 37 aa0204ac aa000f0300010100010101000202106709d9 aa1204bc aa05f0080116030000032969 aa1204bc");
        for (size_t i = 0; i < stream_length; i++) { // byte by byte, no gaps
            panel_rx(panel, stream + i, 1, now);
            now += 87;
        }
        now += 1000;
        panel_tick(panel, now);
    }
    check("byte stream with foreign frames: two polls answered", s_tx_frames == 2);
    tx_reset();
    bus(panel, "aa1204bd");              // bad XOR
    check_tx("bad XOR ignored", "");

    // ---- touch: one Value change per poll in the state frame (changed mask, Value mask, temp) ----
    // state frame: aa 00 0e | changed LE16 | values LE16 | temp 23.3 C (e9 00) | 0 x7 | xor
    tx_reset();
    panel_touch(panel, 2, true, now);    // bottom-left = button 2
    now += 200000;
    panel_touch(panel, 2, false, now);   // tap shorter than a poll: still 1 then 0
    poll(panel, 0x12);
    poll(panel, 0x12);
    poll(panel, 0x12);
    check_tx("tap on button 2: Value 1, Value 0, then bare ACK",
             "aa000e04000400e900000000000000004d aa000e04000000e9000000000000000049 aa0001ab ");
    {
        const uint8_t *payload;
        size_t payload_length;
        const uint8_t reply[] = {0xaa, 0x00, 0x0e, 0x04, 0x00, 0x04, 0x00, 0xe9, 0x00,
                                 0, 0, 0, 0, 0, 0, 0, 0x4d};
        check("CLU-side framing accepts the state frame", tfbus_parse_response(reply, sizeof reply, &payload, &payload_length));
    }

    tx_reset();
    panel_touch(panel, 0, true, now);
    poll(panel, 0x12);
    now += 2600000;
    poll(panel, 0x12);                   // still held: Value stays 1, nothing new
    panel_touch(panel, 0, false, now);
    poll(panel, 0x12);
    check_tx("hold button 0: Value 1 while held, 0 on release",
             "aa000e01000100e900000000000000004d aa0001ab aa000e01000000e900000000000000004c ");

    // ---- press turned into a swipe: an ordinary release for the CLU ----
    tx_reset();
    panel_touch(panel, 1, true, now);
    now += 100000;
    panel_touch_cancel(panel, 1, now);
    poll(panel, 0x12);
    poll(panel, 0x12);
    check_tx("cancel: Value 1 then 0", "aa000e02000200e900000000000000004d aa000e02000000e900000000000000004f ");
    check("cancel releases the field", !panel->slot_down[1]);

    // ---- button Mode (class 0x17 [mask16, mode]): bistable toggles on press, locked is silent ----
    tx_reset();
    frame(panel, 0x12, 0x17, (const uint8_t[]){0x08, 0x00, BUTTON_MODE_BISTABLE}, 3); // button 3
    check_tx("0x17 button config acked", "aa0001ab ");
    check("0x17 mask 0x0008 mode 1 -> button 3 bistable, others unchanged",
          panel->button_mode[3] == BUTTON_MODE_BISTABLE && panel->button_mode[2] == BUTTON_MODE_MONOSTABLE);
    tx_reset();
    panel_touch(panel, 3, true, now);
    panel_touch(panel, 3, false, now);
    poll(panel, 0x12);
    poll(panel, 0x12);
    check_tx("bistable 1st press: Value 1, kept after release", "aa000e08000800e900000000000000004d aa0001ab ");
    check("bistable ON without LED_CMD: LED green", panel_slot_led(panel, 3, now) == LEDS_GREEN);
    tx_reset();
    panel_touch(panel, 3, true, now);
    panel_touch_cancel(panel, 3, now);
    poll(panel, 0x12);
    poll(panel, 0x12);
    check_tx("bistable 2nd press: Value 0", "aa000e08000000e9000000000000000045 aa0001ab ");
    check("bistable OFF: LED off", panel_slot_led(panel, 3, now) == LEDS_OFF);
    tx_reset();
    panel_touch(panel, 3, true, now);    // on again, then back to monostable: Value dropped
    panel_touch(panel, 3, false, now);
    frame(panel, 0x12, 0x17, (const uint8_t[]){0x08, 0x00, BUTTON_MODE_MONOSTABLE}, 3);
    check("bistable -> monostable drops the toggled Value", !(panel->button_values & 0x0008));
    tx_reset();
    poll(panel, 0x12);
    poll(panel, 0x12);
    poll(panel, 0x12);
    check_tx("Value 1 then 0 reported for button 3",
             "aa000e08000800e900000000000000004d aa000e08000000e9000000000000000045 aa0001ab ");
    frame(panel, 0x12, 0x17, (const uint8_t[]){0x08, 0x00, BUTTON_MODE_LOCKED}, 3);
    tx_reset();
    panel_touch(panel, 3, true, now);
    panel_touch(panel, 3, false, now);
    poll(panel, 0x12);
    check_tx("locked: no Value change", "aa0001ab ");
    frame(panel, 0x12, 0x17, (const uint8_t[]){0x08, 0x00, BUTTON_MODE_MONOSTABLE}, 3);

    // ---- change queue full: the current Values still go out ----
    for (int i = 0; i < PANEL_CHANGE_QUEUE_SIZE / 2 + 1; i++) {
        panel_touch(panel, 2, true, now);
        panel_touch(panel, 2, false, now);
    }
    panel_touch(panel, 2, true, now);    // dropped from the full queue, Value now 1
    for (int i = 0; i < PANEL_CHANGE_QUEUE_SIZE; i++) {
        poll(panel, 0x12);
    }
    tx_reset();
    poll(panel, 0x12);
    poll(panel, 0x12);
    check_tx("queue overflow: Value 1 reported from the current state",
             "aa000e04000400e900000000000000004d aa0001ab ");
    panel_touch(panel, 2, false, now);
    poll(panel, 0x12);

    // ---- pages: swipe -> page 2, slot 0 = button 4 ----
    panel_swipe(panel, +1, now);
    check("swipe next page", panel->page == 2 && panel_slot_button(panel, 0) == 4);
    panel_touch(panel, 0, true, now);
    tx_reset();
    poll(panel, 0x12);
    check_tx("page 2 slot 0 -> button 4 Value 1", "aa000e10001000e900000000000000004d ");
    panel_touch(panel, 0, false, now);
    poll(panel, 0x12);
    panel_swipe(panel, -1, now);

    // ---- SetLabel (real CLU: 0x30 [button, 0x0a, text]): corner label only, OLED untouched ----
    {
        uint8_t label_block[22] = {0x00, 0x0a, 'a', 'a', 'a', 'a'};
        frame(panel, 0x12, 0x30, label_block, sizeof label_block);
        bool oled_lit = false;
        for (int y = 0; y < OLED_H; y++) {
            for (int x = 0; x < OLED_W; x++) {
                oled_lit |= panel_oled_pixel(panel, x, y);
            }
        }
        check("SetLabel: label stored, nothing drawn on the OLED",
              strcmp(panel->label[0], "aaaa") == 0 && !oled_lit);
    }

    // ---- LEDs ----
    frame(panel, 0x12, 0x18, (const uint8_t[]){0x08, 0x00, 0x10}, 3);        // RedLedSwitchOn btn 3 (mask 0x08)
    check("RedLedSwitchOn", panel_slot_led(panel, 3, now) == LEDS_RED);
    frame(panel, 0x12, 0x18, (const uint8_t[]){0x08, 0x00, 0x00}, 3);        // ShowOK btn 3: flash green, then back to red
    check("ShowOK flashes green",
          panel_slot_led(panel, 3, now) == LEDS_GREEN || panel_slot_led(panel, 3, now) == LEDS_OFF);
    now += 2100000;
    panel_tick(panel, now);
    check("ShowOK ends, previous state restored", panel_slot_led(panel, 3, now) == LEDS_RED);
    frame(panel, 0x12, 0x18, (const uint8_t[]){0x08, 0x00, 0x04}, 3);
    check("LedSwitchOff", panel_slot_led(panel, 3, now) == LEDS_OFF);

    // ---- display: PrintText + DRAW (ESP32 CLU driver frames) ----
    {
        uint8_t print_text[22] = {0x0a, 10, 20, 1, 'H', 'i'}, frame_buffer[32];
        size_t frame_length = tfbus_cmd(frame_buffer, sizeof frame_buffer, 0x12, print_text[0], print_text + 1, 21);
        panel_rx(panel, frame_buffer, frame_length, now);
        now += 1000;
        frame_length = tfbus_cmd(frame_buffer, sizeof frame_buffer, 0x12, 0x19, NULL, 0);
        panel_rx(panel, frame_buffer, frame_length, now);
        now += 1000;
        panel_tick(panel, now);
        // 'H' column 0 = 0x7F: pixels (10, 20..26) on, (10, 27) off
        check("PrintText + DRAW -> pixels shown", panel->oled_mode == OLED_FREEDRAW &&
                                                     panel_oled_pixel(panel, 10, 20) &&
                                                     panel_oled_pixel(panel, 10, 26) && !panel_oled_pixel(panel, 10, 27));
        frame_length = tfbus_cmd(frame_buffer, sizeof frame_buffer, 0x12, 0x0a, (const uint8_t[]){0, 0, 2, 'X'}, 4);
        panel_rx(panel, frame_buffer, frame_length, now);
        now += 1100000;
        panel_tick(panel, now);
        check("draw without DisplayContent -> ! TIMEOUT !", panel->oled_mode == OLED_TIMEOUT);

        // BUZZER_BEEP 1000 Hz 100 ms vol 8 (ESP32 driver SetBeep frame)
        uint16_t frequency_hz, duration_ms, volume;
        panel_take_beep(panel, &frequency_hz, &duration_ms, &volume);
        frame_length = tfbus_cmd(frame_buffer, sizeof frame_buffer, 0x12, 0x0f,
                                 (const uint8_t[]){0xe8, 0x03, 100, 0, 8, 0, 0}, 7);
        panel_rx(panel, frame_buffer, frame_length, now);
        now += 1000;
        panel_tick(panel, now);
        check("SetBeep", panel_take_beep(panel, &frequency_hz, &duration_ms, &volume) && frequency_hz == 1000 &&
                             duration_ms == 100 && volume == 8);
        uint8_t buzzer_command[22] = {0x0c, 0};
        frame(panel, 0x12, 0x31, buzzer_command, sizeof buzzer_command); // BuzzerValue 0
        panel_rx(panel, frame_buffer, frame_length, now);
        now += 1000;
        panel_tick(panel, now);
        check("BuzzerValue 0 mutes", !panel_take_beep(panel, &frequency_hz, &duration_ms, &volume));
    }

    // ---- RESET drops the address ----
    bus(panel, "aaff0752");
    tx_reset();
    poll(panel, 0x12);
    check("after RESET: no address, poll unanswered", panel->address == 0 && s_tx_frames == 0);

    check("every frame sent parses on the CLU side", s_tx_unparsable == 0);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

// Replay a whole real bus capture (CLU frames + every module's replies) into a panel with the
// SN of the capture's panel at 0x12: it must answer exactly the frames addressed to it.
static int replay(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        printf("cannot open %s\n", path);
        return 1;
    }
    static panel_t replay_panel = {.transmit = on_tx};
    panel_init(&replay_panel, 0x0f057b79);
    char line[512];
    uint64_t time_us = 1000000;
    int frames_to_me = 0, polls_to_me = 0;
    tx_reset();
    int frames = 0;
    while (fgets(line, sizeof line, file)) {
        char *raw = strstr(line, "raw: ");
        if (!raw) {
            continue;
        }
        uint8_t bytes[64];
        size_t count = unhex(bytes, raw + 5);
        if (count >= 4 && bytes[1] == replay_panel.address && replay_panel.address) {
            frames_to_me++;
            polls_to_me += bytes[2] == 0x04;
        }
        panel_rx(&replay_panel, bytes, count, time_us);
        time_us += 600; // inter-frame gap
        panel_tick(&replay_panel, time_us);
        frames++;
        if (s_tx_hex_length > sizeof s_tx_hex - 256) {
            s_tx_hex_length = 0;
        }
    }
    fclose(file);
    printf("     replayed %d frames: %d to the panel (%d polls), panel sent %d, bad %u, unknown %u\n", frames,
           frames_to_me, polls_to_me, s_tx_frames, replay_panel.frames_bad, replay_panel.frames_unknown);
    check("replay: addressed by the capture's ADDR_ASSIGN", replay_panel.address == 0x12 || frames_to_me > 0);
    check("replay: one reply per frame addressed to the panel (+ SEEK/ADDR_ASSIGN acks)",
          s_tx_frames >= frames_to_me && s_tx_frames <= frames_to_me + 4);
    check("replay: no unknown frame types", replay_panel.frames_unknown == 0);
    check("replay: every reply parses on the CLU side", s_tx_unparsable == 0);
    return 0;
}

static void check(const char *name, int condition)
{
    printf("%s %s\n", condition ? "ok  " : "FAIL", name);
    failures += !condition;
}

static void check_tx(const char *name, const char *expected)
{
    int matches = strcmp(s_tx_hex, expected) == 0;
    check(name, matches);
    if (!matches) {
        printf("  got  %s\n  want %s\n", s_tx_hex, expected);
    }
}

static void tx_reset(void)
{
    s_tx_hex_length = 0;
    s_tx_hex[0] = 0;
    s_tx_frames = 0;
}

static void on_tx(const uint8_t *frame, size_t frame_length, void *context)
{
    (void)context;
    for (size_t i = 0; i < frame_length; i++) {
        s_tx_hex_length += sprintf(s_tx_hex + s_tx_hex_length, "%02x", frame[i]);
    }
    s_tx_hex[s_tx_hex_length++] = ' ';
    s_tx_hex[s_tx_hex_length] = 0;
    s_tx_frames++;
    // what the CLU sees: its streaming parser + response validation
    const uint8_t *payload;
    size_t payload_length;
    tfbus_ident_t ident;
    if (!tfbus_parse_response(frame, frame_length, &payload, &payload_length) ||
        (payload_length && tfbus_parse_tlv(payload, payload_length, (tfbus_tlv_t[8]){0}, 8) < 0 &&
         !(tfbus_parse_discover(payload, payload_length, &ident) && ident.hw_type == PANEL_HW_TYPE))) {
        s_tx_unparsable++;
    }
}

static void on_log(const char *message, void *context)
{
    (void)context;
    printf("     log: %s\n", message);
}

static void frame(panel_t *panel, uint8_t dest, uint8_t type, const uint8_t *payload, size_t payload_length)
{
    uint8_t frame_buffer[32];
    size_t frame_length = tfbus_build(frame_buffer, sizeof frame_buffer, dest, type, payload, payload_length);
    panel_rx(panel, frame_buffer, frame_length, now);
    now += 1000;
    panel_tick(panel, now);
}

// CLU frame on the bus, followed by an idle gap
static void bus(panel_t *panel, const char *hex)
{
    uint8_t bytes[64];
    size_t count = unhex(bytes, hex);
    panel_rx(panel, bytes, count, now);
    now += 1000;
    panel_tick(panel, now);
}

// Quiet bus for duration_us, with the main loop ticking every 1 ms
static void idle_bus(panel_t *panel, uint64_t duration_us)
{
    for (uint64_t end_us = now + duration_us; now < end_us;) {
        now += 1000;
        panel_tick(panel, now);
    }
}

static void poll(panel_t *panel, uint8_t address)
{
    uint8_t poll_frame[4] = {0xaa, address, 0x04, 0};
    poll_frame[3] = tfbus_xor(poll_frame, 3);
    panel_rx(panel, poll_frame, 4, now);
    now += 1000;
    panel_tick(panel, now);
}

static size_t unhex(uint8_t *out, const char *hex)
{
    size_t count = 0;
    for (; hex[0] && hex[1]; hex += 2) {
        if (*hex == ' ') {
            hex--;
            continue;
        }
        unsigned byte_value;
        sscanf(hex, "%2x", &byte_value);
        out[count++] = (uint8_t)byte_value;
    }
    return count;
}
