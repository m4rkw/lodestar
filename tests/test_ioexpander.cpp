// Tests for ioexpander.ino — NCV7240 bit manipulation and SPI framing

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

// -- Forward declarations ---------------------------------------------------

void ioexp_init();
void ioexp_update();
void ioexp_set_mode(byte channel, byte mode);
void ioexp_set(byte channel, byte value);

// -- Include file under test ------------------------------------------------

#include "../ioexpander.ino"

// -- Helpers ----------------------------------------------------------------

void reset_ioexp() {
    ioexp_reg[0] = 0;
    ioexp_reg[1] = 0;
    SPI.mock_reset();
    mock_reset();
}

// ===========================================================================

void test_init_all_standby() {
    reset_ioexp();
    ioexp_init();
    ASSERT_EQ(ioexp_reg[0], 0);
    ASSERT_EQ(ioexp_reg[1], 0);
}

void test_set_mode_channel0_on() {
    reset_ioexp();
    ioexp_set_mode(IOEXP_AIO1, IOEXP_ON);  // channel 0, mode 10
    // Channel 0 maps to reg[1] bits 1:0
    ASSERT_EQ(ioexp_reg[1] & 0x03, 0b10);
    ASSERT_EQ(ioexp_reg[0], 0);  // high byte untouched
}

void test_set_mode_channel0_off() {
    reset_ioexp();
    ioexp_set_mode(IOEXP_AIO1, IOEXP_OFF);  // channel 0, mode 11
    ASSERT_EQ(ioexp_reg[1] & 0x03, 0b11);
}

void test_set_mode_channel0_standby() {
    reset_ioexp();
    ioexp_set_mode(IOEXP_AIO1, IOEXP_ON);
    ioexp_set_mode(IOEXP_AIO1, IOEXP_STANDBY);
    ASSERT_EQ(ioexp_reg[1] & 0x03, 0b00);
}

void test_set_mode_channel1() {
    reset_ioexp();
    ioexp_set_mode(IOEXP_AIO2, IOEXP_ON);  // channel 1, bits 3:2
    ASSERT_EQ((ioexp_reg[1] >> 2) & 0x03, 0b10);
    ASSERT_EQ(ioexp_reg[1] & 0x03, 0b00);  // channel 0 undisturbed
}

void test_set_mode_channel4_high_byte() {
    reset_ioexp();
    ioexp_set_mode(IOEXP_DIO1, IOEXP_ON);  // channel 4 → reg[0] bits 1:0
    ASSERT_EQ(ioexp_reg[0] & 0x03, 0b10);
    ASSERT_EQ(ioexp_reg[1], 0);  // low byte untouched
}

void test_multiple_channels_independent() {
    reset_ioexp();
    ioexp_set_mode(IOEXP_AIO1, IOEXP_ON);   // ch0
    ioexp_set_mode(IOEXP_AIO3, IOEXP_OFF);  // ch2
    ASSERT_EQ(ioexp_reg[1] & 0x03, 0b10);         // ch0: ON
    ASSERT_EQ((ioexp_reg[1] >> 4) & 0x03, 0b11);  // ch2: OFF
    ASSERT_EQ((ioexp_reg[1] >> 2) & 0x03, 0b00);  // ch1: still standby
}

void test_ioexp_set_high_sets_on() {
    reset_ioexp();
    ioexp_set(IOEXP_AIO1, HIGH);
    ASSERT_EQ(ioexp_reg[1] & 0x03, IOEXP_ON);
}

void test_ioexp_set_low_sets_standby() {
    reset_ioexp();
    ioexp_set(IOEXP_AIO1, HIGH);  // first set ON
    ioexp_set(IOEXP_AIO1, LOW);   // then set STANDBY
    ASSERT_EQ(ioexp_reg[1] & 0x03, IOEXP_STANDBY);
}

void test_spi_transfer_order() {
    reset_ioexp();
    ioexp_set_mode(IOEXP_AIO1, IOEXP_ON);
    // ioexp_update sends reg[0] (high byte) first, then reg[1] (low byte)
    // find the last two transfers in the SPI log
    ASSERT_TRUE(SPI.log_count >= 2);
    int last = SPI.log_count;
    ASSERT_EQ(SPI.log[last - 2].tx, ioexp_reg[0]);  // high byte first
    ASSERT_EQ(SPI.log[last - 1].tx, ioexp_reg[1]);  // low byte second
}

// ===========================================================================

int main() {
    printf("ioexpander.ino tests:\n");

    RUN_TEST(test_init_all_standby);
    RUN_TEST(test_set_mode_channel0_on);
    RUN_TEST(test_set_mode_channel0_off);
    RUN_TEST(test_set_mode_channel0_standby);
    RUN_TEST(test_set_mode_channel1);
    RUN_TEST(test_set_mode_channel4_high_byte);
    RUN_TEST(test_multiple_channels_independent);
    RUN_TEST(test_ioexp_set_high_sets_on);
    RUN_TEST(test_ioexp_set_low_sets_standby);
    RUN_TEST(test_spi_transfer_order);

    TEST_REPORT();
}
