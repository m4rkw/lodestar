
void gps_init() {
  DEBUG_FUNCTION_CALL();

#ifdef PIN_STANDBY_GPS
  pinMode(PIN_STANDBY_GPS, OUTPUT);
  digitalWrite(PIN_STANDBY_GPS, LOW);
#endif

  pinMode(PIN_RESET_GPS, OUTPUT);
  digitalWrite(PIN_RESET_GPS, HIGH);  // hold in reset initially
}

void gps_open() {
#if MODEM_BG96
  gps_port.begin(115200);
#else
  gps_port.begin(9600);
#endif
}

void gps_close() {
  gps_port.end();

  // float serial pins to prevent reset issues on STM32
  class HWS : public HardwareSerial {
    public: serial_t* getSerial() { return &_serial; }
  };
  serial_t* ser = static_cast<HWS*>(&gps_port)->getSerial();
  pinMode(pinNametoDigitalPin(ser->pin_rx), INPUT);
  pinMode(pinNametoDigitalPin(ser->pin_tx), INPUT);
}

void gps_setup() {
  DEBUG_FUNCTION_CALL();

  gps_off();
  gps_on();
  gps_wakeup();

  // read the first 4 lines within timeout
  unsigned long t = millis();
  for(int i=0; i<4; ++i) {
    int c = -1;
    while (c != '\n' && (millis() - t < 5000))
      if(gps_port.available()) {
        c = gps_port.read();
#if defined(DEBUG)
          debug_port.write(c);
#endif
      }
  }
  if(millis() - t > 5000) DEBUG_PRINT("GPS not responding");
}

void gps_on() {
  DEBUG_FUNCTION_CALL();

  delay(100);
  digitalWrite(PIN_RESET_GPS, LOW);  // release from reset
  delay(500);
  gps_open();
}

void gps_off() {
  DEBUG_FUNCTION_CALL();

  gps_close();
  digitalWrite(PIN_RESET_GPS, HIGH);  // hold in reset
  delay(100);
}

void gps_wakeup() {
  // exit GPS standby
  gps_port.print("\r\n");
}

//collect GPS data from serial port, returns 1 if fix acquired
int collect_gps_data() {
    byte fix = 0;
    int retry = 0;

    char tmp[15];

    float flat, flon;
    unsigned long fix_age, time_gps, date_gps;
    unsigned short failed_checksum;

    unsigned long start_time = millis();
    unsigned long last_net_check = 0;

    //time-bounded loop for GPS fix (max GPS_FIX_TIMEOUT ms)
    while ((unsigned long)(millis() - start_time) < GPS_FIX_TIMEOUT) {
        watchdog_kick();

        // poll network registration in parallel with GPS fix
        extern byte network_ready;
        if (!network_ready && (unsigned long)(millis() - last_net_check) >= 5000) {
          last_net_check = millis();
          int reg = gsm_get_network_status();
          if (reg == 1 || reg == 5) {
            network_ready = 1;
            gsm_post_register();
            gsm_set_apn();
          }
        }

        while (gps_port.available()) {
            char c = gps_port.read();

            //debug
            #ifdef DEBUG
                debug_port.print(c);
            #endif

            if(fix == 1) { //fix already acquired
                debug_print(F("GPS already available, breaking"));
                break;
            }

            if(gps.encode(c)) {
                // check for new timestamp first (only GPRMC updates time in TinyGPS)
                gps.get_datetime(&date_gps, &time_gps, &fix_age);
                if((last_time_gps == time_gps) && (last_date_gps == date_gps))
                    continue;

                //check if altitude acquired, otherwise continue
                float falt = gps.f_altitude(); // +/- altitude in meters
                float fc = gps.f_course(); // course in degrees
                float fkmph = gps.f_speed_kmph(); // speed in km/hr

                //retry to get fix in case no valid altitude or course supplied (max 5 times)
                if(retry < 5) {
                    if(falt == 1000000) {
                        debug_print(F("Invalid altitude, retrying."));
                        retry++;
                        continue;
                    }
                    if(fc == TinyGPS::GPS_INVALID_F_ANGLE) {
                        debug_print(F("Invalid course, retrying."));
                        retry++;
                        continue;
                    }
                    if(date_gps == 0) {
                        debug_print(F("Invalid date, retrying."));
                        retry++;
                        continue;
                    }
                }

                debug_print(F("GPS fix received."));
                gps.f_get_position(&flat, &flon, &fix_age);

                fix = 1;

                // store parsed GPS values into globals
                dtostrf(flat, 1, 6, lat_current);
                if (flon < 0) {
                  lon_current[0] = '-';
                  dtostrf(-flon, 1, 6, lon_current + 1);
                } else {
                  dtostrf(flon, 1, 6, lon_current);
                }
                gps_speed = fkmph;
                gps_altitude = falt;
                gps_heading = fc;
                gps_hdop = gps.hdop();
                gps_sats = gps.satellites();

                if(fix_age == TinyGPS::GPS_INVALID_AGE)
                    debug_print(F("No fresh fix detected"));
                else if(fix_age > 1000)
                    debug_print(F("Warning: possible stale data!"));
                else {
                    // Only use GPS time as fallback — modem NITZ time is more
                    // accurate since GPS NMEA time reflects fix start, not now
                    extern bool gsm_clock_was_set;
                    if (!gsm_clock_was_set) {
                      ltoa(date_gps + 1000000, tmp, 10);
                      time_char[0] = tmp[1]; time_char[1] = tmp[2]; time_char[2] = '/';
                      time_char[3] = tmp[3]; time_char[4] = tmp[4]; time_char[5] = '/';
                      time_char[6] = tmp[5]; time_char[7] = tmp[6]; time_char[8] = ',';

                      ltoa(time_gps + 100000000, tmp, 10);
                      time_char[9] = tmp[1]; time_char[10] = tmp[2]; time_char[11] = ':';
                      time_char[12] = tmp[3]; time_char[13] = tmp[4]; time_char[14] = ':';
                      time_char[15] = tmp[5]; time_char[16] = tmp[6];
                      snprintf(&time_char[17], sizeof(time_char) - 17, ".%06lu+00", micros() % 1000000);

                      gsm_set_time();
                    }
                }

                last_time_gps = time_gps;
                last_date_gps = date_gps;

            }
        }

        if(fix == 1) {
            //fix was found
            debug_print(F("collect_gps_data(): fix acquired"));
            break;
        }

        delay(10);
    }

    unsigned long _chars;
    unsigned short _sentences;
    gps.stats(&_chars, &_sentences, &failed_checksum);
    debug_print(F("Failed checksums:"));
    debug_print(failed_checksum);

    if(fix != 1) {
        debug_print(F("collect_gps_data(): fix not acquired, given up."));
    }
    return fix;
}

