// Tests for data.ino — battery alerts and CSV format

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

// -- Globals ----------------------------------------------------------------

struct settings {
    char apn[64]; char user[20]; char pwd[20];
    char sim_pin[5]; char imei[20];
    int loop_interval;
    int8_t always_on; int8_t movement_alarm;
    uint8_t psk[32];
};

settings config;
char data_current[DATA_LIMIT];
int data_index = 0;
char time_char[32];
char lat_current[32];
char lon_current[32];
float gps_speed, gps_altitude, gps_heading;
long gps_hdop, gps_sats;
float battery_v;
byte battery_warning_status;
byte powered_on;
byte send_int_to_server;
char ignition;
byte set_power_state;
unsigned long wake_start_millis;
byte use_cached_gps;
int cell_mcc, cell_mnc;
unsigned long cell_lac, cell_cid;
char cell_rat[8];
byte cell_location;
byte cell_fields_dirty;
byte engine_running = 0;
int below_voltage_count = 0;
unsigned long total_sleep_seconds = 0;
#define ENGINE_STOPPED_COUNT 10

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

// Stub out read_mcu_temperature — the real one reads STM32 calibration
// addresses that don't exist in the test environment
#define MOCK_READ_MCU_TEMPERATURE
float read_mcu_temperature() { return 25.0f; }



// -- Stubs ------------------------------------------------------------------

int mock_gps_fix_result = 1;
int acc_read_x = 0, acc_read_y = 0, acc_read_z = 0;

void gsm_get_time() {}
void gsm_get_serving_cell() {}
int collect_gps_data() { return mock_gps_fix_result; }
int acc_read(int *x, int *y, int *z) {
    *x = acc_read_x; *y = acc_read_y; *z = acc_read_z;
    return 0;
}
int gsm_send_data() { return 1; }
int alert_send() { return 1; }
byte last_send_ok;

// -- Include file under test ------------------------------------------------

#include "../data.ino"

// -- Helpers ----------------------------------------------------------------

void reset_state() {
    memset(&config, 0, sizeof(config));
    config.loop_interval = 3600;
    config.always_on = 1;
    memset(data_current, 0, sizeof(data_current));
    data_index = 0;
    strlcpy(time_char, "14/03/26,12:00:00.000000+00", sizeof(time_char));
    strlcpy(lat_current, "51.500000", sizeof(lat_current));
    strlcpy(lon_current, "-0.100000", sizeof(lon_current));
    gps_speed = 55.5; gps_altitude = 100.0; gps_heading = 180.0;
    gps_hdop = 120; gps_sats = 8;
    battery_v = 13.0;
    battery_warning_status = 0;
    powered_on = 0;
    send_int_to_server = 0;
    ignition = 1;  // engine OFF
    set_power_state = 0;
    wake_start_millis = 0;
    total_sleep_seconds = 0;
    captured_alert_count = 0;
    mock_gps_fix_result = 1;
    use_cached_gps = 0;
    cell_mcc = 0; cell_mnc = 0;
    cell_lac = 0; cell_cid = 0;
    cell_rat[0] = '\0';
    cell_location = 0;
    cell_fields_dirty = 1;  // firmware starts dirty so first packet always sends
    acc_read_x = 10; acc_read_y = -20; acc_read_z = 1000;
    mock_millis_value = 60000;
    mock_analog_value[AIN_S_INLEVEL] = 310;  // ~12V
}

// ===========================================================================
// Battery warning alert tests
// ===========================================================================

void test_battery_warning_fires_when_confirmed_low() {
    reset_state();
    // ADC value that gives voltage below BATTERY_WARNING_LEVEL (11.9f)
    // v = adc * (12.0 * 3.3 / 1024) ≈ adc * 0.03867
    // For 11.8V: adc ≈ 305
    mock_analog_value[AIN_S_INLEVEL] = 305;
    data_reset();
    collect_data(1);
    // confirm_battery_low re-samples 4 times, all from same mock ADC → confirmed
    ASSERT_EQ(captured_alert_count, 1);
    ASSERT_STRCONTAINS(captured_alerts[0].msg, "low battery");
    ASSERT_EQ(captured_alerts[0].priority, 2);
}

void test_battery_warning_no_alert_above_threshold() {
    reset_state();
    mock_analog_value[AIN_S_INLEVEL] = 340;  // ~13.1V, well above threshold
    data_reset();
    collect_data(1);
    ASSERT_EQ(captured_alert_count, 0);
}

void test_battery_warning_no_duplicate() {
    reset_state();
    mock_analog_value[AIN_S_INLEVEL] = 305;

    // first low reading triggers alert
    data_reset();
    collect_data(1);
    ASSERT_EQ(captured_alert_count, 1);

    // another low reading — no duplicate (battery_warning_status is set)
    data_reset();
    collect_data(1);
    ASSERT_EQ(captured_alert_count, 1);
}

void test_battery_warning_resets_on_recovery() {
    reset_state();
    mock_analog_value[AIN_S_INLEVEL] = 305;

    // trigger the alert
    data_reset();
    collect_data(1);
    ASSERT_EQ(captured_alert_count, 1);

    // voltage recovers — resets warning status
    mock_analog_value[AIN_S_INLEVEL] = 340;
    data_reset();
    collect_data(1);
    ASSERT_EQ(battery_warning_status, 0);

    // drops again — should fire a new alert
    mock_analog_value[AIN_S_INLEVEL] = 305;
    data_reset();
    collect_data(1);
    ASSERT_EQ(captured_alert_count, 2);
}

// Battery auto-poweroff is now in the sleep loop (firmware.ino), not in collect_data.
// See test_firmware.cpp for those tests.

// ===========================================================================
// CSV format tests
// ===========================================================================

void test_csv_ignition_mapping() {
    reset_state();
    data_reset();
    collect_data(0);  // ignitionState=0 means ON → CSV field should be 1
    ASSERT_STRCONTAINS(data_current, ",1,");  // ignition=1 somewhere in the CSV
}

void test_csv_contains_config_sync() {
    reset_state();
    send_int_to_server = 1;
    config.loop_interval = 1800;
    config.always_on = 1;
    config.movement_alarm = 0;
    data_reset();
    collect_data(1);
    ASSERT_STRCONTAINS(data_current, ",int=1800;ao=1;ma=0");
}

void test_csv_no_suffix_when_neither_flag() {
    reset_state();
    send_int_to_server = 0;
    data_reset();
    collect_data(1);
    // should not contain ri= or int= suffix
    ASSERT_TRUE(strstr(data_current, ",ri=") == NULL);
    ASSERT_TRUE(strstr(data_current, ",int=") == NULL);
}

void test_csv_contains_waketime() {
    reset_state();
    wake_start_millis = 5000;
    mock_millis_value = 50000;  // 45s of wake time
    data_reset();
    collect_data(1);
    // waketime = (50000 - 5000) / 1000 = 45
    ASSERT_STRCONTAINS(data_current, ",45");
}

void test_data_reset() {
    reset_state();
    data_current[0] = 'x';
    data_index = 5;
    data_reset();
    ASSERT_EQ(data_current[0], '\0');
    ASSERT_EQ(data_index, 0);
}

// ===========================================================================
// Cached GPS tests (no movement in 24h)
// ===========================================================================

void test_cached_gps_skips_gps_fix() {
    reset_state();
    use_cached_gps = 1;
    mock_gps_fix_result = 0;  // GPS would fail, but shouldn't be called
    strlcpy(lat_current, "51.500000", sizeof(lat_current));
    strlcpy(lon_current, "-0.100000", sizeof(lon_current));
    data_reset();
    int result = collect_data(1);
    ASSERT_EQ(result, 1);  // returns success despite GPS mock returning 0
    ASSERT_STRCONTAINS(data_current, "51.500000");
    ASSERT_STRCONTAINS(data_current, "-0.100000");
}

void test_cached_gps_zeros_speed_hdop_sats() {
    reset_state();
    use_cached_gps = 1;
    gps_speed = 55.5;
    gps_hdop = 120;
    gps_sats = 8;
    data_reset();
    collect_data(1);
    // speed, hdop, sats should be zeroed
    ASSERT_STRCONTAINS(data_current, ",0.00,");  // speed
    ASSERT_STRCONTAINS(data_current, ",0,0,");   // hdop,sats
}

void test_cached_gps_preserves_altitude_heading() {
    reset_state();
    use_cached_gps = 1;
    gps_altitude = 100.0;
    gps_heading = 180.0;
    data_reset();
    collect_data(1);
    ASSERT_STRCONTAINS(data_current, "100.00");
    ASSERT_STRCONTAINS(data_current, "180.00");
}

// ===========================================================================
// Cell tower info tests
// ===========================================================================

void test_csv_contains_cell_fields() {
    reset_state();
    cell_mcc = 234; cell_mnc = 10;
    cell_lac = 6699; cell_cid = 15437;
    strlcpy(cell_rat, "CATM1", sizeof(cell_rat));
    data_reset();
    collect_data(1);
    ASSERT_STRCONTAINS(data_current, "mcc=234;mnc=10;lac=6699;cid=15437;cl=0;rat=CATM1");
}

void test_csv_rat_omitted_when_unknown() {
    // If the modem hasn't given us a RAT yet (cell_rat empty), the rat= token
    // should not appear in the extras — keeps the string tight and lets the
    // server distinguish "unknown" from "GSM".
    reset_state();
    cell_mcc = 234; cell_mnc = 10;
    cell_lac = 6699; cell_cid = 15437;
    cell_rat[0] = '\0';
    data_reset();
    collect_data(1);
    ASSERT_STRCONTAINS(data_current, "mcc=234;mnc=10;lac=6699;cid=15437;cl=0");
    ASSERT_TRUE(strstr(data_current, "rat=") == NULL);
}

void test_csv_cell_location_flag_when_gps_fails() {
    reset_state();
    mock_gps_fix_result = 0;
    cell_mcc = 234; cell_mnc = 10;
    cell_lac = 21127; cell_cid = 20447;
    data_reset();
    int result = collect_data(1);
    ASSERT_EQ(result, 1);  // cell info available, send proceeds
    ASSERT_EQ(cell_location, 1);
    ASSERT_STRCONTAINS(data_current, ";cl=1");
    // lat/lon should be 0 (no GPS fix)
    ASSERT_STRCONTAINS(data_current, ",0,0,");
}

void test_no_fix_when_no_gps_and_no_cell() {
    reset_state();
    mock_gps_fix_result = 0;
    cell_mcc = 0;  // no cell info
    data_reset();
    int result = collect_data(1);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(cell_location, 0);
}

// Bandwidth optimization: cell fields only emitted when dirty or
// cell_location fallback is active.

void test_cell_fields_skipped_when_not_dirty() {
    reset_state();
    cell_mcc = 234; cell_mnc = 10;
    cell_lac = 6699; cell_cid = 15437;
    cell_fields_dirty = 0;  // already acknowledged by server
    data_reset();
    collect_data(1);
    ASSERT_TRUE(strstr(data_current, "mcc=") == NULL);
    ASSERT_TRUE(strstr(data_current, ";cl=") == NULL);
}

void test_cell_fields_included_when_dirty() {
    reset_state();
    cell_mcc = 234; cell_mnc = 10;
    cell_lac = 6699; cell_cid = 15437;
    cell_fields_dirty = 1;
    data_reset();
    collect_data(1);
    ASSERT_STRCONTAINS(data_current, "mcc=234;mnc=10;lac=6699;cid=15437;cl=0");
}

void test_cell_location_always_includes_even_when_clean() {
    // cell_location=1 means GPS failed and we're reporting cell position —
    // must always carry the cell fields regardless of dirty state.
    reset_state();
    mock_gps_fix_result = 0;
    cell_mcc = 234; cell_mnc = 10;
    cell_lac = 21127; cell_cid = 20447;
    cell_fields_dirty = 0;  // already acknowledged
    data_reset();
    int result = collect_data(1);
    ASSERT_EQ(result, 1);
    ASSERT_STRCONTAINS(data_current, "mcc=234;mnc=10;lac=21127;cid=20447;cl=1");
}

void test_send_data_clears_dirty_on_success() {
    reset_state();
    cell_mcc = 234; cell_mnc = 10;
    cell_lac = 6699; cell_cid = 15437;
    cell_fields_dirty = 1;
    data_reset();
    collect_data(1);
    send_data();
    ASSERT_EQ(cell_fields_dirty, 0);
}

// ===========================================================================
// Accelerometer fields
// ===========================================================================

void test_csv_contains_accel_fields() {
    reset_state();
    acc_read_x = 10; acc_read_y = -20; acc_read_z = 1000;
    data_reset();
    collect_data(1);
    ASSERT_STRCONTAINS(data_current, ",ax=10;ay=-20;az=1000");
}

void test_csv_contains_uptime_suffix() {
    reset_state();
    wake_start_millis = 5000;
    mock_millis_value = 50000;  // total uptime = 50s
    data_reset();
    collect_data(1);
    ASSERT_STRCONTAINS(data_current, ",up=50;");
}

void test_csv_uptime_includes_sleep_seconds() {
    // millis() doesn't advance during STOP2; total_sleep_seconds tracks the
    // STOP2 time so uptime reflects wall-clock time since boot.
    reset_state();
    mock_millis_value = 12000;             // 12s awake
    total_sleep_seconds = 86400;           // plus one day asleep
    data_reset();
    collect_data(1);
    ASSERT_STRCONTAINS(data_current, ",up=86412;");
}

void test_csv_contains_mcu_temp_suffix() {
    // stub returns 25.0, dtostrf formats as "25.0"
    reset_state();
    data_reset();
    collect_data(1);
    ASSERT_STRCONTAINS(data_current, "mt=25.0");
}

void test_csv_accel_fields_on_cell_fallback() {
    // Accel should still be present when reporting via cell location
    reset_state();
    mock_gps_fix_result = 0;
    cell_mcc = 234; cell_mnc = 10;
    cell_lac = 6699; cell_cid = 15437;
    acc_read_x = 5; acc_read_y = 5; acc_read_z = 1020;
    data_reset();
    collect_data(1);
    ASSERT_STRCONTAINS(data_current, ",ax=5;ay=5;az=1020");
}

// ===========================================================================

int main() {
    printf("data.ino tests:\n");

    RUN_TEST(test_battery_warning_fires_when_confirmed_low);
    RUN_TEST(test_battery_warning_no_alert_above_threshold);
    RUN_TEST(test_battery_warning_no_duplicate);
    RUN_TEST(test_battery_warning_resets_on_recovery);

    RUN_TEST(test_csv_ignition_mapping);
    RUN_TEST(test_csv_contains_config_sync);
    RUN_TEST(test_csv_no_suffix_when_neither_flag);
    RUN_TEST(test_csv_contains_waketime);
    RUN_TEST(test_data_reset);

    RUN_TEST(test_cached_gps_skips_gps_fix);
    RUN_TEST(test_cached_gps_zeros_speed_hdop_sats);
    RUN_TEST(test_cached_gps_preserves_altitude_heading);

    RUN_TEST(test_csv_contains_cell_fields);
    RUN_TEST(test_csv_rat_omitted_when_unknown);
    RUN_TEST(test_csv_cell_location_flag_when_gps_fails);
    RUN_TEST(test_no_fix_when_no_gps_and_no_cell);

    RUN_TEST(test_cell_fields_skipped_when_not_dirty);
    RUN_TEST(test_cell_fields_included_when_dirty);
    RUN_TEST(test_cell_location_always_includes_even_when_clean);
    RUN_TEST(test_send_data_clears_dirty_on_success);

    RUN_TEST(test_csv_contains_accel_fields);
    RUN_TEST(test_csv_accel_fields_on_cell_fallback);

    RUN_TEST(test_csv_contains_uptime_suffix);
    RUN_TEST(test_csv_uptime_includes_sleep_seconds);
    RUN_TEST(test_csv_contains_mcu_temp_suffix);

    TEST_REPORT();
}
