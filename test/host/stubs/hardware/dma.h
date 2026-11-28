// Host stand-in for hardware/dma.h as board/bsp/pio_rgb.c and bsp_dma_channel_irq.c use it.
#pragma once
#include "pico_host.h"

typedef struct {
    volatile uintptr_t read_addr; // host: pointer-sized (32-bit register on the RP2350)
    volatile uint32_t write_addr;
    volatile uint32_t transfer_count;
    volatile uint32_t al1_ctrl;
    volatile uint32_t al3_read_addr_trig;
} dma_channel_hw_t;
typedef struct {
    dma_channel_hw_t ch[16];
} dma_hw_t;
extern dma_hw_t g_fake_dma_hw;
#define dma_hw (&g_fake_dma_hw)
#define DMA_CH0_CTRL_TRIG_EN_BITS 0x00000001u
#define DMA_IRQ_0 10
#define DMA_IRQ_1 11

enum dma_channel_transfer_size { DMA_SIZE_8 = 0, DMA_SIZE_16 = 1, DMA_SIZE_32 = 2 };
typedef struct {
    enum dma_channel_transfer_size data_size;
    bool read_increment, write_increment, irq_quiet;
    uint chain_to, dreq;
} dma_channel_config;

dma_channel_config dma_channel_get_default_config(uint channel);
void channel_config_set_transfer_data_size(dma_channel_config *config, enum dma_channel_transfer_size size);
void channel_config_set_read_increment(dma_channel_config *config, bool increment);
void channel_config_set_write_increment(dma_channel_config *config, bool increment);
void channel_config_set_dreq(dma_channel_config *config, uint dreq);
void channel_config_set_chain_to(dma_channel_config *config, uint chain_to);
void channel_config_set_irq_quiet(dma_channel_config *config, bool irq_quiet);
void dma_channel_configure(uint channel, const dma_channel_config *config, volatile void *write_address,
                           const volatile void *read_address, uint transfer_count, bool trigger);
void dma_channel_claim(uint channel);
int dma_claim_unused_channel(bool required);
void dma_channel_set_read_addr(uint channel, const volatile void *read_address, bool trigger);
void dma_channel_abort(uint channel);
void dma_channel_set_irq1_enabled(uint channel, bool enabled);
bool dma_channel_get_irq1_status(uint channel);
void dma_channel_acknowledge_irq1(uint channel);
void dma_irqn_set_channel_enabled(uint irq_index, uint channel, bool enabled);
void dma_irqn_acknowledge_channel(uint irq_index, uint channel);
void hw_set_bits(volatile uint32_t *address, uint32_t mask);
void hw_clear_bits(volatile uint32_t *address, uint32_t mask);
