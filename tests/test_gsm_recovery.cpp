// Tests for gsm_send_recovery() — progressive escalation on consecutive
// send failures. The real function lives in gsm.ino which we can't compile
// here because of its modem dependencies, so we mirror it below and test
// the tier logic directly against mock counters.

#include "mocks/Arduino.h"
#include "mocks/SPI.h"
#include "generated_config.h"
#include "test_framework.h"
int test_count = 0;
int test_failures = 0;

// Required mock-state globals (linker deps from Arduino.h mocks)
int      mock_pin_mode[MOCK_NUM_PINS];
int      mock_pin_value[MOCK_NUM_PINS];
int      mock_analog_value[MOCK_NUM_PINS];
unsigned long mock_millis_value;
unsigned long mock_micros_value;
HardwareSerial SerialUSB;
SPIClass SPI;

// -- Recovery thresholds (must match gsm.ino) -------------------------------

#define GSM_RECOVERY_SOFT_THRESHOLD 3
#define GSM_RECOVERY_HARD_THRESHOLD 5

// -- Tracking state ---------------------------------------------------------

int gsm_send_failures = 0;
int deactivate_calls  = 0;
int power_cycle_calls = 0;

void gsm_deactivate_stub()    { deactivate_calls++; }
void modem_power_cycle_stub() { power_cycle_calls++; }

// -- Function under test (mirrored from gsm.ino) ----------------------------

void gsm_send_recovery() {
    if (gsm_send_failures < GSM_RECOVERY_SOFT_THRESHOLD) {
        return;
    }
    if (gsm_send_failures < GSM_RECOVERY_HARD_THRESHOLD) {
        gsm_deactivate_stub();
        return;
    }
    modem_power_cycle_stub();
}

// -- Helpers ----------------------------------------------------------------

void reset_state() {
    gsm_send_failures = 0;
    deactivate_calls = 0;
    power_cycle_calls = 0;
}

// -- Tests ------------------------------------------------------------------

void test_no_action_on_first_failure() {
    reset_state();
    gsm_send_failures = 1;
    gsm_send_recovery();
    ASSERT_EQ(deactivate_calls, 0);
    ASSERT_EQ(power_cycle_calls, 0);
}

void test_no_action_on_second_failure() {
    reset_state();
    gsm_send_failures = 2;
    gsm_send_recovery();
    ASSERT_EQ(deactivate_calls, 0);
    ASSERT_EQ(power_cycle_calls, 0);
}

void test_pdp_deactivate_at_soft_threshold() {
    reset_state();
    gsm_send_failures = 3;
    gsm_send_recovery();
    ASSERT_EQ(deactivate_calls, 1);
    ASSERT_EQ(power_cycle_calls, 0);
}

void test_pdp_deactivate_within_soft_range() {
    reset_state();
    gsm_send_failures = 4;
    gsm_send_recovery();
    ASSERT_EQ(deactivate_calls, 1);
    ASSERT_EQ(power_cycle_calls, 0);
}

void test_power_cycle_at_hard_threshold() {
    reset_state();
    gsm_send_failures = 5;
    gsm_send_recovery();
    ASSERT_EQ(deactivate_calls, 0);
    ASSERT_EQ(power_cycle_calls, 1);
}

void test_power_cycle_persists_above_threshold() {
    reset_state();
    gsm_send_failures = 12;
    gsm_send_recovery();
    ASSERT_EQ(deactivate_calls, 0);
    ASSERT_EQ(power_cycle_calls, 1);
}

void test_escalation_sequence() {
    reset_state();
    // walk through failure sequence — no power cycle under hard threshold
    for (int f = 1; f <= 4; f++) {
        gsm_send_failures = f;
        gsm_send_recovery();
    }
    ASSERT_EQ(power_cycle_calls, 0);

    gsm_send_failures = 5;
    gsm_send_recovery();
    ASSERT_EQ(power_cycle_calls, 1);

    gsm_send_failures = 6;
    gsm_send_recovery();
    ASSERT_EQ(power_cycle_calls, 2);
}

// ===========================================================================

int main() {
    printf("gsm recovery escalation tests:\n");

    RUN_TEST(test_no_action_on_first_failure);
    RUN_TEST(test_no_action_on_second_failure);
    RUN_TEST(test_pdp_deactivate_at_soft_threshold);
    RUN_TEST(test_pdp_deactivate_within_soft_range);
    RUN_TEST(test_power_cycle_at_hard_threshold);
    RUN_TEST(test_power_cycle_persists_above_threshold);
    RUN_TEST(test_escalation_sequence);

    TEST_REPORT();
}
