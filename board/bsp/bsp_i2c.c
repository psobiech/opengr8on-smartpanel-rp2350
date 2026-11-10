#include "bsp_i2c.h"

#include "hardware/i2c.h"

// opengr8on: bounded transfers with checked results. The vendor code used the *_blocking calls
// (hang forever on a stuck bus) and ignored errors, so callers converted uninitialised buffers
// (odd temperatures). ~23 us per byte at 400 kHz; allow generous slack.
#define I2C_TIMEOUT_US(n) (2000u + 100u * (unsigned)(n))

static bool wr(uint8_t addr, const uint8_t *b, size_t n, bool nostop)
{
    return i2c_write_timeout_us(BSP_I2C_NUM, addr, b, n, nostop, I2C_TIMEOUT_US(n)) == (int)n;
}

static bool rd(uint8_t addr, uint8_t *b, size_t n)
{
    return i2c_read_timeout_us(BSP_I2C_NUM, addr, b, n, false, I2C_TIMEOUT_US(n)) == (int)n;
}

bool bsp_i2c_write(uint8_t device_addr, uint8_t *buffer, size_t len)
{
    return wr(device_addr, buffer, len, false);
}

bool bsp_i2c_write_reg8(uint8_t device_addr, uint8_t reg_addr, uint8_t *buffer, size_t len)
{
    uint8_t write_buffer[len + 1];
    write_buffer[0] = reg_addr;
    memcpy(write_buffer + 1, buffer, len);
    return wr(device_addr, write_buffer, len + 1, false);
}

bool bsp_i2c_read_reg8(uint8_t device_addr, uint8_t reg_addr, uint8_t *buffer, size_t len)
{
    return wr(device_addr, &reg_addr, 1, true) && rd(device_addr, buffer, len);
}

bool bsp_i2c_write_reg16(uint8_t device_addr, uint16_t reg_addr, uint8_t *buffer, size_t len)
{
    uint8_t write_buffer[len + 2];
    write_buffer[0] = (uint8_t)(reg_addr >> 8);
    write_buffer[1] = (uint8_t)(reg_addr);
    memcpy(write_buffer + 2, buffer, len);
    return wr(device_addr, write_buffer, len + 2, false);
}

bool bsp_i2c_read_reg16(uint8_t device_addr, uint16_t reg_addr, uint8_t *buffer, size_t len)
{
    uint8_t write_buffer[2];
    write_buffer[0] = (uint8_t)(reg_addr >> 8);
    write_buffer[1] = (uint8_t)(reg_addr);
    return wr(device_addr, write_buffer, 2, true) && rd(device_addr, buffer, len);
}

void bsp_i2c_init(void)
{
    i2c_init(BSP_I2C_NUM, 400 * 1000);
    gpio_set_function(BSP_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BSP_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(BSP_I2C_SDA_PIN);
    gpio_pull_up(BSP_I2C_SCL_PIN);
}
