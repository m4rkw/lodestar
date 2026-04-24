// Crypto: hardware TRNG nonces + ChaCha20-Poly1305 envelope helpers.
//
// The STM32L476 RNG peripheral needs a 48 MHz clock on CLK48; we use HSI48,
// which is independent of the PLL and stays available across our power
// modes. Init is done once at boot from setup().

#include "chacha20_poly1305.h"

static RNG_HandleTypeDef hrng;
static byte rng_ready = 0;

void crypto_init() {
  // CLK48: HSI48 → RNG. STM32L476 has HSI48 dedicated for USB/RNG.
  RCC_OscInitTypeDef osc = {0};
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
  osc.HSI48State     = RCC_HSI48_ON;
  osc.PLL.PLLState   = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
    debug_print(F("crypto: HSI48 enable failed"));
    return;
  }

  RCC_PeriphCLKInitTypeDef pclk = {0};
  pclk.PeriphClockSelection = RCC_PERIPHCLK_RNG;
  pclk.RngClockSelection    = RCC_RNGCLKSOURCE_HSI48;
  if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
    debug_print(F("crypto: RNG clk mux failed"));
    return;
  }

  __HAL_RCC_RNG_CLK_ENABLE();
  hrng.Instance = RNG;
  if (HAL_RNG_Init(&hrng) != HAL_OK) {
    debug_print(F("crypto: RNG init failed"));
    return;
  }

  if (!cp_self_test()) {
    debug_print(F("crypto: AEAD self-test FAILED"));
    return;
  }

  rng_ready = 1;
  debug_print(F("crypto: ready"));
}

// Fill `out` with `len` random bytes from the hardware TRNG.  Returns 1 on
// success, 0 on hardware failure (caller MUST refuse to send on failure —
// reusing a nonce destroys the AEAD's confidentiality).
int crypto_random(uint8_t *out, size_t len) {
  if (!rng_ready) return 0;
  while (len) {
    uint32_t r;
    if (HAL_RNG_GenerateRandomNumber(&hrng, &r) != HAL_OK) return 0;
    size_t n = len < 4 ? len : 4;
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(r >> (8 * i));
    out += n; len -= n;
  }
  return 1;
}

// Hex (64 chars) → 32 bytes. Returns 1 on success, 0 on bad input.
int crypto_psk_from_hex(const char *hex, uint8_t out[32]) {
  for (int i = 0; i < 32; i++) {
    int hi = -1, lo = -1;
    char a = hex[2 * i], b = hex[2 * i + 1];
    if      (a >= '0' && a <= '9') hi = a - '0';
    else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
    else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
    if      (b >= '0' && b <= '9') lo = b - '0';
    else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
    else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
    if (hi < 0 || lo < 0) return 0;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 1;
}
