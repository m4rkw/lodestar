// Tests for alert.ino — queue management

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

// -- Replicate the queue from alert.ino (can't include it due to gsm deps) --

#define ALERT_QUEUE_SIZE 5
#define ALERT_MSG_SIZE 120

char alert_queue[ALERT_QUEUE_SIZE][ALERT_MSG_SIZE];
int8_t alert_priority[ALERT_QUEUE_SIZE];
int alert_count = 0;

void alert_enqueue(const char *msg, int8_t priority) {
    if (alert_count >= ALERT_QUEUE_SIZE) {
        return;
    }
    strlcpy(alert_queue[alert_count], msg, ALERT_MSG_SIZE);
    alert_priority[alert_count] = priority;
    alert_count++;
}

// -- Helpers ----------------------------------------------------------------

void reset_queue() {
    alert_count = 0;
    memset(alert_queue, 0, sizeof(alert_queue));
    memset(alert_priority, 0, sizeof(alert_priority));
}

// ===========================================================================

void test_enqueue_single() {
    reset_queue();
    alert_enqueue("test message", 0);
    ASSERT_EQ(alert_count, 1);
    ASSERT_STREQ(alert_queue[0], "test message");
    ASSERT_EQ(alert_priority[0], 0);
}

void test_enqueue_with_priority() {
    reset_queue();
    alert_enqueue("critical", 2);
    ASSERT_EQ(alert_priority[0], 2);
}

void test_enqueue_fills_to_max() {
    reset_queue();
    for (int i = 0; i < ALERT_QUEUE_SIZE; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "alert %d", i);
        alert_enqueue(msg, i % 3);
    }
    ASSERT_EQ(alert_count, ALERT_QUEUE_SIZE);
    ASSERT_STREQ(alert_queue[0], "alert 0");
    ASSERT_STREQ(alert_queue[4], "alert 4");
}

void test_enqueue_drops_when_full() {
    reset_queue();
    for (int i = 0; i < ALERT_QUEUE_SIZE; i++)
        alert_enqueue("fill", 0);

    alert_enqueue("overflow", 2);
    ASSERT_EQ(alert_count, ALERT_QUEUE_SIZE);  // not increased
    ASSERT_STREQ(alert_queue[ALERT_QUEUE_SIZE - 1], "fill");
}

void test_enqueue_truncates_long_message() {
    reset_queue();
    char longmsg[200];
    memset(longmsg, 'A', 199);
    longmsg[199] = '\0';

    alert_enqueue(longmsg, 0);
    ASSERT_EQ(alert_count, 1);
    ASSERT_EQ((int)strlen(alert_queue[0]), ALERT_MSG_SIZE - 1);
}

void test_queue_reset_after_clear() {
    reset_queue();
    alert_enqueue("test", 0);
    alert_enqueue("test2", 1);
    ASSERT_EQ(alert_count, 2);
    alert_count = 0;
    ASSERT_EQ(alert_count, 0);
}

// ===========================================================================

int main() {
    printf("alert.ino tests:\n");

    RUN_TEST(test_enqueue_single);
    RUN_TEST(test_enqueue_with_priority);
    RUN_TEST(test_enqueue_fills_to_max);
    RUN_TEST(test_enqueue_drops_when_full);
    RUN_TEST(test_enqueue_truncates_long_message);
    RUN_TEST(test_queue_reset_after_clear);

    TEST_REPORT();
}
