CXX      = clang++
CXXFLAGS = -std=c++14 -g -Wall -Wextra -Wno-writable-strings -Wno-unused-parameter \
           -Wno-missing-field-initializers -Wno-deprecated-declarations \
           -funsigned-char \
           -I mocks -I . -I ..
LDFLAGS  = -lm

include config_params.mk

GENERATED = generated_config.h
TESTS     = test_commands test_settings test_firmware test_data test_alert \
            test_ioexpander test_power \
            test_gsm_recovery test_cell test_dns test_crypto

.PHONY: all test clean

all: $(GENERATED) $(TESTS)

test: all
	@failures=0; \
	for t in $(TESTS); do \
		printf "\n=== $$t ===\n"; \
		./$$t || failures=$$((failures + 1)); \
	done; \
	printf "\n========================================\n"; \
	if [ $$failures -eq 0 ]; then \
		printf "All test suites passed.\n"; \
	else \
		printf "$$failures test suite(s) had failures.\n"; \
		exit 1; \
	fi

# -- config.h generation from config_params.mk ------------------------------

$(GENERATED): config_params.mk Makefile
	@echo "// Auto-generated from config_params.mk — do not edit" > $@
	@echo "" >> $@
ifdef DEBUG
	@echo "#define DEBUG $(DEBUG)" >> $@
endif
	@echo "#define NETWORK_MODE $(NETWORK_MODE)" >> $@
	@echo "#define RAT_CHECK_INTERVAL $(RAT_CHECK_INTERVAL)" >> $@
	@echo "#define LOW_POWER_STANDBY $(LOW_POWER_STANDBY)" >> $@
	@echo "#define ALWAYS_ON_POWER $(ALWAYS_ON_POWER)" >> $@
	@echo "#define RELAY_CONNECTED $(RELAY_CONNECTED)" >> $@
	@echo "#define GSM_STAY_ONLINE $(GSM_STAY_ONLINE)" >> $@
	@echo "#define HIGH_FREQUENCY_TELEMETRY $(HIGH_FREQUENCY_TELEMETRY)" >> $@
	@echo "#define SD_BUFFERING $(SD_BUFFERING)" >> $@
	@echo "" >> $@
	@echo "#define HOSTNAME \"$(HOSTNAME)\"" >> $@
	@echo "#define UDP_PORT \"$(UDP_PORT)\"" >> $@
	@echo "#define UDP_PACKET_SIZE $(UDP_PACKET_SIZE)" >> $@
	@echo "#define CONNECT_RETRY $(CONNECT_RETRY)" >> $@
	@echo "" >> $@
	@echo "#define PSK_HEX \"$(PSK_HEX)\"" >> $@
	@echo "#define DEFAULT_APN \"$(DEFAULT_APN)\"" >> $@
	@echo "#define DEFAULT_USER \"$(DEFAULT_USER)\"" >> $@
	@echo "#define DEFAULT_PASS \"$(DEFAULT_PASS)\"" >> $@
	@echo "" >> $@
	@echo "#define DATA_LIMIT $(DATA_LIMIT)" >> $@
	@echo "#define MODEM_REPLY_SIZE $(MODEM_REPLY_SIZE)" >> $@
	@echo "" >> $@
	@echo "#define GSM_MODEM_COMMAND_TIMEOUT $(GSM_MODEM_COMMAND_TIMEOUT)" >> $@
	@echo "#define GSM_AT_TIMEOUT $(GSM_AT_TIMEOUT)" >> $@
	@echo "#define GSM_CONNECT_TIMEOUT $(GSM_CONNECT_TIMEOUT)" >> $@
	@echo "#define GSM_RECEIVE_TIMEOUT $(GSM_RECEIVE_TIMEOUT)" >> $@
	@echo "#define GPS_FIX_TIMEOUT $(GPS_FIX_TIMEOUT)" >> $@
	@echo "" >> $@
	@echo "#define GSM_SEND_FAILURES_REBOOT $(GSM_SEND_FAILURES_REBOOT)" >> $@
	@echo "#define GSM_REPLY_FAILURES_REBOOT $(GSM_REPLY_FAILURES_REBOOT)" >> $@
	@echo "" >> $@
	@echo "#define ENGINE_OFF_LOOP_INTERVAL $(ENGINE_OFF_LOOP_INTERVAL)" >> $@
	@echo "#define BATCH_HEADROOM $(BATCH_HEADROOM)" >> $@
	@echo "" >> $@
	@echo "#define BATTERY_WARNING_LEVEL $(BATTERY_WARNING_LEVEL)" >> $@
	@echo "#define BATTERY_POWEROFF_LEVEL $(BATTERY_POWEROFF_LEVEL)" >> $@
	@echo "#define SLEEP_SAFETY_VOLTAGE $(SLEEP_SAFETY_VOLTAGE)" >> $@
	@echo "#define ENGINE_RUNNING_VOLTAGE $(ENGINE_RUNNING_VOLTAGE)" >> $@
	@echo "#define IGNITION_ON_SLEEP_INTERVAL $(IGNITION_ON_SLEEP_INTERVAL)" >> $@
	@echo "#define VOLTAGE_POLL_INTERVAL $(VOLTAGE_POLL_INTERVAL)" >> $@
	@echo "" >> $@
	@echo "#define DEFAULT_MOVEMENT_ALARM $(DEFAULT_MOVEMENT_ALARM)" >> $@
	@echo "#define ACC_MOVEMENT_THRESHOLD $(ACC_MOVEMENT_THRESHOLD)" >> $@
	@echo "#define ACC_WAKE_THRESHOLD $(ACC_WAKE_THRESHOLD)" >> $@
	@echo "#define MOVEMENT_CONFIRM_MS $(MOVEMENT_CONFIRM_MS)" >> $@
	@echo "#define MOVEMENT_CONFIRM_HITS $(MOVEMENT_CONFIRM_HITS)" >> $@
	@echo "#define MOVEMENT_INACTIVITY_RESET $(MOVEMENT_INACTIVITY_RESET)" >> $@
	@echo "#define NO_MOVEMENT_GPS_SKIP $(NO_MOVEMENT_GPS_SKIP)" >> $@
	@echo "" >> $@
	@echo "// -- Batch size --" >> $@
	@echo "#if SD_BUFFERING" >> $@
	@echo "  #define BATCH_SIZE 5" >> $@
	@echo "#else" >> $@
	@echo "  #define BATCH_SIZE 1" >> $@
	@echo "#endif" >> $@
	@echo "" >> $@
	@echo "// -- State machine --" >> $@
	@echo "enum MainState { STATE_IDLE, STATE_GPS_COLLECT," >> $@
	@echo "                 STATE_SEND, STATE_SLEEP, STATE_IGNITION_SLEEP };" >> $@

# -- Test binaries -----------------------------------------------------------

test_%: test_%.cpp $(GENERATED) mocks/Arduino.h mocks/SPI.h test_framework.h
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# test_crypto links the C AEAD implementation directly.
test_crypto: test_crypto.cpp ../chacha20_poly1305.c ../chacha20_poly1305.h test_framework.h
	$(CXX) $(CXXFLAGS) -x c++ ../chacha20_poly1305.c test_crypto.cpp -o $@ $(LDFLAGS)

clean:
	rm -f $(TESTS) $(GENERATED)
