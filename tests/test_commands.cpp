// Tests for commands.ino — command parsing and execution

#include "mocks/Arduino.h"
#include "mocks/SPI.h"
#include "generated_config.h"
#include "test_framework.h"
int test_count = 0;
int test_failures = 0;

// -- Mock state (Arduino globals) -------------------------------------------

int      mock_pin_mode[MOCK_NUM_PINS];
int      mock_pin_value[MOCK_NUM_PINS];
int      mock_analog_value[MOCK_NUM_PINS];
unsigned long mock_millis_value;
unsigned long mock_micros_value;
HardwareSerial SerialUSB;
SPIClass SPI;

// -- Globals from firmware.ino that sms.ino references ----------------------

struct settings {
    char apn[64]; char user[20]; char pwd[20];
    char sim_pin[5]; char imei[20];
    int loop_interval;
    int8_t always_on; int8_t movement_alarm;
    uint8_t psk[32];
};

settings config;
byte save_config;
byte set_power_state;
byte power_off_relay;
byte power_reboot;
byte send_int_to_server;
char ignition;
char lat_current[32];
char lon_current[32];
float battery_v;

// movement globals (referenced by movereset command)
byte movement_alert_level;
long movement_cooldown_secs;
long movement_idle_secs;
int saved_loop_interval;

// -- Alert capture for verification -----------------------------------------

#define MAX_ALERTS 16
struct CapturedAlert { char msg[120]; int8_t priority; };
CapturedAlert captured_alerts[MAX_ALERTS];
int captured_alert_count = 0;

void alert_enqueue(const char *msg, int8_t priority) {
    if (captured_alert_count < MAX_ALERTS) {
        strlcpy(captured_alerts[captured_alert_count].msg, msg, 120);
        captured_alerts[captured_alert_count].priority = priority;
        captured_alert_count++;
    }
}

// -- Stubs for functions called by sms.ino ----------------------------------

int collect_data_calls = 0;
int send_data_calls = 0;
int collect_data(int) { collect_data_calls++; return 1; }
void send_data()      { send_data_calls++; }

// -- Include the file under test --------------------------------------------

#include "../commands.ino"

// -- Test helpers -----------------------------------------------------------

void reset_state() {
    memset(&config, 0, sizeof(config));
    config.loop_interval = 3600;
    save_config = 0;
    set_power_state = 0;
    power_off_relay = 0;
    power_reboot = 0;
    send_int_to_server = 0;
    ignition = 1;
    battery_v = 12.5;
    strlcpy(lat_current, "51.500000", sizeof(lat_current));
    strlcpy(lon_current, "-0.100000", sizeof(lon_current));
    captured_alert_count = 0;
    collect_data_calls = 0;
    send_data_calls = 0;
}

// ===========================================================================
// Tests
// ===========================================================================

// -- int= validation -------------------------------------------------------

void test_int_normal() {
    reset_state();
    char cmd[] = "int=900";
    cmd_run(cmd);
    ASSERT_EQ(config.loop_interval, 900);
    ASSERT_EQ(save_config, 1);
    ASSERT_EQ(send_int_to_server, 1);
}

void test_int_clamps_minimum() {
    reset_state();
    char cmd[] = "int=5";
    cmd_run(cmd);
    ASSERT_EQ(config.loop_interval, 10);
}

void test_int_zero_disables() {
    reset_state();
    char cmd[] = "int=0";
    cmd_run(cmd);
    ASSERT_EQ(config.loop_interval, 0);
}

// -- alwayson= power state change ------------------------------------------

void test_alwayson_change_triggers_power_state() {
    reset_state();
    config.always_on = 0;
    char cmd[] = "alwayson=1";
    cmd_run(cmd);
    ASSERT_EQ(config.always_on, 1);
    ASSERT_EQ(set_power_state, 1);
    ASSERT_EQ(save_config, 1);
    ASSERT_EQ(send_int_to_server, 1);
}

void test_alwayson_no_change_no_power_state() {
    reset_state();
    config.always_on = 1;
    char cmd[] = "alwayson=1";
    cmd_run(cmd);
    ASSERT_EQ(config.always_on, 1);
    ASSERT_EQ(set_power_state, 0);  // no change → no relay action
}

// -- Multiple commands in one string ----------------------------------------

void test_multiple_commands() {
    reset_state();
    config.movement_alarm = 1;
    char cmd[] = "int=120,movealarm=0";
    cmd_run(cmd);
    ASSERT_EQ(config.loop_interval, 120);
    ASSERT_EQ(config.movement_alarm, 0);
}

// -- locatenow vs locate ---------------------------------------------------

void test_locatenow_collects_and_sends() {
    reset_state();
    char cmd[] = "locatenow";
    cmd_run(cmd);
    ASSERT_EQ(collect_data_calls, 1);
    ASSERT_EQ(send_data_calls, 1);
    ASSERT_TRUE(captured_alert_count > 0);
    ASSERT_STRCONTAINS(captured_alerts[0].msg, "google:");
}

void test_locate_does_not_collect() {
    reset_state();
    char cmd[] = "locate";
    cmd_run(cmd);
    ASSERT_EQ(collect_data_calls, 0);
    ASSERT_EQ(send_data_calls, 0);
    ASSERT_TRUE(captured_alert_count > 0);
}

// -- movealarm -------------------------------------------------------------

void test_movealarm_off() {
    reset_state();
    config.movement_alarm = 1;
    char cmd[] = "movealarm=0";
    cmd_run(cmd);
    ASSERT_EQ(config.movement_alarm, 0);
}

// -- reboot and poweroff ---------------------------------------------------

void test_reboot() {
    reset_state();
    char cmd[] = "reboot";
    cmd_run(cmd);
    ASSERT_EQ(power_reboot, 1);
}

void test_poweroff() {
    reset_state();
    char cmd[] = "poweroff";
    cmd_run(cmd);
    ASSERT_EQ(power_off_relay, 1);
}

// ===========================================================================

int main() {
    printf("commands.ino tests:\n");

    RUN_TEST(test_int_normal);
    RUN_TEST(test_int_clamps_minimum);
    RUN_TEST(test_int_zero_disables);

    RUN_TEST(test_alwayson_change_triggers_power_state);
    RUN_TEST(test_alwayson_no_change_no_power_state);

    RUN_TEST(test_multiple_commands);

    RUN_TEST(test_locatenow_collects_and_sends);
    RUN_TEST(test_locate_does_not_collect);

    RUN_TEST(test_movealarm_off);

    RUN_TEST(test_reboot);
    RUN_TEST(test_poweroff);

    TEST_REPORT();
}
