// Tests for power.ino — relay sequencing via ioexpander

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

// -- ioexp_set call log for verification ------------------------------------

#define IOEXP_LOG_MAX 16
struct IoexpCall { byte channel; byte value; };
IoexpCall ioexp_log[IOEXP_LOG_MAX];
int ioexp_log_count = 0;

#define IOEXP_AIO1 0
#define IOEXP_AIO2 1

void ioexp_set(byte channel, byte value) {
    if (ioexp_log_count < IOEXP_LOG_MAX) {
        ioexp_log[ioexp_log_count].channel = channel;
        ioexp_log[ioexp_log_count].value = value;
        ioexp_log_count++;
    }
}

// -- Functions under test (copied from power.ino) ---------------------------

// Must match the value in power.ino (16-bit RTC wakeup counter, 1Hz).
#define RTC_WAKEUP_MAX_SECONDS 65535

// Simulates one iteration of the STOP2 sleep loop in firmware.ino:
//   - pick a wake duration from the pending countdown
//   - clamp to RTC_WAKEUP_MAX_SECONDS (the actual HW sleep length)
//   - decrement the countdown by the clamped value (so it tracks real time,
//     not the requested duration)
// Returns the clamped wake_seconds used for this cycle.
long sleep_loop_step(long *telemetry_remaining) {
    long wake_seconds = *telemetry_remaining;
    if (wake_seconds > RTC_WAKEUP_MAX_SECONDS) wake_seconds = RTC_WAKEUP_MAX_SECONDS;
    if (*telemetry_remaining > 0) *telemetry_remaining -= wake_seconds;
    return wake_seconds;
}

void set_power_supply_off() {
    ioexp_set(IOEXP_AIO1, LOW);
    delay(50);
    ioexp_set(IOEXP_AIO2, HIGH);
    delay(350);
    ioexp_set(IOEXP_AIO2, LOW);
}

void set_power_supply_mode() {
    if (config.always_on == 1) {
        ioexp_set(IOEXP_AIO2, LOW);
        delay(50);
        ioexp_set(IOEXP_AIO1, HIGH);
        delay(350);
        ioexp_set(IOEXP_AIO1, LOW);
    } else {
        ioexp_set(IOEXP_AIO1, LOW);
        delay(50);
        ioexp_set(IOEXP_AIO2, HIGH);
        delay(350);
        ioexp_set(IOEXP_AIO2, LOW);
    }
}

// -- Helpers ----------------------------------------------------------------

void reset_state() {
    memset(&config, 0, sizeof(config));
    ioexp_log_count = 0;
}

// ===========================================================================

void test_always_on_relay_sequence() {
    reset_state();
    config.always_on = 1;
    set_power_supply_mode();

    // Sequence: AIO2 LOW (release), AIO1 HIGH (pulse), AIO1 LOW (release)
    ASSERT_EQ(ioexp_log_count, 3);
    ASSERT_EQ(ioexp_log[0].channel, IOEXP_AIO2);
    ASSERT_EQ(ioexp_log[0].value, LOW);
    ASSERT_EQ(ioexp_log[1].channel, IOEXP_AIO1);
    ASSERT_EQ(ioexp_log[1].value, HIGH);
    ASSERT_EQ(ioexp_log[2].channel, IOEXP_AIO1);
    ASSERT_EQ(ioexp_log[2].value, LOW);
}

void test_ignition_only_relay_sequence() {
    reset_state();
    config.always_on = 0;
    set_power_supply_mode();

    // Sequence: AIO1 LOW (release), AIO2 HIGH (pulse), AIO2 LOW (release)
    ASSERT_EQ(ioexp_log_count, 3);
    ASSERT_EQ(ioexp_log[0].channel, IOEXP_AIO1);
    ASSERT_EQ(ioexp_log[0].value, LOW);
    ASSERT_EQ(ioexp_log[1].channel, IOEXP_AIO2);
    ASSERT_EQ(ioexp_log[1].value, HIGH);
    ASSERT_EQ(ioexp_log[2].channel, IOEXP_AIO2);
    ASSERT_EQ(ioexp_log[2].value, LOW);
}

void test_power_off_ignores_config() {
    reset_state();
    config.always_on = 1;  // config says always-on...
    set_power_supply_off();  // ...but poweroff always goes to ignition-only

    // Same sequence as ignition-only
    ASSERT_EQ(ioexp_log_count, 3);
    ASSERT_EQ(ioexp_log[0].channel, IOEXP_AIO1);
    ASSERT_EQ(ioexp_log[0].value, LOW);
    ASSERT_EQ(ioexp_log[1].channel, IOEXP_AIO2);
    ASSERT_EQ(ioexp_log[1].value, HIGH);
    ASSERT_EQ(ioexp_log[2].channel, IOEXP_AIO2);
    ASSERT_EQ(ioexp_log[2].value, LOW);
}

void test_power_off_does_not_read_config() {
    // Verify set_power_supply_off produces the same result regardless of config
    for (int ao = 0; ao <= 1; ao++) {
        reset_state();
        config.always_on = ao;
        set_power_supply_off();
        ASSERT_EQ(ioexp_log[1].channel, IOEXP_AIO2);  // always pulses AIO2
        ASSERT_EQ(ioexp_log[1].value, HIGH);
    }
}

// ===========================================================================
// Sleep loop countdown — reproduces the 18h-wake bug when loop_interval=86400

void test_sleep_loop_short_interval_single_cycle() {
    // 3600s fits within the RTC counter, should wake exactly once
    long remaining = 3600;
    long slept = sleep_loop_step(&remaining);
    ASSERT_EQ(slept, 3600);
    ASSERT_EQ(remaining, 0);
}

void test_sleep_loop_exact_max() {
    // 65535s hits the boundary exactly
    long remaining = RTC_WAKEUP_MAX_SECONDS;
    long slept = sleep_loop_step(&remaining);
    ASSERT_EQ(slept, RTC_WAKEUP_MAX_SECONDS);
    ASSERT_EQ(remaining, 0);
}

void test_sleep_loop_long_interval_clamped() {
    // loop_interval=86400 (24h) — first cycle clamped to 65535, remainder left
    long remaining = 86400;
    long slept = sleep_loop_step(&remaining);
    ASSERT_EQ(slept, RTC_WAKEUP_MAX_SECONDS);
    ASSERT_EQ(remaining, 86400 - RTC_WAKEUP_MAX_SECONDS);  // 20865s left
}

void test_sleep_loop_long_interval_totals_to_requested() {
    // Full simulation: loop_interval=86400 should take two cycles to
    // accumulate the full 86400s before telemetry fires.
    long remaining = 86400;
    long total_slept = 0;
    int cycles = 0;
    while (remaining > 0 && cycles < 10) {
        total_slept += sleep_loop_step(&remaining);
        cycles++;
    }
    ASSERT_EQ(total_slept, 86400);
    ASSERT_EQ(cycles, 2);
    ASSERT_EQ(remaining, 0);
}

// ===========================================================================

int main() {
    printf("power.ino tests:\n");

    RUN_TEST(test_always_on_relay_sequence);
    RUN_TEST(test_ignition_only_relay_sequence);
    RUN_TEST(test_power_off_ignores_config);
    RUN_TEST(test_power_off_does_not_read_config);
    RUN_TEST(test_sleep_loop_short_interval_single_cycle);
    RUN_TEST(test_sleep_loop_exact_max);
    RUN_TEST(test_sleep_loop_long_interval_clamped);
    RUN_TEST(test_sleep_loop_long_interval_totals_to_requested);

    TEST_REPORT();
}
