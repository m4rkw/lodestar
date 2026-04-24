// Tests for cell info parsers (gsm.ino — gsm_parse_reg_urc, gsm_parse_qeng_servingcell)

#define CELL_UNIT_TEST  // skips runtime glue that touches the modem

#include "mocks/Arduino.h"
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

// -- Globals the parsers touch ----------------------------------------------

int           cell_mcc = 0, cell_mnc = 0;
unsigned long cell_lac = 0, cell_cid = 0;
char          cell_rat[8] = "";

// -- Parsers under test (copied from gsm.ino) -------------------------------

// Header-level prototype so we can include the implementation below.
int  gsm_parse_reg_urc(const char *reply);
int  gsm_parse_qeng_servingcell(const char *reply);
int  cell_refresh_due(uint32_t now_ms, uint32_t last_ms, unsigned char stale);

#include "../cell.ino"

// -- Helpers ----------------------------------------------------------------

void reset_cell() {
    cell_mcc = 0; cell_mnc = 0;
    cell_lac = 0; cell_cid = 0;
    cell_rat[0] = '\0';
}

// ===========================================================================
// +CREG URC (GSM registration)
// ===========================================================================

void test_creg_urc_home_with_location() {
    reset_cell();
    const char *reply = "\r\n+CREG: 1,\"1A2B\",\"0000C5F3\",0\r\n";
    int changed = gsm_parse_reg_urc(reply);
    ASSERT_EQ(changed, 1);
    ASSERT_EQ(cell_lac, 0x1A2B);
    ASSERT_EQ(cell_cid, 0x0000C5F3);
}

void test_cereg_urc_roaming_lte() {
    reset_cell();
    // +CEREG with stat=5 (roaming), TAC/CI hex, AcT=8 (eMTC/Cat-M1)
    const char *reply = "\r\n+CEREG: 5,\"FFFE\",\"0123ABCD\",8\r\n";
    int changed = gsm_parse_reg_urc(reply);
    ASSERT_EQ(changed, 1);
    ASSERT_EQ(cell_lac, 0xFFFE);
    ASSERT_EQ(cell_cid, 0x0123ABCD);
}

void test_creg_not_registered_no_update() {
    reset_cell();
    cell_lac = 0x1234; cell_cid = 0xABCD;
    // stat=0 means not registered — no location fields
    const char *reply = "\r\n+CREG: 0\r\n";
    int changed = gsm_parse_reg_urc(reply);
    ASSERT_EQ(changed, 0);
    // cache preserved
    ASSERT_EQ(cell_lac, 0x1234);
    ASSERT_EQ(cell_cid, 0xABCD);
}

void test_reg_urc_no_change_when_same_values() {
    reset_cell();
    cell_lac = 0x1A2B; cell_cid = 0x0000C5F3;
    const char *reply = "\r\n+CREG: 1,\"1A2B\",\"0000C5F3\",0\r\n";
    int changed = gsm_parse_reg_urc(reply);
    ASSERT_EQ(changed, 0);  // no-op when identical
}

void test_reg_urc_absent_returns_zero() {
    reset_cell();
    const char *reply = "\r\nOK\r\n";
    int changed = gsm_parse_reg_urc(reply);
    ASSERT_EQ(changed, 0);
}

// ===========================================================================
// +QENG: "servingcell" (resolves mcc/mnc plus lac/cid)
// ===========================================================================

void test_qeng_servingcell_lte() {
    reset_cell();
    // Real BG96 Cat-M1 response, trimmed for width
    const char *reply =
        "\r\n+QENG: \"servingcell\",\"NOCONN\",\"eMTC\",\"FDD\","
        "234,10,6A2B,255,3749,20,3,3,1A2B,-105,-12,-82,12,29\r\nOK\r\n";
    int ok = gsm_parse_qeng_servingcell(reply);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(cell_mcc, 234);
    ASSERT_EQ(cell_mnc, 10);
    ASSERT_EQ(cell_lac, 0x1A2B);
    ASSERT_EQ(cell_cid, 0x6A2B);
    ASSERT_STREQ(cell_rat, "CATM1");
}

void test_qeng_servingcell_nbiot() {
    reset_cell();
    // NB-IoT uses the same LTE-shape format as eMTC.
    const char *reply =
        "\r\n+QENG: \"servingcell\",\"NOCONN\",\"NBIoT\",\"FDD\","
        "234,10,6A2B,255,3749,20,3,3,1A2B,-105,-12,-82,12,29\r\nOK\r\n";
    int ok = gsm_parse_qeng_servingcell(reply);
    ASSERT_EQ(ok, 1);
    ASSERT_STREQ(cell_rat, "NBIOT");
}

void test_qeng_servingcell_gsm() {
    reset_cell();
    const char *reply =
        "\r\n+QENG: \"servingcell\",\"NOCONN\",\"GSM\","
        "234,30,1A2B,AB12,54,900,\"-\",-72,33,0,0,-,-,-,-,-,-\r\nOK\r\n";
    int ok = gsm_parse_qeng_servingcell(reply);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(cell_mcc, 234);
    ASSERT_EQ(cell_mnc, 30);
    ASSERT_EQ(cell_lac, 0x1A2B);
    ASSERT_EQ(cell_cid, 0xAB12);
    ASSERT_STREQ(cell_rat, "GSM");
}

void test_qeng_servingcell_catm_token() {
    // Some BG96 firmwares emit "CAT-M" instead of the documented "eMTC".
    reset_cell();
    const char *reply =
        "\r\n+QENG: \"servingcell\",\"NOCONN\",\"CAT-M\",\"FDD\","
        "234,10,6A2B,255,3749,20,3,3,1A2B,-105,-12,-82,12,29\r\nOK\r\n";
    int ok = gsm_parse_qeng_servingcell(reply);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(cell_mcc, 234);
    ASSERT_STREQ(cell_rat, "CATM1");
}

void test_qeng_sentinel_numeric_updates_rat_only() {
    // BG96 in CONNECT on Cat-M1 before SIB1 decode: numeric fields are
    // sentinels but the RAT token is valid.  Parser must clear stale numeric
    // cache (it's no longer this cell) and still propagate the new RAT.
    reset_cell();
    cell_mcc = 234; cell_mnc = 10;
    cell_lac = 0x1A2B; cell_cid = 0x6A2B;
    strlcpy(cell_rat, "GSM", sizeof(cell_rat));
    const char *reply =
        "\r\n+QENG: \"servingcell\",\"CONNECT\",\"CAT-M\",\"FDD\","
        "65535,65535,FFFFFFFF,212,-1,20,3,3,FFFF,-109,-16,-75,11,0\r\nOK\r\n";
    int ok = gsm_parse_qeng_servingcell(reply);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(cell_mcc, 0);
    ASSERT_EQ(cell_mnc, 0);
    ASSERT_EQ(cell_lac, 0UL);
    ASSERT_EQ(cell_cid, 0UL);
    ASSERT_STREQ(cell_rat, "CATM1");
}

void test_qeng_sentinel_numeric_unknown_rat_returns_zero() {
    // Sentinel numerics AND an unrecognised RAT token — nothing to cache.
    reset_cell();
    cell_mcc = 234; cell_mnc = 10;
    cell_lac = 0x1A2B; cell_cid = 0x6A2B;
    strlcpy(cell_rat, "GSM", sizeof(cell_rat));
    const char *reply =
        "\r\n+QENG: \"servingcell\",\"NOCONN\",\"UNKNOWN\",\"FDD\","
        "65535,65535,FFFFFFFF,0,-1,20,3,3,FFFF,0,0,0,0,0\r\nOK\r\n";
    int ok = gsm_parse_qeng_servingcell(reply);
    ASSERT_EQ(ok, 0);
    ASSERT_EQ(cell_mcc, 234);
    ASSERT_STREQ(cell_rat, "GSM");
}

void test_qeng_rat_change_marks_dirty() {
    // Same cell (mcc/mnc/lac/cid) but different RAT — must still return 1
    // so the server gets the updated RAT on the next packet.
    reset_cell();
    cell_mcc = 234; cell_mnc = 10;
    cell_lac = 0x1A2B; cell_cid = 0x6A2B;
    strlcpy(cell_rat, "NBIOT", sizeof(cell_rat));
    const char *reply =
        "\r\n+QENG: \"servingcell\",\"NOCONN\",\"eMTC\",\"FDD\","
        "234,10,6A2B,255,3749,20,3,3,1A2B,-105,-12,-82,12,29\r\nOK\r\n";
    int ok = gsm_parse_qeng_servingcell(reply);
    ASSERT_EQ(ok, 1);
    ASSERT_STREQ(cell_rat, "CATM1");
}

void test_qeng_search_state_no_update() {
    reset_cell();
    cell_mcc = 234; cell_mnc = 10;
    const char *reply =
        "\r\n+QENG: \"servingcell\",\"SEARCH\"\r\nOK\r\n";
    int ok = gsm_parse_qeng_servingcell(reply);
    ASSERT_EQ(ok, 0);
    // cache preserved
    ASSERT_EQ(cell_mcc, 234);
    ASSERT_EQ(cell_mnc, 10);
}

void test_qeng_malformed_returns_zero() {
    reset_cell();
    const char *reply = "\r\n+QENG: garbage\r\nOK\r\n";
    int ok = gsm_parse_qeng_servingcell(reply);
    ASSERT_EQ(ok, 0);
}

void test_qeng_same_values_returns_zero() {
    reset_cell();
    cell_mcc = 234; cell_mnc = 10;
    cell_lac = 0x1A2B; cell_cid = 0x6A2B;
    strlcpy(cell_rat, "CATM1", sizeof(cell_rat));
    const char *reply =
        "\r\n+QENG: \"servingcell\",\"NOCONN\",\"eMTC\",\"FDD\","
        "234,10,6A2B,255,3749,20,3,3,1A2B,-105,-12,-82,12,29\r\nOK\r\n";
    int ok = gsm_parse_qeng_servingcell(reply);
    ASSERT_EQ(ok, 0);  // no change — don't mark dirty
}

// ===========================================================================
// cell_refresh_due — time-based + URC-driven refresh trigger
// ===========================================================================

void test_refresh_due_stale_flag_forces_refresh() {
    // URC has signalled a change — refresh is due regardless of elapsed time.
    int due = cell_refresh_due(/*now=*/1000, /*last=*/1000, /*stale=*/1);
    ASSERT_EQ(due, 1);
}

void test_refresh_due_within_interval_not_stale() {
    // Less than CELL_REFRESH_INTERVAL_MS elapsed, no URC — skip.
    int due = cell_refresh_due(/*now=*/20000, /*last=*/0, /*stale=*/0);
    ASSERT_EQ(due, 0);
}

void test_refresh_due_at_interval_triggers_refresh() {
    // Exactly at the interval boundary — refresh.
    int due = cell_refresh_due(/*now=*/30000, /*last=*/0, /*stale=*/0);
    ASSERT_EQ(due, 1);
}

void test_refresh_due_beyond_interval_triggers_refresh() {
    // Long journey without a URC — the time-based trigger must fire.
    int due = cell_refresh_due(/*now=*/120000, /*last=*/30000, /*stale=*/0);
    ASSERT_EQ(due, 1);
}

void test_refresh_due_millis_wrap_handled() {
    // millis() rolls over every ~49 days.  Unsigned subtraction must give a
    // small positive delta across the wrap, not a huge negative one.
    uint32_t last = 0xFFFFFF00U;  // just before wrap
    uint32_t now  = 0x00000100U;  // wrapped, 512ms after `last`
    int due = cell_refresh_due(now, last, /*stale=*/0);
    ASSERT_EQ(due, 0);  // only ~512ms elapsed, well under interval
}

// ===========================================================================

int main() {
    printf("cell.ino tests:\n");

    RUN_TEST(test_creg_urc_home_with_location);
    RUN_TEST(test_cereg_urc_roaming_lte);
    RUN_TEST(test_creg_not_registered_no_update);
    RUN_TEST(test_reg_urc_no_change_when_same_values);
    RUN_TEST(test_reg_urc_absent_returns_zero);

    RUN_TEST(test_qeng_servingcell_lte);
    RUN_TEST(test_qeng_servingcell_nbiot);
    RUN_TEST(test_qeng_servingcell_gsm);
    RUN_TEST(test_qeng_servingcell_catm_token);
    RUN_TEST(test_qeng_sentinel_numeric_updates_rat_only);
    RUN_TEST(test_qeng_sentinel_numeric_unknown_rat_returns_zero);
    RUN_TEST(test_qeng_search_state_no_update);
    RUN_TEST(test_qeng_malformed_returns_zero);
    RUN_TEST(test_qeng_same_values_returns_zero);
    RUN_TEST(test_qeng_rat_change_marks_dirty);

    RUN_TEST(test_refresh_due_stale_flag_forces_refresh);
    RUN_TEST(test_refresh_due_within_interval_not_stale);
    RUN_TEST(test_refresh_due_at_interval_triggers_refresh);
    RUN_TEST(test_refresh_due_beyond_interval_triggers_refresh);
    RUN_TEST(test_refresh_due_millis_wrap_handled);

    TEST_REPORT();
}
