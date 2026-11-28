// Host stand-in for FatFS (ff.h) as src/icons.c uses it; test_icons.c fakes the functions.
#pragma once

#include <stdint.h>

typedef unsigned int UINT;
typedef uint64_t FSIZE_t;
typedef unsigned char BYTE;

typedef enum {
    FR_OK = 0,
    FR_DISK_ERR,
    FR_INT_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_INVALID_NAME,
    FR_DENIED,
    FR_EXIST,
    FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE,
    FR_NOT_ENABLED,
    FR_NO_FILESYSTEM,
} FRESULT;

typedef struct {
    int mounted;
} FATFS;

typedef struct {
    const uint8_t *data;
    FSIZE_t size;
} FIL;

#define FA_READ 0x01
#define f_size(file) ((file)->size)

FRESULT f_mount(FATFS *filesystem, const char *path, BYTE option);
FRESULT f_unmount(const char *path);
FRESULT f_open(FIL *file, const char *path, BYTE mode);
FRESULT f_read(FIL *file, void *buffer, UINT bytes_to_read, UINT *bytes_read);
FRESULT f_close(FIL *file);
