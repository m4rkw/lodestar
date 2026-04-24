// Tests for settings.ino — defaults and validation

#include "mocks/Arduino.h"
#include "mocks/SPI.h"
#include "generated_config.h"
#include "test_framework.h"
int test_count = 0;
int test_failures = 0;

// -- Mock state -------------------------------------------------------------

int      mock_pin_mode[MOCK_NUM_PINS];
int      mock_pin_value[MOCK_NUM_PINS];
int      mock_analog_value[MOCK_NUM_PINS];
unsigned long mock_millis_value;
unsigned long mock_micros_value;
HardwareSerial SerialUSB;
SPIClass SPI;

// -- Settings struct (must match firmware.ino) ------------------------------

struct settings {
    char apn[64]; char user[20]; char pwd[20];
    char sim_pin[5]; char imei[20];
    int loop_interval;
    int8_t always_on; int8_t movement_alarm;
    uint8_t psk[32];
};

settings config;

// -- Stubs ------------------------------------------------------------------

int  settings_load_sd() { return 0; }
void settings_save_sd() {}

// settings.ino calls crypto_psk_from_hex(); tests don't link the crypto TU.
extern "C" int crypto_psk_from_hex(const char *hex, uint8_t out[32]) {
    (void)hex;
    for (int i = 0; i < 32; i++) out[i] = 0;
    return 1;
}

// -- Forward declarations (Arduino auto-generates these) --------------------

void settings_defaults();
void settings_validate();
void settings_print();
int  settings_load();
void settings_save();

// -- Include file under test ------------------------------------------------

#include "../settings.ino"

// -- Helpers ----------------------------------------------------------------

void fill_corrupt(settings *s) {
    memset(s, 0xFF, sizeof(settings));
}

// ===========================================================================
// Tests
// ===========================================================================

// -- Defaults ---------------------------------------------------------------

void test_defaults_loop_interval() {
    settings_defaults();
    ASSERT_EQ(config.loop_interval, ENGINE_OFF_LOOP_INTERVAL);
}

void test_defaults_always_on_off() {
    settings_defaults();
    ASSERT_EQ(config.always_on, 0);
}

void test_defaults_movement_alarm() {
    settings_defaults();
    ASSERT_EQ(config.movement_alarm, DEFAULT_MOVEMENT_ALARM);
}

void test_defaults_apn() {
    settings_defaults();
    ASSERT_STREQ(config.apn, DEFAULT_APN);
}

// -- Validation: corrupt flash (0xFF) values --------------------------------

void test_validate_corrupt_apn() {
    settings_defaults();
    config.apn[0] = (char)255;
    settings_validate();
    ASSERT_STREQ(config.apn, DEFAULT_APN);
}

void test_validate_corrupt_user() {
    settings_defaults();
    config.user[0] = (char)255;
    settings_validate();
    ASSERT_STREQ(config.user, DEFAULT_USER);
}

void test_validate_corrupt_password() {
    settings_defaults();
    config.pwd[0] = (char)255;
    settings_validate();
    ASSERT_STREQ(config.pwd, DEFAULT_PASS);
}

// -- Validation: loop_interval boundaries -----------------------------------

void test_validate_interval_too_low() {
    settings_defaults();
    config.loop_interval = 5;
    settings_validate();
    ASSERT_EQ(config.loop_interval, ENGINE_OFF_LOOP_INTERVAL);
}

void test_validate_interval_large_accepted() {
    settings_defaults();
    config.loop_interval = 100000;
    settings_validate();
    ASSERT_EQ(config.loop_interval, 100000);  // no upper bound
}

void test_validate_interval_min_boundary() {
    settings_defaults();
    config.loop_interval = 10;
    settings_validate();
    ASSERT_EQ(config.loop_interval, 10);  // accepted
}

void test_validate_interval_zero_disables() {
    settings_defaults();
    config.loop_interval = 0;
    settings_validate();
    ASSERT_EQ(config.loop_interval, 0);  // 0 = disabled
}

// -- Validation: boolean fields ---------------------------------------------

void test_validate_always_on_invalid() {
    settings_defaults();
    config.always_on = 2;
    settings_validate();
    ASSERT_EQ(config.always_on, 0);
}

void test_validate_always_on_negative() {
    settings_defaults();
    config.always_on = -1;
    settings_validate();
    ASSERT_EQ(config.always_on, 0);
}

void test_validate_movement_alarm_invalid() {
    settings_defaults();
    config.movement_alarm = 5;
    settings_validate();
    ASSERT_EQ(config.movement_alarm, DEFAULT_MOVEMENT_ALARM);
}

// -- Validation: valid values pass through ----------------------------------

void test_validate_all_valid_unchanged() {
    settings_defaults();
    config.loop_interval = 1800;
    config.always_on = 1;
    config.movement_alarm = 0;
    settings_validate();
    ASSERT_EQ(config.loop_interval, 1800);
    ASSERT_EQ(config.always_on, 1);
    ASSERT_EQ(config.movement_alarm, 0);
}

// ===========================================================================

int main() {
    printf("settings.ino tests:\n");

    RUN_TEST(test_defaults_loop_interval);
    RUN_TEST(test_defaults_always_on_off);
    RUN_TEST(test_defaults_movement_alarm);
    RUN_TEST(test_defaults_apn);

    RUN_TEST(test_validate_corrupt_apn);
    RUN_TEST(test_validate_corrupt_user);
    RUN_TEST(test_validate_corrupt_password);

    RUN_TEST(test_validate_interval_too_low);
    RUN_TEST(test_validate_interval_large_accepted);
    RUN_TEST(test_validate_interval_min_boundary);
    RUN_TEST(test_validate_interval_zero_disables);

    RUN_TEST(test_validate_always_on_invalid);
    RUN_TEST(test_validate_always_on_negative);
    RUN_TEST(test_validate_movement_alarm_invalid);
    RUN_TEST(test_validate_all_valid_unchanged);

    TEST_REPORT();
}
