
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

void settings_validate() {
    int tmp;

    tmp = config.apn[0];
    if(tmp == 255) {
        debug_print(F("settings_validate(): APN not set, setting default"));
        strlcpy(config.apn, DEFAULT_APN, 20);
    }

    tmp = config.user[0];
    if(tmp == 255) {
        debug_print(F("settings_validate(): APN user not set, setting default"));
        strlcpy(config.user, DEFAULT_USER, 20);
    }

    tmp = config.pwd[0];
    if(tmp == 255) {
        debug_print(F("settings_validate(): APN password not set, setting default"));
        strlcpy(config.pwd, DEFAULT_PASS, 20);
    }

    if(config.loop_interval != 0 && config.loop_interval < 10) {
        debug_print(F("settings_validate(): loop_interval invalid, setting default"));
        config.loop_interval = ENGINE_OFF_LOOP_INTERVAL;
    }

    if(config.always_on != 0 && config.always_on != 1) {
        debug_print(F("settings_validate(): always_on invalid, setting default"));
        config.always_on = 0;
    }

    if(config.movement_alarm != 0 && config.movement_alarm != 1) {
        debug_print(F("settings_validate(): movement_alarm invalid, setting default"));
        config.movement_alarm = DEFAULT_MOVEMENT_ALARM;
    }

}

// Returns 1 if loaded from SD, 0 if using defaults
int settings_load() {
    debug_print(F("settings_load() started"));

    // start with compile-time defaults
    settings_defaults();

    int from_sd = 0;

#if SD_ENABLED
    // try to load from SD card
    if (settings_load_sd()) {
        debug_print(F("settings_load(): loaded from SD"));
        settings_validate();
        from_sd = 1;
    } else {
        debug_print(F("settings_load(): no SD config, using defaults"));
    }
#endif

    // always use compile-time APN/credentials (not saved to SD)
    strlcpy(config.apn, DEFAULT_APN, 64);
    strlcpy(config.user, DEFAULT_USER, 20);
    strlcpy(config.pwd, DEFAULT_PASS, 20);

    settings_print();
    debug_print(F("settings_load() finished"));
    return from_sd;
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

void settings_save() {
#if SD_ENABLED
    debug_print(F("settings_save() started"));
    settings_save_sd();
    debug_print(F("settings_save() finished"));
#endif
}
