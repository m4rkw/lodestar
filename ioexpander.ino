
#if RELAY_CONNECTED

// IOExpander - NCV7240 Octal Low-Side Relay Driver for Polaris board
// Controls AIO1-4 and DIO1-4 outputs via 16-bit SPI interface
// Directly via PIN_C_OUT_CS (active low chip select)
//
// Each channel has a 2-bit mode:
//   00 = Standby (disabled, default)
//   01 = Input   (controlled by IN1/IN2 GPIO pins)
//   10 = ON      (output active, sinks to GND)
//   11 = OFF     (inactive, open-load detection)
//
// 16-bit SPI frame, MSB first:
//   [CH8:CH7:CH6:CH5] [CH4:CH3:CH2:CH1]
//   high byte           low byte

// Channel indices
#define IOEXP_AIO1  0
#define IOEXP_AIO2  1
#define IOEXP_AIO3  2
#define IOEXP_AIO4  3
#define IOEXP_DIO1  4
#define IOEXP_DIO2  5
#define IOEXP_DIO3  6
#define IOEXP_DIO4  7

// Channel modes
#define IOEXP_STANDBY  0b00
#define IOEXP_INPUT    0b01
#define IOEXP_ON       0b10
#define IOEXP_OFF      0b11

// 16-bit register: 2 bits per channel, channels 1-4 in low byte, 5-8 in high byte
byte ioexp_reg[2] = {0, 0};

void ioexp_init() {
  SPI.begin();
  pinMode(PIN_C_OUT_CS, OUTPUT);
  digitalWrite(PIN_C_OUT_CS, HIGH);

#ifdef PIN_C_OUT_ENA
  pinMode(PIN_C_OUT_ENA, OUTPUT);
  digitalWrite(PIN_C_OUT_ENA, LOW);
#endif

  // all channels standby
  ioexp_reg[0] = 0;
  ioexp_reg[1] = 0;
  ioexp_update();
}

void ioexp_update() {
  // NCV7240 uses SPI Mode 1 (CPOL=0, CPHA=1), MSB first, max 5MHz
  SPI.beginTransaction(SPISettings(500000, MSBFIRST, SPI_MODE1));
  digitalWrite(PIN_C_OUT_CS, LOW);
  SPI.transfer(ioexp_reg[0]);  // high byte: channels 5-8
  SPI.transfer(ioexp_reg[1]);  // low byte:  channels 1-4
  digitalWrite(PIN_C_OUT_CS, HIGH);
  SPI.endTransaction();
}

void ioexp_set_mode(byte channel, byte mode) {
  // channel 0-7, mode is 2-bit value (IOEXP_STANDBY/INPUT/ON/OFF)
  byte bit_pos = (channel % 4) * 2;
  byte reg_idx = (channel < 4) ? 1 : 0;
  ioexp_reg[reg_idx] = (ioexp_reg[reg_idx] & ~(0b11 << bit_pos)) | ((mode & 0b11) << bit_pos);
  ioexp_update();
}

// Convenience: turn channel on (sink to GND)
void ioexp_set(byte channel, byte value) {
  ioexp_set_mode(channel, value ? IOEXP_ON : IOEXP_STANDBY);
}

#endif // RELAY_CONNECTED
