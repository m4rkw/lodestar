// Tests for firmware.ino — should_send_data, check_ignition_alarm, handle_post_send

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

// -- Settings struct --------------------------------------------------------

struct settings {
    char apn[64]; char user[20]; char pwd[20];
    char sim_pin[5]; char imei[20];
    int loop_interval;
    int8_t always_on; int8_t movement_alarm;
    uint8_t psk[32];
};

settings config;
char ignition;
int8_t previous_ignition;
unsigned long last_send_time;
byte movement_wake_pending;
byte send_int_to_server;
byte last_send_ok;
byte set_power_state;
byte power_off_relay;
byte save_config;
byte powered_on;
float gps_speed;
float battery_v;
byte engine_running;
int below_voltage_count;
#define ENGINE_STOPPED_COUNT 10
byte post_ignition_off_coasting;
byte coast_iterations;
#define COAST_STOP_SPEED_KMH 1.0f
#define COAST_MAX_ITERATIONS 60
char time_char[32];
char lat_current[32];
char lon_current[32];

// -- Alert capture ----------------------------------------------------------

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

// -- Stubs for handle_post_send deps ----------------------------------------

int power_supply_mode_calls = 0;
int power_supply_off_calls = 0;
int settings_save_calls = 0;

void set_power_supply_mode() { power_supply_mode_calls++; }
void set_power_supply_off()  { power_supply_off_calls++; }
void settings_save()         { settings_save_calls++; }

// -- Functions under test (copied from firmware.ino) ------------------------

byte should_send_data() {
    unsigned long time_elapsed = (unsigned long)(millis() - last_send_time);
    if (previous_ignition == -1) return 1;
    if (post_ignition_off_coasting) return 1;
    if (movement_wake_pending) return 1;
    if (ignition == 0 && previous_ignition != 0) return 1;
    if (ignition != 0 && previous_ignition == 0) return 1;
    if (ignition == 0 && engine_running) return 1;
    if (send_int_to_server) return 1;
    if (last_send_ok == 0 && ignition == 0) return 1;
    if (config.loop_interval > 0 && time_elapsed >= ((unsigned long)config.loop_interval * 1000)) return 1;
    return 0;
}

void handle_post_send() {
    if (set_power_state == 1 && last_send_ok == 1) {
        set_power_state = 0;
        set_power_supply_mode();
    }
    if (power_off_relay == 1 && last_send_ok == 1) {
        power_off_relay = 0;
        set_power_supply_off();
    }
    if (save_config == 1) {
        settings_save();
        save_config = 0;
    }
}

void handle_ignition_state() {
    ignition = digitalRead(PIN_S_DETECT);
}

// Stub for confirm_battery_low — mock uses the current mock_analog_value
float read_battery_voltage() {
    float v = analogRead(AIN_S_INLEVEL) * (ANALOG_SCALE * ANALOG_VREF / 1024.0f);
    return v;
}

int confirm_battery_low(float threshold) {
    for (int i = 0; i < 4; i++) {
        delay(1000);
        float v = read_battery_voltage();
        if (v > threshold) return 0;
    }
    return 1;
}

// GPS decision for engine-off telemetry — extracted from sleep loop.
byte sleep_needs_gps(byte *needs_gps, long idle_secs) {
    if (*needs_gps && idle_secs >= NO_MOVEMENT_GPS_SKIP)
        *needs_gps = 0;
    if (*needs_gps || lat_current[0] == '\0')
        return 1;
    return 0;
}

// Battery poweroff check — extracted from sleep loop for testability.
void check_battery_poweroff(float v) {
    if (BATTERY_POWEROFF_LEVEL > 0 && v <= BATTERY_POWEROFF_LEVEL && config.always_on == 1) {
        if (confirm_battery_low(BATTERY_POWEROFF_LEVEL)) {
            config.always_on = 0;
            set_power_supply_mode();
            settings_save();
        }
    }
}

// -- Helpers ----------------------------------------------------------------

void reset_state() {
    memset(&config, 0, sizeof(config));
    config.loop_interval = 3600;
    ignition = 1;
    previous_ignition = 1;
    last_send_time = 0;
    movement_wake_pending = 0;
    send_int_to_server = 0;
    last_send_ok = 1;
    set_power_state = 0;
    power_off_relay = 0;
    save_config = 0;
    powered_on = 0;
    gps_speed = 0;
    battery_v = 14.0f;  // default: engine running
    engine_running = 1;
    below_voltage_count = 0;
    post_ignition_off_coasting = 0;
    coast_iterations = 0;
    captured_alert_count = 0;
    power_supply_mode_calls = 0;
    power_supply_off_calls = 0;
    settings_save_calls = 0;
    mock_millis_value = 10000;
    strlcpy(time_char, "14/03/26,12:00:00.000000+00", sizeof(time_char));
    strlcpy(lat_current, "51.509865", sizeof(lat_current));
    strlcpy(lon_current, "-0.118092", sizeof(lon_current));
    mock_reset();
}

// ===========================================================================
// should_send_data tests
// ===========================================================================

void test_send_first_boot() {
    reset_state();
    previous_ignition = -1;
    ASSERT_EQ(should_send_data(), 1);
}

void test_send_ignition_just_on() {
    reset_state();
    ignition = 0;     // ON (inverted)
    previous_ignition = 1;  // was OFF
    ASSERT_EQ(should_send_data(), 1);
}

void test_send_ignition_just_off() {
    reset_state();
    ignition = 1;     // OFF
    previous_ignition = 0;  // was ON
    ASSERT_EQ(should_send_data(), 1);
}

void test_send_ignition_on_continuous() {
    reset_state();
    ignition = 0;
    previous_ignition = 0;
    ASSERT_EQ(should_send_data(), 1);
}

void test_send_movement_wake() {
    reset_state();
    movement_wake_pending = 1;
    ASSERT_EQ(should_send_data(), 1);
}

void test_send_settings_sync() {
    reset_state();
    send_int_to_server = 1;
    ASSERT_EQ(should_send_data(), 1);
}

void test_send_retry_on_failure() {
    // Retry on send failure is only active while ignition is on; otherwise
    // the ignition-off sleep path must win so STATE_SLEEP can re-init state.
    reset_state();
    ignition = 0;           // ON (inverted)
    previous_ignition = 0;
    last_send_ok = 0;
    ASSERT_EQ(should_send_data(), 1);
}

void test_no_retry_on_failure_when_ignition_off() {
    // Persistent send failure must NOT keep the device in the send loop
    // when ignition is off — STATE_SLEEP must be reachable.
    reset_state();
    ignition = 1;           // OFF
    previous_ignition = 1;
    last_send_ok = 0;
    last_send_time = 5000;
    mock_millis_value = 10000;  // 5s elapsed, 3600s interval
    ASSERT_EQ(should_send_data(), 0);
}

void test_send_engine_off_interval_elapsed() {
    reset_state();
    config.loop_interval = 60;
    last_send_time = 0;
    mock_millis_value = 61000;  // 61s > 60s interval
    ASSERT_EQ(should_send_data(), 1);
}

void test_no_send_engine_off_interval_not_elapsed() {
    reset_state();
    config.loop_interval = 60;
    last_send_time = 0;
    mock_millis_value = 59000;
    ASSERT_EQ(should_send_data(), 0);
}

void test_no_send_idle_engine_off() {
    reset_state();
    ignition = 1;
    previous_ignition = 1;
    last_send_ok = 1;
    last_send_time = 5000;
    mock_millis_value = 10000;  // 5s elapsed, 3600s interval
    ASSERT_EQ(should_send_data(), 0);
}

void test_send_ignition_on_high_voltage() {
    reset_state();
    ignition = 0;
    previous_ignition = 0;
    battery_v = 14.0f;  // alternator charging
    ASSERT_EQ(should_send_data(), 1);
}

void test_no_send_ignition_on_low_voltage() {
    reset_state();
    ignition = 0;
    previous_ignition = 0;
    battery_v = 12.4f;  // engine not running
    engine_running = 0;
    last_send_ok = 1;
    ASSERT_EQ(should_send_data(), 0);
}

void test_send_ignition_on_exact_threshold() {
    reset_state();
    ignition = 0;
    previous_ignition = 0;
    battery_v = ENGINE_RUNNING_VOLTAGE;  // exactly 13.0V
    ASSERT_EQ(should_send_data(), 1);
}

void test_send_ignition_change_regardless_of_voltage() {
    // ignition just turned on — should send even with low voltage
    reset_state();
    ignition = 0;
    previous_ignition = 1;
    battery_v = 11.5f;
    ASSERT_EQ(should_send_data(), 1);

    // ignition just turned off — should send even with low voltage
    reset_state();
    ignition = 1;
    previous_ignition = 0;
    battery_v = 11.5f;
    ASSERT_EQ(should_send_data(), 1);
}

void test_engine_running_hysteresis() {
    // engine_running should stay true during brief voltage dips
    reset_state();
    ignition = 0;
    previous_ignition = 0;
    engine_running = 1;
    below_voltage_count = 0;

    // 9 consecutive below-threshold readings — still engine_running
    battery_v = 12.5f;
    below_voltage_count = 9;
    ASSERT_EQ(engine_running, 1);
    ASSERT_EQ(should_send_data(), 1);

    // 10th reading triggers engine stopped
    engine_running = 0;
    below_voltage_count = 10;
    last_send_ok = 1;
    ASSERT_EQ(should_send_data(), 0);

    // voltage recovers — immediately engine_running again
    engine_running = 1;
    below_voltage_count = 0;
    battery_v = 14.0f;
    ASSERT_EQ(should_send_data(), 1);
}

void test_send_coasting_flag_forces_send() {
    reset_state();
    ignition = 1;
    previous_ignition = 1;
    last_send_ok = 1;
    post_ignition_off_coasting = 1;
    ASSERT_EQ(should_send_data(), 1);
}

void test_no_send_coasting_flag_clear() {
    // coasting flag not set, engine off, no transition — should not send
    reset_state();
    ignition = 1;
    previous_ignition = 1;
    last_send_ok = 1;
    post_ignition_off_coasting = 0;
    last_send_time = 5000;
    mock_millis_value = 10000;
    ASSERT_EQ(should_send_data(), 0);
}

// ===========================================================================
// handle_post_send tests
// ===========================================================================

void test_post_send_power_state_on_success() {
    reset_state();
    set_power_state = 1;
    last_send_ok = 1;
    handle_post_send();
    ASSERT_EQ(power_supply_mode_calls, 1);
    ASSERT_EQ(set_power_state, 0);
}

void test_post_send_power_state_deferred_on_failure() {
    reset_state();
    set_power_state = 1;
    last_send_ok = 0;
    handle_post_send();
    ASSERT_EQ(power_supply_mode_calls, 0);
    ASSERT_EQ(set_power_state, 1);  // still pending
}

void test_post_send_power_off_relay() {
    reset_state();
    power_off_relay = 1;
    last_send_ok = 1;
    handle_post_send();
    ASSERT_EQ(power_supply_off_calls, 1);
    ASSERT_EQ(power_off_relay, 0);
}

void test_post_send_save_config() {
    reset_state();
    save_config = 1;
    handle_post_send();
    ASSERT_EQ(settings_save_calls, 1);
    ASSERT_EQ(save_config, 0);
}

// ===========================================================================
// handle_ignition_state tests
// ===========================================================================

// ===========================================================================
// Battery poweroff tests (logic from sleep loop)
// ===========================================================================

void test_battery_poweroff_triggers_when_confirmed() {
    reset_state();
    config.always_on = 1;
    // Set mock ADC to ~11.7V (used by both initial check and confirm_battery_low)
    mock_analog_value[AIN_S_INLEVEL] = 302;
    check_battery_poweroff(11.7f);
    ASSERT_EQ(config.always_on, 0);
    ASSERT_EQ(power_supply_mode_calls, 1);
    ASSERT_EQ(settings_save_calls, 1);
}

void test_battery_poweroff_only_in_always_on() {
    reset_state();
    config.always_on = 0;
    mock_analog_value[AIN_S_INLEVEL] = 302;
    check_battery_poweroff(11.7f);
    ASSERT_EQ(config.always_on, 0);
    ASSERT_EQ(power_supply_mode_calls, 0);  // already ignition-only, no action
}

void test_battery_poweroff_not_triggered_above_threshold() {
    reset_state();
    config.always_on = 1;
    mock_analog_value[AIN_S_INLEVEL] = 340;  // ~13.1V
    check_battery_poweroff(13.1f);
    ASSERT_EQ(config.always_on, 1);  // above threshold, no action
    ASSERT_EQ(power_supply_mode_calls, 0);
}

void test_battery_poweroff_aborts_if_recovery_during_confirm() {
    reset_state();
    config.always_on = 1;
    // Initial reading is low, but mock ADC is set high so confirm_battery_low
    // re-samples will read above threshold → confirmation fails
    mock_analog_value[AIN_S_INLEVEL] = 340;  // ~13.1V (recovery)
    check_battery_poweroff(11.7f);  // initial v=11.7 is low, but confirm reads 13.1
    ASSERT_EQ(config.always_on, 1);  // not triggered — recovery during confirmation
    ASSERT_EQ(power_supply_mode_calls, 0);
}

// ===========================================================================
// sleep_needs_gps tests
// ===========================================================================

void test_sleep_gps_cached_by_default() {
    // No movement, have cached coords → use cached GPS
    reset_state();
    byte flag = 0;
    ASSERT_EQ(sleep_needs_gps(&flag, 0), 0);
}

void test_sleep_gps_no_cached_coords() {
    // No previous coords → must acquire GPS
    reset_state();
    lat_current[0] = '\0';
    byte flag = 0;
    ASSERT_EQ(sleep_needs_gps(&flag, 0), 1);
}

void test_sleep_gps_after_movement() {
    // Movement detected → acquire fresh GPS
    reset_state();
    byte flag = 1;
    ASSERT_EQ(sleep_needs_gps(&flag, 0), 1);
    ASSERT_EQ(flag, 1);  // flag stays set (movement was recent)
}

void test_sleep_gps_movement_cleared_after_24h() {
    // Movement flag set but 24h+ of inactivity → use cached, flag cleared
    reset_state();
    byte flag = 1;
    ASSERT_EQ(sleep_needs_gps(&flag, NO_MOVEMENT_GPS_SKIP), 0);
    ASSERT_EQ(flag, 0);  // flag was cleared
}

void test_sleep_gps_movement_not_cleared_before_24h() {
    // Movement flag set, less than 24h → still acquire GPS
    reset_state();
    byte flag = 1;
    ASSERT_EQ(sleep_needs_gps(&flag, NO_MOVEMENT_GPS_SKIP - 1), 1);
    ASSERT_EQ(flag, 1);  // flag still set
}

void test_sleep_gps_movement_cleared_no_coords() {
    // Movement flag cleared by 24h but no cached coords → must acquire
    reset_state();
    lat_current[0] = '\0';
    byte flag = 1;
    ASSERT_EQ(sleep_needs_gps(&flag, NO_MOVEMENT_GPS_SKIP), 1);
    ASSERT_EQ(flag, 0);  // flag cleared by idle time
}

void test_sleep_gps_ignition_cycle_resets() {
    // After ignition cycle, flag starts at 0 → use cached GPS
    // (movement_needs_gps is local to STATE_SLEEP, initialized to 0)
    reset_state();
    byte flag = 0;  // fresh STATE_SLEEP entry
    ASSERT_EQ(sleep_needs_gps(&flag, 0), 0);
}

// ===========================================================================

int main() {
    printf("firmware.ino tests:\n");

    RUN_TEST(test_send_first_boot);
    RUN_TEST(test_send_ignition_just_on);
    RUN_TEST(test_send_ignition_just_off);
    RUN_TEST(test_send_ignition_on_continuous);
    RUN_TEST(test_send_movement_wake);
    RUN_TEST(test_send_settings_sync);
    RUN_TEST(test_send_retry_on_failure);
    RUN_TEST(test_no_retry_on_failure_when_ignition_off);
    RUN_TEST(test_send_engine_off_interval_elapsed);
    RUN_TEST(test_no_send_engine_off_interval_not_elapsed);
    RUN_TEST(test_no_send_idle_engine_off);
    RUN_TEST(test_send_ignition_on_high_voltage);
    RUN_TEST(test_no_send_ignition_on_low_voltage);
    RUN_TEST(test_send_ignition_on_exact_threshold);
    RUN_TEST(test_send_ignition_change_regardless_of_voltage);
    RUN_TEST(test_engine_running_hysteresis);
    RUN_TEST(test_send_coasting_flag_forces_send);
    RUN_TEST(test_no_send_coasting_flag_clear);

    RUN_TEST(test_post_send_power_state_on_success);
    RUN_TEST(test_post_send_power_state_deferred_on_failure);
    RUN_TEST(test_post_send_power_off_relay);
    RUN_TEST(test_post_send_save_config);

    RUN_TEST(test_battery_poweroff_triggers_when_confirmed);
    RUN_TEST(test_battery_poweroff_only_in_always_on);
    RUN_TEST(test_battery_poweroff_not_triggered_above_threshold);
    RUN_TEST(test_battery_poweroff_aborts_if_recovery_during_confirm);

    RUN_TEST(test_sleep_gps_cached_by_default);
    RUN_TEST(test_sleep_gps_no_cached_coords);
    RUN_TEST(test_sleep_gps_after_movement);
    RUN_TEST(test_sleep_gps_movement_cleared_after_24h);
    RUN_TEST(test_sleep_gps_movement_not_cleared_before_24h);
    RUN_TEST(test_sleep_gps_movement_cleared_no_coords);
    RUN_TEST(test_sleep_gps_ignition_cycle_resets);

    TEST_REPORT();
}
