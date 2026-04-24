// Tests for chacha20_poly1305.c — RFC 8439 vector + tamper detection.

#include <cstdio>
#include <cstring>
#include "test_framework.h"

extern "C" {
#include "../chacha20_poly1305.h"
}

int test_count = 0;
int test_failures = 0;

void test_rfc_vector() {
    ASSERT_TRUE(cp_self_test());
}

void test_round_trip_short() {
    uint8_t key[32], nonce[12];
    for (int i = 0; i < 32; i++) key[i]   = (uint8_t)i;
    for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(0xa0 + i);
    const char *msg = "hello";
    uint8_t ct[8], tag[16], pt[8];
    cp_seal(key, nonce, NULL, 0, (const uint8_t*)msg, 5, ct, tag);
    ASSERT_TRUE(cp_open(key, nonce, NULL, 0, ct, 5, tag, pt));
    ASSERT_TRUE(memcmp(pt, msg, 5) == 0);
}

void test_round_trip_with_aad() {
    uint8_t key[32], nonce[12];
    for (int i = 0; i < 32; i++) key[i]   = (uint8_t)(i + 1);
    for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)i;
    const char *aad = "357706041234567";
    const char *msg = "01/01/26,00:00:00+0,51.5,-0.1\n";
    size_t aad_len = strlen(aad), msg_len = strlen(msg);
    uint8_t ct[64], tag[16], pt[64];
    cp_seal(key, nonce, (const uint8_t*)aad, aad_len,
            (const uint8_t*)msg, msg_len, ct, tag);
    ASSERT_TRUE(cp_open(key, nonce, (const uint8_t*)aad, aad_len,
                        ct, msg_len, tag, pt));
    ASSERT_TRUE(memcmp(pt, msg, msg_len) == 0);
}

void test_tag_tamper() {
    uint8_t key[32] = {0}, nonce[12] = {0};
    const char *msg = "ABCDEFGH";
    uint8_t ct[8], tag[16], pt[8];
    cp_seal(key, nonce, NULL, 0, (const uint8_t*)msg, 8, ct, tag);
    tag[0] ^= 1;
    ASSERT_TRUE(cp_open(key, nonce, NULL, 0, ct, 8, tag, pt) == 0);
}

void test_ct_tamper() {
    uint8_t key[32] = {0}, nonce[12] = {0};
    const char *msg = "ABCDEFGH";
    uint8_t ct[8], tag[16], pt[8];
    cp_seal(key, nonce, NULL, 0, (const uint8_t*)msg, 8, ct, tag);
    ct[3] ^= 1;
    ASSERT_TRUE(cp_open(key, nonce, NULL, 0, ct, 8, tag, pt) == 0);
}

void test_aad_tamper() {
    uint8_t key[32] = {0}, nonce[12] = {0};
    const char *aad = "imei:1";
    const char *msg = "data";
    uint8_t ct[4], tag[16], pt[4];
    cp_seal(key, nonce, (const uint8_t*)aad, 6, (const uint8_t*)msg, 4, ct, tag);
    const char *bad_aad = "imei:2";
    ASSERT_TRUE(cp_open(key, nonce, (const uint8_t*)bad_aad, 6, ct, 4, tag, pt) == 0);
}

void test_wrong_key() {
    uint8_t key[32] = {0}, key2[32] = {0}, nonce[12] = {0};
    key2[0] = 1;
    const char *msg = "hi";
    uint8_t ct[2], tag[16], pt[2];
    cp_seal(key, nonce, NULL, 0, (const uint8_t*)msg, 2, ct, tag);
    ASSERT_TRUE(cp_open(key2, nonce, NULL, 0, ct, 2, tag, pt) == 0);
}

void test_empty_plaintext() {
    uint8_t key[32] = {0}, nonce[12] = {0};
    uint8_t tag[16];
    cp_seal(key, nonce, NULL, 0, NULL, 0, NULL, tag);
    ASSERT_TRUE(cp_open(key, nonce, NULL, 0, NULL, 0, tag, NULL));
    tag[0] ^= 1;
    ASSERT_TRUE(cp_open(key, nonce, NULL, 0, NULL, 0, tag, NULL) == 0);
}

int main() {
    printf("crypto:\n");
    RUN_TEST(test_rfc_vector);
    RUN_TEST(test_round_trip_short);
    RUN_TEST(test_round_trip_with_aad);
    RUN_TEST(test_tag_tamper);
    RUN_TEST(test_ct_tamper);
    RUN_TEST(test_aad_tamper);
    RUN_TEST(test_wrong_key);
    RUN_TEST(test_empty_plaintext);
    TEST_REPORT();
}
