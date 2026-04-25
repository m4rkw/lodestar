
//tracker config
#include "config.h"

#define debug_port SerialUSB
#include "debug.h"

//External libraries
#include <SPI.h>
#include <TinyGPS.h>

#ifdef DEBUG
#define debug_print(x)    debug_port.println(x)
#else
#define debug_print(x)
#endif

// Variables will change:
char data_current[DATA_LIMIT];     //data collected in one go, max 2500 chars
int data_index = 0;                //current data index (where last data record stopped)
char time_char[32];                         //time attached to every data line
char modem_reply[MODEM_REPLY_SIZE];         //data received from modem
byte power_reboot = 0;                     //flag to reboot everything (used after new settings have been saved)

unsigned long last_time_gps, last_date_gps;

TinyGPS gps;

int gsm_send_failures = 0;

char lat_current[32];
char lon_current[32];
float gps_speed, gps_altitude, gps_heading;
long gps_hdop, gps_sats;

byte battery_warning_status = 0;
//settings structure
struct settings {
  char apn[64];
  char user[20];
  char pwd[20];
  char sim_pin[5];                //PIN for SIM card
  char imei[20];                    //IMEI number
  int loop_interval;
  int8_t always_on;
  int8_t movement_alarm;
  uint8_t psk[32];                  //ChaCha20-Poly1305 pre-shared key
};

settings config;

//define serial ports
#define gps_port Serial1
#define gsm_port Serial2

byte send_int_to_server = 0;

char ignition;
int8_t previous_ignition = -1;

byte last_send_ok = 0;

byte set_power_state = 0;
byte power_off_relay = 0;
byte powered_on = 1;

byte movement_alert_level = 0;
long movement_cooldown_secs = 0;
long movement_idle_secs = 0;
const int movement_cooldowns[] = {300, 900, 1800, 3600};  // 5, 15, 30, 60 min

int saved_loop_interval = -1;

byte movement_wake_pending = 0;
byte use_cached_gps = 0;

// Coast-to-stop: after ignition off while moving, keep sending until stopped
byte post_ignition_off_coasting = 0;
byte coast_iterations = 0;
#define COAST_STOP_SPEED_KMH 1.0f
#define COAST_MAX_ITERATIONS 60


char pending_server_cmd[128] = "";

float battery_v;
byte engine_running = 0;
int below_voltage_count = 0;
#define ENGINE_STOPPED_COUNT 10

unsigned long wake_start_millis = 0;

// Accumulates time spent in STOP2 (millis() doesn't advance while the CPU is
// halted). Added to millis()/1000 when reporting uptime so the server sees
// real wall-clock time since the last cold boot. Resets to 0 on reset.
unsigned long total_sleep_seconds = 0;

// Cell tower cache. Updated by +CREG/+CEREG URCs and AT+QENG polls.
// Included in every telemetry packet so server can diagnose roaming / coverage
// issues, and used as a GPS fallback location when no fix is available.
int           cell_mcc = 0, cell_mnc = 0;
unsigned long cell_lac = 0, cell_cid = 0;
char          cell_rat[8] = "";   // canonical RAT code: CATM1 / NBIOT / GSM / WCDMA / LTE
byte          cell_location = 0;  // set per-packet by data.ino

// Cell fields are only emitted when this flag is set — saves bandwidth on
// the 1Hz telemetry stream. Set on wake, on any URC-driven cell change,
// and on boot; cleared after a successful send.
byte          cell_fields_dirty = 1;

// Resolve HOSTNAME once per session via AT+QIDNSGIP and hand the IP to
// QIOPEN instead of the hostname. Avoids BG96 "system busy" (568) when
// QIOPEN races with in-flight DNS after a socket teardown. Empty means
// unresolved — cleared on wake and on connect failure.
char          cached_server_ip[16] = "";

#if NETWORK_MODE == 2 && defined(MODEM_BG96)
byte rat_retry_done = 0;
#endif

byte network_ready = 0;

// Network search retry interval for ignition-on / engine-not-running (seconds)
#define NETWORK_RETRY_INTERVAL 300

// Max seconds addressable by the RTC wakeup timer in CK_SPRE_16BITS mode
// (1Hz clock, 16-bit counter). Callers that want to sleep longer must loop.
#define RTC_WAKEUP_MAX_SECONDS 65535

void setup() {
  wake_start_millis = millis();
  device_init();
  gsm_init();
  gps_init();

#ifdef DEBUG
  debug_port.begin(9600);
  delay(2000);
#endif
  led_off();

  crypto_init();
  settings_load();

  acc_init();
#if ALWAYS_ON_POWER
  rtc_wakeup_init();
#endif
  gps_setup();
  gsm_setup();  // configures modem and starts network search (async)

  data_reset();

  // acquire GPS fix while modem registers in the background
  debug_print(F("GPS fix + network registration in parallel"));
  {
    unsigned long start = millis();
    unsigned long last_net_check = 0;
    int gps_fix = 0;
    while ((unsigned long)(millis() - start) < GPS_FIX_TIMEOUT) {
      // check network status every 5s (AT commands block NMEA reception)
      if (!network_ready && (unsigned long)(millis() - last_net_check) >= 5000) {
        last_net_check = millis();
        int reg = gsm_get_network_status();
        if (reg == 1 || reg == 5) {
          network_ready = 1;
          gsm_post_register();
          gsm_set_apn();
        }
      }

      // read NMEA data from GPS
      while (gps_port.available()) {
        char c = gps_port.read();
        if (gps.encode(c)) {
          unsigned long date_tmp, time_tmp, fix_age;
          gps.get_datetime(&date_tmp, &time_tmp, &fix_age);
          if (date_tmp != 0 && fix_age < 2000) {
            gps_fix = 1;
            debug_print(F("GPS fix acquired during startup"));
            break;
          }
        }
      }
      if (gps_fix) break;
    }
    if (!gps_fix) debug_print(F("No GPS fix during startup"));
  }

  // if network wasn't ready during GPS loop, wait a bit longer
  if (!network_ready) {
    unsigned long net_start = millis();
    while (!network_ready && (unsigned long)(millis() - net_start) < (unsigned long)NETWORK_REGISTRATION_TIMEOUT * 1000) {
      int reg = gsm_get_network_status();
      if (reg == 1 || reg == 5) {
        network_ready = 1;
        gsm_post_register();
        gsm_set_apn();
      } else {
        status_delay(3000);
      }
    }
    if (!network_ready) {
      debug_print(F("No network at startup, will retry"));
    }
  }


#if RELAY_CONNECTED
  // ensure relay matches config (e.g. after battery poweroff reset it to ignition-only)
  set_power_state = 1;
#endif
  // wake_start_millis stays at the value set at the top of setup() —
  // waketime in telemetry reflects total time since power-on, including
  // GPS acquisition and network registration.

  // Enable hardware watchdog only after startup's long operations (GPS fix,
  // network registration) have completed.
  watchdog_init();
}

// State machine globals
MainState main_state = STATE_IDLE;
unsigned long last_send_time = 0;
int buffered_records = 0;

void handle_ignition_state() {
  ignition = digitalRead(PIN_S_DETECT);

  if (ignition == 0) {
    // ignition ON (inverted logic) — alarm handled server-side
    post_ignition_off_coasting = 0;
  }
}

void handle_post_send() {
#if RELAY_CONNECTED
  // handle power state changes
  if (set_power_state == 1 && last_send_ok == 1) {
    set_power_state = 0;
    set_power_supply_mode();
  }
  if (power_off_relay == 1 && last_send_ok == 1) {
    power_off_relay = 0;
    set_power_supply_off();
  }
#endif

}

#if ALWAYS_ON_POWER
// Decide whether engine-off telemetry needs a fresh GPS fix.
// Returns 1 = acquire GPS, 0 = use cached coordinates.
// Clears *needs_gps after NO_MOVEMENT_GPS_SKIP seconds of inactivity.
byte sleep_needs_gps(byte *needs_gps, long idle_secs) {
  if (*needs_gps && idle_secs >= NO_MOVEMENT_GPS_SKIP)
    *needs_gps = 0;
  if (*needs_gps || lat_current[0] == '\0')
    return 1;
  return 0;
}
#endif

byte should_send_data() {
  unsigned long time_elapsed = (unsigned long)(millis() - last_send_time);

  // always send on first boot
  if (previous_ignition == -1) return 1;

  // coasting to stop after ignition off
  if (post_ignition_off_coasting) return 1;

  // woke from deep sleep due to movement
  if (movement_wake_pending) return 1;

  // ignition just turned on
  if (ignition == 0 && previous_ignition != 0) return 1;

  // ignition just turned off
  if (ignition != 0 && previous_ignition == 0) return 1;

  // ignition is on and engine running (alternator charging)
  if (ignition == 0 && engine_running) return 1;

  // ignition on, engine not running — send at shorter interval
  if (ignition == 0 && !engine_running
      && time_elapsed >= (unsigned long)IGNITION_ON_SLEEP_INTERVAL * 1000) return 1;

  // need to sync settings to server
  if (send_int_to_server) return 1;

  // last send failed, retry — but only while ignition is on. When ignition
  // is off we must allow STATE_IDLE to fall through to STATE_SLEEP so the
  // wake path can re-init modem/GPS state cleanly; otherwise a persistent
  // failure traps the device in a GPS_COLLECT→IDLE loop forever.
  if (last_send_ok == 0 && ignition == 0) return 1;

  // engine-off telemetry interval elapsed (loop wakes every 60s, but only send at this interval)
  if (config.loop_interval > 0 && time_elapsed >= ((unsigned long)config.loop_interval * 1000)) return 1;

  return 0;
}

void loop() {
  watchdog_kick();

  if (power_reboot == 1) {
    reboot();
    power_reboot = 0;
  }

  switch (main_state) {
    case STATE_IDLE:
      handle_ignition_state();

      // wait for network before attempting to send
      if (!network_ready) {
        int reg = gsm_get_network_status();
        if (reg == 1 || reg == 5) {
          // just registered
          network_ready = 1;
          gsm_post_register();
          gsm_set_apn();
        } else if (ignition == 1) {
          // ignition off — go to sleep, normal engine-off behavior
#if ALWAYS_ON_POWER
          main_state = STATE_SLEEP;
#endif
          break;
        } else {
          read_battery_voltage();
          if (engine_running) {
            // engine running — keep modem searching, poll periodically
            gsm_print_signal_info(1);
            status_delay(5000);
            break;
          }
#if ALWAYS_ON_POWER
          // ignition on, engine not running — if data pending, collect with
          // parallel network polling; otherwise STOP2 for 5min then retry
          if (should_send_data()) {
            main_state = STATE_GPS_COLLECT;
            break;
          }
          debug_print(F("No network, sleeping 5min"));
          enter_low_power_standby();
          enter_low_power_standby_minimal();
          rtc_wakeup_set(NETWORK_RETRY_INTERVAL);
          enter_stop2_with_wake(0, 1);  // wake on RTC or ignition-off
          exit_low_power_standby_minimal();
          wake_start_millis = millis();
          cell_fields_dirty = 1;  // re-send cell info on first post-wake packet
          cached_server_ip[0] = '\0';  // drop DNS cache; cell/PDP may have shifted
          if (rtc_wakeup_fired()) total_sleep_seconds += NETWORK_RETRY_INTERVAL;
          rtc_wakeup_disable();
          delay(50);
          if (digitalRead(PIN_S_DETECT) != 0) {
            // ignition turned off during sleep
            main_state = STATE_SLEEP;
            break;
          }
          // still ignition on — restore modem, will check network next iteration
          exit_low_power_standby_modem();
#else
          // no always-on power — poll periodically
          status_delay(5000);
#endif
          break;
        }
      }

      // poll battery voltage periodically so engine start is detected quickly
      {
        static unsigned long last_voltage_poll = 0;
        if ((unsigned long)(millis() - last_voltage_poll) >= (unsigned long)VOLTAGE_POLL_INTERVAL * 1000) {
          read_battery_voltage();
          last_voltage_poll = millis();
        }
      }

      // check if we should collect and send data
      if (should_send_data()) {
        main_state = STATE_GPS_COLLECT;
        break;
      }

#if ALWAYS_ON_POWER
      // ignition on but engine not running (low voltage) — sleep with polling
      if (ignition == 0) {
        main_state = STATE_IGNITION_SLEEP;
        break;
      }

      // engine off — deep sleep
      if (ignition == 1) {
        main_state = STATE_SLEEP;
        break;
      }
#endif

      break;

    case STATE_GPS_COLLECT: {
      int gps_fix = collect_data(ignition);

      // wait for network if not ready after GPS fix
      if (!network_ready) {
        unsigned long net_start = millis();
        while (!network_ready && (unsigned long)(millis() - net_start) < (unsigned long)NETWORK_REGISTRATION_TIMEOUT * 1000) {
          int reg = gsm_get_network_status();
          if (reg == 1 || reg == 5) {
            network_ready = 1;
            gsm_post_register();
            gsm_set_apn();
          } else {
            status_delay(3000);
          }
        }
      }

      if (!gps_fix) {
        // no GPS fix — discard record, don't attempt send
        debug_print(F("No GPS fix, skipping send"));
        data_reset();
        last_send_time = millis();  // avoid immediate retry
        previous_ignition = ignition;
#if ALWAYS_ON_POWER
        if (ignition == 1) {
          // engine off — go straight to sleep instead of retrying
          main_state = STATE_SLEEP;
        } else
#endif
        {
          main_state = STATE_IDLE;
        }
        break;
      }

      buffered_records++;

      // Send immediately when: batch full, engine off, first boot, settings sync, or retry
      if (buffered_records >= BATCH_SIZE || ignition != 0
          || previous_ignition == -1 || send_int_to_server || last_send_ok == 0
          || data_index >= DATA_LIMIT - BATCH_HEADROOM
          ) {
        main_state = STATE_SEND;
      } else {
        main_state = STATE_IDLE;
      }
      break;
    }

    case STATE_SEND: {
      extern byte read_udp_response;
#if ALWAYS_ON_POWER
      // read server response on: first boot, ignition change, engine off, or stopped
      read_udp_response = (previous_ignition == -1
                        || previous_ignition != ignition
                        || ignition != 0
                        || gps_speed < 0.005f);
#else
      read_udp_response = 0;
#endif

      send_data();
      last_send_time = millis();
      buffered_records = 0;
      data_reset();

#if ALWAYS_ON_POWER
      // process any commands received from server UDP response
      if (pending_server_cmd[0] != '\0' && read_udp_response) {
        cmd_run(pending_server_cmd);
        pending_server_cmd[0] = '\0';
        extern int alert_count;
        if (alert_count > 0) alert_send();
      }
#endif

      handle_post_send();

      movement_wake_pending = 0;

      // Coast-to-stop: if engine was running and ignition just turned off
      // with speed still above threshold, keep sending until stopped
      if (previous_ignition == 0 && ignition != 0
          && gps_speed > COAST_STOP_SPEED_KMH && !post_ignition_off_coasting) {
        post_ignition_off_coasting = 1;
        coast_iterations = 0;
        debug_print(F("Coasting to stop"));
      }
      if (post_ignition_off_coasting) {
        coast_iterations++;
        if (gps_speed <= COAST_STOP_SPEED_KMH || coast_iterations >= COAST_MAX_ITERATIONS) {
          post_ignition_off_coasting = 0;
          debug_print(F("Coast complete"));
        }
      }

      if (gsm_send_failures > 0) {
        post_ignition_off_coasting = 0;
        // Progressive escalation based on failure count (see gsm_send_recovery):
        // 1-2: no-op (natural retry), 3-4: PDP deactivate, 5+: modem power cycle.
        gsm_send_recovery();
      }

#if NETWORK_MODE == 2 && defined(MODEM_BG96)
      // if we registered on a fallback RAT, retry Cat-M1 once after RAT_CHECK_INTERVAL
      if (gsm_send_failures == 0) {
        extern byte gsm_on_preferred_rat;
        if (ignition == 0 && !gsm_on_preferred_rat && !rat_retry_done
            && RAT_CHECK_INTERVAL > 0
            && (millis() - wake_start_millis) >= ((unsigned long)RAT_CHECK_INTERVAL * 1000)) {
          gsm_retry_preferred_rat();
          rat_retry_done = 1;
        }
      }
#endif

      previous_ignition = ignition;
      main_state = STATE_IDLE;
      break;
    }

#if ALWAYS_ON_POWER
    case STATE_SLEEP: {
      debug_print(F("Entering deep sleep (STOP2)"));
      if (config.loop_interval > 0) {
        char sbuf[40];
        snprintf(sbuf, sizeof(sbuf), "will wake again in %ds", config.loop_interval);
        debug_print(sbuf);
      } else {
        debug_print(F("telemetry disabled, wake on ignition/movement only"));
      }

      byte use_accel = config.movement_alarm;

      // take baseline accelerometer reading before sleep
      if (use_accel) {
        acc_read_baseline();
        acc_configure_wake_interrupt();
        for (int i = 0; i < 10 && !digitalRead(PIN_S_ACC_INT1); i++) delay(100);
      }

      // full shutdown: modem, GPS, USB, 5V rail, SPI, slow CPU
      enter_low_power_standby();

      // countdown timer (seconds remaining until next telemetry)
      // when int=0: periodic battery check only if relay connected (for auto-poweroff)
      long telemetry_remaining;
      if (config.loop_interval > 0)
        telemetry_remaining = (long)config.loop_interval;
#if RELAY_CONNECTED
      else
        telemetry_remaining = (long)BATTERY_CHECK_INTERVAL;
#else
      else
        telemetry_remaining = 0;  // no timer wake — ignition/movement only
#endif
      byte movement_needs_gps = 0;

      for (;;) {
        // set wake interval from telemetry countdown (or movement cooldown if sooner)
        long wake_seconds = telemetry_remaining;
        if (use_accel && movement_cooldown_secs > 0
            && (wake_seconds <= 0 || movement_cooldown_secs < wake_seconds))
          wake_seconds = movement_cooldown_secs;

        // RTC wakeup timer caps at ~18.2h; loop over longer intervals so the
        // post-wake countdown reflects real elapsed time, not the requested
        // duration.
        if (wake_seconds > RTC_WAKEUP_MAX_SECONDS)
          wake_seconds = RTC_WAKEUP_MAX_SECONDS;

        if (wake_seconds > 0)
          rtc_wakeup_set((uint32_t)wake_seconds);

        // enter STOP2 — CPU halts until accel/ignition/RTC interrupt
        enter_stop2_with_wake(use_accel, 0);

        // --- CPU resumes here after wake ---
        exit_low_power_standby_minimal();
        wake_start_millis = millis();
        cell_fields_dirty = 1;  // re-send cell info on first post-wake packet
        cached_server_ip[0] = '\0';  // drop DNS cache; cell/PDP may have shifted

        if (use_accel) acc_disable_interrupt();
        rtc_wakeup_disable();

        // debounce: ignition sensing needs time to settle after 5V restore,
        // sample multiple times to reject noise glitches
        delay(200);
        byte ign_on = (digitalRead(PIN_S_DETECT) == 0);
        if (ign_on) { delay(100); ign_on = (digitalRead(PIN_S_DETECT) == 0); }

        if (ign_on) {
          debug_print(F("Woke: ignition"));
          exit_low_power_standby_modem_gps();
          movement_alert_level = 0;
          movement_cooldown_secs = 0;
          movement_idle_secs = 0;
          if (saved_loop_interval >= 0) {
            config.loop_interval = saved_loop_interval;
            saved_loop_interval = -1;
          }
#if NETWORK_MODE == 2 && defined(MODEM_BG96)
          rat_retry_done = 0;  // new driving session, allow one retry
#endif
          break;  // exit sleep loop → STATE_IDLE
        }

        byte timer_wake = rtc_wakeup_fired();
        if (timer_wake) total_sleep_seconds += wake_seconds;

        if (timer_wake) {
          debug_print(F("Woke: timer"));

          // decrement telemetry countdown by elapsed time
          if (telemetry_remaining > 0) telemetry_remaining -= wake_seconds;

          // track movement cooldown and inactivity using RTC-based elapsed time
          if (movement_cooldown_secs > 0) {
            movement_cooldown_secs -= wake_seconds;
            if (movement_cooldown_secs < 0) movement_cooldown_secs = 0;
          }
          movement_idle_secs += wake_seconds;
          if (movement_idle_secs >= MOVEMENT_INACTIVITY_RESET) {
            movement_alert_level = 0;
          }

          byte do_telemetry = (telemetry_remaining <= 0 && config.loop_interval > 0);

          // check battery — skip telemetry if too low
          float v = read_battery_voltage();
          char vbuf[8];
          dtostrf(v, 1, 1, vbuf);
          debug_print(vbuf);

          if (v >= SLEEP_SAFETY_VOLTAGE) {
            byte modem_up = 0;
            if (do_telemetry) {
              if (sleep_needs_gps(&movement_needs_gps, movement_idle_secs)) {
                exit_low_power_standby_modem_gps();  // GPS + network in parallel
              } else {
                exit_low_power_standby_modem();
                use_cached_gps = 1;
              }
              modem_up = 1;
              int gps_fix = collect_data(ignition);
              if (!use_cached_gps) gps_off();
              use_cached_gps = 0;
              // wait for network if not ready after GPS fix
              if (!network_ready) {
                unsigned long net_start = millis();
                while (!network_ready && (unsigned long)(millis() - net_start) < (unsigned long)NETWORK_REGISTRATION_TIMEOUT * 1000) {
                  int reg = gsm_get_network_status();
                  if (reg == 1 || reg == 5) {
                    network_ready = 1;
                    gsm_post_register();
                    gsm_set_apn();
                  } else {
                    status_delay(3000);
                  }
                }
              }
              if (gps_fix) {
#if ALWAYS_ON_POWER
                extern byte read_udp_response;
                read_udp_response = 1;
#endif
                send_data();
              } else {
                debug_print(F("No GPS fix, skipping send"));
              }
              data_reset();
#if ALWAYS_ON_POWER
              // process any commands received from server UDP response
              byte had_sync_pending = send_int_to_server;
              if (pending_server_cmd[0] != '\0') {
                cmd_run(pending_server_cmd);
                pending_server_cmd[0] = '\0';
              }
              extern int alert_count;
              if (alert_count > 0) alert_send_standalone();
              // If a command just set send_int_to_server, push config now.
              // Only needed when the command is what set the flag — if it was
              // already set, the first send already included the config suffix.
              if (send_int_to_server && !had_sync_pending) {
                extern byte read_udp_response;
                use_cached_gps = 1;
                collect_data(ignition);
                read_udp_response = 1;
                send_data();
                data_reset();
                use_cached_gps = 0;
                pending_server_cmd[0] = '\0';
              }
              if (power_reboot) reboot();
#endif
            }

            // retry any unsent alerts (e.g. movement alert that failed due to no signal)
            {
              extern int alert_count;
              if (alert_count > 0 && !modem_up) {
                debug_print(F("Retrying unsent alerts"));
                exit_low_power_standby_modem();
                modem_up = 1;
                alert_send_standalone();
                if (pending_server_cmd[0] != '\0') {
                  cmd_run(pending_server_cmd);
                  pending_server_cmd[0] = '\0';
                }
                  if (power_reboot) reboot();
              }
            }

            if (modem_up) {
              enter_low_power_standby_modem();
            }
          } else {
            debug_print(F("Low voltage, skipping"));

#if RELAY_CONNECTED
            // check for battery-critical auto-poweroff (always-on → ignition-only)
            // Don't save settings — on next ignition boot, always_on=1 is restored from SD
            // and set_power_supply_mode() switches back to always-on.
            if (BATTERY_POWEROFF_LEVEL > 0 && v <= BATTERY_POWEROFF_LEVEL && config.always_on == 1) {
              if (confirm_battery_low(BATTERY_POWEROFF_LEVEL)) {
                debug_print(F("Battery critical, switching to ignition-only"));
                config.always_on = 0;
                set_power_supply_mode();  // kills power immediately (ignition is off)
              }
            }
#endif
          }

          if (do_telemetry) {
            telemetry_remaining = config.loop_interval;
          }
#if RELAY_CONNECTED
          else if (config.loop_interval == 0 && telemetry_remaining <= 0) {
            telemetry_remaining = BATTERY_CHECK_INTERVAL;
          }
#endif

        } else {
          // movement wake (accelerometer)
          debug_print(F("Woke: movement"));
          if (acc_confirm_movement()) {
            movement_idle_secs = 0;  // any confirmed movement resets inactivity timer
            movement_needs_gps = 1;  // get fresh GPS on next timer wake

            // override interval if disabled or very long
            if (saved_loop_interval < 0 && (config.loop_interval == 0 || config.loop_interval > MOVEMENT_TEMPORARY_ENGINE_OFF_INTERVAL)) {
              saved_loop_interval = config.loop_interval;
              config.loop_interval = MOVEMENT_TEMPORARY_ENGINE_OFF_INTERVAL;
              telemetry_remaining = MOVEMENT_TEMPORARY_ENGINE_OFF_INTERVAL;
            }

            if (movement_cooldown_secs <= 0) {
              int tilt_tenths, delta_mg;
              acc_get_movement_info(&tilt_tenths, &delta_mg);

              char tilt_str[8];
              dtostrf(tilt_tenths / 10.0f, 1, 1, tilt_str);

              exit_low_power_standby_modem();

              char msg[80];
              snprintf(msg, sizeof(msg), "movement: %sdeg tilt, %dmg force", tilt_str, delta_mg);
              alert_enqueue(msg, 2);
              alert_send_standalone();

              // escalating cooldown: 5min → 15min → 30min → 60min
              movement_cooldown_secs = movement_cooldowns[movement_alert_level];
              if (movement_alert_level < 3) movement_alert_level++;

              // process any commands from server UDP response (allows remote disable of alerts)
              if (pending_server_cmd[0] != '\0') {
                cmd_run(pending_server_cmd);
                pending_server_cmd[0] = '\0';
                extern int alert_count;
                if (alert_count > 0) alert_send_standalone();
              }

              if (power_reboot) reboot();
              enter_low_power_standby_modem();
            } else {
              // cooldown active — still retry any unsent alerts from a previous failed attempt
              extern int alert_count;
              if (alert_count > 0) {
                debug_print(F("Retrying unsent alerts (cooldown)"));
                exit_low_power_standby_modem();
                alert_send_standalone();
                if (pending_server_cmd[0] != '\0') {
                  cmd_run(pending_server_cmd);
                  pending_server_cmd[0] = '\0';
                }
                  if (power_reboot) reboot();
                enter_low_power_standby_modem();
              } else {
                debug_print(F("Cooldown active, ignoring"));
              }
            }
          } else {
            debug_print(F("Movement not confirmed, ignoring"));
          }
        }

        // re-check ignition before going back to sleep — it may have
        // turned on during a timer/movement wake (no EXTI edge to catch it)
        if (digitalRead(PIN_S_DETECT) == 0) {
          debug_print(F("Ignition on, exiting sleep"));
          exit_low_power_standby_modem_gps();
          movement_alert_level = 0;
          movement_cooldown_secs = 0;
          movement_idle_secs = 0;
          if (saved_loop_interval >= 0) {
            config.loop_interval = saved_loop_interval;
            saved_loop_interval = -1;
          }
          break;
        }

        // prepare for next sleep cycle
        if (use_accel) {
          acc_read_baseline();
          acc_configure_wake_interrupt();
          for (int i = 0; i < 10 && !digitalRead(PIN_S_ACC_INT1); i++) delay(100);
        }
        enter_low_power_standby_minimal();
      }

      previous_ignition = ignition;
      main_state = STATE_IDLE;
      break;
    }

    case STATE_IGNITION_SLEEP: {
      // Ignition on but battery below ENGINE_RUNNING_VOLTAGE — engine not running.
      // Keep modem and GPS alive (restarting them wastes more power than idling).
      // Poll voltage, send telemetry at intervals.
      debug_print(F("Ignition on, engine not running"));

      unsigned long last_telemetry = last_send_time;

      for (;;) {
        status_delay((unsigned long)VOLTAGE_POLL_INTERVAL * 1000);

        handle_ignition_state();
        if (ignition != 0) {
          debug_print(F("Ignition off"));
          break;
        }

        read_battery_voltage();
        if (engine_running) {
          debug_print(F("Voltage recovered, resuming tracking"));
          break;
        }

        // time for telemetry?
        if ((unsigned long)(millis() - last_telemetry) >= (unsigned long)IGNITION_ON_SLEEP_INTERVAL * 1000) {
          debug_print(F("Ignition sleep: telemetry"));

          int gps_fix = collect_data(ignition);
          if (gps_fix) {
#if ALWAYS_ON_POWER
            extern byte read_udp_response;
            read_udp_response = 1;
#endif
            send_data();
          } else {
            debug_print(F("No GPS fix, skipping send"));
          }
          data_reset();

#if ALWAYS_ON_POWER
          byte had_sync_pending2 = send_int_to_server;
          if (pending_server_cmd[0] != '\0') {
            cmd_run(pending_server_cmd);
            pending_server_cmd[0] = '\0';
          }
          extern int alert_count;
          if (alert_count > 0) alert_send();
          if (send_int_to_server && !had_sync_pending2) {
            extern byte read_udp_response;
            use_cached_gps = 1;
            collect_data(ignition);
            read_udp_response = 1;
            send_data();
            data_reset();
            use_cached_gps = 0;
            pending_server_cmd[0] = '\0';
          }
          if (power_reboot) reboot();
#endif

          last_telemetry = millis();
        }
      }

      // Do NOT update previous_ignition here. If we broke the loop due to
      // ignition turning off, leaving previous_ignition at 0 allows
      // STATE_IDLE's should_send_data() to detect the 0→1 transition and
      // fire one final telemetry with the fresh ignition-off state.
      // If we broke due to voltage recovery, previous_ignition is already 0.
      main_state = STATE_IDLE;
      break;
    }
#endif // ALWAYS_ON_POWER

    // STATE_ERROR_RECOVERY removed — recovery is handled inline in STATE_SEND
    // by calling gsm_send_recovery() which power-cycles the modem and resets
    // network_ready, then returning to STATE_IDLE for normal network polling.
  }
}

void device_init() {
  //setup led pin
  pinMode(PIN_POWER_LED, OUTPUT);
  digitalWrite(PIN_POWER_LED, LOW);

#ifdef PIN_C_REBOOT
  pinMode(PIN_C_REBOOT, OUTPUT);
  digitalWrite(PIN_C_REBOOT, LOW);  //this is required for HW rev 2.3 and earlier
#endif

  //setup ignition detection
  pinMode(PIN_S_DETECT, INPUT);

  // setup CAN in low power
  pinMode(PIN_CAN_RS, OUTPUT);
  digitalWrite(PIN_CAN_RS, HIGH);

  // Polaris: enable 5V rail, init SPI accessories, IOExpander for relay outputs
  pinMode(PIN_C_5V_ENABLE, OUTPUT);
  digitalWrite(PIN_C_5V_ENABLE, HIGH);

  pinMode(PIN_C_ACC_CS, OUTPUT);
  digitalWrite(PIN_C_ACC_CS, HIGH);

  // accelerometer INT1 pin (for STOP2 wake-on-motion)
  pinMode(PIN_S_ACC_INT1, INPUT);

#if RELAY_CONNECTED
  // init SPI output driver (controls AIO1/AIO2 for latching relay)
  ioexp_init();
#endif
}
