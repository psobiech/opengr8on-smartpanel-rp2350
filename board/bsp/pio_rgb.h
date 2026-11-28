#ifndef __PIO_RGB_H__
#define __PIO_RGB_H__

#include "pico/stdlib.h"
#include <stdio.h>
#include "pico/stdlib.h"
#include "bsp_dma_channel_irq.h"

typedef struct  {

// public
    uint16_t width;
    uint16_t height;
    uint16_t *framebuffer1;
    uint16_t *framebuffer2;
    uint16_t *transfer_buffer1;
    uint16_t *transfer_buffer2;
    size_t transfer_size;
    size_t pclk_freq;
    channel_irq_callback_t dma_flush_done_cb;
    // opengr8on: line table mode (framebuffer1 / the modes below unused): `height` read addresses,
    // one per screen row, then NULL. Each frame the DMA sends `width` pixels from every address in
    // turn (a control DMA channel feeds them to the pixel channel). dma_flush_done_cb runs at every
    // frame start, after the frame was started from line_table (DMA IRQ). Replace the table with
    // pio_rgb_set_line_table().
    const uint16_t *const *line_table;
    struct {
        bool double_buffer;
        bool enabled_transfer;
        bool enabled_psram;
    } mode;
// private
    uint16_t *_framebuffer;
    bool change_framebuffer_flag;
    uint16_t transfer_index;
    uint16_t transfer_index_max;

} pio_rgb_info_t;


typedef struct 
{
    uint hsync_pin;
    uint vsync_pin;
    uint plck_pin;
    uint de_pin;
    uint data0_pin;
} pio_rgb_pin_t;



#ifdef __cplusplus
extern "C" {
#endif 

void pio_rgb_init(pio_rgb_info_t *info, pio_rgb_pin_t *pin);

/**
 * @brief 切换帧缓冲区
 * Switching frame buffers
 */
void pio_rgb_change_framebuffer(void);

/**
 * @brief 获取空闲的帧缓冲区
 * Get free frame buffer
 */

 uint16_t *pio_rgb_get_free_framebuffer(void);


/**
 * @brief 更新帧缓冲区的内容
 * Update the contents of the frame buffer
 */
void pio_rgb_update_framebuffer(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t *color_p);

/**
 * opengr8on: stop (false) / restart (true) the RGB scan-out (PIO state machines + DMA).
 * Restart begins with a new frame.
 */
void pio_rgb_set_scanout_enabled(bool enabled);

/**
 * opengr8on: line table mode. The table the next frame starts from (call with interrupts disabled
 * when the DMA IRQ could start a frame in between).
 */
void pio_rgb_set_line_table(const uint16_t *const *line_table);

/**
 * opengr8on: line table mode. Rows of the current frame whose DMA has started (0..height): row
 * n - 1 is being sent, rows below it have been read. Safe to call from the other core.
 */
int pio_rgb_lines_started(void);


#ifdef __cplusplus
}
#endif

#endif