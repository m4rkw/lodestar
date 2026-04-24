// Tests for DNS URC parser (dns.ino — gsm_parse_dnsgip)
//
// BG96 AT+QIDNSGIP=1,"<host>" emits two URC shapes:
//   +QIURC: "dnsgip",<err>,<IP_count>,<DNS_ttl>   — status URC
//   +QIURC: "dnsgip","<ip>"                       — IP URC (one per address)
// The parser extracts the IPv4 address string from the IP URC form.

#include "mocks/Arduino.h"
#include "generated_config.h"
#include "test_framework.h"
#include <string.h>

int test_count = 0;
int test_failures = 0;

// -- Mock state -------------------------------------------------------------

int      mock_pin_mode[MOCK_NUM_PINS];
int      mock_pin_value[MOCK_NUM_PINS];
int      mock_analog_value[MOCK_NUM_PINS];
unsigned long mock_millis_value;
unsigned long mock_micros_value;
HardwareSerial SerialUSB;

// -- Parser under test ------------------------------------------------------

int gsm_parse_dnsgip(const char *reply, char *ip_out, int ip_out_len);

#include "../dns.ino"

// ===========================================================================

void test_parse_ipv4_url() {
    char ip[16] = "";
    const char *reply = "\r\n+QIURC: \"dnsgip\",\"203.0.113.42\"\r\n";
    int ok = gsm_parse_dnsgip(reply, ip, sizeof(ip));
    ASSERT_EQ(ok, 1);
    ASSERT_STREQ(ip, "203.0.113.42");
}

void test_parse_picks_first_ip_from_multi_reply() {
    // Real BG96: status URC followed by multiple IP URCs — take the first.
    char ip[16] = "";
    const char *reply =
        "\r\nOK\r\n"
        "\r\n+QIURC: \"dnsgip\",0,2,60\r\n"
        "\r\n+QIURC: \"dnsgip\",\"198.51.100.7\"\r\n"
        "\r\n+QIURC: \"dnsgip\",\"198.51.100.8\"\r\n";
    int ok = gsm_parse_dnsgip(reply, ip, sizeof(ip));
    ASSERT_EQ(ok, 1);
    ASSERT_STREQ(ip, "198.51.100.7");
}

void test_parse_status_urc_alone_returns_zero() {
    // Just the status URC with no IP yet — nothing to extract.
    char ip[16] = "";
    const char *reply = "\r\n+QIURC: \"dnsgip\",0,1,60\r\n";
    int ok = gsm_parse_dnsgip(reply, ip, sizeof(ip));
    ASSERT_EQ(ok, 0);
}

void test_parse_error_status_returns_zero() {
    // Non-zero error (e.g. NXDOMAIN) — no IP forthcoming.
    char ip[16] = "";
    const char *reply = "\r\n+QIURC: \"dnsgip\",550,0,0\r\n";
    int ok = gsm_parse_dnsgip(reply, ip, sizeof(ip));
    ASSERT_EQ(ok, 0);
}

void test_parse_missing_dnsgip_returns_zero() {
    char ip[16] = "";
    const char *reply = "\r\nOK\r\n";
    int ok = gsm_parse_dnsgip(reply, ip, sizeof(ip));
    ASSERT_EQ(ok, 0);
}

void test_parse_rejects_non_numeric_chars() {
    // Defensive: if something other than [0-9.] appears inside the quotes,
    // reject rather than copy garbage.
    char ip[16] = "";
    const char *reply = "\r\n+QIURC: \"dnsgip\",\"host.example\"\r\n";
    int ok = gsm_parse_dnsgip(reply, ip, sizeof(ip));
    ASSERT_EQ(ok, 0);
}

void test_parse_rejects_missing_close_quote() {
    char ip[16] = "";
    const char *reply = "\r\n+QIURC: \"dnsgip\",\"1.2.3.4";
    int ok = gsm_parse_dnsgip(reply, ip, sizeof(ip));
    ASSERT_EQ(ok, 0);
}

void test_parse_respects_output_buffer_size() {
    // 16-byte buffer can't hold a pathological over-long "IP" — reject.
    char ip[8] = "";
    const char *reply = "\r\n+QIURC: \"dnsgip\",\"123.456.789.012\"\r\n";
    int ok = gsm_parse_dnsgip(reply, ip, sizeof(ip));
    ASSERT_EQ(ok, 0);
}

// ===========================================================================

int main() {
    printf("dns.ino tests:\n");

    RUN_TEST(test_parse_ipv4_url);
    RUN_TEST(test_parse_picks_first_ip_from_multi_reply);
    RUN_TEST(test_parse_status_urc_alone_returns_zero);
    RUN_TEST(test_parse_error_status_returns_zero);
    RUN_TEST(test_parse_missing_dnsgip_returns_zero);
    RUN_TEST(test_parse_rejects_non_numeric_chars);
    RUN_TEST(test_parse_rejects_missing_close_quote);
    RUN_TEST(test_parse_respects_output_buffer_size);

    TEST_REPORT();
}
