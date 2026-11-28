// Host tests for the board drivers the firmware relies on: the bounded I2C helpers
// (board/bsp/bsp_i2c.c), the GT911 touch controller (bsp_gt911.c) and the QMI8658 IMU
// (bsp_qmi8658.c), all included (static functions). The I2C bus is a fake with one register file
// per device (auto-increment, the GT911 with 16-bit register addresses) and the side effects the
// drivers depend on (QMI8658 CTRL9 handshake, STATUS1 cleared by reading). Covered: the GT911
// touch-level config rewrite (only the two level bytes, checksum, Config_Fresh, written only when
// they differ), touch reports (ready bit, clamped point count, failed transfers, release), the
// rotations, the controller search at boot; the QMI8658 search at both addresses, the temperature,
// the tap engine configuration (both parameter halves, CTRL9 handshake and its timeout, a failed
// CTRL1 read) and tap polling.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../board/bsp/bsp_i2c.c"
#include "../../board/bsp/bsp_gt911.c"
#include "../../board/bsp/bsp_qmi8658.c"

static int failures;

// ---- fake I2C bus ------------------------------------------------------------------------------
typedef struct {
    uint8_t address;
    bool present;
    bool register16;      // 16-bit register addresses (GT911)
    uint16_t pointer;     // next register read / written
    uint8_t registers[65536];
} fake_device_t;

static fake_device_t g_gt911 = {.address = GT911_DEVICE_ADDR, .present = true, .register16 = true};
static fake_device_t g_imu = {.address = QMI8658_SLAVE_ADDR_L, .present = true};
static int g_fail_reads_from = -1;   // a read starting at this register fails (-1: none)
static int g_fail_reads_remaining;   // that many times
static bool g_fail_writes;
static bool g_ctrl9_responds = true; // QMI8658 sets STATUSINT bit 7 for a CTRL9 command
static int g_writes;                 // write transfers with data (not just a register address)
static uint16_t g_last_write_register;
static uint8_t g_last_write[256];
static size_t g_last_write_length;
static char g_imu_write_log[2048];   // "reg=value " for every byte written to the IMU
static uint32_t g_last_timeout_us;
static uint64_t g_now_us;
static uint32_t g_slept_ms;
static char g_gpio_log[512];
static gpio_irq_callback_t g_gpio_callback;
static uint g_gpio_callback_pin;
static bool g_gt911_answers_after_sleep; // the product ID appears after the first sleep_ms(1000)
static int g_fake_i2c_storage;
i2c_inst_t *const i2c1 = (i2c_inst_t *)&g_fake_i2c_storage;

static void test_i2c(void);
static void test_gt911_init(void);
static void test_gt911_touch_levels(void);
static void test_gt911_reports(void);
static void test_gt911_rotation(void);
static void test_qmi8658_init(void);
static void test_qmi8658_temperature(void);
static void test_qmi8658_tap(void);
static void check(const char *name, int condition);
static fake_device_t *device_at(uint8_t address);
static void gt911_report(uint8_t status, int point_count, ...);
static void fill_config(uint8_t touch_level, uint8_t leave_level);
static const char *imu_value_written_last(uint8_t register_address);
static void gpio_record(const char *format, ...);

int main(void)
{
    test_i2c();
    test_gt911_init();
    test_gt911_touch_levels();
    test_gt911_reports();
    test_gt911_rotation();
    test_qmi8658_init();
    test_qmi8658_temperature();
    test_qmi8658_tap();
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

// ---- tests -----------------------------------------------------------------------------------
static void test_i2c(void)
{
    g_gpio_log[0] = 0;
    bsp_i2c_init();
    check("I2C: SDA / SCL as I2C with pull-ups", strcmp(g_gpio_log, "func 6,3; func 7,3; pull_up 6; pull_up 7; ") == 0);
    uint8_t buffer[184];
    check("I2C read: succeeds on a present device", bsp_i2c_read_reg16(GT911_DEVICE_ADDR, 0x8047, buffer, sizeof buffer));
    check("I2C read: timeout grows with the length", g_last_timeout_us == 2000u + 100u * sizeof buffer);
    check("I2C: an absent device fails instead of hanging", !bsp_i2c_read_reg8(0x10, 0x00, buffer, 1) &&
                                                                !bsp_i2c_write_reg8(0x10, 0x00, buffer, 1) &&
                                                                !bsp_i2c_write(0x10, buffer, 1));
    buffer[0] = 0x00, buffer[1] = 0x42;
    check("I2C raw write: register byte first", bsp_i2c_write(QMI8658_SLAVE_ADDR_L, buffer, 2) &&
                                                    g_imu.registers[0x00] == 0x42);
}

static void test_gt911_init(void)
{
    bsp_touch_interface_t *touch;
    bsp_touch_info_t info = {.width = 480, .height = 480};
    check("GT911: no info, no interface", !bsp_touch_new_gt911(&touch, NULL));
    check("GT911: interface", bsp_touch_new_gt911(&touch, &info) && touch == bsp_gt911_get_touch_interface() &&
                                  touch->read == bsp_gt911_read && touch->init == bsp_gt911_init);
    memcpy(g_gt911.registers + GT911_REG_PRODUCT_ID, "000", 4);
    g_gt911_answers_after_sleep = true;
    g_gpio_log[0] = 0;
    g_slept_ms = 0;
    touch->init();
    static const char k_reset_sequence[] = "init 16; dir 16,1; init 17; dir 17,1; put 17,0; put 16,1; put 16,0; put 16,1; ";
    check("GT911 init: reset pulse on RST, INT low during reset (address 0x5d)",
          strncmp(g_gpio_log, k_reset_sequence, sizeof k_reset_sequence - 1) == 0);
    check("GT911 init: searched again after 1 s until it answers", g_slept_ms == 50 + 50 + 250 + 1000);
    check("GT911 init: INT pin an input with pull-up, falling-edge IRQ",
          strstr(g_gpio_log, "dir 17,0; pull_up 17; irq 17,4,1; ") != NULL && g_gpio_callback_pin == BSP_GT911_INT_PIN);
    uint16_t rotation = 9;
    touch->get_rotation(&rotation);
    check("GT911: rotation 0 by default", rotation == 0);
}

static void test_gt911_touch_levels(void)
{
    fill_config(80, 60);
    int writes_before = g_writes;
    check("touch levels: written", bsp_gt911_set_touch_levels(60, 40) && g_writes == writes_before + 1);
    check("touch levels: config block, checksum and Config_Fresh in one write (0x8047, 186 bytes)",
          g_last_write_register == GT911_REG_CONFIG && g_last_write_length == 186);
    bool only_levels_changed = true;
    for (int i = 0; i < 184; i++) {
        uint8_t expected = i == 12 ? 60 : i == 13 ? 40 : (uint8_t)(i * 7 + 0x41);
        only_levels_changed &= g_last_write[i] == expected;
    }
    check("touch levels: only bytes 12 / 13 changed, the config version kept", only_levels_changed &&
                                                                                    g_last_write[0] == 0x41);
    uint8_t sum = 0;
    for (int i = 0; i < 185; i++) {
        sum += g_last_write[i];
    }
    check("touch levels: checksum makes the byte sum 0", sum == 0);
    check("touch levels: Config_Fresh = 1", g_last_write[185] == 1);
    check("touch levels: the GT911 gets time to apply it", g_slept_ms == 100);

    writes_before = g_writes;
    check("touch levels already set: nothing written", bsp_gt911_set_touch_levels(60, 40) && g_writes == writes_before);
    fill_config(60, 50);
    check("only the leave level differs: written", bsp_gt911_set_touch_levels(60, 40) && g_writes == writes_before + 1);

    g_fail_reads_from = GT911_REG_CONFIG;
    g_fail_reads_remaining = 1;
    writes_before = g_writes;
    check("config read failed: not written", !bsp_gt911_set_touch_levels(70, 50) && g_writes == writes_before);
    fill_config(80, 60);
    g_fail_writes = true;
    check("config write failed: reported", !bsp_gt911_set_touch_levels(60, 40));
    g_fail_writes = false;
    g_fail_reads_from = GT911_REG_CONFIG;
    g_fail_reads_remaining = 1;
    bsp_gt911_print_config(); // diagnostics only: a failed read is just logged
}

static void test_gt911_reports(void)
{
    bsp_touch_interface_t *touch = bsp_gt911_get_touch_interface();
    bsp_touch_data_t data;
    g_gt911.registers[GT911_EREG_READ_XY] = 0x82;
    g_gpio_callback(BSP_GT911_INT_PIN, GPIO_IRQ_EDGE_RISE);
    touch->read();
    check("no falling edge: the GT911 is not read", !touch->get_data(&data));

    g_now_us = 5000;
    gt911_report(0x82, 2, 3, 100, 200, 40, 7, 479, 0, 12);
    check("report: ready bit acknowledged (status cleared)", g_gt911.registers[GT911_EREG_READ_XY] == 0);
    check("report: stamped", bsp_gt911_last_report_us() == 5000);
    check("report: two points, x / y / pressure / track id",
          touch->get_data(&data) && data.points == 2 && data.coords[0].id == 3 && data.coords[0].x == 100 &&
              data.coords[0].y == 200 && data.coords[0].pressure == 40 && data.coords[1].id == 7 &&
              data.coords[1].x == 479 && data.coords[1].y == 0);

    int writes_before = g_writes;
    touch->read();
    check("same IRQ: read once", g_writes == writes_before);

    g_now_us = 6000;
    gt911_report(0x02, 0);
    check("buffer not ready: the last report kept", touch->get_data(&data) && data.points == 2 &&
                                                         bsp_gt911_last_report_us() == 5000);

    gt911_report(0x8f, 5, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5);
    check("15 points claimed: clamped to 5", touch->get_data(&data) && data.points == 5 && data.coords[4].id == 5);

    g_fail_reads_from = GT911_EREG_READ_XY;
    g_fail_reads_remaining = 1;
    gt911_report(0x81, 1, 9, 10, 10, 1);
    check("status read failed: points kept", touch->get_data(&data) && data.points == 5);
    touch->read(); // the flag was kept: retried on the next poll
    check("status read failed: retried on the next poll", touch->get_data(&data) && data.points == 1 &&
                                                               data.coords[0].id == 9);

    g_now_us = 7000;
    g_fail_reads_from = GT911_EREG_READ_XY + 1;
    g_fail_reads_remaining = 1;
    gt911_report(0x82, 2, 1, 50, 50, 1, 2, 60, 60, 1);
    check("coordinate read failed: the last report kept, stamped", touch->get_data(&data) && data.points == 1 &&
                                                                        bsp_gt911_last_report_us() == 7000);

    gt911_report(0x80, 0);
    check("0 points: released", !touch->get_data(&data));
    check("no data buffer: false", !touch->get_data(NULL));
}

static void test_gt911_rotation(void)
{
    bsp_touch_info_t info = {.width = 800, .height = 480};
    bsp_touch_interface_t *touch;
    bsp_touch_new_gt911(&touch, &info);
    gt911_report(0x81, 1, 1, 10, 20, 1);
    static const struct {
        uint16_t rotation;
        int x, y;
        const char *name;
    } k_rotations[] = {
        {0, 10, 20, "rotation 0: as reported"},
        {1, 20, 479 - 10, "rotation 1 (800x480 kept): 90 degrees"},
        {2, 479 - 10, 799 - 20, "rotation 2 (swapped to 480x800): 180 degrees"},
        {3, 799 - 20, 10, "rotation 3 (swapped to 800x480): 270 degrees, x inside the screen"},
    };
    for (size_t i = 0; i < sizeof k_rotations / sizeof k_rotations[0]; i++) {
        bsp_touch_data_t data;
        touch->set_rotation(k_rotations[i].rotation);
        bool mapped = touch->get_data(&data) && data.coords[0].x == k_rotations[i].x && data.coords[0].y == k_rotations[i].y;
        if (!mapped) {
            printf("  got (%u, %u)\n", data.coords[0].x, data.coords[0].y);
        }
        check(k_rotations[i].name, mapped);
    }
    touch->set_rotation(1);
    touch->set_rotation(0);
    check("rotation 0 after 1: width and height back", g_touch_info->width == 480 && g_touch_info->height == 800);
    touch->set_rotation(3);
    check("rotation 3: landscape (width > height)", g_touch_info->width == 800 && g_touch_info->height == 480);
    bsp_touch_info_t square = {.width = 480, .height = 480};
    bsp_touch_new_gt911(&touch, &square);
}

static void test_qmi8658_init(void)
{
    g_imu.registers[QMI8658Register_WhoAmI] = 0x05;
    check("QMI8658 at 0x6a: found", bsp_qmi8658_init() == 1 && QMI8658_slave_addr == QMI8658_SLAVE_ADDR_L);
    check("QMI8658 init: CTRL1 address auto-increment, big endian", strstr(g_imu_write_log, "2=60 ") != NULL);
    g_imu.address = QMI8658_SLAVE_ADDR_H;
    check("QMI8658 at 0x6b: found after 5 tries at 0x6a", bsp_qmi8658_init() == 1 &&
                                                              QMI8658_slave_addr == QMI8658_SLAVE_ADDR_H);
    g_imu.present = false;
    check("no QMI8658: init fails (the RP2350 sensor is used)", bsp_qmi8658_init() == 0);
    g_imu.present = true;
    g_imu.address = QMI8658_SLAVE_ADDR_L;
    bsp_qmi8658_init();
}

static void test_qmi8658_temperature(void)
{
    g_imu.registers[QMI8658Register_Tempearture_L] = 0x80;
    g_imu.registers[QMI8658Register_Tempearture_L + 1] = 0x19;
    check("temperature: 0x1980 / 256 = 25.5 C", bsp_qmi8658_readTemp() == 25.5f);
    g_imu.registers[QMI8658Register_Tempearture_L] = 0x00;
    g_imu.registers[QMI8658Register_Tempearture_L + 1] = 0xff;
    check("temperature: signed (0xff00 = -1 C)", bsp_qmi8658_readTemp() == -1.0f);
    g_fail_reads_from = QMI8658Register_Tempearture_L;
    g_fail_reads_remaining = 1;
    check("temperature: failed transfer = NAN, not stale buffer contents", isnan(bsp_qmi8658_readTemp()));
}

static void test_qmi8658_tap(void)
{
    g_imu_write_log[0] = 0;
    g_imu.registers[QMI8658Register_Ctrl1] = 0x60;
    check("tap engine: configured", bsp_qmi8658_enable_tap(0.5f, 0.2f));
    // first half: peak window 20, priority Z Y X, tap window 50, double-tap window 55, Cal4_H 1, CTRL9 0x0c
    const char *first_half = strstr(g_imu_write_log, "0b=14 0c=05 0d=32 0e=00 0f=37 10=00 12=01 0a=0c 0a=00 ");
    // second half: alpha 0.0625, gamma 0.25, peak 0.5 g^2 * 1024, quiet 0.2 g^2 * 1024, Cal4_H 2, CTRL9 0x0c
    const char *second_half = strstr(g_imu_write_log, "0b=08 0c=20 0d=00 0e=02 0f=cc 10=00 12=02 0a=0c 0a=00 ");
    check("tap engine: first parameter half, then CTRL9 Configure Tap + ACK", first_half != NULL);
    check("tap engine: second half with the thresholds, then CTRL9 + ACK", second_half != NULL && second_half > first_half);
    check("tap engine: accelerometer off while configuring, on at the end",
          strncmp(g_imu_write_log, "08=00 ", 6) == 0 && strcmp(imu_value_written_last(QMI8658Register_Ctrl7), "01") == 0);
    check("tap engine: INT1 enabled, the rest of CTRL1 kept", g_imu.registers[QMI8658Register_Ctrl1] == 0x68);
    check("tap engine: CTRL8 handshake on STATUSINT, activity on INT1, tap on",
          strcmp(imu_value_written_last(QMI8658_REG_CTRL8), "c1") == 0);

    g_ctrl9_responds = false;
    g_now_us = 0;
    check("CTRL9 not completed: tap detection off", !bsp_qmi8658_enable_tap(0.5f, 0.2f));
    check("CTRL9 not completed: given up after one timeout (the second command is skipped)",
          g_now_us >= QMI8658_CTRL9_TIMEOUT_US && g_now_us < 2 * QMI8658_CTRL9_TIMEOUT_US);
    check("CTRL9 not completed: tap bit left off", strcmp(imu_value_written_last(QMI8658_REG_CTRL8), "c0") == 0);
    g_ctrl9_responds = true;

    g_imu.registers[QMI8658Register_Ctrl1] = 0x60;
    g_fail_reads_from = QMI8658Register_Ctrl1;
    g_fail_reads_remaining = 1;
    bool configured = bsp_qmi8658_enable_tap(0.5f, 0.2f);
    check("CTRL1 read failed: CTRL1 not overwritten with garbage", g_imu.registers[QMI8658Register_Ctrl1] == 0x60);
    check("CTRL1 read failed: tap detection reported off", !configured);

    g_imu.registers[QMI8658Register_Status1] = QMI8658_STATUS1_TAP;
    check("tap: detected", bsp_qmi8658_tap_detected());
    check("tap: reading STATUS1 cleared it", !bsp_qmi8658_tap_detected());
    g_imu.registers[QMI8658Register_Status1] = QMI8658_STATUS1_TAP;
    g_fail_reads_from = QMI8658Register_Status1;
    g_fail_reads_remaining = 1;
    check("tap: failed transfer = no tap", !bsp_qmi8658_tap_detected());
}

// ---- helpers ---------------------------------------------------------------------------------
static void check(const char *name, int condition)
{
    printf("%s %s\n", condition ? "ok  " : "FAIL", name);
    failures += !condition;
}

static fake_device_t *device_at(uint8_t address)
{
    if (g_gt911.present && address == g_gt911.address) {
        return &g_gt911;
    }
    if (g_imu.present && address == g_imu.address) {
        return &g_imu;
    }
    return NULL;
}

// A GT911 report (status byte, point_count (track id, x, y, pressure) quadruples) and its IRQ, read.
static void gt911_report(uint8_t status, int point_count, ...)
{
    va_list points;
    va_start(points, point_count);
    g_gt911.registers[GT911_EREG_READ_XY] = status;
    for (int point = 0; point < point_count; point++) {
        uint8_t *point_registers = g_gt911.registers + GT911_EREG_READ_XY + 1 + 8 * point;
        int track_id = va_arg(points, int), x = va_arg(points, int), y = va_arg(points, int);
        int pressure = va_arg(points, int);
        point_registers[0] = (uint8_t)track_id;
        point_registers[1] = (uint8_t)x, point_registers[2] = (uint8_t)(x >> 8);
        point_registers[3] = (uint8_t)y, point_registers[4] = (uint8_t)(y >> 8);
        point_registers[5] = (uint8_t)pressure, point_registers[6] = (uint8_t)(pressure >> 8);
    }
    va_end(points);
    g_gpio_callback(BSP_GT911_INT_PIN, GPIO_IRQ_EDGE_FALL);
    bsp_gt911_get_touch_interface()->read();
}

// GT911 config block with a recognisable pattern (byte 0 = config version 0x41) and the levels.
static void fill_config(uint8_t touch_level, uint8_t leave_level)
{
    for (int i = 0; i < 184; i++) {
        g_gt911.registers[GT911_REG_CONFIG + i] = (uint8_t)(i * 7 + 0x41);
    }
    g_gt911.registers[GT911_REG_CONFIG + 12] = touch_level;
    g_gt911.registers[GT911_REG_CONFIG + 13] = leave_level;
    g_slept_ms = 0;
}

// The value last written to an IMU register, as two hex digits ("" if never written).
static const char *imu_value_written_last(uint8_t register_address)
{
    static char value[3];
    char key[8];
    snprintf(key, sizeof key, "%02x=", register_address);
    value[0] = 0;
    for (const char *found = strstr(g_imu_write_log, key); found; found = strstr(found + 1, key)) {
        if (found == g_imu_write_log || found[-1] == ' ') {
            memcpy(value, found + 3, 2);
        }
    }
    return value;
}

static void gpio_record(const char *format, ...)
{
    size_t length = strlen(g_gpio_log);
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(g_gpio_log + length, sizeof g_gpio_log - length, format, arguments);
    va_end(arguments);
    length = strlen(g_gpio_log);
    snprintf(g_gpio_log + length, sizeof g_gpio_log - length, "; ");
}

// ---- Pico SDK fakes --------------------------------------------------------------------------
uint i2c_init(i2c_inst_t *i2c, uint baudrate)
{
    (void)i2c;
    return baudrate;
}

// The register address (1 or 2 bytes), then data written from there on (auto-increment).
int i2c_write_timeout_us(i2c_inst_t *i2c, uint8_t address, const uint8_t *source, size_t length, bool nostop,
                         uint timeout_us)
{
    (void)i2c, (void)nostop;
    g_last_timeout_us = timeout_us;
    fake_device_t *device = device_at(address);
    if (!device || g_fail_writes) {
        return -2; // PICO_ERROR_GENERIC: no ACK
    }
    size_t address_length = device->register16 ? 2 : 1;
    device->pointer = device->register16 ? (uint16_t)(source[0] << 8 | source[1]) : source[0];
    if (length > address_length) {
        g_writes++;
        g_last_write_register = device->pointer;
        g_last_write_length = length - address_length;
        memcpy(g_last_write, source + address_length,
               g_last_write_length < sizeof g_last_write ? g_last_write_length : sizeof g_last_write);
    }
    for (size_t i = address_length; i < length; i++) {
        uint16_t register_address = device->pointer++;
        device->registers[register_address] = source[i];
        if (device == &g_imu) {
            size_t log_length = strlen(g_imu_write_log);
            snprintf(g_imu_write_log + log_length, sizeof g_imu_write_log - log_length, "%02x=%02x ",
                     register_address, source[i]);
            if (register_address == QMI8658Register_Ctrl9) { // command: done flag set; ACK (0): cleared
                bool done = source[i] != QMI8658_CTRL9_CMD_ACK && g_ctrl9_responds;
                device->registers[QMI8658_REG_STATUS_INT] =
                    (uint8_t)(done ? device->registers[QMI8658_REG_STATUS_INT] | QMI8658_STATUS_INT_CTRL9_DONE
                                   : device->registers[QMI8658_REG_STATUS_INT] & ~QMI8658_STATUS_INT_CTRL9_DONE);
            }
        }
    }
    return (int)length;
}

int i2c_read_timeout_us(i2c_inst_t *i2c, uint8_t address, uint8_t *destination, size_t length, bool nostop,
                        uint timeout_us)
{
    (void)i2c, (void)nostop;
    g_last_timeout_us = timeout_us;
    fake_device_t *device = device_at(address);
    if (!device) {
        return -2;
    }
    if (g_fail_reads_remaining && device->pointer == g_fail_reads_from) {
        g_fail_reads_remaining--;
        return -3; // PICO_ERROR_TIMEOUT
    }
    for (size_t i = 0; i < length; i++) {
        uint16_t register_address = device->pointer++;
        destination[i] = device->registers[register_address];
        if (device == &g_imu && register_address == QMI8658Register_Status1) {
            device->registers[register_address] = 0; // cleared by reading
        }
    }
    return (int)length;
}

uint64_t time_us_64(void)
{
    return g_now_us;
}

void sleep_ms(uint32_t delay_ms)
{
    g_slept_ms += delay_ms;
    g_now_us += (uint64_t)delay_ms * 1000;
    if (delay_ms == 1000 && g_gt911_answers_after_sleep) {
        memcpy(g_gt911.registers + GT911_REG_PRODUCT_ID, "911", 4);
        g_gt911_answers_after_sleep = false;
    }
}

void gpio_init(uint gpio)
{
    gpio_record("init %u", gpio);
}

void gpio_put(uint gpio, bool value)
{
    gpio_record("put %u,%d", gpio, value);
}

void gpio_set_dir(uint gpio, bool out)
{
    gpio_record("dir %u,%d", gpio, out);
}

void gpio_set_function(uint gpio, enum gpio_function function)
{
    gpio_record("func %u,%d", gpio, function);
}

void gpio_pull_up(uint gpio)
{
    gpio_record("pull_up %u", gpio);
}

void gpio_set_irq_enabled_with_callback(uint gpio, uint32_t event_mask, bool enabled, gpio_irq_callback_t callback)
{
    gpio_record("irq %u,%u,%d", gpio, (unsigned)event_mask, enabled);
    g_gpio_callback = callback;
    g_gpio_callback_pin = gpio;
}

bool gpio_get(uint gpio)
{
    (void)gpio;
    return false;
}
