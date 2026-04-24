#ifndef MOCK_ARDUINO_H
#define MOCK_ARDUINO_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

// -- Arduino types ----------------------------------------------------------

typedef uint8_t byte;

// -- Constants --------------------------------------------------------------

#define HIGH 1
#define LOW  0
#define INPUT  0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define FALLING 2
#define RISING  3

// -- Pin definitions (Polaris board) ----------------------------------------

#define PIN_S_DETECT     10
#define PIN_POWER_LED    11
#define PIN_C_5V_ENABLE  12
#define PIN_C_ACC_CS     13
#define PIN_S_ACC_INT1   14
#define PIN_C_OUT_CS     15
#define PIN_C_PWR_GSM    16
#define PIN_C_KILL_GSM   17
#define PIN_RESET_GPS    18
#define PIN_CAN_RS       19
#define AIN_S_INLEVEL    20
#define PIN_STATUS_GSM   21
#define PIN_RING_GSM     22
#define PIN_WAKE_GSM     23
#define PIN_C_REBOOT     24

// ADC calibration for battery voltage divider
#define ANALOG_SCALE  12.0f
#define ANALOG_VREF   3.3f

// -- F() macro (no-op on host) ----------------------------------------------

#define F(x) (x)

// -- Mock state -------------------------------------------------------------

#define MOCK_NUM_PINS 32

extern int      mock_pin_mode[MOCK_NUM_PINS];
extern int      mock_pin_value[MOCK_NUM_PINS];
extern int      mock_analog_value[MOCK_NUM_PINS];
extern unsigned long mock_millis_value;
extern unsigned long mock_micros_value;

inline void mock_reset() {
    memset(mock_pin_mode, 0, sizeof(mock_pin_mode));
    memset(mock_pin_value, 0, sizeof(mock_pin_value));
    memset(mock_analog_value, 0, sizeof(mock_analog_value));
    mock_millis_value = 0;
    mock_micros_value = 0;
}

// -- Arduino API stubs ------------------------------------------------------

inline void     pinMode(int pin, int mode)          { if (pin < MOCK_NUM_PINS) mock_pin_mode[pin] = mode; }
inline void     digitalWrite(int pin, int val)      { if (pin < MOCK_NUM_PINS) mock_pin_value[pin] = val; }
inline int      digitalRead(int pin)                { return (pin < MOCK_NUM_PINS) ? mock_pin_value[pin] : 0; }
inline int      analogRead(int pin)                 { return (pin < MOCK_NUM_PINS) ? mock_analog_value[pin] : 0; }
inline unsigned long millis()                       { return mock_millis_value; }
inline unsigned long micros()                       { return mock_micros_value; }
inline void     delay(unsigned long)                {}
inline int      digitalPinToInterrupt(int pin)      { return pin; }
inline void     attachInterrupt(int, void(*)(), int){}
inline void     detachInterrupt(int)                {}

// -- dtostrf (may not exist on all platforms) --------------------------------

inline char *dtostrf(double val, signed char width, unsigned char prec, char *buf) {
    sprintf(buf, "%*.*f", width, prec, val);
    return buf;
}

// -- ltoa -------------------------------------------------------------------

inline char *ltoa(long val, char *buf, int base) {
    if (base == 10) sprintf(buf, "%ld", val);
    else sprintf(buf, "%lx", val);
    return buf;
}

// -- Minimal HardwareSerial mock -------------------------------------------

class HardwareSerial {
public:
    char _buf[4096];
    int _buf_len;
    char _input[4096];
    int _input_pos;
    int _input_len;

    HardwareSerial() : _buf_len(0), _input_pos(0), _input_len(0) {
        _buf[0] = '\0';
        _input[0] = '\0';
    }
    void begin(long) {}
    void end() {}
    void print(const char *s) {
        int len = strlen(s);
        if (_buf_len + len < (int)sizeof(_buf)) {
            memcpy(_buf + _buf_len, s, len);
            _buf_len += len;
            _buf[_buf_len] = '\0';
        }
    }
    void println(const char *s) { print(s); print("\r\n"); }
    void println(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); println(b); }
    void write(char c) { if (_buf_len < (int)sizeof(_buf) - 1) { _buf[_buf_len++] = c; _buf[_buf_len] = '\0'; } }
    int available() { return _input_len - _input_pos; }
    int read() { return (_input_pos < _input_len) ? _input[_input_pos++] : -1; }

    void mock_inject(const char *data) {
        int len = strlen(data);
        memcpy(_input, data, len);
        _input_len = len;
        _input_pos = 0;
    }
    void mock_clear() {
        _buf_len = 0; _buf[0] = '\0';
        _input_pos = 0; _input_len = 0; _input[0] = '\0';
    }
};

extern HardwareSerial SerialUSB;

// -- debug stubs (match firmware.ino) ---------------------------------------

#define debug_port SerialUSB

#ifdef DEBUG
  #define debug_print(x) debug_port.println(x)
#else
  #define debug_print(x)
#endif

// -- debug.h macros (no-ops in test) ----------------------------------------

#define DEBUG_FUNCTION_CALL()
#define DEBUG_FUNCTION_PRINT(...)
#define DEBUG_FUNCTION_PRINTLN(...)
#define DEBUG_PRINT(...)
#define DEBUG_PRINTLN(...)

// -- Stubs for things tests don't care about --------------------------------

inline void __disable_irq() {}
inline void NVIC_SystemReset() {}

// -- STM32 HAL RCC stubs (reset-cause detection, no-ops in test) ------------

#define RCC_FLAG_IWDGRST 0x01
#define RCC_FLAG_WWDGRST 0x02
#define RCC_FLAG_SFTRST  0x03
#define RCC_FLAG_BORRST  0x04
#define RCC_FLAG_PINRST  0x05
#define RCC_FLAG_LPWRRST 0x06
#define RCC_FLAG_OBLRST  0x07

// Mock state: set to one of the RCC_FLAG_* values to simulate a reset cause.
extern uint32_t mock_rcc_reset_flag;

inline uint32_t __HAL_RCC_GET_FLAG(uint32_t flag) { return mock_rcc_reset_flag == flag ? 1 : 0; }
inline void __HAL_RCC_CLEAR_RESET_FLAGS() { mock_rcc_reset_flag = 0; }

// usb console stubs
inline void usbd_interface_deinit() {}
inline void usbd_interface_init() {}

#endif // MOCK_ARDUINO_H
