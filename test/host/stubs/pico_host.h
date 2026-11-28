// Host stand-in for the Pico SDK headers the firmware sources include (src/main.c, src/scanout.c,
// src/icons.c and the board headers they pull in). Only declarations: each host test defines the
// functions it needs as fakes that record the calls (test_main.c, test_scanout.c, test_icons.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef unsigned int uint;

#define __not_in_flash(group)
#define __not_in_flash_func(function_name) function_name

// pico/time.h
typedef int32_t alarm_id_t;
typedef int64_t (*alarm_callback_t)(alarm_id_t id, void *user_data);
typedef struct {
    uint64_t _private_us_since_boot;
} absolute_time_t;
uint64_t time_us_64(void);
void sleep_us(uint64_t delay_us);
void sleep_ms(uint32_t delay_ms);
alarm_id_t add_alarm_in_us(uint64_t delay_us, alarm_callback_t callback, void *user_data, bool fire_if_past);
bool cancel_alarm(alarm_id_t alarm_id);
absolute_time_t make_timeout_time_us(uint64_t delay_us);
bool best_effort_wfe_or_timeout(absolute_time_t timeout_timestamp);

// pico/stdlib.h, pico/platform
bool stdio_init_all(void);
void panic(const char *format, ...);
void __sev(void);
void __wfe(void);

// hardware/clocks.h
enum clock_index { clk_gpout0 = 0, clk_ref, clk_sys, clk_peri };
#define CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS 1
bool set_sys_clock_khz(uint32_t frequency_khz, bool required);
bool clock_configure(enum clock_index clock, uint32_t source, uint32_t auxiliary_source, uint32_t source_frequency,
                     uint32_t frequency);
uint32_t clock_get_hz(enum clock_index clock);

// hardware/gpio.h
enum gpio_function { GPIO_FUNC_UART = 2, GPIO_FUNC_I2C = 3, GPIO_FUNC_PWM = 4 };
#define GPIO_OUT 1
void gpio_init(uint gpio);
void gpio_put(uint gpio, bool value);
void gpio_set_dir(uint gpio, bool out);
void gpio_set_function(uint gpio, enum gpio_function function);

// hardware/pwm.h
uint pwm_gpio_to_slice_num(uint gpio);
uint pwm_gpio_to_channel(uint gpio);
void pwm_set_chan_level(uint slice, uint channel, uint16_t level);
void pwm_set_enabled(uint slice, bool enabled);
void pwm_set_clkdiv(uint slice, float divider);
void pwm_set_wrap(uint slice, uint16_t wrap);

// hardware/uart.h
typedef struct {
    uint32_t dr;
} uart_hw_t;
typedef struct uart_inst uart_inst_t;
extern uart_inst_t *const uart1;
enum uart_parity { UART_PARITY_NONE };
#define UART1_IRQ 34
uint uart_init(uart_inst_t *uart, uint baudrate);
void uart_set_format(uart_inst_t *uart, uint data_bits, uint stop_bits, enum uart_parity parity);
void uart_set_fifo_enabled(uart_inst_t *uart, bool enabled);
void uart_set_irq_enables(uart_inst_t *uart, bool rx_has_data, bool tx_needs_data);
bool uart_is_readable(uart_inst_t *uart);
uart_hw_t *uart_get_hw(uart_inst_t *uart);
void uart_write_blocking(uart_inst_t *uart, const uint8_t *source, size_t length);

// hardware/irq.h
typedef void (*irq_handler_t)(void);
void irq_set_exclusive_handler(uint irq, irq_handler_t handler);
void irq_set_priority(uint irq, uint8_t priority);
void irq_set_enabled(uint irq, bool enabled);

// hardware/sync.h
typedef volatile uint32_t spin_lock_t;
uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t status);
int spin_lock_claim_unused(bool required);
spin_lock_t *spin_lock_init(uint lock_number);
uint32_t spin_lock_blocking(spin_lock_t *lock);
void spin_unlock(spin_lock_t *lock, uint32_t saved_interrupts);

// hardware/adc.h
#define ADC_TEMPERATURE_CHANNEL_NUM 8
void adc_init(void);
void adc_set_temp_sensor_enabled(bool enable);
void adc_select_input(uint input);
uint16_t adc_read(void);

// hardware/i2c.h
typedef struct i2c_inst i2c_inst_t;
extern i2c_inst_t *const i2c1;
uint i2c_init(i2c_inst_t *i2c, uint baudrate);
int i2c_write_timeout_us(i2c_inst_t *i2c, uint8_t address, const uint8_t *source, size_t length, bool nostop,
                         uint timeout_us);
int i2c_read_timeout_us(i2c_inst_t *i2c, uint8_t address, uint8_t *destination, size_t length, bool nostop,
                        uint timeout_us);

// hardware/gpio.h (inputs, interrupts)
#define GPIO_IN 0
enum gpio_irq_level { GPIO_IRQ_LEVEL_LOW = 1, GPIO_IRQ_LEVEL_HIGH = 2, GPIO_IRQ_EDGE_FALL = 4, GPIO_IRQ_EDGE_RISE = 8 };
typedef void (*gpio_irq_callback_t)(uint gpio, uint32_t event_mask);
void gpio_pull_up(uint gpio);
bool gpio_get(uint gpio);
void gpio_set_irq_enabled_with_callback(uint gpio, uint32_t event_mask, bool enabled, gpio_irq_callback_t callback);

// pico/unique_id.h
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8
typedef struct {
    uint8_t id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES];
} pico_unique_board_id_t;
void pico_get_unique_board_id(pico_unique_board_id_t *board_id);

// pico/multicore.h
void multicore_launch_core1(void (*entry)(void));
