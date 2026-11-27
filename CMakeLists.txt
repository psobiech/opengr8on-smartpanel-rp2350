# Grenton SMART_PANEL_FM_4 emulator for the Waveshare RP2350-Touch-LCD-4.
#
#   cmake -B build -DPICO_SDK_PATH=~/pico-sdk [-DPANEL_SN=0x0f05xxxx] [-DPANEL_DISPLAY_BRIGHTNESS=70] [-DPANEL_DISPLAY_DIM_BRIGHTNESS=5]
#              [-DPANEL_DISPLAY_BLANK=OFF]
#   cmake --build build -j       -> build/smartpanel.uf2
cmake_minimum_required(VERSION 3.13)

# Board definition + display/touch/IMU drivers vendored from Waveshare's demo package (board/README.md).
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/board)
set(PICO_BOARD waveshare_rp2350_touch_lcd_4)

include(pico_sdk_import.cmake)
project(smartpanel_rp2350 C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_library(bsp STATIC
    board/bsp/bsp_st7701.c board/bsp/pio_rgb.c board/bsp/bsp_dma_channel_irq.c
    board/bsp/bsp_gt911.c board/bsp/bsp_i2c.c board/bsp/bsp_qmi8658.c)
target_include_directories(bsp PUBLIC board/bsp)
pico_generate_pio_header(bsp ${CMAKE_CURRENT_LIST_DIR}/board/bsp/pio_rgb.pio)
target_compile_options(bsp PRIVATE -w) # vendor code, unmodified
target_link_libraries(bsp PUBLIC pico_stdlib hardware_spi hardware_i2c hardware_pwm hardware_dma
                      hardware_pio hardware_irq)

add_executable(smartpanel src/main.c src/ui.c core/panel_core.c)
target_include_directories(smartpanel PRIVATE core src)
target_compile_options(smartpanel PRIVATE -Wall -Wextra -Wno-unused-parameter)
set(PANEL_SN "0" CACHE STRING "Serial number (0 = derive 0x0f05fxxx from the flash unique ID)")
set(PANEL_DISPLAY_BRIGHTNESS "70" CACHE STRING "Backlight while the display is awake, percent (up to 100)")
set(PANEL_DISPLAY_DIM_BRIGHTNESS "5" CACHE STRING "Backlight while the display is dimmed, percent (0..PANEL_DISPLAY_BRIGHTNESS)")
option(PANEL_DISPLAY_BLANK "Blank the display 10 s after it starts dimming (ProximityTimeout without a touch); OFF: stay dimmed" ON)
target_compile_definitions(smartpanel PRIVATE PANEL_SN=${PANEL_SN} PANEL_DISPLAY_BRIGHTNESS=${PANEL_DISPLAY_BRIGHTNESS}
                           PANEL_DISPLAY_DIM_BRIGHTNESS=${PANEL_DISPLAY_DIM_BRIGHTNESS}
                           PANEL_DISPLAY_BLANK=$<BOOL:${PANEL_DISPLAY_BLANK}> PICO_EMBED_XIP_SETUP=1)
target_link_libraries(smartpanel pico_stdlib pico_unique_id hardware_uart hardware_pwm hardware_adc
                      hardware_irq hardware_sync bsp m)
pico_enable_stdio_usb(smartpanel 1)
pico_enable_stdio_uart(smartpanel 0)
pico_add_extra_outputs(smartpanel)
