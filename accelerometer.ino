
// LIS2HH12 accelerometer driver - SPI interface
// Shares SPI bus with IOExpander, uses PIN_C_ACC_CS

// LIS2HH12 registers
#define ACC_WHO_AM_I    0x0F
#define ACC_CTRL1       0x20
#define ACC_CTRL2       0x21
#define ACC_CTRL3       0x22  // INT1 routing
#define ACC_CTRL4       0x23
#define ACC_CTRL5       0x24
#define ACC_CTRL6       0x25
#define ACC_CTRL7       0x26
#define ACC_STATUS      0x27
#define ACC_OUT_X_L     0x28
#define ACC_IG_CFG1     0x30
#define ACC_IG_SRC1     0x31
#define ACC_IG_THS_X1   0x32
#define ACC_IG_THS_Y1   0x33
#define ACC_IG_THS_Z1   0x34
#define ACC_IG_DUR1     0x35

#define ACC_WHO_AM_I_VAL 0x41

// SPI read/write helpers (SPI Mode 3 per vendor reference)
byte acc_read_reg(byte reg) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE3));
  digitalWrite(PIN_C_ACC_CS, LOW);
  SPI.transfer(reg | 0x80);  // bit 7 = read
  byte val = SPI.transfer(0x00);
  digitalWrite(PIN_C_ACC_CS, HIGH);
  SPI.endTransaction();
  return val;
}

void acc_write_reg(byte reg, byte val) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE3));
  digitalWrite(PIN_C_ACC_CS, LOW);
  SPI.transfer(reg & 0x7F);  // bit 7 = 0 for write
  SPI.transfer(val);
  digitalWrite(PIN_C_ACC_CS, HIGH);
  SPI.endTransaction();
}

byte acc_ready = 0;

void acc_init() {
  DEBUG_FUNCTION_CALL();

  // Soft reset via CTRL5 bit 6
  acc_write_reg(ACC_CTRL5, 0x40);
  delay(50);

  // Wait for soft reset to complete (bit 6 auto-clears)
  unsigned long st = millis();
  while (acc_read_reg(ACC_CTRL5) & 0x40) {
    if (millis() - st > 1000) break;
    delay(10);
  }

  byte id = acc_read_reg(ACC_WHO_AM_I);
  char hexbuf[8];
  snprintf(hexbuf, sizeof(hexbuf), "0x%02X", id);
  debug_print(F("ACC WHO_AM_I:"));
  debug_print(hexbuf);

  if (id != ACC_WHO_AM_I_VAL) {
    debug_print(F("ACC unexpected ID"));
    return;
  }

  // Enable auto-increment, disable I2C (SPI-only mode)
  acc_write_reg(ACC_CTRL4, 0x06);

  // HP filter routed to IG1, faster cutoff for quicker settling
  // DFC=10 (ODR/9 = 1.1Hz cutoff), HPIS1=1
  acc_write_reg(ACC_CTRL2, 0x41);
  // Reset HP filter — starts converging now, will settle during setup (~5s needed)
  acc_read_reg(0x1B);

  // 10Hz ODR, BDU enabled, all axes enabled
  // CTRL1: ODR[2:0]=001, BDU=1, ZEN=1, YEN=1, XEN=1 => 0x2F
  acc_write_reg(ACC_CTRL1, 0x2F);
  delay(10);

  byte ctrl1 = acc_read_reg(ACC_CTRL1);
  snprintf(hexbuf, sizeof(hexbuf), "0x%02X", ctrl1);
  debug_print(F("ACC CTRL1:"));
  debug_print(hexbuf);

  if (ctrl1 == 0x2F) {
    acc_ready = 1;
    debug_print(F("ACC init OK (LIS2HH12)"));
  } else {
    debug_print(F("ACC init FAILED"));
  }
}

// Read X, Y, Z in milli-g (±2g default full scale, 0.061 mg/LSB)
int acc_read(int *x_mg, int *y_mg, int *z_mg) {
  if (!acc_ready) return -1;

  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE3));
  digitalWrite(PIN_C_ACC_CS, LOW);
  SPI.transfer(ACC_OUT_X_L | 0x80);  // read with auto-increment
  byte xl = SPI.transfer(0);
  byte xh = SPI.transfer(0);
  byte yl = SPI.transfer(0);
  byte yh = SPI.transfer(0);
  byte zl = SPI.transfer(0);
  byte zh = SPI.transfer(0);
  digitalWrite(PIN_C_ACC_CS, HIGH);
  SPI.endTransaction();

  int16_t raw_x = (int16_t)((xh << 8) | xl);
  int16_t raw_y = (int16_t)((yh << 8) | yl);
  int16_t raw_z = (int16_t)((zh << 8) | zl);

  *x_mg = (int)((long)raw_x * 61 / 1000);
  *y_mg = (int)((long)raw_y * 61 / 1000);
  *z_mg = (int)((long)raw_z * 61 / 1000);

  return 0;
}

#if ALWAYS_ON_POWER

int acc_baseline_x, acc_baseline_y, acc_baseline_z;

void acc_read_baseline() {
  if (!acc_ready) return;
  acc_read(&acc_baseline_x, &acc_baseline_y, &acc_baseline_z);
}

// Compute tilt angle change (tenths of a degree) and acceleration delta magnitude (mg)
// between current reading and pre-sleep baseline
int acc_get_movement_info(int *tilt_tenths, int *delta_mg) {
  int cx, cy, cz;
  if (acc_read(&cx, &cy, &cz) != 0) return -1;

  int dx = cx - acc_baseline_x;
  int dy = cy - acc_baseline_y;
  int dz = cz - acc_baseline_z;

  // total acceleration change magnitude
  *delta_mg = (int)sqrtf((float)dx*dx + (float)dy*dy + (float)dz*dz);

  // angle between baseline and current gravity vectors
  long dot = (long)acc_baseline_x*cx + (long)acc_baseline_y*cy + (long)acc_baseline_z*cz;
  float mag_b = sqrtf((float)acc_baseline_x*acc_baseline_x + (float)acc_baseline_y*acc_baseline_y + (float)acc_baseline_z*acc_baseline_z);
  float mag_c = sqrtf((float)cx*cx + (float)cy*cy + (float)cz*cz);

  float cos_a = (mag_b > 0 && mag_c > 0) ? (float)dot / (mag_b * mag_c) : 1.0f;
  if (cos_a > 1.0f) cos_a = 1.0f;
  if (cos_a < -1.0f) cos_a = -1.0f;

  *tilt_tenths = (int)(acosf(cos_a) * (1800.0f / M_PI));

  return 0;
}

// Poll accelerometer to confirm sustained movement (not just a bump).
// Takes readings over MOVEMENT_CONFIRM_MS and requires MOVEMENT_CONFIRM_HITS
// above-threshold samples to confirm. Returns 1 if confirmed, 0 if transient.
int acc_confirm_movement() {
  if (!acc_ready) return 0;

  int hits = 0;
  unsigned long start = millis();

  while ((millis() - start) < MOVEMENT_CONFIRM_MS) {
    int cx, cy, cz;
    if (acc_read(&cx, &cy, &cz) != 0) break;

    int dx = cx - acc_baseline_x;
    int dy = cy - acc_baseline_y;
    int dz = cz - acc_baseline_z;
    int delta = (int)sqrtf((float)dx*dx + (float)dy*dy + (float)dz*dz);

    if (delta >= ACC_MOVEMENT_THRESHOLD) hits++;
    delay(100);  // 10Hz sample rate
  }

  debug_print(F("ACC confirm hits:"));
  debug_print(hits);

  return (hits >= MOVEMENT_CONFIRM_HITS);
}

// Configure sleep-to-wake on INT1 (PD11)
// INT1 = HIGH when inactive (still), LOW when active (moving)
// Use FALLING edge EXTI to wake on motion
#define ACC_ACT_THS  0x1E
#define ACC_ACT_DUR  0x1F

void acc_configure_wake_interrupt() {
  if (!acc_ready) return;

  // Disable all interrupt sources
  acc_write_reg(ACC_CTRL3, 0x00);
  acc_write_reg(ACC_IG_CFG1, 0x00);
  acc_write_reg(ACC_CTRL7, 0x00);
  acc_write_reg(ACC_ACT_THS, 0x00);
  acc_write_reg(ACC_ACT_DUR, 0x00);

  // Power-down to fully reset ACT/INACT state machine
  acc_write_reg(ACC_CTRL1, 0x00);
  delay(20);

  // Power back on: 10Hz ODR, BDU, all axes
  acc_write_reg(ACC_CTRL1, 0x2F);

  // Reset HP filter reference so it converges from current position
  acc_read_reg(0x1B);

  // Wait for HP filter to settle (~500ms is >3 time constants at 1.1Hz cutoff)
  delay(500);

  // Activity threshold: 15.6mg/LSB at ±2g
  acc_write_reg(ACC_ACT_THS, ACC_WAKE_THRESHOLD);
  // Inactivity duration: 3 samples at 10Hz = 300ms to enter sleep
  acc_write_reg(ACC_ACT_DUR, 3);

  // Route sleep-to-wake status to INT1
  acc_write_reg(ACC_CTRL3, 0x20);  // INT1_INACT
}

void acc_disable_interrupt() {
  if (!acc_ready) return;
  acc_write_reg(ACC_CTRL3, 0x00);
}

#endif // ALWAYS_ON_POWER
