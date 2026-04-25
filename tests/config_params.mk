# TrackerHub firmware configuration parameters
# This file is the source of truth for compile-time config.
# The Makefile generates config.h from these values.

# -- Debug -------------------------------------------------------------------
# Uncomment to enable serial debug output
# DEBUG = 1

# -- Protocol ----------------------------------------------------------------
NETWORK_MODE         = 0
RAT_CHECK_INTERVAL   = 300
LOW_POWER_STANDBY    = 1
ALWAYS_ON_POWER      = 1
RELAY_CONNECTED      = 1
GSM_STAY_ONLINE      = 1
HIGH_FREQUENCY_TELEMETRY = 1

# -- Network -----------------------------------------------------------------
HOSTNAME             = tracker.rkw.io
UDP_PORT             = 65480
UDP_PACKET_SIZE      = 1200
CONNECT_RETRY        = 3

# -- Credentials (override for production) -----------------------------------
PSK_HEX              = 0000000000000000000000000000000000000000000000000000000000000000
DEFAULT_APN          = test.apn
DEFAULT_USER         = testuser
DEFAULT_PASS         =

# -- Buffers -----------------------------------------------------------------
DATA_LIMIT           = 2500
MODEM_REPLY_SIZE     = 256

# -- Timeouts ----------------------------------------------------------------
GSM_MODEM_COMMAND_TIMEOUT = 10
GSM_AT_TIMEOUT       = 3000
GSM_CONNECT_TIMEOUT  = 15000
GSM_RECEIVE_TIMEOUT  = 10000
GPS_FIX_TIMEOUT      = 60000

# -- Error recovery ----------------------------------------------------------
GSM_SEND_FAILURES_REBOOT  = 5
GSM_REPLY_FAILURES_REBOOT = 10

# -- Telemetry ---------------------------------------------------------------
ENGINE_OFF_LOOP_INTERVAL  = 3600
BATCH_HEADROOM            = 400

# -- Battery -----------------------------------------------------------------
BATTERY_WARNING_LEVEL     = 11.9f
BATTERY_POWEROFF_LEVEL    = 11.8f
SLEEP_SAFETY_VOLTAGE      = 12.0f
ENGINE_RUNNING_VOLTAGE    = 13.0f
IGNITION_ON_SLEEP_INTERVAL = 300
VOLTAGE_POLL_INTERVAL     = 30

# -- Accelerometer / movement -----------------------------------------------
DEFAULT_MOVEMENT_ALARM    = 1
ACC_MOVEMENT_THRESHOLD    = 150
ACC_WAKE_THRESHOLD        = 10
MOVEMENT_CONFIRM_MS       = 3000
MOVEMENT_CONFIRM_HITS     = 3
MOVEMENT_INACTIVITY_RESET   = 1800
NO_MOVEMENT_GPS_SKIP        = 86400
