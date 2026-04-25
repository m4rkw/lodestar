
//gsm functions
int gsm_reply_failures = 0;
bool gsm_clock_was_set = false;

#if NETWORK_MODE == 2 && defined(MODEM_BG96)
byte gsm_on_preferred_rat = 0;  // 1 = on Cat-M1, 0 = on fallback RAT
#endif


#if MODEM_UG96 || MODEM_EG91 || MODEM_BG96
#define MODEM_CMDSET 1
#define AT_CONTEXT "AT+QICSGP=1,1,"
#define AT_ACTIVATE "AT+QIACT=1\r"
#define AT_DEACTIVATE "AT+QIDEACT=1\r"
#define AT_CONFIGDNS "AT+QIDNSCFG=1,"
#define AT_LOCALIP "AT+QIACT?\r"
#define AT_OPEN "AT+QIOPEN=1,0,"
#define AT_CLOSE "AT+QICLOSE=0\r"
#define AT_SEND "AT+QISEND=0,"
#define AT_RECEIVE "AT+QIRD=0,"
#define AT_STAT "AT+QISTATE=1,0\r"
#define AT_QUERYACK "AT+QISEND=0,0\r"
#define AT_ACKRESP "+QISEND: "
#define AT_NTP "AT+QNTP=1,"
#else
#define MODEM_CMDSET 0
#define AT_CONTEXT "AT+QIREGAPP="
#define AT_ACTIVATE "AT+QIACT\r"
#define AT_DEACTIVATE "AT+QIDEACT\r"
#define AT_CONFIGDNS "AT+QIDNSCFG="
#define AT_LOCALIP "AT+QILOCIP\r"
#define AT_OPEN "AT+QIOPEN=0,"
#define AT_CLOSE "AT+QICLOSE=0\r"
#define AT_SEND "AT+QISEND=0,"
#define AT_RECEIVE "AT+QIRD=0,1,0,"
#define AT_STAT "AT+QISTATE\r"
#define AT_QUERYACK "AT+QISACK=0\r"
#define AT_ACKRESP "+QISACK: "
#define AT_NTP "AT+QNTP="
#endif

#ifndef GSM_STAY_ONLINE
    #define GSM_STAY_ONLINE 1     // 0 == Disconnect Session after each send of data. 1 == Stay Online to keep session active (Default)
#endif

// Progressive recovery thresholds. See gsm_send_recovery() for usage.
#ifndef GSM_RECOVERY_SOFT_THRESHOLD
    #define GSM_RECOVERY_SOFT_THRESHOLD 3   // PDP deactivate at or above this count
#endif
#ifndef GSM_RECOVERY_HARD_THRESHOLD
    #define GSM_RECOVERY_HARD_THRESHOLD 5   // Modem power cycle at or above this count
#endif

void gsm_init() {
  //setup modem pins
  DEBUG_FUNCTION_CALL();

  pinMode(PIN_C_PWR_GSM, OUTPUT);
  digitalWrite(PIN_C_PWR_GSM, LOW);

  pinMode(PIN_C_KILL_GSM, OUTPUT);
  digitalWrite(PIN_C_KILL_GSM, LOW);

  pinMode(PIN_STATUS_GSM, INPUT_PULLUP);
  pinMode(PIN_RING_GSM, INPUT_PULLUP);

  pinMode(PIN_WAKE_GSM, OUTPUT);
  digitalWrite(PIN_WAKE_GSM, HIGH);

  gsm_open();
}

void gsm_open() {
  gsm_port.begin(115200);
}

void gsm_close() {
  gsm_port.end();
}

bool gsm_power_status() {
#if (OPENTRACKER_HW_REV >= 0x0300) || MODEM_UG96 || MODEM_BG96
  // inverted status signal
  return digitalRead(PIN_STATUS_GSM) != HIGH;
#else
  // help discharge floating pin, by temporarily setting as output low
  PIO_Configure(
    g_APinDescription[PIN_STATUS_GSM].pPort,
    PIO_OUTPUT_0,
    g_APinDescription[PIN_STATUS_GSM].ulPin,
    g_APinDescription[PIN_STATUS_GSM].ulPinConfiguration);
  pinMode(PIN_STATUS_GSM, INPUT);
  delay(1);
  // read modem power status
  return digitalRead(PIN_STATUS_GSM) != LOW;
#endif
}

void gsm_on() {
  //turn on the modem
  DEBUG_FUNCTION_CALL();

  int k=0;
  for (;;) {
    watchdog_kick();
    if(!gsm_power_status()) { // now off, turn on
      unsigned long t = millis();
      digitalWrite(PIN_C_PWR_GSM, HIGH);
      while (!gsm_power_status() && (millis() - t < 1000));
      t = millis();
      digitalWrite(PIN_C_PWR_GSM, LOW);
      while (!gsm_power_status() && (millis() - t < 15000))
        delay(100);
      status_delay(1000);
    }

    // auto-baudrate
    if (gsm_send_at())
      break;
    DEBUG_FUNCTION_PRINTLN("failed auto-baudrate");

    if (++k >= 5) // max attempts
      break;

    gsm_off(0);
    gsm_off(1);

    status_delay(1000);

    DEBUG_FUNCTION_PRINT("try again ");
    DEBUG_PRINTLN(k);
  }

  // make sure it's not sleeping
  gsm_wakeup();
}

void gsm_off(int emergency) {
  //turn off the modem
  DEBUG_FUNCTION_CALL();
  gsm_clock_was_set = false;

  unsigned long t = millis();

  if(emergency) {
    digitalWrite(PIN_C_KILL_GSM, HIGH);
    while (gsm_power_status() && (millis() - t < 5000))
      delay(100);
    digitalWrite(PIN_C_KILL_GSM, LOW);
    status_delay(1000);
  }
  else
  if(gsm_power_status()) { // now on, turn off
#if MODEM_UG96 || MODEM_BG96
    // graceful shutdown — gives modem up to 5s to flush NVM cache,
    // then hardware kill if it hasn't powered off yet
    gsm_port.print("AT+QPOWD=1\r");
    gsm_wait_for_reply(1,0);

    // reset timer: gsm_wait_for_reply() may have consumed most of the window
    // if the modem was unresponsive, leaving no time for the kill pulse
    unsigned long tshut = millis();
    while (gsm_power_status() && (millis() - tshut < 5000))
      delay(100);

    if (gsm_power_status()) {
      // modem didn't shut down in time — hardware kill
      // BG96 PWR_KEY must be held for ≥650ms for reliable shutdown
      digitalWrite(PIN_C_KILL_GSM, HIGH);
      unsigned long tkill = millis();
      while (gsm_power_status() && (millis() - tkill < 2000))
        delay(100);
      digitalWrite(PIN_C_KILL_GSM, LOW);
    }
#else
    digitalWrite(PIN_C_PWR_GSM, HIGH);
    delay(750);
    digitalWrite(PIN_C_PWR_GSM, LOW);

    while (gsm_power_status() && (millis() - t < 12500))
      delay(100);
#endif
  }
  gsm_get_reply(1);
}

void gsm_standby() {
  // clear wake signal
#if OPENTRACKER_HW_REV >= 0x0300
  digitalWrite(PIN_WAKE_GSM, LOW);
#else
  digitalWrite(PIN_WAKE_GSM, HIGH);
#endif
  // standby GSM
  gsm_port.print("AT+CFUN=4\r");
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QSCLK=1\r");
  gsm_wait_for_reply(1,0);
}

void gsm_wakeup() {
  // wake GSM
#if OPENTRACKER_HW_REV >= 0x0300
  digitalWrite(PIN_WAKE_GSM, HIGH);
#else
  digitalWrite(PIN_WAKE_GSM, LOW);
#endif
  delay(1000);
  gsm_port.print("AT+QSCLK=0\r");
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+CFUN=1\r");
  gsm_wait_for_reply(1,0);
}

void gsm_enter_sleep() {
  gsm_standby();
}

void gsm_exit_sleep() {
  gsm_wakeup();
  gsm_wait_for_reply(1, 0);  // wait for modem ready
}

void gsm_setup() {
  DEBUG_FUNCTION_CALL();

  //turn off modem
  gsm_off(1);

  //blink modem restart
  blink_start();

  //turn on modem
  gsm_on();

#if MODEM_BG96
  // enable GNSS antenna power
  pinMode(PIN_C_ANTON, OUTPUT);
  digitalWrite(PIN_C_ANTON, HIGH);
  // route GNSS NMEA to UART and enable GNSS engine
  gsm_port.print("AT+QGPSCFG=\"outport\",\"uartnmea\"\r");
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QGPS=1\r");
  gsm_wait_for_reply(1,0);
#endif

  //configure
  gsm_config();
}

void gsm_enable_time_sync() {
  // disable time-sync reports
  gsm_port.print("AT+CTZR=0\r");
  gsm_wait_for_reply(1,0);

  // enable GSM time synchronization
#if MODEM_M95
  gsm_port.print("AT+QNITZ=1\r");
  gsm_wait_for_reply(1,0);

  gsm_port.print("AT+CTZU=2\r");
#else
  gsm_port.print("AT+CTZU=1\r");
#endif
  gsm_wait_for_reply(1,0);
}

void gsm_config() {
  //supply PIN code if needed
  gsm_set_pin();

  // wait up to 1 minute
  gsm_wait_modem_ready(120000);

  //get GSM IMEI
  gsm_get_imei();

  //enable synchronization to GSM time
  gsm_enable_time_sync();

  //misc GSM startup commands (disable echo)
  gsm_startup_cmd();

#if MODEM_BG96
  // NVM config (band/mode/etc) — only written on change, persists across reboots.
  // No CFUN=0/1 cycle: let modem keep cached cell info for faster reattach.

  #if NETWORK_MODE > 0
  // PS-only service domain (required for data-only SIMs on LTE)
  gsm_port.print("AT+QCFG=\"servicedomain\",1,1\r");
  #else
  // CS+PS service domain for GSM
  gsm_port.print("AT+QCFG=\"servicedomain\",2,1\r");
  #endif
  gsm_wait_for_reply(1,0);

  #if NETWORK_MODE == 1
  // NB-IoT only
  gsm_port.print("AT+QCFG=\"nwscanmode\",3,1\r");  // LTE only, save to NVM
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QCFG=\"iotopmode\",1,1\r");  // NB-IoT only
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QCFG=\"band\",0,0,A0E189F,1\r");  // NB-IoT all supported bands
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QCFG=\"nwscanseq\",030201,1\r");
  gsm_wait_for_reply(1,0);
  #elif NETWORK_MODE == 2
  // LTE Cat-M1 with NB-IoT and 2G fallback
  gsm_port.print("AT+QCFG=\"nwscanmode\",3,1\r");  // LTE only
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QCFG=\"iotopmode\",0,1\r");  // Cat-M1 only
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QCFG=\"band\",0,80080,0,1\r");  // Cat-M1 B8 (900MHz) + B20 (800MHz)
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QCFG=\"nwscanseq\",030201,1\r");  // Cat-M1 first
  gsm_wait_for_reply(1,0);
  #elif NETWORK_MODE == 3
  // LTE Cat-M1 only (no fallback)
  gsm_port.print("AT+QCFG=\"nwscanmode\",3,1\r");  // LTE only
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QCFG=\"iotopmode\",0,1\r");  // Cat-M1 only
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QCFG=\"band\",0,A0E189F,0,1\r");  // Cat-M1 all supported bands
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QCFG=\"nwscanseq\",030201,1\r");
  gsm_wait_for_reply(1,0);
  #else
  // GSM/2G only
  gsm_port.print("AT+QCFG=\"nwscanmode\",1,1\r");  // GSM only
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QCFG=\"iotopmode\",2,1\r");
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QCFG=\"nwscanseq\",030102,1\r");
  gsm_wait_for_reply(1,0);
  #endif

  // set default PDP context APN before attach (required for Cat-M1/NB-IoT)
  #if NETWORK_MODE > 0
  gsm_port.print("AT+CGDCONT=1,\"IP\",\"");
  gsm_port.print(config.apn);
  gsm_port.print("\"\r");
  gsm_wait_for_reply(1,0);
  #endif

  // operator selection
#if PREFERRED_PLMN > 0
  // manual with automatic fallback (mode 4): try preferred PLMN first,
  // fall back to auto if unavailable
  char cops_cmd[32];
  snprintf(cops_cmd, sizeof(cops_cmd), "AT+COPS=4,2,\"%d\"\r", PREFERRED_PLMN);
  gsm_port.print(cops_cmd);
#else
  gsm_port.print("AT+COPS=0\r");
#endif
  gsm_wait_for_reply(1,0);
#endif

  // network search is now async — AT+COPS=0 was issued above,
  // modem will register in the background while GPS acquires a fix.
  // Caller checks gsm_get_network_status() when ready to send.
}

// Called after successful network registration to sync time and record RAT.
void gsm_post_register() {
#if NETWORK_MODE == 2 && defined(MODEM_BG96)
  // record whether we registered on Cat-M1
  gsm_port.print("AT+QNWINFO\r");
  gsm_wait_for_reply(1, 1);
  gsm_on_preferred_rat = (strstr(modem_reply, "CAT-M1") != NULL);
  if (gsm_on_preferred_rat)
    debug_print(F("Registered on Cat-M1"));
  else
    debug_print(F("Registered on fallback RAT, will retry Cat-M1 later"));
#endif

  //synchronize modem clock
  gsm_check_time_sync();

#if MODEM_BG96
  // Populate the cell cache so the first telemetry packet carries current
  // MCC/MNC/LAC/CI.  Subsequent refreshes are URC-driven (see gsm.ino URC
  // hook) — this is the only unconditional refresh per wake.
  gsm_refresh_cell_info();
#endif
}

void gsm_print_signal_info(int longer) {
#ifdef DEBUG
  gsm_port.print("AT+CSQ\r");
  gsm_wait_for_reply(1,0);
  if (longer > 0) {
    gsm_port.print("AT+COPS?\r");
    gsm_wait_for_reply(1,0);
#if MODEM_BG96
    gsm_port.print("AT+QNWINFO\r");
    gsm_wait_for_reply(1,0);
#endif
#if MODEM_CMDSET
    gsm_port.print("AT+QENG=\"servingcell\"\r");
    gsm_wait_for_reply(1,0);
#else
    gsm_port.print("AT+QENG=1,0\r");
    gsm_wait_for_reply(1,0);
#endif
  }
  if (longer > 1) {
    gsm_port.print("AT+COPS=?\r");
    gsm_wait_for_reply(1,0);
  }
#endif
}


void gsm_wait_modem_ready(int timeout) {
  // wait for modem ready (attached to network)
  unsigned long t = millis();
  do {
    gsm_print_signal_info(0); // debug

    int pas = gsm_get_modem_status();
    if(pas==0 || pas==3 || pas==4) break;
    status_delay(3000);
  }
  while ((long)(millis() - t) < timeout);
}

void gsm_wait_network_ready(int timeout) {
  // wait for modem ready (attached to network)
  unsigned long t = millis();
  do {
    gsm_print_signal_info(1); // debug

    int reg = gsm_get_network_status();
    if(reg==1 || reg==5 || reg==3) break;
    status_delay(3000);
  }
  while ((long)(millis() - t) < timeout);
}

void gsm_set_time() {
  DEBUG_FUNCTION_CALL();

  // time_char is DD/MM/YY,HH:MM:SS.uuuuuu+TZ
  // AT+CCLK expects YY/MM/DD,HH:MM:SS+TZ (no microseconds)
  char cclk[sizeof(time_char)];
  strlcpy(cclk, time_char, sizeof(cclk));

  // swap DD (pos 0-1) with YY (pos 6-7)
  char dd0 = cclk[0], dd1 = cclk[1];
  cclk[0] = cclk[6]; cclk[1] = cclk[7];
  cclk[6] = dd0;     cclk[7] = dd1;

  // strip microseconds (.nnnnnn) — find '.' after seconds, copy TZ over it
  char *dot = strchr(cclk + 17, '.');
  if (dot) {
    char *tz = dot + 1;
    while (*tz && *tz != '+' && *tz != '-') tz++;
    if (*tz) memmove(dot, tz, strlen(tz) + 1);
    else *dot = '\0';
  }

  gsm_port.print("AT+CCLK=\"");
  gsm_port.print(cclk);
  gsm_port.print("\"\r");

  gsm_wait_for_reply(1,0);
  gsm_clock_was_set = true;
}

void gsm_check_time_sync() {
#if MODEM_BG96
  gsm_port.print("AT+QLTS=2\r");
#else
  gsm_port.print("AT+QLTS\r");
#endif
  gsm_wait_for_reply(1,1);

  gsm_get_time();

  // If NITZ didn't set the clock (common on Cat-M1/NB-IoT), try NTP
  if (!gsm_clock_was_set) {
    gsm_port.print(AT_NTP "\"pool.ntp.org\",123\r");
    gsm_wait_for_reply(1,1);
    // QNTP updates the modem clock asynchronously; wait and re-read
    status_delay(3000);
    gsm_get_time();
  }
}

void gsm_set_pin() {
  DEBUG_FUNCTION_CALL();

  for (int k=0; k<5; ++k) {
    //checking if PIN is set
    gsm_port.print("AT+CPIN?\r");

    gsm_wait_for_reply(1,1);

    char *tmp = strstr(modem_reply, "SIM PIN");
    if(tmp!=NULL) {
      DEBUG_FUNCTION_PRINTLN("PIN is required");

      //checking if pin is valid one
      if(config.sim_pin[0] == 255) {
        DEBUG_FUNCTION_PRINTLN("PIN is not supplied.");
      } else {
        if(strlen(config.sim_pin) == 4) {
          DEBUG_FUNCTION_PRINTLN("PIN supplied, sending to modem.");

          gsm_port.print("AT+CPIN=");
          gsm_port.print(config.sim_pin);
          gsm_port.print("\r");

          gsm_wait_for_reply(1,0);

          tmp = strstr(modem_reply, "OK");
          if(tmp!=NULL) {
            DEBUG_FUNCTION_PRINTLN("PIN is accepted");
          } else {
            DEBUG_FUNCTION_PRINTLN("PIN is not accepted");
          }
          break;
        } else {
          DEBUG_FUNCTION_PRINTLN("PIN supplied, but has invalid length. Not sending to modem.");
          break;
        }
      }
    }
    tmp = strstr(modem_reply, "READY");
    if(tmp!=NULL) {
      DEBUG_FUNCTION_PRINTLN("PIN is not required");
      break;
    }
    status_delay(2000);
  }
}

void gsm_get_time() {
  DEBUG_FUNCTION_CALL();

  //clean any serial data

  gsm_get_reply(0);

  //get time from modem
  gsm_port.print("AT+CCLK?\r");

  gsm_wait_for_reply(1,1);

  char *tmp = strstr(modem_reply, "+CCLK: ");
  if (tmp)
    tmp = strtok(tmp + 7, "\"\r");
  if (tmp) {
    // Validate clock — BG96 defaults to 80/01/06 after power cycle.
    // Only update time_char if year is plausible (20–79).
    int yy = (tmp[0] - '0') * 10 + (tmp[1] - '0');
    if (yy >= 20 && yy < 80) {
      gsm_clock_was_set = true;
      // Modem returns YY/MM/DD — swap to DD/MM/YY to match GPS/server format
      char dd0 = tmp[6], dd1 = tmp[7];
      tmp[6] = tmp[0]; tmp[7] = tmp[1];  // YY into pos 6-7
      tmp[0] = dd0;    tmp[1] = dd1;     // DD into pos 0-1
      // Find timezone offset (+/- after seconds)
      char *tz = strrchr(tmp, '+');
      if (!tz) tz = strrchr(tmp, '-');
      if (tz) {
        unsigned long us = micros() % 1000000;
        int base_len = tz - tmp;
        snprintf(time_char, sizeof(time_char), "%.*s.%06lu%s", base_len, tmp, us, tz);
      } else {
        strlcpy(time_char, tmp, sizeof(time_char));
      }
    }

    DEBUG_FUNCTION_PRINT("result=");
    DEBUG_PRINTLN(time_char);
  }
}

void gsm_startup_cmd() {
  DEBUG_FUNCTION_CALL();

  //disable echo for sent IP data
  gsm_port.print("AT+QISDE=0\r");

  gsm_wait_for_reply(1,0);

#if MODEM_M95
  //set receiving IP data by command
  gsm_port.print("AT+QINDI=1\r");

  gsm_wait_for_reply(1,0);

  //set multiple socket support
  gsm_port.print("AT+QIMUX=1\r");

  gsm_wait_for_reply(1,0);
#endif

#if GSM_USE_QUECLOCATOR_TIMEOUT > 0
  //set QuectLocator timeout
  gsm_port.print("AT+QLOCCFG=\"timeout\",");
  gsm_port.print(GSM_USE_QUECLOCATOR_TIMEOUT);
  gsm_port.print("\r");

  gsm_wait_for_reply(1,0);
#endif

#if MODEM_BG96
  // Enable registration URCs with location info (TAC/LAC + CI).
  // Cache is refreshed by gsm_parse_reg_urc() whenever the network changes
  // (roaming handover, cell reselection with new TAC, etc.), so every
  // telemetry packet carries current-as-of-last-change cell info.
  gsm_port.print("AT+CREG=2\r");
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+CEREG=2\r");
  gsm_wait_for_reply(1,0);
#endif
}

void gsm_get_imei() {
  DEBUG_FUNCTION_CALL();

  //get modem's imei
  gsm_port.print("AT+GSN\r");

  status_delay(500);
  gsm_get_reply(1);

  //reply data stored to modem_reply
  char *tmp = strstr(modem_reply, "AT+GSN\r\r\n");
  if (tmp) {
    tmp += strlen("AT+GSN\r\r\n");
    char *tmpval = strtok(tmp, "\r");

    //copy data to main IMEI var
    if (tmpval)
      strlcpy(config.imei, tmpval, sizeof(config.imei));
  }
  DEBUG_FUNCTION_PRINT("result=");
  DEBUG_PRINTLN(config.imei);
}

int gsm_send_at() {
  DEBUG_FUNCTION_CALL();

  int ret = 0;
  for (int k=0; k<5; ++k) {
    gsm_port.print("ATE1\r");
    status_delay(50);

    gsm_get_reply(1);
    ret = (strstr(modem_reply, "ATE1") != NULL)
      && (strstr(modem_reply, "OK") != NULL);
    if (ret) break;

    status_delay(1000);
  }
  DEBUG_FUNCTION_PRINT("returned=");
  DEBUG_PRINTLN(ret);
  return ret;
}

int gsm_get_modem_status() {
  DEBUG_FUNCTION_CALL();

  gsm_port.print("AT+CPAS\r");

  int pas = -1; // unexpected reply
  for (int k=0; k<10; ++k) {
    status_delay(50);
    gsm_get_reply(0);

    char *tmp = strstr(modem_reply, "+CPAS:");
    if(tmp!=NULL) {
      pas = atoi(tmp+6);
      break;
    }
  }
  gsm_wait_for_reply(1,0);

  DEBUG_FUNCTION_PRINT("returned=");
  DEBUG_PRINTLN(pas);
  return pas;
}

int gsm_parse_reg_reply(const char *prefix) {
  int reg = -1;
  for (int k=0; k<10; ++k) {
    status_delay(50);
    gsm_get_reply(0);
    char *tmp = strstr(modem_reply, prefix);
    if(tmp!=NULL)
      tmp = strchr(tmp + strlen(prefix), ',');
    if(tmp!=NULL) {
      reg = atoi(tmp+1);
      break;
    }
  }
  gsm_wait_for_reply(1,0);
  return reg;
}

int gsm_get_network_status() {
  DEBUG_FUNCTION_CALL();

#if NETWORK_MODE == 2
  // fallback mode: check LTE first, then GSM
  gsm_port.print("AT+CEREG?\r");
  int reg = gsm_parse_reg_reply("+CEREG:");
  if (reg != 1 && reg != 5) {
    // not registered on LTE, check GSM
    gsm_port.print("AT+CGREG?\r");
    reg = gsm_parse_reg_reply("+CGREG:");
  }
#elif NETWORK_MODE > 0
  // LTE-only modes: use CEREG
  gsm_port.print("AT+CEREG?\r");
  int reg = gsm_parse_reg_reply("+CEREG:");
#else
  // GSM mode: use CGREG
  gsm_port.print("AT+CGREG?\r");
  int reg = gsm_parse_reg_reply("+CGREG:");
#endif

  DEBUG_FUNCTION_PRINT("returned=");
  DEBUG_PRINTLN(reg);
  return reg;
}

int gsm_disconnect() {
  int ret = 0;
  DEBUG_FUNCTION_CALL();
#if GSM_DISCONNECT_AFTER_SEND
  // option to close data session after each server connection
  ret = gsm_deactivate();
#else
  //close connection, if previous attempts left it open
  gsm_port.print(AT_CLOSE);
  gsm_wait_for_reply(MODEM_CMDSET,0);

  //ignore errors (will be taken care during connect)
  ret = 1;
#endif
  return ret;
}

int gsm_deactivate() {
  // disable data session
  int ret = 0;
  DEBUG_FUNCTION_CALL();

  //disconnect GSM
  gsm_port.print(AT_DEACTIVATE);
  gsm_wait_for_reply(MODEM_CMDSET,0);

#if MODEM_CMDSET
  //check if result contains OK
  char *tmp = strstr(modem_reply, "OK");
#else
  //check if result contains DEACT OK
  char *tmp = strstr(modem_reply, "DEACT OK");
#endif

  if(tmp!=NULL)
    ret = 1;

  return ret;
}

int gsm_set_apn()  {
  DEBUG_FUNCTION_CALL();

  //disconnect GSM
  gsm_port.print(AT_DEACTIVATE);
  gsm_wait_for_reply(MODEM_CMDSET,0);

  //addon_event(ON_MODEM_ACTIVATION);

  //set all APN data, dns, etc
  gsm_port.print(AT_CONTEXT "\"");
  gsm_port.print(config.apn);
  gsm_port.print("\",\"");
  gsm_port.print(config.user);
  gsm_port.print("\",\"");
  gsm_port.print(config.pwd);
  gsm_port.print("\"");
  gsm_port.print("\r");

  gsm_wait_for_reply(1,0);

#if MODEM_M95
  gsm_port.print("AT+QIDNSIP=1\r");
  gsm_wait_for_reply(1,0);
#endif

  gsm_port.print(AT_ACTIVATE);

  // wait for GPRS contex activation (first time)
  unsigned long t = millis();
  do {
    gsm_wait_for_reply(1,0);
    if(modem_reply[0] != 0) break;
  }
  while (millis() - t < 60000);
  if (strstr(modem_reply, "OK") == NULL)
    return 0;

  // verify we have a local IP address
  gsm_port.print(AT_LOCALIP);
#if MODEM_M95
  status_delay(500);
  gsm_get_reply(1);
  if (strstr(modem_reply, ".") == NULL)
    return 0;
#else
  gsm_wait_for_reply(1,0);
  if (strstr(modem_reply, "OK") == NULL)
    return 0;
#endif
  gsm_send_at();

  // set google DNS (TODO: make optional)
  gsm_port.print(AT_CONFIGDNS "\"8.8.8.8\"\r");
  gsm_wait_for_reply(1,0);
  if (strstr(modem_reply, "OK") == NULL)
    return 0;

  return 1;
}

int gsm_get_connection_status() {
  DEBUG_FUNCTION_CALL();

  int ret = -1; //unknown

  gsm_get_reply(1); //flush buffer
  gsm_port.print(AT_STAT);

  gsm_wait_for_reply(1,0);

#if MODEM_CMDSET
  char *tmp = strtok(modem_reply, ",");
  if (tmp != NULL && strstr(modem_reply, "+QISTATE:") != NULL) {
    for (int k=0; k<5; ++k) {
      tmp = strtok(NULL, ",");
    }
    if (tmp != NULL) {
      ret = atoi(tmp);
      DEBUG_PRINTLN(ret);
#if MODEM_UG96
      if (ret == 3)
#else
      if (ret == 2)
#endif
        ret = 1; // already connected
      else
        ret = 2; // previous connection failed, should close
    }

    gsm_wait_for_reply(1,0); // catch OK
  }
  else if (strstr(modem_reply, "OK") != NULL)
    ret = 0; // ready to connect

  // check also data packet connection is active

  gsm_get_reply(1); //flush buffer
  gsm_port.print("AT+CGACT?\r");

  gsm_wait_for_reply(1,1);

  tmp = strstr(modem_reply, "+CGACT:");
  if(tmp!=NULL) {
    tmp = strtok(tmp + 7, ",");
    if(tmp!=NULL) {
      tmp = strtok(NULL, ",");
      if(tmp!=NULL) {
        if (atoi(tmp) != 1)
          ret = -2; // force deactivation
      }
    }
  }

#else
  if (strstr(modem_reply, "OK\r\n") != NULL) {
    gsm_wait_for_reply(0,0);
    if (strstr(modem_reply, "IP IND") != NULL ||
      strstr(modem_reply, "PDP DEACT") != NULL) {
      ret = -2; // force deactivation
    }
    // find socket status
    for (int i=0; i<6; ++i) {
      gsm_wait_for_reply(0,0);

      if (ret == -1 && strstr(modem_reply, "+QISTATE: 0,") != NULL) {
        if (strstr(modem_reply, "INITIAL") != NULL ||
          strstr(modem_reply, "CLOSE") != NULL)
          ret = 0; // ready to connect

        if (strstr(modem_reply, "CONNECTED") != NULL)
          ret = 1; // already connected

        if (strstr(modem_reply, "CONNECTING") != NULL)
          ret = 2; // previous connection failed, should close
      }
    }
    gsm_wait_for_reply(1,0); // catch final OK
  }
#endif
  DEBUG_FUNCTION_PRINT("returned=");
  DEBUG_PRINTLN(ret);
  return ret;
}

#if MODEM_CMDSET
// Resolve HOSTNAME via AT+QIDNSGIP and cache the IP for the session.
// Returns 1 on success (cached_server_ip populated), 0 on failure.
// A failed resolve leaves cached_server_ip empty so the caller will try
// again on its next connect attempt.
int gsm_resolve_hostname() {
  DEBUG_FUNCTION_CALL();

  gsm_port.print("AT+QIDNSGIP=1,\"");
  gsm_port.print(HOSTNAME);
  gsm_port.print("\"\r");
  gsm_wait_for_reply(1, 0);  // OK

  // Poll for the +QIURC: "dnsgip","<ip>" URC. Status URC fires first with
  // +QIURC: "dnsgip",<err>,<cnt>,<ttl> then the IP URC(s) follow.
  long timer = millis();
  cached_server_ip[0] = '\0';
  while (millis() - timer < GSM_CONNECT_TIMEOUT) {
    gsm_get_reply(1);
    if (gsm_parse_dnsgip(modem_reply, cached_server_ip,
                         sizeof(cached_server_ip))) {
      char buf[48];
      snprintf(buf, sizeof(buf), "DNS %s -> %s", HOSTNAME, cached_server_ip);
      debug_print(buf);
      return 1;
    }
    delay(100);
  }
  debug_print(F("DNS resolve failed"));
  cached_server_ip[0] = '\0';
  return 0;
}
#endif

int gsm_connect() {
  int ret = 0;

  DEBUG_FUNCTION_CALL();

  //try to connect multiple times
  for(int i=0;i<CONNECT_RETRY;i++) {
    // On retries, unconditionally close any zombie socket before reopening.
    // A previous failed QIOPEN (e.g. aborted mid-handshake by a PDP deact)
    // leaves connectID=0 claimed on the modem, causing the next QIOPEN to
    // fail with error 563 "socket Identity has been used". AT+QICLOSE=0
    // clears that state. Safe when no socket is open (returns OK).
    if (i > 0) {
      gsm_port.print(AT_CLOSE);
      gsm_wait_for_reply(MODEM_CMDSET,0);
    }

    // connect only when modem is ready
    if (gsm_get_modem_status() == 0) {
      // check if connected from previous attempts
      int ipstat = gsm_get_connection_status();

      if (ipstat > 1) {
        //close connection, if previous attempts failed
        gsm_port.print(AT_CLOSE);
        gsm_wait_for_reply(MODEM_CMDSET,0);
        ipstat = 0;
      }
      if (ipstat < 0) {
        //deactivate required
        gsm_port.print(AT_DEACTIVATE);
        gsm_wait_for_reply(MODEM_CMDSET,0);
        ipstat = 0;

#if MODEM_CMDSET
        gsm_port.print(AT_ACTIVATE);
        gsm_wait_for_reply(1,0);

        gsm_port.print(AT_CONFIGDNS "\"8.8.8.8\"\r");
        gsm_wait_for_reply(1,0);
#endif
      }
      if (ipstat == 0) {
        DEBUG_PRINT("Connecting to remote server... ");
        DEBUG_PRINTLN(i);

#if MODEM_CMDSET
        // Resolve hostname once per session. Passing the IP literal to
        // QIOPEN avoids a BG96 "system busy" (568) race where the modem's
        // resolver hasn't finished while a socket is still being torn down.
        if (cached_server_ip[0] == '\0') {
          gsm_resolve_hostname();
        }
        const char *connect_host = (cached_server_ip[0] != '\0')
                                     ? cached_server_ip : HOSTNAME;
#else
        const char *connect_host = HOSTNAME;
#endif

        //open socket connection to remote host
        //opening connection
        gsm_port.print(AT_OPEN "\"UDP\",\"");
        gsm_port.print(connect_host);
        gsm_port.print("\",");
#if MODEM_M95
        gsm_port.print("\"");
#endif
        gsm_port.print(UDP_PORT);
#if MODEM_M95
        gsm_port.print("\"");
#endif
        gsm_port.print("\r");

        gsm_wait_for_reply(1, 0); // OK sent first

        long timer = millis();
        if(strstr(modem_reply, "OK")==NULL)
          ipstat = 0;
        else
        do {
          gsm_get_reply(1);

#if MODEM_CMDSET
          // PDP context torn down by the network mid-handshake — the QIOPEN
          // will never complete. Bail out immediately instead of waiting the
          // full GSM_CONNECT_TIMEOUT (~60s) for nothing. The next retry
          // iteration reactivates the context via ipstat<0 path.
          if(strstr(modem_reply, "pdpdeact")!=NULL) {
            ipstat = -1;
            break;
          }
          char *tmp = strstr(modem_reply, "+QIOPEN: 0,");
          if(tmp!=NULL) {
            tmp += strlen("+QIOPEN: 0,");
            if (atoi(tmp)==0)
              ipstat = 1;
            else
              ipstat = 0;
            break;
          }
#else
          if(strstr(modem_reply, "CONNECT OK")!=NULL) {
            ipstat = 1;
            break;
          }
          if(strstr(modem_reply, "CONNECT FAIL")!=NULL ||
            strstr(modem_reply, "ERROR")!=NULL) {
            ipstat = 0;
            break;
          }
#endif
          delay(100);
        } while (millis() - timer < GSM_CONNECT_TIMEOUT);
      }

      if(ipstat == 1) {
        DEBUG_PRINT("Connected to remote server: ");
        DEBUG_PRINTLN(HOSTNAME);

        ret = 1;
        break;
      } else {
        DEBUG_PRINT("Can not connect to remote server: ");
        DEBUG_PRINTLN(HOSTNAME);
        // debug only:
        gsm_port.print("AT+CEER\r");
        gsm_wait_for_reply(1,0);
        gsm_port.print("AT+QIGETERROR\r");
        gsm_wait_for_reply(1,0);
#if MODEM_CMDSET
        // Drop the cached IP so the next retry re-resolves. Protects us
        // from a stale record after a server IP change or DNS flake.
        cached_server_ip[0] = '\0';
#endif
      }
    }

    delay(1000); // wait 1s before retrying
  }
  return ret;
}

int gsm_send_begin(int data_len) {
  //sending header packet to remote host
  gsm_port.print(AT_SEND);
  gsm_port.print(data_len);
  gsm_port.print("\r");

  gsm_wait_for_reply(1,0);
  if (strncmp(modem_reply, "> ", 2) == 0)
    return 1; // accepted, can send data
  return 0; // error, cannot send data
}

int gsm_send_done() {
  gsm_wait_for_reply(1,0);
  if (strncmp(modem_reply, "SEND OK", 7) == 0)
    return 1; // send successful
  return 0; // error
}

// Wire format (binary, one envelope per datagram):
//   [1] imei_len  [imei_len] IMEI ASCII  [12] nonce  [N] ciphertext  [16] tag
// AAD = the IMEI bytes (binds the envelope to this device).
// Plaintext = one or more CSV records, '\n' separated (existing format).
//
// Buffer is static (not on stack) because UDP_PACKET_SIZE can be 1200.
static uint8_t udp_envelope[UDP_PACKET_SIZE];

// Nonce of the most recently sent request. The server echoes this into the
// response AAD, so only a response bound to a request we actually issued this
// session will authenticate — captured responses can't be replayed across a
// reboot (new RNG output = new last_request_nonce).
static uint8_t last_request_nonce[CP_NONCE_BYTES];
static bool    last_request_nonce_set = false;

int gsm_send_udp_current() {
  debug_print(F("gsm_send_udp(): sending data."));

#if MODEM_BG96
  // Refresh on URC-signalled change (cell_info_stale) OR on a periodic timer.
  // BG96 CREG/CEREG URCs aren't reliable for cell-level handovers within the
  // same TAC, so the time-based trigger is the real safety net for long
  // journeys — without it the server only ever sees the starting cell.
  extern uint32_t last_cell_refresh_ms;
  if (cell_refresh_due((uint32_t)millis(), last_cell_refresh_ms, cell_info_stale)) {
    gsm_refresh_cell_info();
  }
#endif

  int data_len = strlen(data_current);

  // Ensure data ends with newline so the server can split records inside the
  // decrypted plaintext when the firmware sends a batch.
  if (data_len > 0 && data_len < DATA_LIMIT - 1 && data_current[data_len - 1] != '\n') {
    data_current[data_len] = '\n';
    data_len++;
    data_current[data_len] = '\0';
  }

  int imei_len = strlen(config.imei);
  if (imei_len <= 0 || imei_len > 20) {
    DEBUG_FUNCTION_PRINTLN("bad imei length");
    return 0;
  }
  int envelope_overhead = 1 + imei_len + CP_NONCE_BYTES + CP_TAG_BYTES;
  int max_pt = UDP_PACKET_SIZE - envelope_overhead;
  if (max_pt <= 0) {
    DEBUG_FUNCTION_PRINTLN("envelope overhead exceeds UDP_PACKET_SIZE");
    return 0;
  }

  int offset = 0;
  while (offset < data_len) {
    int remaining = data_len - offset;
    int chunk_len = (remaining <= max_pt) ? remaining : max_pt;

    // Avoid splitting mid-record: find last newline within chunk
    if (chunk_len < remaining) {
      for (int i = chunk_len - 1; i >= 0; i--) {
        if (data_current[offset + i] == '\n') {
          chunk_len = i + 1;
          break;
        }
      }
    }

    int dgram_len = 1 + imei_len + CP_NONCE_BYTES + chunk_len + CP_TAG_BYTES;
    udp_envelope[0] = (uint8_t)imei_len;
    memcpy(udp_envelope + 1, config.imei, imei_len);

    uint8_t *nonce = udp_envelope + 1 + imei_len;
    if (!crypto_random(nonce, CP_NONCE_BYTES)) {
      // Reusing a nonce destroys ChaCha20-Poly1305 confidentiality. Refusing
      // to send is the only safe response to RNG failure.
      DEBUG_FUNCTION_PRINTLN("rng failure — refusing to send");
      return 0;
    }
    uint8_t *ct  = nonce + CP_NONCE_BYTES;
    uint8_t *tag = ct    + chunk_len;
    cp_seal(config.psk, nonce,
            udp_envelope + 1, (size_t)imei_len,
            (const uint8_t*)&data_current[offset], (size_t)chunk_len,
            ct, tag);

    // Record nonce for challenge-response: the server binds its reply to
    // this value via the response AAD.
    memcpy(last_request_nonce, nonce, CP_NONCE_BYTES);
    last_request_nonce_set = true;

    debug_print(F("UDP datagram size:"));
    debug_print(dgram_len);

    if (!gsm_send_begin(dgram_len)) {
      DEBUG_FUNCTION_PRINTLN("send refused");
      return 0;
    }
    gsm_port.write(udp_envelope, dgram_len);
    if (!gsm_send_done()) {
      DEBUG_FUNCTION_PRINTLN("send error");
      return 0;
    }

    offset += chunk_len;
  }

  debug_print(F("UDP send completed"));
  return 1;
}

// Read and parse the server's UDP response.
//
// Server response wire format (binary):
//   [12] nonce  [N] ciphertext  [16] tag
// AAD = device IMEI || last_request_nonce (the nonce from our most recent
// outgoing request). The server sealed the reply with this AAD, so if we
// don't have a matching request outstanding (e.g. replayed datagram from a
// prior exchange, or datagram sent before this boot) AEAD auth fails.
//
// Decrypted plaintext is the same compact CSV the firmware used pre-crypto:
//   1,int,ao,ma[,cmd]
//     field 0: 1=ok
//     field 1: engine-off interval (seconds)
//     field 2: always-on (0/1)
//     field 3: movement alarm (0/1)
//     field 4: command string (optional, may contain commas)

// Read exactly `n` bytes from the modem port within `timeout_ms`.
// Returns the number of bytes read (== n on success, < n on timeout).
static int gsm_read_raw(uint8_t *buf, int n, unsigned long timeout_ms) {
  int got = 0;
  unsigned long start = millis();
  while (got < n) {
    if (gsm_port.available()) {
      buf[got++] = (uint8_t)gsm_port.read();
      start = millis();  // reset on progress
    } else if ((unsigned long)(millis() - start) > timeout_ms) {
      break;
    }
  }
  return got;
}

// Issue AT+QIRD=0,256 and read one binary datagram into `buf` (max `max` bytes).
// Returns the byte count on success, 0 if no datagram is queued, -1 on error.
//
// AT+QIRD response shape:
//   <CR><LF>+QIRD: <n><CR><LF><n bytes binary><CR><LF>OK<CR><LF>
// We parse the header byte-by-byte to avoid string-ops over binary data.
static int gsm_read_qird(uint8_t *buf, int max) {
  gsm_port.print(AT_RECEIVE "256\r");

  // Skip until "+QIRD: " then read the decimal length.
  const char marker[] = "+QIRD: ";
  int matched = 0;
  unsigned long start = millis();
  while (matched < (int)sizeof(marker) - 1) {
    if ((unsigned long)(millis() - start) > 1000) return -1;
    if (!gsm_port.available()) continue;
    char c = (char)gsm_port.read();
    if (c == marker[matched]) matched++;
    else matched = (c == marker[0]) ? 1 : 0;
  }

  int nbytes = 0;
  while (1) {
    if ((unsigned long)(millis() - start) > 1000) return -1;
    if (!gsm_port.available()) continue;
    char c = (char)gsm_port.read();
    if (c >= '0' && c <= '9') nbytes = nbytes * 10 + (c - '0');
    else if (c == '\r' || c == '\n') break;
    else return -1;
  }

  // Consume LF (or further whitespace) right after the length line.
  while (1) {
    if ((unsigned long)(millis() - start) > 1000) return -1;
    if (!gsm_port.available()) continue;
    char c = (char)gsm_port.read();
    if (c == '\n') break;
    if (c != '\r') return -1;
  }

  if (nbytes == 0) return 0;
  if (nbytes > max) return -1;

  if (gsm_read_raw(buf, nbytes, 1000) != nbytes) return -1;

  // Drain trailing \r\nOK\r\n via the normal path so URCs etc. are processed.
  gsm_wait_for_reply(1, 1);
  return nbytes;
}

void gsm_read_udp_response() {
  extern char pending_server_cmd[128];

  // Drain any stale datagrams from the socket buffer.  Stale datagrams may
  // be encrypted under a different nonce/key so we just discard the bytes.
  for (;;) {
    static uint8_t scratch[UDP_PACKET_SIZE];
    int n = gsm_read_qird(scratch, sizeof(scratch));
    if (n <= 0) break;
    debug_print(F("UDP: drained stale datagram"));
  }

  // poll for +QIURC: "recv" notification (up to 2s)
  unsigned long resp_start = millis();
  byte got_recv = 0;
  while ((unsigned long)(millis() - resp_start) < 2000) {
    gsm_get_reply(1);
    if (strstr(modem_reply, "recv") != NULL) {
      got_recv = 1;
      break;
    }
    delay(100);
  }
  if (!got_recv) {
    debug_print(F("UDP: no response"));
    return;
  }

  static uint8_t resp[UDP_PACKET_SIZE];
  int n = gsm_read_qird(resp, sizeof(resp));
  if (n <= 0) {
    debug_print(F("UDP: empty response"));
    return;
  }
  if (n < CP_NONCE_BYTES + CP_TAG_BYTES) {
    debug_print(F("UDP: response too short"));
    return;
  }

  int ct_len = n - CP_NONCE_BYTES - CP_TAG_BYTES;
  uint8_t *nonce = resp;
  uint8_t *ct    = resp + CP_NONCE_BYTES;
  uint8_t *tag   = ct + ct_len;

  static uint8_t pt[UDP_PACKET_SIZE];
  int imei_len = strlen(config.imei);
  if (!last_request_nonce_set) {
    debug_print(F("UDP: response before any request"));
    return;
  }
  // AAD = IMEI || last_request_nonce
  static uint8_t aad[20 + CP_NONCE_BYTES];
  if (imei_len > 20) {
    debug_print(F("UDP: bad imei length"));
    return;
  }
  memcpy(aad, config.imei, imei_len);
  memcpy(aad + imei_len, last_request_nonce, CP_NONCE_BYTES);
  if (!cp_open(config.psk, nonce,
               aad, (size_t)(imei_len + CP_NONCE_BYTES),
               ct, (size_t)ct_len, tag, pt)) {
    debug_print(F("UDP: auth failed"));
    return;
  }

  // Null-terminate so the existing text-parsing logic below works unchanged.
  if (ct_len >= (int)sizeof(pt)) ct_len = sizeof(pt) - 1;
  pt[ct_len] = '\0';
  char *body = (char*)pt;

  debug_print(F("UDP response:"));
  debug_print(body);

  // parse CSV fields
  // full:  status,int,ao,ma[,cmd]   (RELAY_CONNECTED || ALWAYS_ON_POWER)
  // slim:  status,int[,cmd]         (neither)
  // field 0 must be "1" (ok)
  if (body[0] != '1' || body[1] != ',') {
    debug_print(F("UDP: bad status"));
    return;
  }

  char *p = body + 2;  // skip "1,"

#if RELAY_CONNECTED || ALWAYS_ON_POWER
  // full response: extract 3 config values (int, ao, ma)
  int fields[3];
  for (int i = 0; i < 3; i++) {
    fields[i] = atoi(p);
    if (i < 2) {
      p = strchr(p, ',');
      if (!p) {
        debug_print(F("UDP: truncated response"));
        return;
      }
      p++;  // skip comma
    }
  }

  int r_int = fields[0];
  int r_ao  = fields[1];
  int r_ma  = fields[2];

  if (!send_int_to_server) {
    if ((r_int == 0 || r_int >= 10) && r_int != config.loop_interval) {
      config.loop_interval = r_int;
    }
    if (r_ao != config.always_on) {
      set_power_state = 1;
      config.always_on = r_ao;
    }
    if (r_ma != config.movement_alarm) {
      config.movement_alarm = r_ma;
    }
  } else {
    if (r_int == config.loop_interval && r_ao == config.always_on
        && r_ma == config.movement_alarm) {
      debug_print(F("UDP: server confirmed config sync"));
      send_int_to_server = 0;
    } else {
      debug_print(F("UDP: server stale, keeping sync pending"));
    }
  }

  // cmd after 4th comma: "1,int,ao,ma,cmd..."
  #define UDP_CMD_COMMAS 4
#else
  // slim response: extract 1 config value (int)
  int r_int = atoi(p);

  if (!send_int_to_server) {
    if ((r_int == 0 || r_int >= 10) && r_int != config.loop_interval) {
      config.loop_interval = r_int;
    }
  } else {
    if (r_int == config.loop_interval) {
      debug_print(F("UDP: server confirmed config sync"));
      send_int_to_server = 0;
    } else {
      debug_print(F("UDP: server stale, keeping sync pending"));
    }
  }

  // cmd after 2nd comma: "1,int,cmd..."
  #define UDP_CMD_COMMAS 2
#endif

  // extract optional cmd field
  char *cmd_p = body;
  for (int i = 0; i < UDP_CMD_COMMAS; i++) {
    cmd_p = strchr(cmd_p, ',');
    if (!cmd_p) break;
    cmd_p++;
  }
  if (cmd_p && *cmd_p) {
    // trim trailing whitespace/newlines
    int len = strlen(cmd_p);
    while (len > 0 && (cmd_p[len-1] == '\r' || cmd_p[len-1] == '\n' || cmd_p[len-1] == ' '))
      len--;
    if (len > 0 && len < (int)sizeof(pending_server_cmd)) {
      memcpy(pending_server_cmd, cmd_p, len);
      pending_server_cmd[len] = '\0';
      debug_print(F("UDP cmd:"));
      debug_print(pending_server_cmd);
    }
  }

  // reset flags that were pending for the server
  powered_on = 0;
}

byte read_udp_response = 0;

int gsm_send_data() {
  int ret_tmp = 0;

  //send 2 ATs
  gsm_send_at();

  //addon_event(ON_SEND_STARTED);

#if GSM_STAY_ONLINE
  // check if already connected, only reconnect if needed
  int ipstat = gsm_get_connection_status();
  if (ipstat == 1) {
    // already connected, reuse connection
    ret_tmp = 1;
  } else {
    // not connected, close stale state and reconnect
    if (ipstat > 1) {
      gsm_port.print(AT_CLOSE);
      gsm_wait_for_reply(MODEM_CMDSET,0);
    } else if (ipstat < 0) {
      gsm_deactivate();
      gsm_set_apn();
    }
    ret_tmp = gsm_connect();
  }
#else
  //make sure there is no connection
  gsm_disconnect();

  //opening connection
  ret_tmp = gsm_connect();
#endif

  if(ret_tmp == 1) {
    //connection opened, just send data
    ret_tmp = gsm_send_udp_current();
    if (ret_tmp && read_udp_response) {
      gsm_read_udp_response();
    }
  }

#if GSM_STAY_ONLINE
  if (!ret_tmp) {
    // only disconnect on failure to allow retry from clean state
    gsm_disconnect();
  }
#else
  gsm_disconnect(); // always
#endif

  if(ret_tmp) {
    gsm_send_failures = 0;

    //addon_event(ON_SEND_COMPLETED);
  } else {
    DEBUG_PRINT("Error, can not send data or no connection.");

    gsm_send_failures++;
    //addon_event(ON_SEND_FAILED);
  }

  return ret_tmp;
}

// -- Alert send functions (called from alert.ino) ----------------------------

int alert_send_udp() {
  extern char alert_queue[][ALERT_MSG_SIZE];
  extern int8_t alert_priority[];
  extern int alert_count;

  // Build the plaintext (joined "A,priority,msg\n" lines) into the envelope's
  // ciphertext slot, then encrypt in place. udp_envelope is owned by
  // gsm_send_udp_current; alerts share the buffer because send paths don't
  // overlap (we're not in a re-entrant context).
  int imei_len = strlen(config.imei);
  if (imei_len <= 0 || imei_len > 20) return 0;
  int header_len = 1 + imei_len + CP_NONCE_BYTES;
  uint8_t *pt_dst = udp_envelope + header_len;
  int pt_max = UDP_PACKET_SIZE - header_len - CP_TAG_BYTES;

  int pt_len = 0;
  for (int i = 0; i < alert_count; i++) {
    int n = snprintf((char*)pt_dst + pt_len,
                     (pt_len < pt_max) ? (pt_max - pt_len) : 0,
                     "A,%d,%s\n", (int)alert_priority[i], alert_queue[i]);
    if (n <= 0 || pt_len + n > pt_max) {
      debug_print(F("alert UDP: payload too large"));
      return 0;
    }
    pt_len += n;
  }

  udp_envelope[0] = (uint8_t)imei_len;
  memcpy(udp_envelope + 1, config.imei, imei_len);
  uint8_t *nonce = udp_envelope + 1 + imei_len;
  if (!crypto_random(nonce, CP_NONCE_BYTES)) {
    debug_print(F("alert UDP: rng failure"));
    return 0;
  }
  uint8_t *ct  = pt_dst;          // encrypt in place
  uint8_t *tag = ct + pt_len;
  cp_seal(config.psk, nonce,
          udp_envelope + 1, (size_t)imei_len,
          pt_dst, (size_t)pt_len,
          ct, tag);

  int dgram_len = header_len + pt_len + CP_TAG_BYTES;
  if (!gsm_send_begin(dgram_len)) {
    debug_print(F("alert UDP: send refused"));
    return 0;
  }
  gsm_port.write(udp_envelope, dgram_len);
  if (!gsm_send_done()) {
    debug_print(F("alert UDP: send error"));
    return 0;
  }

  debug_print(F("alert UDP: sent"));
  alert_count = 0;

  return 1;
}

// update and return index to modem_reply buffer
int gsm_read_line(int index = 0) {
  char inChar = 0; // Where to store the character read
  long last = millis();

  do {
    if(gsm_port.available()) {
      inChar = gsm_port.read(); // always read if available
      last = millis();
      if(index < (int)sizeof(modem_reply)-1) { // One less than the size of the array
        modem_reply[index] = inChar; // Store it
        index++; // Increment where to write next

        if(index == sizeof(modem_reply)-1 || (inChar == '\n')) { //some data still available, keep it in serial buffer
          break;
        }
      }
    }
  } while(gsm_port.available() || (signed long)(millis() - last) < 10); // allow some inter-character delay

  if (index > 0 && (inChar == '\r') && index < (int)sizeof(modem_reply)-1) {
    modem_reply[index] = '\n'; // sometimes newline is missing, fix it
    ++index;
  }
  modem_reply[index] = '\0'; // Null terminate the string
  return index;
}

// use fullBuffer != 0 if you want to read multiple lines
void gsm_get_reply(int fullBuffer) {
  //get reply from the modem
  int index = 0, end = 0;
  if (!fullBuffer) memset(modem_reply, 0, sizeof(modem_reply));

  do {
    end = gsm_read_line(index);
    if (end > index)
      index = end;
    else
      break;
  } while(fullBuffer && index < (int)sizeof(modem_reply)-1);

  if(index > 0) {
    DEBUG_PRINT("Modem Reply:");
    DEBUG_PRINTLN(modem_reply);

    //addon_event(ON_MODEM_REPLY);

#if MODEM_BG96
    if (gsm_parse_reg_urc(modem_reply)) {
      cell_info_stale = 1;
      cell_fields_dirty = 1;
    }
#endif
  }
}

// use allowOK = 0 if OK comes before the end of the modem reply
void gsm_wait_for_reply(int allowOK, int fullBuffer) {
  gsm_wait_for_reply(allowOK, fullBuffer, GSM_MODEM_COMMAND_TIMEOUT);
}

void gsm_wait_for_reply(int allowOK, int fullBuffer, int maxseconds) {
  unsigned long timeout = millis();

  memset(modem_reply, 0, sizeof(modem_reply));
  int ret = 0;

  //get reply from the modem
  int index = 0, end = 0;

  do {
    if (fullBuffer) //keep past lines
      index = end;
    else // overwrite
      index = 0;
    end = gsm_read_line(index);

    if(end > index) {
      DEBUG_PRINT("Modem Line:");
      DEBUG_PRINTLN(&modem_reply[index]);

      //addon_event(ON_MODEM_REPLY);

#if MODEM_BG96
      // Scan each line for URCs before potentially overwriting it on the
      // next iteration (fullBuffer=0 mode).  URCs can arrive async in the
      // middle of any command reply.
      if (gsm_parse_reg_urc(&modem_reply[index])) {
        cell_info_stale = 1;
        cell_fields_dirty = 1;
      }
#endif

      if (gsm_is_final_result(&modem_reply[index], allowOK)) {
        ret = 1;
        break;
      }
    } else if ((signed long)(millis() - timeout) > (maxseconds * 1000)) {
      break;
    } else {
      status_delay(50);
    }
  } while(index < (int)sizeof(modem_reply)-1);

  if (ret == 0) {
    DEBUG_PRINT("Warning: timed out waiting for last modem reply");
  }

  if(index > 0) {
    DEBUG_PRINT("Modem Reply:");
    DEBUG_PRINTLN(modem_reply);
  }

  // check that modem is actually alive and sending replies to commands
  if (modem_reply[0] == 0) {
    DEBUG_PRINT("Reply failure count:");
    gsm_reply_failures++;
    DEBUG_PRINTLN(gsm_reply_failures);
  } else {
    gsm_reply_failures = 0;
  }
  if (GSM_REPLY_FAILURES_REBOOT > 0 && gsm_reply_failures >= GSM_REPLY_FAILURES_REBOOT) {
    // modem is unresponsive — power-cycle it
    modem_power_cycle();
    gsm_reply_failures = 0;
  }
}

int gsm_is_final_result(const char* reply, int allowOK) {
  unsigned int reply_len = strlen(reply);
  // DEBUG_PRINTLN(allowOK);
  // DEBUG_PRINTLN(reply_len);

  #define STARTS_WITH(b) ( reply_len >= strlen(b) && strncmp(reply, (b), strlen(b)) == 0)
  #define ENDS_WITH(b) ( reply_len >= strlen(b) && strcmp(reply + reply_len - strlen(b), (b)) == 0)
  #define CONTAINS(b) ( reply_len >= strlen(b) && strstr(reply, (b)) != NULL)

  if(allowOK && ENDS_WITH("\r\nOK\r\n")) {
    return true;
  }
  if(allowOK && STARTS_WITH("OK\r\n")) {
    return true;
  }
  if(STARTS_WITH("+CME ERROR:")) {
    return true;
  }
  if(STARTS_WITH("+CMS ERROR:")) {
    return true;
  }
  // +QIRD: is NOT a final result — the data body follows on the next line(s)
  // before the actual OK terminator
  if(STARTS_WITH("+QISTATE: ")) {
    return true;
  }
  if(STARTS_WITH("> ")) {
    return true;
  }
  if(STARTS_WITH("ALREADY CONNECT\r\n")) {
    return true;
  }
  if(STARTS_WITH("BUSY\r\n")) {
    return true;
  }
  if(STARTS_WITH("CONNECT\r\n")) {
    return true;
  }
  if(ENDS_WITH("CONNECT OK\r\n")) {
    return true;
  }
  if(ENDS_WITH("CONNECT FAIL\r\n")) {
    return true;
  }
  if(STARTS_WITH("CLOSED\r\n")) {
    return true;
  }
  if(ENDS_WITH("CLOSE OK\r\n")) {
    return true;
  }
  if(STARTS_WITH("DEACT OK\r\n")) {
    return true;
  }
  if(STARTS_WITH("ERROR")) {
    return true;
  }
  if(STARTS_WITH("NO ANSWER\r\n")) {
    return true;
  }
  if(STARTS_WITH("NO CARRIER\r\n")) {
    return true;
  }
  if(STARTS_WITH("NO DIALTONE\r\n")) {
    return true;
  }
  if(STARTS_WITH("SEND OK\r\n")) {
    return true;
  }
  if(STARTS_WITH("SEND FAIL\r\n")) {
    return true;
  }
  if(STARTS_WITH("STATE: ")) {
    return true;
  }
  // URCs that are NOT final results — keep waiting
  if(STARTS_WITH("RING\r\n")) {
    return false;
  }
  if(STARTS_WITH("+CMTI:")) {
    return false;
  }
  return false;
}

void gsm_reset_failure_count() {
  gsm_send_failures = 0;
}

// Progressive escalation recovery. Called by STATE_SEND after a failed
// send. The cost of recovery scales with how many consecutive failures we
// have observed:
//   1-2 failures — do nothing. gsm_disconnect() was already called after
//                  the failed send, so the next attempt naturally reopens
//                  the socket. Transient cellular failures shouldn't cost
//                  more than this.
//   3-4 failures — tear down the PDP context. Next gsm_connect() will
//                  reactivate it via the ipstat<0 path. Fixes stuck
//                  connection state without killing network registration.
//   5+ failures  — full modem power cycle. Last resort.
// The MCU reboot at N failures has been removed entirely — it provides
// nothing over the modem power cycle and burns recovery time.
void gsm_send_recovery() {
  DEBUG_FUNCTION_CALL();

  if (gsm_send_failures < GSM_RECOVERY_SOFT_THRESHOLD) {
    debug_print(F("Send recovery: soft (retry only)"));
    return;
  }

  if (gsm_send_failures < GSM_RECOVERY_HARD_THRESHOLD) {
    debug_print(F("Send recovery: deactivating PDP context"));
    gsm_deactivate();
    return;
  }

  debug_print(F("Send recovery: power-cycling modem"));
  modem_power_cycle();
}

// Placeholder — RAT upgrade not currently attempted due to modem instability
// with COPS deregister/re-register cycles. The modem stays on whatever RAT
// it initially registered on (GSM-first scan sequence).
#if NETWORK_MODE == 2 && defined(MODEM_BG96)
void gsm_retry_preferred_rat() {
}
#endif

void gsm_debug() {
  gsm_port.print("AT+QLOCKF=?\r");
  status_delay(2000);
  gsm_get_reply(0);

  gsm_port.print("AT+QBAND?\r");
  status_delay(2000);
  gsm_get_reply(0);

  gsm_port.print("AT+CGMR\r");
  status_delay(2000);
  gsm_get_reply(0);

  gsm_port.print("AT+CGMM\r");
  status_delay(2000);
  gsm_get_reply(0);

  gsm_port.print("AT+CGSN\r");
  status_delay(2000);
  gsm_get_reply(0);

  gsm_port.print("AT+CREG?\r");

  status_delay(2000);
  gsm_get_reply(0);

  gsm_port.print("AT+CSQ\r");

  status_delay(2000);
  gsm_get_reply(0);

  gsm_port.print("AT+QENG?\r");

  status_delay(2000);
  gsm_get_reply(0);

  gsm_port.print("AT+COPS?\r");

  status_delay(2000);
  gsm_get_reply(0);

  gsm_port.print("AT+COPS=?\r");

  status_delay(6000);
  gsm_get_reply(0);
}
