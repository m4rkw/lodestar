// Tests for settings.ino — compile-time defaults

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

// settings.ino calls crypto_psk_from_hex(); tests don't link the crypto TU.
extern "C" int crypto_psk_from_hex(const char *hex, uint8_t out[32]) {
    (void)hex;
    for (int i = 0; i < 32; i++) out[i] = 0;
    return 1;
}

// -- Forward declarations (Arduino auto-generates these) --------------------

void settings_defaults();
void settings_print();
void settings_load();

// -- Include file under test ------------------------------------------------

#include "../settings.ino"

// ===========================================================================
// Tests
// ===========================================================================

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

void test_defaults_user() {
    settings_defaults();
    ASSERT_STREQ(config.user, DEFAULT_USER);
}

void test_defaults_pass() {
    settings_defaults();
    ASSERT_STREQ(config.pwd, DEFAULT_PASS);
}

void test_load_applies_defaults() {
    memset(&config, 0, sizeof(config));
    settings_load();
    ASSERT_EQ(config.loop_interval, ENGINE_OFF_LOOP_INTERVAL);
    ASSERT_STREQ(config.apn, DEFAULT_APN);
}

// ===========================================================================

int main() {
    printf("settings.ino tests:\n");

    RUN_TEST(test_defaults_loop_interval);
    RUN_TEST(test_defaults_always_on_off);
    RUN_TEST(test_defaults_movement_alarm);
    RUN_TEST(test_defaults_apn);
    RUN_TEST(test_defaults_user);
    RUN_TEST(test_defaults_pass);
    RUN_TEST(test_load_applies_defaults);

    TEST_REPORT();
}
