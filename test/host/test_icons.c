// Host tests for the button icon cache (src/icons.c, included: its state is static). FatFS is a
// fake over files held in memory (the PNG fixtures), the decoder is the real core/png_icon.c.
// Covered: the queue (one icon per icons_poll), the missing card and its retry, the card lost
// while reading, file name checks (the name comes from the CLU), file / decode errors, the
// negative ('~name'), eviction of icons not on screen and the 32-name table.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/icons.c"

static int failures;

// ---- fake card ---------------------------------------------------------------------------------
typedef struct {
    char path[40];
    uint8_t *data;
    size_t size;
    FRESULT open_result; // FR_OK: readable
    bool short_read;     // f_read returns one byte less
} fake_file_t;

static fake_file_t g_files[48];
static int g_file_count;
static FRESULT g_mount_result = FR_OK;
static int g_mounts, g_unmounts, g_opens, g_reads;
static uint64_t g_now_us;

static void test_without_card(void);
static void test_load_and_cache(void);
static void test_file_names(void);
static void test_file_errors(void);
static void test_card_lost(void);
static void test_eviction(void);
static void test_name_table(void);
static void check(const char *name, int condition);
static void reset_cache(void);
static fake_file_t *add_file(const char *name, const uint8_t *data, size_t size);
static fake_file_t *add_fixture(const char *name, const char *fixture);
static const png_icon_t *find_after_poll(const char *name);

int main(void)
{
    test_without_card();
    test_load_and_cache();
    test_file_names();
    test_file_errors();
    test_card_lost();
    test_eviction();
    test_name_table();
    for (int i = 0; i < g_file_count; i++) {
        free(g_files[i].data);
    }
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

// ---- tests -----------------------------------------------------------------------------------
static void test_without_card(void)
{
    g_now_us = 1000000;
    g_mount_result = FR_NOT_READY;
    icons_init();
    check("boot without a card: mount tried once", g_mounts == 1 && !s_card_mounted);
    check("empty name: no icon, nothing queued", icons_find("") == NULL && find_entry("") == NULL);
    check("unknown icon: NULL while queued", icons_find("bulb_on") == NULL && find_entry("bulb_on")->state == ICON_QUEUED);
    icons_poll();
    check("no card: failed without another mount within RETRY_US", g_mounts == 1 &&
                                                                      find_entry("bulb_on")->state == ICON_FAILED);
    icons_poll();
    check("nothing queued: icons_poll does nothing", g_mounts == 1 && g_opens == 0);
    add_fixture("bulb_on", "bulb_on.png");
    g_mount_result = FR_OK;
    g_now_us += RETRY_US - 1;
    check("failed icon: not retried before RETRY_US", icons_find("bulb_on") == NULL &&
                                                          find_entry("bulb_on")->state == ICON_FAILED);
    g_now_us += 1;
    check("failed icon on screen: queued again after RETRY_US", icons_find("bulb_on") == NULL &&
                                                                    find_entry("bulb_on")->state == ICON_QUEUED);
    icons_poll();
    check("card inserted meanwhile: mounted on the retry", g_mounts == 2 && s_card_mounted);
    const png_icon_t *icon = icons_find("bulb_on");
    check("card inserted meanwhile: the icon loads", icon && icon->width == 64 && icon->height == 64);

    reset_cache();
    g_mount_result = FR_NOT_READY;
    s_card_mounted = false;
    s_mount_attempt_us = g_now_us - RETRY_US;
    icons_find("bulb_on");
    icons_poll();
    check("still no card after RETRY_US: mount tried again, icon failed",
          g_mounts == 3 && find_entry("bulb_on")->state == ICON_FAILED);
    g_mount_result = FR_OK;
    s_card_mounted = true;
}

static void test_load_and_cache(void)
{
    reset_cache();
    add_fixture("bulb_off", "bulb_off.png");
    int opens_before = g_opens;
    icons_find("bulb_on");
    icons_find("bulb_off");
    icons_poll();
    check("one icon per icons_poll", g_opens == opens_before + 1 && find_entry("bulb_on")->state == ICON_READY &&
                                          find_entry("bulb_off")->state == ICON_QUEUED);
    icons_poll();
    const png_icon_t *bulb_on = icons_find("bulb_on"), *bulb_off = icons_find("bulb_off");
    check("both loaded, in their own slots", bulb_on && bulb_off && bulb_on != bulb_off);
    check("cached: no further card reads", icons_find("bulb_on") == bulb_on && g_opens == opens_before + 2);
    check("path: 0:/icons/<name>.png", strcmp(g_files[0].path, "0:/icons/bulb_on.png") == 0);

    const png_icon_t *negative = find_after_poll("~bulb_on");
    bool inverted = negative && negative->width == bulb_on->width;
    for (int i = 0; inverted && i < bulb_on->width * bulb_on->height; i++) {
        if (bulb_on->alphas[i] == 255) {
            inverted = negative->alphas[i] == 255 && (uint16_t)(negative->colors[i] ^ bulb_on->colors[i]) == 0xffff;
        }
    }
    check("'~name': the same file as a negative, cached separately", inverted && g_opens == opens_before + 3);
}

static void test_file_names(void)
{
    static const char *const k_bad_names[] = {"../secret", "dir/icon", "dir\\icon", "c:icon", ".hidden", "~", "tab\tname"};
    reset_cache();
    for (size_t i = 0; i < sizeof k_bad_names / sizeof k_bad_names[0]; i++) {
        char message[64];
        snprintf(message, sizeof message, "not a file name, never opened: \"%s\"", k_bad_names[i]);
        int opens_before = g_opens;
        check(message, find_after_poll(k_bad_names[i]) == NULL && g_opens == opens_before &&
                           find_entry(k_bad_names[i])->state == ICON_FAILED);
    }
    check("a 16-character name is kept whole", find_after_poll("bulb_on_16_chars") == NULL &&
                                                    find_entry("bulb_on_16_chars") != NULL);
}

static void test_file_errors(void)
{
    reset_cache();
    check("missing file: failed, the card stays mounted", find_after_poll("absent") == NULL && s_card_mounted &&
                                                              g_unmounts == 0);
    static uint8_t large_file[WORK_BYTES + 1];
    add_file("large", large_file, sizeof large_file);
    int reads_before = g_reads;
    check("file larger than the work buffer: failed, not read", find_after_poll("large") == NULL &&
                                                                     g_reads == reads_before);
    static const uint8_t not_png[] = "GIF89a, not a PNG";
    add_file("gif", not_png, sizeof not_png);
    check("not a PNG: failed", find_after_poll("gif") == NULL && find_entry("gif")->state == ICON_FAILED);
    add_fixture("short", "bulb_on.png")->short_read = true;
    check("short read: failed", find_after_poll("short") == NULL);
    add_fixture("denied", "bulb_on.png")->open_result = FR_DENIED;
    check("open error other than a card error: failed, still mounted", find_after_poll("denied") == NULL &&
                                                                            s_card_mounted);
    check("failed slots are free again", !s_slot_taken[0] && !s_slot_taken[1]);
}

static void test_card_lost(void)
{
    static const FRESULT k_card_errors[] = {FR_NOT_READY, FR_DISK_ERR, FR_INVALID_DRIVE, FR_NOT_ENABLED};
    for (size_t i = 0; i < sizeof k_card_errors / sizeof k_card_errors[0]; i++) {
        reset_cache();
        char name[16];
        snprintf(name, sizeof name, "lost%u", (unsigned)i);
        add_fixture(name, "bulb_on.png")->open_result = k_card_errors[i];
        int unmounts_before = g_unmounts;
        find_after_poll(name);
        char message[64];
        snprintf(message, sizeof message, "card error %d on open: unmounted, mounted again later", k_card_errors[i]);
        check(message, g_unmounts == unmounts_before + 1 && !s_card_mounted);
        s_card_mounted = true;
    }
}

static void test_eviction(void)
{
    static const char *const k_names[ICON_SLOTS + 1] = {"icon0", "icon1", "icon2", "icon3", "icon4", "icon5"};
    reset_cache();
    for (int i = 0; i <= ICON_SLOTS; i++) {
        add_fixture(k_names[i], "rgba8.png");
    }
    for (int i = 0; i < ICON_SLOTS; i++) {
        find_after_poll(k_names[i]);
    }
    bool all_loaded = true;
    for (int i = 0; i < ICON_SLOTS; i++) {
        all_loaded &= icons_find(k_names[i]) != NULL;
    }
    check("five icons fill the cache", all_loaded);
    icons_find(k_names[ICON_SLOTS]);
    icons_poll();
    check("all on screen: a sixth icon waits in the queue", find_entry(k_names[ICON_SLOTS])->state == ICON_QUEUED);
    g_now_us += ICON_IN_USE_US;
    for (int i = 1; i < ICON_SLOTS; i++) { // icon0 left the screen
        icons_find(k_names[i]);
    }
    int slot_of_icon0 = find_entry(k_names[0])->slot;
    const png_icon_t *icon5 = find_after_poll(k_names[ICON_SLOTS]);
    check("the icon off screen gives its slot", icon5 == &s_icons[slot_of_icon0] && find_entry(k_names[0]) == NULL);
    check("an evicted icon loads again when shown again", find_after_poll(k_names[0]) == NULL &&
                                                              find_entry(k_names[0])->state == ICON_QUEUED);
}

static void test_name_table(void)
{
    reset_cache();
    char name[16];
    for (int i = 0; i < ICON_NAMES; i++) {
        snprintf(name, sizeof name, "name%d", i);
        icons_find(name);
    }
    check("32 names on screen: a new name is not taken", icons_find("one_more") == NULL &&
                                                             find_entry("one_more") == NULL);
    g_now_us += ICON_IN_USE_US;
    for (int i = 1; i < ICON_NAMES; i++) { // name0 left the screen
        snprintf(name, sizeof name, "name%d", i);
        icons_find(name);
    }
    icons_find("one_more");
    check("a new name replaces the one off screen", find_entry("one_more") != NULL && find_entry("name0") == NULL);
}

// ---- helpers ---------------------------------------------------------------------------------
static void check(const char *name, int condition)
{
    printf("%s %s\n", condition ? "ok  " : "FAIL", name);
    failures += !condition;
}

// Empty cache, card mounted; the fake files stay.
static void reset_cache(void)
{
    memset(s_entries, 0, sizeof s_entries);
    memset(s_slot_taken, 0, sizeof s_slot_taken);
    s_card_mounted = true;
    g_now_us += 10 * RETRY_US;
}

static fake_file_t *add_file(const char *name, const uint8_t *data, size_t size)
{
    fake_file_t *file = &g_files[g_file_count++];
    snprintf(file->path, sizeof file->path, "0:/icons/%s.png", name);
    file->data = malloc(size);
    memcpy(file->data, data, size);
    file->size = size;
    file->open_result = FR_OK;
    return file;
}

static fake_file_t *add_fixture(const char *name, const char *fixture)
{
    char path[64];
    snprintf(path, sizeof path, "fixtures/%s", fixture);
    FILE *stream = fopen(path, "rb");
    if (!stream) {
        printf("FAIL cannot open %s (run from test/host)\n", path);
        exit(EXIT_FAILURE);
    }
    static uint8_t contents[WORK_BYTES];
    size_t size = fread(contents, 1, sizeof contents, stream);
    fclose(stream);
    return add_file(name, contents, size);
}

// Look the icon up, load what is queued, look it up again (one main loop turn later).
static const png_icon_t *find_after_poll(const char *name)
{
    icons_find(name);
    icons_poll();
    return icons_find(name);
}

// ---- fakes -----------------------------------------------------------------------------------
uint64_t time_us_64(void)
{
    return g_now_us;
}

FRESULT f_mount(FATFS *filesystem, const char *path, BYTE option)
{
    (void)filesystem, (void)path, (void)option;
    g_mounts++;
    return g_mount_result;
}

FRESULT f_unmount(const char *path)
{
    (void)path;
    g_unmounts++;
    return FR_OK;
}

FRESULT f_open(FIL *file, const char *path, BYTE mode)
{
    (void)mode;
    g_opens++;
    for (int i = 0; i < g_file_count; i++) {
        if (!strcmp(g_files[i].path, path)) {
            if (g_files[i].open_result != FR_OK) {
                return g_files[i].open_result;
            }
            file->data = g_files[i].short_read ? NULL : g_files[i].data; // NULL: f_read comes up short
            file->size = g_files[i].size;
            return FR_OK;
        }
    }
    return FR_NO_FILE;
}

FRESULT f_read(FIL *file, void *buffer, UINT bytes_to_read, UINT *bytes_read)
{
    g_reads++;
    if (!file->data) { // short read
        *bytes_read = bytes_to_read - 1;
        return FR_OK;
    }
    memcpy(buffer, file->data, bytes_to_read);
    *bytes_read = bytes_to_read;
    return FR_OK;
}

FRESULT f_close(FIL *file)
{
    (void)file;
    return FR_OK;
}

const char *FRESULT_str(FRESULT result)
{
    static char text[24];
    snprintf(text, sizeof text, "FRESULT %d", result);
    return text;
}
