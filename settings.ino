
void settings_defaults() {
    debug_print(F("settings_defaults(): setting compile-time defaults"));

    config.always_on = 0;

    strlcpy(config.apn, DEFAULT_APN, 64);
    strlcpy(config.user, DEFAULT_USER, 20);
    strlcpy(config.pwd, DEFAULT_PASS, 20);

    config.loop_interval = ENGINE_OFF_LOOP_INTERVAL;
    config.movement_alarm = DEFAULT_MOVEMENT_ALARM;

    if (!crypto_psk_from_hex(PSK_HEX, config.psk)) {
        debug_print(F("settings_defaults(): PSK_HEX malformed — refusing to send"));
        memset(config.psk, 0, sizeof(config.psk));
    }
}

void settings_load() {
    debug_print(F("settings_load() started"));

    settings_defaults();

    settings_print();
    debug_print(F("settings_load() finished"));
}

void settings_print() {
    char buf[80];
    snprintf(buf, sizeof(buf), "  apn=%s user=%s", config.apn, config.user);
    debug_print(buf);
    snprintf(buf, sizeof(buf), "  int=%d"
#if RELAY_CONNECTED
        " ao=%d"
#endif
#if ALWAYS_ON_POWER
        " ma=%d"
#endif
        ,
        config.loop_interval
#if RELAY_CONNECTED
        , (int)config.always_on
#endif
#if ALWAYS_ON_POWER
        , (int)config.movement_alarm
#endif
        );
    debug_print(buf);
    snprintf(buf, sizeof(buf), "  imei=%s", config.imei);
    debug_print(buf);
}

