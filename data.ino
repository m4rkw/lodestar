//collect and send GPS data for sending

#ifndef MOCK_READ_MCU_TEMPERATURE
// Read STM32L4 internal temperature sensor. Uses factory calibration stored
// at fixed ROM addresses (TS_CAL1 @ 30°C, TS_CAL2 @ 110°C, both at 3.0V ref).
// Returns degrees Celsius as a float.
float read_mcu_temperature() {
  int prev_res = 10;  // Arduino default
  analogReadResolution(12);
  // First read primes the sensor; it needs ~120us stabilisation in practice
  // and a throwaway sample is simpler than fiddling with raw HAL setup.
  (void)analogRead(VTEMP);
  int raw = analogRead(VTEMP);
  analogReadResolution(prev_res);
  int32_t temp_c = __LL_ADC_CALC_TEMPERATURE(3300, raw, LL_ADC_RESOLUTION_12B);
  return (float)temp_c;
}
#endif

float read_battery_voltage() {
  long sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(AIN_S_INLEVEL);
  float v = (sum / 16.0f) * (ANALOG_SCALE * ANALOG_VREF / 1024.0f);
  battery_v = v;

  if (v >= ENGINE_RUNNING_VOLTAGE) {
    below_voltage_count = 0;
    engine_running = 1;
  } else {
    below_voltage_count++;
    if (below_voltage_count >= ENGINE_STOPPED_COUNT)
      engine_running = 0;
  }

  return v;
}

// Re-sample battery to confirm a low reading wasn't a momentary dip.
// Returns 1 only if a second reading 1s later is still at or below threshold.
int confirm_battery_low(float threshold) {
  delay(1000);
  if (read_battery_voltage() > threshold) return 0;
  delay(1000);
  return (read_battery_voltage() <= threshold);
}

void data_reset() {
  memset(data_current, 0, sizeof(data_current));
  data_index = 0;
}

/**
* CSV format: ts,lat,lon,spd,alt,hdg,hdop,sat,bat,ign,up,pon[,extras]
* extras: ri=1 | int=X;ao=X;ma=X;ga=X | mcc=X;mnc=X;lac=X;cid=X;cl=X
*/
int collect_data(int ignitionState) {
    debug_print(F("collect_data() started"));

    gsm_get_time();

    int gps_fix;
    if (use_cached_gps) {
      gps_fix = 1;
      gps_speed = 0;
      gps_hdop = 0;
      gps_sats = 0;
    } else {
#ifdef DEBUG_FORCE_CELL_LOCATION
      gps_fix = 0;  // skip GPS, force cell location path
      debug_print(F("DEBUG: forcing cell location"));
#else
      gps_fix = collect_gps_data();
#endif
    }

    cell_location = 0;
    if (!use_cached_gps && !gps_fix) {
      debug_print(F("No GPS fix"));
      if (cell_mcc != 0) {
        cell_location = 1;
      } else {
        // no GPS and no cell — nothing to send
        return 0;
      }
    }

    // read battery
    read_battery_voltage();

    // format floats (can't use %f on nano newlib)
    char spd[10], alt[10], hdg[10], bat[8];
    dtostrf(gps_speed, 1, 2, spd);
    dtostrf(gps_altitude, 1, 2, alt);
    dtostrf(gps_heading, 1, 2, hdg);
    dtostrf(battery_v, 1, 2, bat);

    // append newline separator if batching
    if (data_index > 0)
      data_current[data_index++] = '\n';

    // build CSV record in one shot
    int remaining = DATA_LIMIT - data_index - 1;
    int n = snprintf(&data_current[data_index], remaining,
      "%s,%s,%s,%s,%s,%s,%ld,%ld,%s,%d,%lu,%d",
      time_char,
      cell_location ? "0" : lat_current,
      cell_location ? "0" : lon_current,
      spd, alt, hdg, gps_hdop, gps_sats,
      bat,
      (ignitionState == 0) ? 1 : 0,
      (millis() - wake_start_millis) / 1000, powered_on);

    if (n > 0 && n < remaining)
      data_index += n;

    // Emit cell tower info only when new-to-the-server (first packet after
    // wake, or changed since last successful send) or when we're using
    // cell as a GPS fallback (cell_location=1 — the fields are load-bearing
    // for the server to infer position).
    if (cell_mcc != 0 && (cell_fields_dirty || cell_location)) {
      n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
        ",mcc=%d;mnc=%d;lac=%lu;cid=%lu;cl=%d",
        cell_mcc, cell_mnc, cell_lac, cell_cid, (int)cell_location);
      if (n > 0) data_index += n;
      // RAT only emitted once we've resolved it via QENG — avoids confusing
      // the server between "unknown" and a stale value on first boot.
      if (cell_rat[0]) {
        n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
          ";rat=%s", cell_rat);
        if (n > 0) data_index += n;
      }
    } else if (cell_rat[0] && cell_fields_dirty) {
      // RAT known but cell numeric unknown (e.g. Cat-M1 CONNECT before SIB1
      // decode).  Emit RAT alone so the server stops showing a stale value
      // from the previous attach on a different radio.
      n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
        ",rat=%s", cell_rat);
      if (n > 0) data_index += n;
    }

    // Accelerometer (milli-g, ±2g range). Included on every packet so the
    // server can see orientation / motion context for each logged position.
    int ax, ay, az;
    if (acc_read(&ax, &ay, &az) == 0) {
      n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
        ",ax=%d;ay=%d;az=%d", ax, ay, az);
      if (n > 0) data_index += n;
    }

    // Total uptime from boot (seconds) and MCU temperature (°C). millis()
    // doesn't advance during STOP2, so we add the RTC-timer sleep total to
    // get real wall-clock uptime since the last cold boot.
    extern unsigned long total_sleep_seconds;
    char mt[8];
    dtostrf(read_mcu_temperature(), 1, 1, mt);
    n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
      ",up=%lu;mt=%s", (millis() / 1000) + total_sleep_seconds, mt);
    if (n > 0) data_index += n;

    // append settings sync fields if needed
    if (send_int_to_server) {
      n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
        ",int=%d"
#if RELAY_CONNECTED
        ";ao=%d"
#endif
#if ALWAYS_ON_POWER
        ";ma=%d"
#endif
        , config.loop_interval
#if RELAY_CONNECTED
        , (int)config.always_on
#endif
#if ALWAYS_ON_POWER
        , (int)config.movement_alarm
#endif
        );
      if (n > 0) data_index += n;
    }


    data_current[data_index] = '\0';

    // battery warning alert (once per low-battery episode)
    if (battery_v < BATTERY_WARNING_LEVEL) {
      if (battery_warning_status == 0 && confirm_battery_low(BATTERY_WARNING_LEVEL)) {
        char vbuf[8];
        dtostrf(battery_v, 1, 2, vbuf);
        char msg[24];
        snprintf(msg, sizeof(msg), "low battery: %sV", vbuf);
        alert_enqueue(msg, 2);
        battery_warning_status = 1;
      }
    } else {
      battery_warning_status = 0;
    }

    debug_print(F("collect_data() completed"));
    return (gps_fix || cell_location) ? 1 : 0;
}


/**
 * This function send collected data using HTTP or TCP
 */

void send_data() {
    debug_print(F("Current:"));
    debug_print(data_current);

    int i = gsm_send_data();

    if(i != 1) {
        debug_print(F("Can not send data"));
        last_send_ok = 0;
    } else {
        debug_print(F("Data sent successfully."));
        last_send_ok = 1;

        // Server has now seen current cell info — skip it in subsequent
        // packets until something changes or we wake from sleep.
        cell_fields_dirty = 0;

        // send any queued alerts now that connection is up
        alert_send();
    }
}
