// Command execution — processes commands received from server via UDP response.
// Command format: "int=3600,movealarm=0" (comma-separated key=value pairs)

void cmd_run(char *cmd) {
    char *tmp;
    long val;

    debug_print(F("cmd_run() started"));

    //checking what command to execute

    // engine off loop interval
    tmp = strstr(cmd, "int=");
    if (tmp!=NULL) {
        tmp += strlen("int=");
        val = atol(tmp);
        if (val != 0 && val < 10) val = 10;
        int old_interval = config.loop_interval;
        config.loop_interval = val;
        save_config = 1;
        send_int_to_server = 1;
        char msg[60];
        snprintf(msg, sizeof(msg), "engine-off interval changed; %d -> %ld", old_interval, val);
        alert_enqueue(msg, 0);
    }

#if ALWAYS_ON_POWER
    tmp = strstr(cmd, "movealarm=");
    if (tmp!=NULL) {
        tmp += strlen("movealarm=");
        val = atoi(tmp);
        config.movement_alarm = (val != 0) ? 1 : 0;
        save_config = 1;
        send_int_to_server = 1;
        if (config.movement_alarm) {
          alert_enqueue("movement alarm ON", 0);
        } else {
          alert_enqueue("movement alarm OFF", 0);
        }
    }
#endif

#if RELAY_CONNECTED
    tmp = strstr(cmd, "alwayson=");
    if (tmp!=NULL) {
        tmp += strlen("alwayson=");
        val = atoi(tmp);

        if (config.always_on != val) {
          set_power_state = 1;
        }

        config.always_on = val;
        save_config = 1;

        if (config.always_on == 1) {
          alert_enqueue("enabling always-on power", 0);
        } else {
          alert_enqueue("enabling ignition-only power", 0);
        }

        send_int_to_server = 1;
    }
#endif

#if ALWAYS_ON_POWER
    tmp = strstr(cmd, "movereset");
    if (tmp!=NULL) {
        movement_alert_level = 0;
        movement_cooldown_secs = 0;
        movement_idle_secs = 0;
        if (saved_loop_interval >= 0) {
          config.loop_interval = saved_loop_interval;
          saved_loop_interval = -1;
          save_config = 1;
        }
        char msg[60];
        snprintf(msg, sizeof(msg), "movement reset; int=%d", config.loop_interval);
        alert_enqueue(msg, 0);
    }
#endif

    // locatenow must be checked before locate
    tmp = strstr(cmd, "locatenow");
    if(tmp!=NULL) {
      debug_print(F("cmd_run(): Locate now command detected"));
      collect_data(ignition);
      send_data();
      char msg[60];
      snprintf(msg,sizeof(msg),"google: %s,%s",lat_current,lon_current);
      alert_enqueue(msg, 0);
    } else {
      tmp = strstr(cmd, "locate");
      if(tmp!=NULL) {
        debug_print(F("cmd_run(): Locate command detected"));
        char msg[60];
        snprintf(msg,sizeof(msg),"google: %s,%s",lat_current,lon_current);
        alert_enqueue(msg, 0);
      }
    }

    // tomtomnow must be checked before tomtom
    tmp = strstr(cmd, "tomtomnow");
    if(tmp!=NULL) {
      debug_print(F("cmd_run(): TomTom now command detected"));
      collect_data(ignition);
      send_data();
      char msg[120];
      snprintf(msg,sizeof(msg),"tomtom: %s,%s",lat_current,lon_current);
      alert_enqueue(msg, 0);
    } else {
      tmp = strstr(cmd, "tomtom");
      if(tmp!=NULL) {
        debug_print(F("cmd_run(): TomTom command detected"));
        char msg[120];
        snprintf(msg,sizeof(msg),"tomtom: %s,%s",lat_current,lon_current);
        alert_enqueue(msg, 0);
      }
    }

    tmp = strstr(cmd, "config");
    if(tmp!=NULL) {
        char msg[160];
        char vbuf[8];
        dtostrf(battery_v, 1, 1, vbuf);
        snprintf(msg, sizeof(msg),
            "int=%d"
#if RELAY_CONNECTED
            " ao=%d"
#endif
#if ALWAYS_ON_POWER
            " ma=%d"
#endif
            " bat=%sV ign=%s up=%lus",
            config.loop_interval,
#if RELAY_CONNECTED
            (int)config.always_on,
#endif
#if ALWAYS_ON_POWER
            (int)config.movement_alarm,
#endif
            vbuf,
            (ignition == 0) ? "on" : "off",
            millis() / 1000);
        alert_enqueue(msg, 0);
    }

    tmp = strstr(cmd, "reboot");
    if(tmp!=NULL) {
        debug_print(F("cmd_run(): reboot command detected"));
        power_reboot = 1;
        alert_enqueue("rebooting", 0);
    }

#if RELAY_CONNECTED
  tmp = strstr(cmd, "poweroff");
  if(tmp!=NULL) {
      debug_print(F("cmd_run(): power off command detected"));
      power_off_relay = 1;
      alert_enqueue("powering off (relay to ignition-only)", 0);
  }
#endif

    debug_print(F("cmd_run() completed"));
}
