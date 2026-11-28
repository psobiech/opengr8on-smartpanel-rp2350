// Host stand-in for the header pioasm generates from board/bsp/pio_rgb.pio (programs and their
// default configs); test_display.c defines them.
#pragma once
#include "hardware/pio.h"

extern const pio_program_t hsync_program, vsync_program, rgb_de_program, rgb_program;
pio_sm_config hsync_program_get_default_config(uint offset);
pio_sm_config vsync_program_get_default_config(uint offset);
pio_sm_config rgb_de_program_get_default_config(uint offset);
pio_sm_config rgb_program_get_default_config(uint offset);
