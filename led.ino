
// LED is only used at startup to confirm power, then turned off.
// device_init() turns LED on (LOW = active-low), led_off() turns it off.

void led_off() {
  digitalWrite(PIN_POWER_LED, HIGH);
}

void status_delay(long ms) {
  if (ms <= 0)
    return;
  // break long delays into chunks so the watchdog gets refreshed
  while (ms > 1000) {
    delay(1000);
    watchdog_kick();
    ms -= 1000;
  }
  delay(ms);
  watchdog_kick();
}

void blink_start() {
}

