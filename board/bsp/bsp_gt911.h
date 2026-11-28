#ifndef __BSP_GT911_H__
#define __BSP_GT911_H__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "bsp_touch.h"

#define BSP_GT911_RST_PIN 16
#define BSP_GT911_INT_PIN 17

#define GT911_LCD_TOUCH_MAX_POINTS (5)

#define GT911_DEVICE_ADDR 0x5D
// #define GT911_ID 0x03

typedef enum
{
    GT911_REG_CONFIG = 0x8047,
    GT911_REG_PRODUCT_ID = 0x8140,
    GT911_EREG_READ_XY = 0x814E,
} gt911_reg_t;

bool bsp_touch_new_gt911(bsp_touch_interface_t **interface, bsp_touch_info_t *info);
bsp_touch_interface_t *bsp_gt911_get_touch_interface(void);
uint64_t bsp_gt911_last_report_us(void); // time of the last GT911 report (touch or release)

// opengr8on: set Screen_Touch_Level / Screen_Leave_Level (touch sensitivity, lower = more sensitive;
// leave < touch). The rest of the config is kept as read from the chip. Written only when they
// differ: the GT911 keeps its config across power cycles.
bool bsp_gt911_set_touch_levels(uint8_t touch_level, uint8_t leave_level);

#endif