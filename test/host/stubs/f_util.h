// Host stand-in for the FatFS driver's f_util.h (FRESULT_str); test_icons.c defines it.
#pragma once

#include "ff.h"

const char *FRESULT_str(FRESULT result);
