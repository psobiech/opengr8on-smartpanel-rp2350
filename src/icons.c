#include "icons.h"

#include <stdio.h>
#include <string.h>

#include "f_util.h"
#include "ff.h"
#include "pico/time.h"

#define ICON_SLOTS     5           // decoded icons in RAM (12.3 KB each): the 4 on screen + 1
#define ICON_NAMES     32          // names known: loaded, queued or failed
#define WORK_BYTES     (48 * 1024) // a PNG file + its decoded rows (37 KB for a 96x96 RGBA image)
#define ICON_IN_USE_US 1000000     // looked up this recently: on screen, never evicted
#define RETRY_US       30000000    // a failed icon on screen / a missing card: tried again this often

typedef enum { ICON_UNUSED = 0, ICON_QUEUED, ICON_READY, ICON_FAILED } icon_state_t;

typedef struct {
    char name[17];          // as the CLU sent it ('~' prefix: negative)
    icon_state_t state;
    int8_t slot;            // s_icons index while ICON_READY
    uint64_t last_used_us;  // last icons_find
    uint64_t failed_us;     // ICON_FAILED since (retried after RETRY_US)
} icon_entry_t;

static icon_entry_t s_entries[ICON_NAMES];
static png_icon_t s_icons[ICON_SLOTS];
static bool s_slot_taken[ICON_SLOTS];
static uint8_t s_work[WORK_BYTES];
static FATFS s_filesystem;
static FIL s_file;
static bool s_card_mounted;
static uint64_t s_mount_attempt_us; // last f_mount (a card that is not there blocks it up to ~1 s)

static bool mount_card(uint64_t now);
static icon_entry_t *find_entry(const char *name);
static icon_entry_t *new_entry(uint64_t now);
static int free_slot(uint64_t now);
static void forget(icon_entry_t *entry);
static void load(icon_entry_t *entry, int slot, uint64_t now);
static bool valid_file_name(const char *file_name);

void icons_init(void)
{
    mount_card(time_us_64());
}

const png_icon_t *icons_find(const char *name)
{
    if (!name[0]) {
        return NULL;
    }
    uint64_t now = time_us_64();
    icon_entry_t *entry = find_entry(name);
    if (!entry) {
        entry = new_entry(now);
        if (!entry) {
            return NULL;
        }
        snprintf(entry->name, sizeof entry->name, "%s", name);
        entry->state = ICON_QUEUED;
        entry->slot = -1;
    }
    entry->last_used_us = now;
    if (entry->state == ICON_FAILED && now - entry->failed_us >= RETRY_US) {
        entry->state = ICON_QUEUED; // still on screen: the card or the file may be there by now
    }
    return entry->state == ICON_READY ? &s_icons[entry->slot] : NULL;
}

void icons_poll(void)
{
    icon_entry_t *entry = NULL;
    for (int i = 0; i < ICON_NAMES && !entry; i++) {
        entry = s_entries[i].state == ICON_QUEUED ? &s_entries[i] : NULL;
    }
    if (!entry) {
        return;
    }
    uint64_t now = time_us_64();
    if (!s_card_mounted && (now - s_mount_attempt_us < RETRY_US || !mount_card(now))) {
        printf("[icons] %s: no SD card\n", entry->name);
        entry->state = ICON_FAILED;
        entry->failed_us = now;
        return;
    }
    int slot = free_slot(now);
    if (slot < 0) {
        return; // every slot holds an icon on screen: retried on the next call
    }
    load(entry, slot, now);
}

static bool mount_card(uint64_t now)
{
    s_mount_attempt_us = now;
    FRESULT result = f_mount(&s_filesystem, "0:", 1);
    s_card_mounted = result == FR_OK;
    if (s_card_mounted) {
        printf("[icons] SD card mounted: button icons from 0:/icons/<name>.png\n");
    } else {
        printf("[icons] no SD card (%s): buttons show their labels only, retried in %u s\n", FRESULT_str(result),
               (unsigned)(RETRY_US / 1000000));
    }
    return s_card_mounted;
}

static icon_entry_t *find_entry(const char *name)
{
    for (int i = 0; i < ICON_NAMES; i++) {
        if (s_entries[i].state != ICON_UNUSED && !strcmp(s_entries[i].name, name)) {
            return &s_entries[i];
        }
    }
    return NULL;
}

// An unused entry, else the least recently used one that is not on screen (forgotten first).
static icon_entry_t *new_entry(uint64_t now)
{
    icon_entry_t *oldest = NULL;
    for (int i = 0; i < ICON_NAMES; i++) {
        icon_entry_t *entry = &s_entries[i];
        if (entry->state == ICON_UNUSED) {
            return entry;
        }
        if (now - entry->last_used_us >= ICON_IN_USE_US && (!oldest || entry->last_used_us < oldest->last_used_us)) {
            oldest = entry;
        }
    }
    if (oldest) {
        forget(oldest);
    }
    return oldest;
}

// A free icon slot, else the one of the least recently used icon not on screen (that name is
// forgotten: loaded again when it is shown again). -1: all on screen.
static int free_slot(uint64_t now)
{
    icon_entry_t *oldest = NULL;
    for (int slot = 0; slot < ICON_SLOTS; slot++) {
        if (!s_slot_taken[slot]) {
            return slot;
        }
    }
    for (int i = 0; i < ICON_NAMES; i++) {
        icon_entry_t *entry = &s_entries[i];
        if (entry->state == ICON_READY && now - entry->last_used_us >= ICON_IN_USE_US &&
            (!oldest || entry->last_used_us < oldest->last_used_us)) {
            oldest = entry;
        }
    }
    if (!oldest) {
        return -1;
    }
    int slot = oldest->slot;
    forget(oldest);
    return slot;
}

static void forget(icon_entry_t *entry)
{
    if (entry->state == ICON_READY) {
        s_slot_taken[entry->slot] = false;
    }
    entry->state = ICON_UNUSED;
}

static void load(icon_entry_t *entry, int slot, uint64_t now)
{
    bool negative = entry->name[0] == '~';
    const char *file_name = entry->name + negative;
    entry->state = ICON_FAILED; // until it is decoded
    entry->failed_us = now;
    if (!valid_file_name(file_name)) {
        printf("[icons] \"%s\": not a file name\n", entry->name);
        return;
    }
    char path[40];
    snprintf(path, sizeof path, "0:/icons/%s.png", file_name);
    FRESULT result = f_open(&s_file, path, FA_READ);
    if (result != FR_OK) {
        printf("[icons] %s: %s\n", path, FRESULT_str(result));
        if (result == FR_NOT_READY || result == FR_DISK_ERR || result == FR_INVALID_DRIVE || result == FR_NOT_ENABLED) {
            f_unmount("0:"); // card gone or failing: mounted again on the next attempt
            s_card_mounted = false;
        }
        return;
    }
    FSIZE_t file_size = f_size(&s_file);
    UINT read_length = 0;
    if (file_size <= sizeof s_work) {
        result = f_read(&s_file, s_work, (UINT)file_size, &read_length);
    }
    f_close(&s_file);
    if (file_size > sizeof s_work || result != FR_OK || read_length != file_size) {
        printf("[icons] %s: %s\n", path, file_size > sizeof s_work ? "file too large" : FRESULT_str(result));
        return;
    }
    png_icon_t *icon = &s_icons[slot];
    png_icon_result_t decoded = png_icon_decode(s_work, read_length, sizeof s_work, negative, icon);
    if (decoded != PNG_ICON_OK) {
        printf("[icons] %s: %s\n", path, png_icon_result_text(decoded));
        return;
    }
    printf("[icons] %s: %ux%u%s\n", path, icon->width, icon->height, negative ? ", negative" : "");
    entry->state = ICON_READY;
    entry->slot = (int8_t)slot;
    s_slot_taken[slot] = true;
}

// The name comes from the CLU: a plain file name in /icons only.
static bool valid_file_name(const char *file_name)
{
    if (!file_name[0] || file_name[0] == '.') {
        return false;
    }
    for (const char *character = file_name; *character; character++) {
        if (*character == '/' || *character == '\\' || *character == ':' || (unsigned char)*character < 0x20) {
            return false;
        }
    }
    return true;
}
