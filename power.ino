#if RELAY_CONNECTED

void set_power_supply_off() {
  // Switch relay to ignition-only without changing config.always_on
  ioexp_set(IOEXP_AIO1, LOW);
  delay(50);
  ioexp_set(IOEXP_AIO2, HIGH);
  delay(350);
  ioexp_set(IOEXP_AIO2, LOW);
}

void set_power_supply_mode() {
  // Latching relay: pulse one coil to switch, then release
  // AIO1 (CH1) = always-on coil, AIO2 (CH2) = ignition-only coil
  if (config.always_on == 1) {
    ioexp_set(IOEXP_AIO2, LOW);
    delay(50);
    ioexp_set(IOEXP_AIO1, HIGH);
    delay(350);
    ioexp_set(IOEXP_AIO1, LOW);
  } else {
    ioexp_set(IOEXP_AIO1, LOW);
    delay(50);
    ioexp_set(IOEXP_AIO2, HIGH);
    delay(350);
    ioexp_set(IOEXP_AIO2, LOW);
  }
}

#endif // RELAY_CONNECTED

// Always declared — referenced by rtc_irq.c (compiled separately as C)
RTC_HandleTypeDef hrtc_wakeup;

#if ALWAYS_ON_POWER

void enter_low_power_standby() {
  DEBUG_FUNCTION_CALL();

  extern byte network_ready;
  network_ready = 0;

  digitalWrite(PIN_POWER_LED, HIGH);

  gps_off();
#if LOW_POWER_STANDBY
  gsm_off(0);               // full modem power-off saves ~1mA vs standby
#else
  gsm_enter_sleep();
#endif
  gsm_close();              // close UART2 peripheral

  // disable GNSS antenna LNA power
#ifdef PIN_C_ANTON
  digitalWrite(PIN_C_ANTON, LOW);
#endif

  // disable 5V rail (powers IOExpander, not needed during sleep)
  digitalWrite(PIN_C_5V_ENABLE, LOW);

  // release SPI bus to avoid parasitic current through level shifters
  SPI.end();

  // disable GSM pin pull-ups (modem is off, pull-ups draw against its outputs)
  pinMode(PIN_STATUS_GSM, INPUT);
  pinMode(PIN_RING_GSM, INPUT);

  // disable USB CDC peripheral
  usb_console_disable();

  cpu_slow_down();
}

// Empty ISR — just needs to exist so EXTI can wake the CPU from STOP2
void stop2_wake_isr() {}

// -- RTC wakeup timer for periodic STOP2 wake --------------------------------

static volatile byte rtc_woke = 0;
static byte rtc_initialised = 0;

void rtc_wakeup_init() {
  if (rtc_initialised) return;

  // LSE already enabled by board's SystemClock_Config (variant.cpp)
  __HAL_RCC_RTC_ENABLE();

  hrtc_wakeup.Instance = RTC;
  hrtc_wakeup.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc_wakeup.Init.AsynchPrediv = 127;
  hrtc_wakeup.Init.SynchPrediv = 255;
  hrtc_wakeup.Init.OutPut = RTC_OUTPUT_DISABLE;
  HAL_RTC_Init(&hrtc_wakeup);

  HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);

  rtc_initialised = 1;
}

void rtc_wakeup_set(uint32_t seconds) {
  if (seconds == 0) return;
  if (seconds > RTC_WAKEUP_MAX_SECONDS) seconds = RTC_WAKEUP_MAX_SECONDS;
  rtc_woke = 0;
  // 1Hz ck_spre clock, counter value = seconds - 1 (counts down to 0 inclusive)
  HAL_RTCEx_SetWakeUpTimer_IT(&hrtc_wakeup, seconds - 1, RTC_WAKEUPCLOCK_CK_SPRE_16BITS);
}

void rtc_wakeup_disable() {
  HAL_RTCEx_DeactivateWakeUpTimer(&hrtc_wakeup);
}

byte rtc_wakeup_fired() {
  return rtc_woke;
}

void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *hrtc) {
  rtc_woke = 1;
}

// -- STOP2 entry --------------------------------------------------------------

void enter_stop2_with_wake(byte use_accel, byte ignition_rising) {
  // Configure EXTI wake sources
  if (use_accel) {
    pinMode(PIN_S_ACC_INT1, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_S_ACC_INT1), stop2_wake_isr, FALLING);
  }
  // FALLING = wake when ignition turns ON (pin LOW), RISING = wake when OFF (pin HIGH)
  attachInterrupt(digitalPinToInterrupt(PIN_S_DETECT), stop2_wake_isr,
                  ignition_rising ? RISING : FALLING);

  // Refresh watchdog to start the sleep with a full counter. IWDG is frozen
  // during STOP2 (IWDG_STOP option byte = 0), so the counter resumes from
  // this value on wake — giving post-wake code the full ~32s to run.
  watchdog_kick();

  // Race guard: the ignition edge we're arming for may have already occurred
  // between the caller's last pin read and the attachInterrupt() above (this
  // window can be hundreds of ms during peripheral shutdown). EXTI is
  // edge-triggered and would not see another transition until the pin
  // toggles back and forward again — leaving the device asleep until the
  // next RTC wake, which can be up to ~18h with loop_interval=86400. This
  // is the most plausible cause of "no telemetry for an entire drive":
  // ignition turns on during the STATE_IDLE→STATE_SLEEP transition, the
  // edge is missed, and the device sleeps through the drive.
  //
  // Re-check the ignition pin with interrupts now armed; if it's already at
  // the post-edge level, skip STOP2 entry. The caller's post-wake logic
  // (firmware.ino's debounce + STATE_SLEEP re-check at line 817) then sees
  // the correct pin state and exits the sleep loop normally.
  //
  // Note: we deliberately do NOT race-guard the accelerometer wake. INT1
  // legitimately stays LOW during continuous movement (e.g. transporter),
  // and skipping STOP2 in that case would busy-loop at full CPU and burn
  // battery. A missed accel edge will be caught by the next RTC wake, and
  // the user's bug is about silent *drives* (ignition-on), which the
  // ignition guard alone covers.
  byte ignition_already;
  if (ignition_rising) {
    // Waiting for RISING edge → wake when pin goes HIGH (ignition off).
    ignition_already = (digitalRead(PIN_S_DETECT) != 0);
  } else {
    // Waiting for FALLING edge → wake when pin goes LOW (ignition on).
    ignition_already = (digitalRead(PIN_S_DETECT) == 0);
  }

  if (!ignition_already) {
    // Enter STOP2 — CPU halts until an EXTI interrupt fires
    HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);
  }

  // CPU resumes here after wake (clock is now MSI 4MHz), or here directly if
  // the race guard above caused us to skip STOP2 entry.
  watchdog_kick();

  if (use_accel) {
    detachInterrupt(digitalPinToInterrupt(PIN_S_ACC_INT1));
  }
  detachInterrupt(digitalPinToInterrupt(PIN_S_DETECT));
}

// Shut down CPU, USB, 5V, SPI only — inverse of exit_low_power_standby_minimal().
// Does NOT touch modem or GPS (they are already off).
void enter_low_power_standby_minimal() {
  digitalWrite(PIN_POWER_LED, HIGH);

  digitalWrite(PIN_C_5V_ENABLE, LOW);
  SPI.end();
  usb_console_disable();
  cpu_slow_down();
}

// Stage 1: restore CPU, USB, 5V, SPI — enough to read pins and accelerometer
void exit_low_power_standby_minimal() {
  DEBUG_FUNCTION_CALL();

  cpu_full_speed();
  usb_console_restore();

  // restore 5V rail
  digitalWrite(PIN_C_5V_ENABLE, HIGH);
  delay(10);

  SPI.begin();
}

// Stage 2: restore modem only (no GPS) — enough to send alerts
void exit_low_power_standby_modem() {
  // restore GNSS antenna LNA power (shared with modem on some boards)
#ifdef PIN_C_ANTON
  digitalWrite(PIN_C_ANTON, HIGH);
#endif

  // restore GSM pin pull-ups
  pinMode(PIN_STATUS_GSM, INPUT_PULLUP);
  pinMode(PIN_RING_GSM, INPUT_PULLUP);

  gsm_open();
#if LOW_POWER_STANDBY
  gsm_on();
  gsm_config();
  extern byte network_ready;
  int reg = gsm_get_network_status();
  if (reg == 1 || reg == 5) {
    network_ready = 1;
    gsm_post_register();
    gsm_set_apn();
  } else {
    network_ready = 0;
  }
#else
  gsm_exit_sleep();
#endif
}

// Stage 3: restore GPS (call after modem is up)
void exit_low_power_standby_gps() {
#if LOW_POWER_STANDBY && MODEM_BG96
  gsm_port.print("AT+QGPSCFG=\"outport\",\"uartnmea\"\r");
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QGPS=1\r");
  gsm_wait_for_reply(1,0);
#endif
  gps_on();
  gps_wakeup();
}

// Full restore with parallel GPS+network — GPS starts acquiring while modem configures
void exit_low_power_standby_modem_gps() {
  // hardware restore (same as exit_low_power_standby_modem)
#ifdef PIN_C_ANTON
  digitalWrite(PIN_C_ANTON, HIGH);
#endif
  pinMode(PIN_STATUS_GSM, INPUT_PULLUP);
  pinMode(PIN_RING_GSM, INPUT_PULLUP);

  gsm_open();
#if LOW_POWER_STANDBY
  gsm_on();

  // start GPS immediately — acquires satellites while modem configures
#if MODEM_BG96
  gsm_port.print("AT+QGPSCFG=\"outport\",\"uartnmea\"\r");
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QGPS=1\r");
  gsm_wait_for_reply(1,0);
#endif
  gps_on();
  gps_wakeup();

  // configure modem and start async network search — GPS is already running
  gsm_config();
#else
  gsm_exit_sleep();
#if MODEM_BG96
  gsm_port.print("AT+QGPSCFG=\"outport\",\"uartnmea\"\r");
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QGPS=1\r");
  gsm_wait_for_reply(1,0);
#endif
  gps_on();
  gps_wakeup();
#endif
}

// Full restore (legacy single-call version)
void exit_low_power_standby() {
  exit_low_power_standby_minimal();
  exit_low_power_standby_modem_gps();
}

// Shut down modem only (CPU, USB, SPI, 5V stay up for accelerometer/sleep re-entry)
void enter_low_power_standby_modem() {
#if LOW_POWER_STANDBY
  gsm_off(0);
#else
  gsm_enter_sleep();
#endif
  gsm_close();

#ifdef PIN_C_ANTON
  digitalWrite(PIN_C_ANTON, LOW);
#endif

  pinMode(PIN_STATUS_GSM, INPUT);
  pinMode(PIN_RING_GSM, INPUT);
}

#endif // ALWAYS_ON_POWER
