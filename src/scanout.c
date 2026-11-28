#include "scanout.h"

#include <string.h>

#include "hardware/sync.h"
#include "pico/multicore.h"
#include "ui.h"

// Ring of composed OLED rows: RING_LINES rows of slack for core1 (~32 us per row at 16 MHz).
#define RING_LINES  8 // power of two
#define LINE_MARGIN SCANOUT_SHIFT_MAX
#define LINE_STRIDE (UI_W + 2 * LINE_MARGIN)

typedef struct {
    const uint16_t *rows[UI_H + 1]; // read address per screen row, then NULL (end of frame)
    int shift_y;                    // screen row y shows stored row y + shift_y
} line_table_t;

// Two tables: the DMA reads one while scanout_set_shift rebuilds the other.
static line_table_t s_line_tables[2];
static volatile uint8_t s_table_sending; // the table the current frame runs from
static volatile uint8_t s_table_queued;  // the table the next frame starts from (pio_rgb line_table)
static volatile uint32_t s_frame_count;
static volatile uint32_t s_late_lines;
static volatile bool s_stopped; // scan-out stopped: core1 gives up the frame and waits for a new one

static uint16_t s_ring[RING_LINES][LINE_STRIDE];
static uint16_t s_black_row[LINE_STRIDE]; // rows shifted in from outside the screen

// OLED handover: core0 writes s_oled_shared, core1 copies it at every frame start.
static spin_lock_t *s_oled_lock;
static uint8_t s_oled_shared[OLED_W * OLED_H / 8];
static uint8_t s_oled_frame[OLED_W * OLED_H / 8]; // core1: the OLED of the frame being sent

static void frame_started(void);
static void core1_main(void);
static void compose_oled_rows(const line_table_t *table, uint32_t frame);
static void build_line_table(line_table_t *table, int dx, int dy);

void scanout_init(pio_rgb_info_t *rgb_info)
{
    s_oled_lock = spin_lock_init(spin_lock_claim_unused(true));
    build_line_table(&s_line_tables[0], 0, 0);
    s_table_sending = s_table_queued = 0;
    rgb_info->line_table = s_line_tables[0].rows;
    rgb_info->dma_flush_done_cb = frame_started;
}

void scanout_start(void)
{
    multicore_launch_core1(core1_main);
}

void scanout_set_enabled(bool enabled)
{
    s_stopped = !enabled;
    pio_rgb_set_scanout_enabled(enabled); // a restart starts a frame (frame_started)
}

void scanout_set_oled(const uint8_t *oled)
{
    uint32_t saved_interrupts = spin_lock_blocking(s_oled_lock);
    memcpy(s_oled_shared, oled, sizeof s_oled_shared);
    spin_unlock(s_oled_lock, saved_interrupts);
}

void scanout_set_shift(int dx, int dy)
{
    // interrupts off: no frame can start (DMA IRQ) while the queued table is being written
    uint32_t saved_interrupts = save_and_disable_interrupts();
    uint8_t target = (uint8_t)(1 - s_table_sending);
    build_line_table(&s_line_tables[target], dx, dy);
    s_table_queued = target;
    pio_rgb_set_line_table(s_line_tables[target].rows);
    restore_interrupts(saved_interrupts);
}

uint32_t scanout_late_lines(void)
{
    return s_late_lines;
}

// pio_rgb frame start (DMA IRQ on core0, right after the frame was started from the queued table).
static void __not_in_flash_func(frame_started)(void)
{
    s_table_sending = s_table_queued;
    s_frame_count++;
    __sev(); // core1 waits for the frame start in WFE
}

static void __not_in_flash_func(core1_main)(void)
{
    uint32_t frame_seen = s_frame_count;
    for (;;) {
        while (s_frame_count == frame_seen) {
            __wfe(); // also while the scan-out is stopped (display blank)
        }
        frame_seen = s_frame_count;
        uint32_t saved_interrupts = spin_lock_blocking(s_oled_lock);
        memcpy(s_oled_frame, s_oled_shared, sizeof s_oled_frame);
        spin_unlock(s_oled_lock, saved_interrupts);
        compose_oled_rows(&s_line_tables[s_table_sending], frame_seen);
    }
}

// Every OLED row of the frame into its ring line, each one after the DMA has finished with the
// line's previous row and before it starts this one. A row the DMA has already started is skipped
// (it shows the ring line's old content for this frame) and counted.
static void __not_in_flash_func(compose_oled_rows)(const line_table_t *table, uint32_t frame)
{
    for (int row = UI_OLED_Y; row < UI_OLED_Y + UI_OLED_H; row++) {
        int screen_row = row - table->shift_y;
        // the line's previous row (row - RING_LINES) is read once the DMA has started the one after it
        int previous_row_done_at = screen_row - RING_LINES + 2;
        while (row - RING_LINES >= UI_OLED_Y && pio_rgb_lines_started() < previous_row_done_at) {
            if (s_frame_count != frame || s_stopped) {
                return; // core1 fell a whole frame behind, or the frame will not finish (blank)
            }
        }
        if (pio_rgb_lines_started() > screen_row) {
            s_late_lines++;
            continue;
        }
        uint16_t *line = s_ring[(row - UI_OLED_Y) & (RING_LINES - 1)];
        ui_compose_oled_row(line + LINE_MARGIN, row, s_oled_frame);
        for (int i = 0; i < LINE_MARGIN; i++) { // a horizontally shifted row repeats its edge pixels
            line[i] = line[LINE_MARGIN];
            line[LINE_MARGIN + UI_W + i] = line[LINE_MARGIN + UI_W - 1];
        }
    }
}

static void build_line_table(line_table_t *table, int dx, int dy)
{
    for (int screen_row = 0; screen_row < UI_H; screen_row++) {
        int row = screen_row + dy;
        const uint16_t *source;
        if (row < 0 || row >= UI_H) {
            source = s_black_row + LINE_MARGIN;
        } else if (ui_row(row)) {
            source = ui_row(row); // the framebuffer has SCANOUT_SHIFT_MAX pixels of margin
        } else {
            source = s_ring[(row - UI_OLED_Y) & (RING_LINES - 1)] + LINE_MARGIN;
        }
        table->rows[screen_row] = source + dx;
    }
    table->rows[UI_H] = NULL;
    table->shift_y = dy;
}
