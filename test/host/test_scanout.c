// Host tests for the display scan-out (src/scanout.c, included: its state is static) and the UI
// framebuffer it reads (src/ui.c, real). The PIO RGB driver, core1 and the spin lock are fakes.
// Covered: every line table address stays inside its buffer for each pixel shift (the DMA reads
// UI_W pixels from it), the table swap at a frame start, the OLED rows composed into the ring
// (edges repeated for a horizontal shift), late rows counted and skipped, giving up a frame when
// the scan-out stops; the touch hit test and that ui_render only redraws the region that changed.
#include <stdio.h>
#include <stdlib.h>

#include "../../src/scanout.c"

static int failures;

static uint16_t *g_framebuffer_allocation; // as src/main.c allocates it: SCANOUT_SHIFT_MAX margin
static const uint16_t *const *g_driver_line_table;
static bool g_driver_enabled = true;
static int g_lines_started;
static bool g_dma_advances; // the DMA starts one more row every 3rd pio_rgb_lines_started call (slower than core1)
static int g_dma_polls;
static void (*g_core1_entry)(void);
static spin_lock_t g_spin_lock;

static void test_line_tables(void);
static void test_table_swap(void);
static void test_compose_oled_rows(void);
static void test_late_and_stopped(void);
static void test_hit(void);
static void test_render_regions(void);
static void check(const char *name, int condition);
static bool inside(const uint16_t *read_start, const uint16_t *buffer, size_t buffer_pixels);
static void screen_snapshot(uint16_t *screen, const uint8_t *oled);
static bool changed_only_inside(const uint16_t *before, const uint16_t *after, int left, int top, int right, int bottom);

int main(void)
{
    size_t allocation_pixels = UI_FRAMEBUFFER_PIXELS + 2 * SCANOUT_SHIFT_MAX;
    g_framebuffer_allocation = calloc(allocation_pixels, sizeof(uint16_t));
    ui_init(g_framebuffer_allocation + SCANOUT_SHIFT_MAX);
    test_line_tables();
    test_table_swap();
    test_compose_oled_rows();
    test_late_and_stopped();
    test_hit();
    test_render_regions();
    free(g_framebuffer_allocation);
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

// ---- tests -----------------------------------------------------------------------------------
static void test_line_tables(void)
{
    static pio_rgb_info_t rgb_info;
    scanout_init(&rgb_info);
    check("init: the driver starts from table 0", rgb_info.line_table == s_line_tables[0].rows &&
                                                      rgb_info.dma_flush_done_cb == frame_started);
    const line_table_t *table = &s_line_tables[0];
    bool rows_match = table->rows[UI_H] == NULL;
    for (int y = 0; y < UI_H; y++) {
        const uint16_t *expected = ui_row(y) ? ui_row(y) : s_ring[(y - UI_OLED_Y) & (RING_LINES - 1)] + LINE_MARGIN;
        rows_match &= table->rows[y] == expected;
    }
    check("no shift: stored rows straight from the framebuffer, OLED rows from the ring, NULL end", rows_match);

    size_t allocation_pixels = UI_FRAMEBUFFER_PIXELS + 2 * SCANOUT_SHIFT_MAX;
    bool all_inside = true, black_outside = true;
    for (int shift_y = -SCANOUT_SHIFT_MAX; shift_y <= SCANOUT_SHIFT_MAX; shift_y++) {
        for (int shift_x = -SCANOUT_SHIFT_MAX; shift_x <= SCANOUT_SHIFT_MAX; shift_x++) {
            line_table_t shifted;
            build_line_table(&shifted, shift_x, shift_y);
            for (int y = 0; y < UI_H; y++) {
                const uint16_t *read_start = shifted.rows[y];
                bool in_ring = false;
                for (int line = 0; line < RING_LINES; line++) {
                    in_ring |= inside(read_start, s_ring[line], LINE_STRIDE);
                }
                bool in_black = inside(read_start, s_black_row, LINE_STRIDE);
                all_inside &= inside(read_start, g_framebuffer_allocation, allocation_pixels) || in_ring || in_black;
                if (y + shift_y < 0 || y + shift_y >= UI_H) {
                    black_outside &= in_black;
                }
            }
            all_inside &= shifted.rows[UI_H] == NULL && shifted.shift_y == shift_y;
        }
    }
    check("every shift: the DMA reads UI_W pixels inside the framebuffer, a ring line or the black row", all_inside);
    check("vertical shift: rows from outside the screen are black", black_outside);
    line_table_t shifted;
    build_line_table(&shifted, 1, 2);
    check("shift (1, 2): screen row 0 shows stored row 2 from pixel 1", shifted.rows[0] == ui_row(2) + 1);
}

static void test_table_swap(void)
{
    s_table_sending = s_table_queued = 0;
    scanout_set_shift(-1, 1);
    check("shift: built into the table not being sent", s_table_queued == 1 &&
                                                            g_driver_line_table == s_line_tables[1].rows &&
                                                            s_line_tables[1].shift_y == 1);
    check("shift: the current frame keeps its table", s_table_sending == 0);
    uint32_t frames_before = s_frame_count;
    frame_started();
    check("frame start: the queued table is the one sent", s_table_sending == 1 && s_frame_count == frames_before + 1);
    scanout_set_shift(0, 0);
    check("next shift: into the other table", s_table_queued == 0 && g_driver_line_table == s_line_tables[0].rows);
    frame_started();
    scanout_set_enabled(false);
    check("stop: driver stopped, core1 told", !g_driver_enabled && s_stopped);
    scanout_set_enabled(true);
    check("restart: driver running", g_driver_enabled && !s_stopped);
    scanout_start();
    check("start: core1 runs the composer", g_core1_entry == core1_main);
}

static void test_compose_oled_rows(void)
{
    static uint8_t oled[OLED_W * OLED_H / 8];
    for (size_t i = 0; i < sizeof oled; i++) {
        oled[i] = (uint8_t)(i * 37);
    }
    scanout_set_oled(oled);
    check("OLED handed over under the lock", memcmp(s_oled_shared, oled, sizeof oled) == 0 && g_spin_lock == 0);
    memcpy(s_oled_frame, s_oled_shared, sizeof s_oled_frame);
    s_late_lines = 0;
    g_lines_started = 0;
    g_dma_advances = true;
    compose_oled_rows(&s_line_tables[0], s_frame_count);
    check("DMA behind core1: no late rows", s_late_lines == 0);
    bool ring_holds_last_rows = true;
    for (int row = UI_OLED_Y + UI_OLED_H - RING_LINES; row < UI_OLED_Y + UI_OLED_H; row++) {
        uint16_t expected[UI_W];
        ui_compose_oled_row(expected, row, oled);
        const uint16_t *line = s_ring[(row - UI_OLED_Y) & (RING_LINES - 1)];
        ring_holds_last_rows &= memcmp(line + LINE_MARGIN, expected, sizeof expected) == 0;
        for (int i = 0; i < LINE_MARGIN; i++) {
            ring_holds_last_rows &= line[i] == expected[0] && line[LINE_MARGIN + UI_W + i] == expected[UI_W - 1];
        }
    }
    check("ring: the last RING_LINES OLED rows, edge pixels repeated in the margins", ring_holds_last_rows);
    check("ring: the DMA was waited for (a line is reused only after its previous row was sent)",
          g_lines_started >= UI_OLED_Y + UI_OLED_H - RING_LINES);
}

static void test_late_and_stopped(void)
{
    for (int line = 0; line < RING_LINES; line++) {
        for (int i = 0; i < LINE_STRIDE; i++) {
            s_ring[line][i] = 0xdead;
        }
    }
    s_late_lines = 0;
    g_lines_started = UI_OLED_Y + UI_OLED_H; // the DMA already past the OLED
    g_dma_advances = false;
    compose_oled_rows(&s_line_tables[0], s_frame_count);
    check("core1 late: every OLED row counted", s_late_lines == UI_OLED_H);
    check("core1 late: rows already sent are not written", s_ring[0][LINE_MARGIN] == 0xdead);

    s_late_lines = 0;
    g_lines_started = 0; // the DMA stuck at the frame start (display blank)
    s_stopped = true;
    compose_oled_rows(&s_line_tables[0], s_frame_count);
    check("scan-out stopped: core1 gives up the frame instead of waiting", s_late_lines == 0 &&
                                                                               s_ring[0][LINE_MARGIN] != 0xdead);
    s_stopped = false;
    compose_oled_rows(&s_line_tables[0], s_frame_count - 1);
    check("a newer frame started: core1 gives up the old one", s_late_lines == 0);
    check("late lines reported", scanout_late_lines() == 0);
}

static void test_hit(void)
{
    static const struct {
        int x, y, expected;
        const char *name;
    } k_hits[] = {
        {0, 0, 0, "top-left corner"},
        {479, 0, 1, "top-right corner"},
        {0, 479, 2, "bottom-left corner"},
        {479, 479, 3, "bottom-right corner"},
        {239, 100, 0, "left of the vertical middle"},
        {240, 100, 1, "right of the vertical middle"},
        {240, 240, -1, "OLED centre: swipes only"},
        {240 - 95, 240, -1, "swipe-only area, left edge"},
        {240 - 96, 240, 2, "just left of the swipe-only area: a quadrant"},
        {240 + 95, 240 - 47, -1, "swipe-only area, top-right edge"},
        {240 + 95, 240 - 48, 1, "just above the swipe-only area"},
        {60, 200, 0, "outer half of the OLED: top-left"},
        {-1, 10, -2, "left of the screen"},
        {10, 480, -2, "below the screen"},
    };
    for (size_t i = 0; i < sizeof k_hits / sizeof k_hits[0]; i++) {
        char message[96];
        snprintf(message, sizeof message, "hit (%d, %d) %s: %d", k_hits[i].x, k_hits[i].y, k_hits[i].name,
                 k_hits[i].expected);
        check(message, ui_hit(k_hits[i].x, k_hits[i].y) == k_hits[i].expected);
    }
}

static void test_render_regions(void)
{
    static uint16_t before[UI_W * UI_H], after[UI_W * UI_H];
    static uint8_t oled[OLED_W * OLED_H / 8];
    static ui_view_t view = {.page = 1, .pages = 4, .link = 2, .address = 0x12, .serial_number = 0x0f05f123};
    static png_icon_t icon = {.width = 8, .height = 8};
    for (int i = 0; i < PANEL_SLOTS; i++) {
        snprintf(view.slot[i].label, sizeof view.slot[i].label, "Button %c", (char)('0' + i));
    }
    ui_render(&view);
    screen_snapshot(before, oled);
    ui_render(&view);
    screen_snapshot(after, oled);
    check("render: an unchanged view draws nothing new", memcmp(before, after, sizeof before) == 0);

    snprintf(view.slot[0].label, sizeof view.slot[0].label, "%s", "Changed");
    ui_render(&view);
    screen_snapshot(after, oled);
    check("label of slot 0 changed: only the top-left band redrawn", memcmp(before, after, sizeof before) != 0 &&
                                                                         changed_only_inside(before, after, 0, 0, UI_W / 2, 110));
    memcpy(before, after, sizeof before);

    view.slot[3].led = LEDS_RED;
    ui_render(&view);
    screen_snapshot(after, oled);
    check("LED of slot 3: only the bottom-right band", changed_only_inside(before, after, UI_W / 2, UI_H - 110, UI_W, UI_H));
    memcpy(before, after, sizeof before);

    memset(icon.colors, 0xff, sizeof icon.colors);
    memset(icon.alphas, 0xff, sizeof icon.alphas);
    view.slot[1].icon = &icon;
    ui_render(&view);
    screen_snapshot(after, oled);
    check("icon on slot 1: drawn in the top-right band", memcmp(before, after, sizeof before) != 0 &&
                                                             changed_only_inside(before, after, UI_W / 2, 0, UI_W, 110));
    memcpy(before, after, sizeof before);

    snprintf(view.slot[2].label, sizeof view.slot[2].label, "%s", "WWWWWWWWWWWWWWW\x01");
    view.slot[2].glow = 255;
    ui_render(&view);
    screen_snapshot(after, oled);
    check("16-character label (cut to fit) and press glow on slot 2: only the bottom-left band",
          memcmp(before, after, sizeof before) != 0 && changed_only_inside(before, after, 0, UI_H - 110, UI_W / 2, UI_H));
    memcpy(before, after, sizeof before);

    view.page = 3;
    ui_render(&view);
    screen_snapshot(after, oled);
    check("page change: only the page dots", memcmp(before, after, sizeof before) != 0 &&
                                                 changed_only_inside(before, after, 0, 118, UI_W, 138));
    memcpy(before, after, sizeof before);

    view.has_temperature = true;
    view.temperature_deci = 215;
    view.link = 1;
    ui_render(&view);
    screen_snapshot(after, oled);
    check("status change: only the status bar", memcmp(before, after, sizeof before) != 0 &&
                                                    changed_only_inside(before, after, 0, 341, UI_W, 365));
    memcpy(before, after, sizeof before);

    oled[0] = 0x80; // top-left OLED dot lit
    screen_snapshot(after, oled);
    check("OLED dot (0, 0): only its 3x3 pixels change",
          memcmp(before, after, sizeof before) != 0 &&
              changed_only_inside(before, after, UI_OLED_X, UI_OLED_Y, UI_OLED_X + UI_OLED_SCALE, UI_OLED_Y + UI_OLED_SCALE));
}

// ---- helpers ---------------------------------------------------------------------------------
static void check(const char *name, int condition)
{
    printf("%s %s\n", condition ? "ok  " : "FAIL", name);
    failures += !condition;
}

// The UI_W pixels read from read_start lie inside buffer.
static bool inside(const uint16_t *read_start, const uint16_t *buffer, size_t buffer_pixels)
{
    return read_start >= buffer && read_start + UI_W <= buffer + buffer_pixels;
}

// The whole screen as the scan-out shows it without a shift.
static void screen_snapshot(uint16_t *screen, const uint8_t *oled)
{
    for (int y = 0; y < UI_H; y++) {
        if (ui_row(y)) {
            memcpy(screen + y * UI_W, ui_row(y), UI_W * sizeof(uint16_t));
        } else {
            ui_compose_oled_row(screen + y * UI_W, y, oled);
        }
    }
}

// Every pixel that differs lies in left <= x < right, top <= y < bottom.
static bool changed_only_inside(const uint16_t *before, const uint16_t *after, int left, int top, int right, int bottom)
{
    for (int y = 0; y < UI_H; y++) {
        for (int x = 0; x < UI_W; x++) {
            if (before[y * UI_W + x] != after[y * UI_W + x] && (x < left || x >= right || y < top || y >= bottom)) {
                printf("  pixel (%d, %d) changed outside (%d, %d)..(%d, %d)\n", x, y, left, top, right, bottom);
                return false;
            }
        }
    }
    return true;
}

// ---- fakes -----------------------------------------------------------------------------------
uint32_t save_and_disable_interrupts(void)
{
    return 0;
}

void restore_interrupts(uint32_t status)
{
    (void)status;
}

int spin_lock_claim_unused(bool required)
{
    (void)required;
    return 0;
}

spin_lock_t *spin_lock_init(uint lock_number)
{
    (void)lock_number;
    return &g_spin_lock;
}

uint32_t spin_lock_blocking(spin_lock_t *lock)
{
    *lock = 1;
    return 0;
}

void spin_unlock(spin_lock_t *lock, uint32_t saved_interrupts)
{
    (void)saved_interrupts;
    *lock = 0;
}

void multicore_launch_core1(void (*entry)(void))
{
    g_core1_entry = entry;
}

void __sev(void)
{
}

void __wfe(void)
{
}

void pio_rgb_set_scanout_enabled(bool enabled)
{
    g_driver_enabled = enabled;
}

void pio_rgb_set_line_table(const uint16_t *const *line_table)
{
    g_driver_line_table = line_table;
}

int pio_rgb_lines_started(void)
{
    if (g_dma_advances && g_lines_started < UI_H && ++g_dma_polls % 3 == 0) {
        g_lines_started++;
    }
    return g_lines_started;
}
