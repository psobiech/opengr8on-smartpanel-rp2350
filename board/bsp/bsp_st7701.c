#include "bsp_st7701.h"
#include "hardware/clocks.h"

#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

#include "pio_rgb.h"

static bsp_display_interface_t *g_display_if;
static bsp_display_info_t *g_display_info;

static uint slice_num;
static uint pwm_channel;

/**
 * @brief LCD backlight brightness control
 */
static void bsp_lcd_brightness_init(void)
{
    float sys_clk = clock_get_hz(clk_sys);
    gpio_set_function(BSP_LCD_BL_PIN, GPIO_FUNC_PWM);
    // Find out which PWM slice is connected to GPIO 0 (it's slice 0)
    slice_num = pwm_gpio_to_slice_num(BSP_LCD_BL_PIN);

    pwm_channel = pwm_gpio_to_channel(BSP_LCD_BL_PIN);

    pwm_set_clkdiv(slice_num, sys_clk / (PWM_FREQ * PWM_WRAP));

    pwm_set_wrap(slice_num, PWM_WRAP);

    pwm_set_chan_level(slice_num, pwm_channel, 0);

    pwm_set_enabled(slice_num, true);
}

/**
 * @brief Set the brightness of the LCD backlight
 */
static void bsp_lcd_set_brightness(uint8_t percent)
{
    if (percent > 100)
    {
        percent = 100;
    }
    pwm_set_chan_level(slice_num, pwm_channel, PWM_WRAP / 100 * (100-percent));

    g_display_info->brightness = percent;
}

/**
 * @brief Reset the LCD
 */
static void bsp_st7701_reset(void)
{
    gpio_put(BSP_LCD_RST_PIN, 1);
    sleep_ms(20);
    gpio_put(BSP_LCD_RST_PIN, 0);
    sleep_ms(20);
    gpio_put(BSP_LCD_RST_PIN, 1);
    sleep_ms(200);
}

bsp_st7701_cmd_t init_cmds[] = {
    // 中文：初始化序列开始
    // English: Initial sequence start
    {.reg = 0xFF, .data = (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, .data_bytes = 5, .delay_ms = 0},
    {.reg = 0xC0, .data = (uint8_t[]){0x3B, 0x00}, .data_bytes = 2, .delay_ms = 0},
    {.reg = 0xC1, .data = (uint8_t[]){0x0D, 0x02}, .data_bytes = 2, .delay_ms = 0},
    {.reg = 0xC2, .data = (uint8_t[]){0x31, 0x05}, .data_bytes = 2, .delay_ms = 0},
    {.reg = 0xCD, .data = (uint8_t[]){0x08}, .data_bytes = 1, .delay_ms = 0},
    
    // 中文：B0-B1 伽马控制命令
    // English: B0-B1 Gamma Control commands
    {.reg = 0xB0, .data = (uint8_t[]){0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, .data_bytes = 16, .delay_ms = 0},  // Positive Voltage Gamma Control
    {.reg = 0xB1, .data = (uint8_t[]){0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, .data_bytes = 16, .delay_ms = 0},  // Negative Voltage Gamma Control
    
    // 中文：PAGE1 配置
    // English: PAGE1 configuration
    {.reg = 0xFF, .data = (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, .data_bytes = 5, .delay_ms = 0},
    {.reg = 0xB0, .data = (uint8_t[]){0x60}, .data_bytes = 1, .delay_ms = 0},  // Vop=4.7375v
    {.reg = 0xB1, .data = (uint8_t[]){0x32}, .data_bytes = 1, .delay_ms = 0},  // VCOM=32
    {.reg = 0xB2, .data = (uint8_t[]){0x07}, .data_bytes = 1, .delay_ms = 0},  // VGH=15v
    {.reg = 0xB3, .data = (uint8_t[]){0x80}, .data_bytes = 1, .delay_ms = 0},
    {.reg = 0xB5, .data = (uint8_t[]){0x49}, .data_bytes = 1, .delay_ms = 0},  // VGL=-10.17v
    {.reg = 0xB7, .data = (uint8_t[]){0x85}, .data_bytes = 1, .delay_ms = 0},
    {.reg = 0xB8, .data = (uint8_t[]){0x21}, .data_bytes = 1, .delay_ms = 0},  // AVDD=6.6 & AVCL=-4.6
    {.reg = 0xC1, .data = (uint8_t[]){0x78}, .data_bytes = 1, .delay_ms = 0},
    {.reg = 0xC2, .data = (uint8_t[]){0x78}, .data_bytes = 1, .delay_ms = 0},
    
    // 中文：E0-E8 扩展命令配置
    // English: E0-E8 extended command configuration
    {.reg = 0xE0, .data = (uint8_t[]){0x00, 0x1B, 0x02}, .data_bytes = 3, .delay_ms = 0},
    {.reg = 0xE1, .data = (uint8_t[]){0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44}, .data_bytes = 11, .delay_ms = 0},
    {.reg = 0xE2, .data = (uint8_t[]){0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00}, .data_bytes = 12, .delay_ms = 0},
    {.reg = 0xE3, .data = (uint8_t[]){0x00, 0x00, 0x11, 0x11}, .data_bytes = 4, .delay_ms = 0},
    {.reg = 0xE4, .data = (uint8_t[]){0x44, 0x44}, .data_bytes = 2, .delay_ms = 0},
    {.reg = 0xE5, .data = (uint8_t[]){0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, .data_bytes = 16, .delay_ms = 0},
    {.reg = 0xE6, .data = (uint8_t[]){0x00, 0x00, 0x11, 0x11}, .data_bytes = 4, .delay_ms = 0},
    {.reg = 0xE7, .data = (uint8_t[]){0x44, 0x44}, .data_bytes = 2, .delay_ms = 0},
    {.reg = 0xE8, .data = (uint8_t[]){0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, .data_bytes = 16, .delay_ms = 0},
    {.reg = 0xEB, .data = (uint8_t[]){0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, .data_bytes = 7, .delay_ms = 0},
    {.reg = 0xEC, .data = (uint8_t[]){0x3C, 0x00}, .data_bytes = 2, .delay_ms = 0},
    {.reg = 0xED, .data = (uint8_t[]){0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, .data_bytes = 16, .delay_ms = 0},
    
    // 中文：VAP & VAN 配置
    // English: VAP & VAN configuration
    {.reg = 0xFF, .data = (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, .data_bytes = 5, .delay_ms = 0},
    {.reg = 0xE5, .data = (uint8_t[]){0xE4}, .data_bytes = 1, .delay_ms = 0},
    {.reg = 0xFF, .data = (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, .data_bytes = 5, .delay_ms = 0},
    
    // 中文：显示模式配置
    // English: Display mode configuration
    {.reg = 0x21, .data = (uint8_t[]){}, .data_bytes = 0, .delay_ms = 0},  // IPS mode (0x21)
    {.reg = 0x3A, .data = (uint8_t[]){0x60}, .data_bytes = 1, .delay_ms = 0},  // RGB666 format
    {.reg = 0x11, .data = (uint8_t[]){}, .data_bytes = 0, .delay_ms = 120},  // Sleep Out (delay 120ms)
    
    // 中文：显示开启
    // English: Display On
    {.reg = 0x29, .data = (uint8_t[]){}, .data_bytes = 0, .delay_ms = 120},
};

void bsp_st7701_spi_write(uint16_t data)
{
    // 中文：从第 8 位到第 0 位
    // English: from the 8th to the 0th
    for (int i = 8; i >= 0; i--)
    { 
        // 中文：设置 MOSI
        // English: Set MOSI
        if (data & (1 << i))
        {
            gpio_put(BSP_LCD_SDA_PIN, 1);
        }
        else
        {
            gpio_put(BSP_LCD_SDA_PIN, 0);
        }

        // 中文：产生 SCK 脉冲
        // English: Generate SCK pulse
        gpio_put(BSP_LCD_SCK_PIN, 1);
        sleep_us(100);
        gpio_put(BSP_LCD_SCK_PIN, 0);
        sleep_us(100);
    }
}

void bsp_st7701_spi_write_reg(uint8_t reg)
{
    uint16_t buf = reg | 0x0000;
    bsp_st7701_spi_write(buf);
}

void bsp_st7701_spi_write_data(uint8_t data)
{
    uint16_t buf = (uint16_t)data | 0x0100;
    bsp_st7701_spi_write(buf);
}

void bsp_st7701_spi_tx_cmd(bsp_st7701_cmd_t *cmds, size_t cmd_len)
{
    for (int i = 0; i < cmd_len; i++)
    {
        gpio_put(BSP_LCD_CS_PIN, 0);
        bsp_st7701_spi_write_reg(cmds[i].reg);
        for (int j = 0; j < cmds[i].data_bytes; j++)
        {
            bsp_st7701_spi_write_data(cmds[i].data[j]);
        }
        if (cmds[i].delay_ms > 0)
        {
            sleep_ms(cmds[i].delay_ms);
        }
        gpio_put(BSP_LCD_CS_PIN, 1);
    }
}

void bsp_st7701_spi_init(void)
{
    gpio_init(BSP_LCD_CS_PIN);
    gpio_init(BSP_LCD_SCK_PIN);
    gpio_init(BSP_LCD_SDA_PIN);
    gpio_init(BSP_LCD_RST_PIN);

    gpio_set_dir(BSP_LCD_CS_PIN, GPIO_OUT);
    gpio_set_dir(BSP_LCD_SCK_PIN, GPIO_OUT);
    gpio_set_dir(BSP_LCD_SDA_PIN, GPIO_OUT);
    gpio_set_dir(BSP_LCD_RST_PIN, GPIO_OUT);
    gpio_put(BSP_LCD_CS_PIN, 1);

    bsp_st7701_reset();

    bsp_st7701_spi_tx_cmd(init_cmds, sizeof(init_cmds) / sizeof(bsp_st7701_cmd_t));
}

/**
 * @brief Initialize the LCD
 */
static void bsp_st7701_init(void)
{
    // Init pins

    // gpio_set_dir(BSP_LCD_EN_PIN, GPIO_OUT);
    // gpio_put(BSP_LCD_EN_PIN, 1);
    bsp_st7701_spi_init();

    // Init brightness
    bsp_lcd_brightness_init();
    // Set brightness
    bsp_lcd_set_brightness(g_display_info->brightness);

    pio_rgb_pin_t pin;
    pio_rgb_info_t *rgb_info = (pio_rgb_info_t *)g_display_info->user_data;

    // printf("pio_rgb_init framebuffer1:0x%x\r\n", rgb_info->framebuffer1);
    // printf("pio_rgb_init framebuffer2:0x%x\r\n", rgb_info->framebuffer2);

    if (rgb_info->framebuffer1 == NULL)
    {
        printf("Error: Framebuffer1 is NULL\r\n");
        return;
    }
    for (size_t i = 0; i < rgb_info->width * rgb_info->height; i++)
    {
        rgb_info->framebuffer1[i] = 0xffff;
    }

    // double buffer mode
    if (rgb_info->mode.double_buffer)
    {
        if (rgb_info->framebuffer2 == NULL)
        {
            printf("Error: Framebuffer2 is NULL\r\n");
            return;
        }
    }

    // psram mode
    if (rgb_info->mode.enabled_psram)
    {
        if (rgb_info->transfer_buffer1 == NULL && rgb_info->transfer_buffer2 == NULL)
        {
            printf("Error: Transfer buffer1 or buffer2 is NULL\r\n");
            return;
        }
    }

    // rgb_info->pclk_freq = BSP_LCD_PCLK_FREQ;
    pin.data0_pin = BSP_LCD_DATA0_PIN;
    pin.de_pin = BSP_LCD_DE_PIN;
    pin.hsync_pin = BSP_LCD_HSYNC_PIN;
    pin.plck_pin = BSP_LCD_PLCK_PIN;
    pin.vsync_pin = BSP_LCD_VSYNC_PIN;

    pio_rgb_init(rgb_info, &pin);
}

void bsp_st7701_flush_dma(bsp_display_area_t *area, uint16_t *color_p)
{
    if (area == NULL && color_p == NULL)
    {
        pio_rgb_change_framebuffer();
    }
    else
    {
        pio_rgb_update_framebuffer(area->x1, area->y1, area->x2, area->y2, color_p);
    }
}

bool bsp_display_new_st7701(bsp_display_interface_t **interface, bsp_display_info_t *info)
{

    // check parameters
    if (info == NULL)
        return false;

    static bsp_display_interface_t display_if;
    static bsp_display_info_t display_info;

    memcpy(&display_info, info, sizeof(bsp_display_info_t));

    display_if.init = bsp_st7701_init;
    display_if.reset = bsp_st7701_reset;
    display_if.set_brightness = bsp_lcd_set_brightness;
    display_if.flush_dma = bsp_st7701_flush_dma;

    *interface = &display_if;
    g_display_if = &display_if;
    g_display_info = &display_info;
    return true;
}
