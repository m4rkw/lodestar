# Lodestar

## Overview

Heavily-optimised power-efficient and featureful firmware for the Fortebit
Polaris range of tracker boards. Sadly the NB-IoT version is no longer available
but it works well with the 2G/3G versions as well.

I suspect the NB-IoT version was discontinued due to Zerynth no longer being
supported, which is understandable but still very disappointing. There's nothing
else on the market that's really comparable to the Polaris in terms of its
functionality and the NB-IoT modem lets you use multi-network SIM cards from
companies like 1nce for near total coverage.

This software is based on the original Opentracker code and is released under
the same license. It is not associated with the original authors or Fortebit in
any way.

## Features

- Sends telemetry at up to one event per second (the maximum rate the GPS device
  can be polled) when the engine is running
- Uses an ultra low power mode when the ignition is off drawing only 2.5mA
- Can robustly detect movement via the accelerometer when in ultra low power
  mode and wake the device to send an alert or telemetry
- Configurable ignition alerts with overnight alarm mode
- Configurable ignition-off wake interval
- Smart battery monitoring
- Supports using a latching relay to cut its own power off in the case of a low
  vehicle battery
- Can be commanded remotely via the web service to change config
- Supports SD card event buffering
- Multi-modem support (M95, UG96, EG91, BG96)
- Bundled with a web application that receives the telemetry and handles
  configuration and alerting
- Web interface showing a Google Maps view of the vehicle and telemetry with
  live updating and journey replay
- Hardware watchdog (IWDG) to catch hangs and automatically reboot
- If the ignition is turned on but the engine isn't started telemetry is sent at
  one event roughly every 30s (this is configurable).
- If the ignition is turned off the device immediately goes into STOP2 sleep
  (~2.5mA current draw) to conserve power
- A README written 100% by a human

## Requirements

- Polaris tracker board
- Antenna
- SIM card
- Vehicle wiring connections (see below)
- SD card for config storage
- A Linux server to run the webserver components on
- Some understanding of what you're doing

## Vehicle wiring and power considerations

At minimum the board needs 12v, ground and the ignition signal. It is
recommended to connect the 12v to permanent 12v live in the vehicle. With the
ignition off (when the device is in STOP2 mode) the power draw is only 2.5mA
which for a typical 40Ah car battery means it could run for nearly a year before
the battery would be down to 50% charge. Obviously if you have engine-off
interval telemetry configured it will periodically wake up to send its position
and that does use some power. On average in my testing it's around 30-40mA for
about a minute, however this is highly variable depending on many factors.

The software has safeguard features designed to guard against battery depletion:

- if the voltage drops below ```BATTERY_WARNING_LEVEL``` it will send a high
  priority alert
- if the voltage drops below ```SLEEP_SAFETY_VOLTAGE``` it will go straight back
  to sleep if ignition-off interval wakes are configured in order to conserve
  power

Despite this if you want to guarantee the device will never deplete the battery
even after long periods of vehicle inactivity you have a couple of options:

1. Connect the 12V input to the ignition signal. This is the simplest option and
ensures the device is only ever powered on when the ignition is on, however you
lose the ability for any ignition-off tracking including ignition-off movement
alerts. There is also usually a voltage cut on the ignition rail when the car is
started which will reset the tracker every time and cause it to take longer to
establish its GSM/GPS fixes.

2. Use a latching relay such as the Electronics Salon D-151 and use it to
programmatically switch between the car's permanent 12V live and the ignition
rail. The firmware has support for this and if this is present you can set
```RELAY_CONNECTED``` to 1 and set ```BATTERY_POWEROFF_LEVEL``` to a voltage you
consider still safe to start the car. If the voltage ever drops below this level
the device will use the relay to switch its power supply to the ignition rail
and effectively shut itself off until the ignition is next turned on. When that
happens it will restore the always_on state if it was previously set.

Note that the relay must be a true bistable latching relay, sometimes latching
relays are advertised on websites like Amazon which are actually flip-flop
latches. These are not suitable as they draw power continuously in order to stay
in their state.

## Hardware watchdog

The firmware supports using the IWDG hardware watchdog feature built into these
boards to catch hangs or peripheral stalls and automatically reset the device.
In order to use this you first need to set IWDG_STOP to 0, by default this is
set to 1 on Polaris boards which means the IWDG timer doesn't stop when the
device sleeps in STOP2 mode, meaning it would reset after 30s of sleep and
continuously reboot.

### Enter DFU mode

```
$ export PORT="/dev/cu.usbmodemPOLARIS10VNB1"

$ echo "=== Triggering DFU bootloader ==="
python3 -c "
import os, termios, fcntl, time
fd = os.open('$PORT', os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[4] = termios.B1200
attrs[5] = termios.B1200
termios.tcsetattr(fd, termios.TCSANOW, attrs)
fcntl.ioctl(fd, 0x20007479)
time.sleep(0.2)
fcntl.ioctl(fd, 0x20007478)
time.sleep(0.2)
os.close(fd)
"
```

### Check the IWDG\_STOP setting

```
/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI -c port=usb1 -ob displ | grep IWDG_STOP
     IWDG_STOP    : 0x1 (IWDG counter active in stop mode)
```

### Set IWDG\_STOP to 0

```
$ /Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI -c port=usb1 -ob IWDG_STOP=0
      -------------------------------------------------------------------
                        STM32CubeProgrammer v2.22.0
      -------------------------------------------------------------------

USB speed   : Full Speed (12MBit/s)
Manuf. ID   : STMicroelectronics
Product ID  : STM32  BOOTLOADER
SN          : 2058358B3843
DFU protocol: 1.1
Board       : --
Device ID   : 0x462
Device name : STM32L45x/L46x
NVM size  : 512 KBytes (default)
Device type : MCU
Revision ID : --
Device CPU  : Cortex-M4

UPLOADING OPTION BYTES DATA ...

  Bank          : 0x00
  Address       : 0x1fff7800
  Size          : 36 Bytes

[==================================================] 100%
```

## Installation instructions

### Deploy the webserver

1. MySQL or MariaDB, import the schema, create a device. The `psk` column in
the `device` table is a 64-character hex string (32 bytes of random key
material) used as the ChaCha20-Poly1305 pre-shared key for telemetry — it
must match the `PSK_HEX` compiled into the firmware. Generate one with
`python3 -c "import secrets; print(secrets.token_hex(32))"` or the bundled
`gen_psk.py`.
2. Deploy the web service to your server, there is an example nginx vhost and
systemd unit [here](https://github.com/m4rkw/lodestar/tree/master/deploy)
3. Copy config.yaml.example to config.yaml and edit accordingly
4. Register a passkey for the web UI using `python3 web/regtoken.py <username>
[hostname]` and open the printed URL. Generate an API bearer token for the
command API with `python3 web/gentoken.py <name>` (keep the output — the
token is only shown once).

### Deploy the firmware

1. Copy config.h.example to config.h and adjust to your needs
2. Compile and flash the firmware to the board

It is recommended to uncomment this line in debug.h:

```
#define DEBUG 1
```

and monitor the serial output initially to ensure everything is working
correctly.

## Notes on security

Telemetry and alerts are sent as ChaCha20-Poly1305 encrypted UDP envelopes,
authenticated with a 32-byte pre-shared key per device (set via `PSK_HEX` in
`config.h` and the matching `psk` column on the server). The IMEI is bound in
as additional authenticated data so envelopes can't be replayed across
devices. Generate a fresh key with `gen_psk.py` before flashing.

The web UI uses WebAuthn (passkey) authentication — users register via a
single-use link produced by `web/regtoken.py` and log in with a platform
authenticator. The command API (`/api/1.0/command`) uses bearer tokens
provisioned via `web/gentoken.py`. See `web/security.md` for the detailed
threat model this setup addresses.

## Running the tests

```
cd tests
make test
```

## Helper scripts

- [tracker](https://github.com/m4rkw/lodestar/tree/master/bin/tracker) - show tracker logs
- [journeys](https://github.com/m4rkw/lodestar/tree/master/bin/journeys) - show recent journey list
- [journeys.py](https://github.com/m4rkw/lodestar/tree/master/cron/journeys.py) - cron script to calculate journeys from the telemetry

Note: these all depend on the m4rkw-db package from Pypi for database access.

## Sending commands remotely

When processing telemetry the system can also process commands. Commands can be
sent using the example [web/command.py](https://github.com/m4rkw/lodestar/tree/master/web/command.py) script, or by sending an HTTPS request authenticated with a bearer token (generate one with `gentoken.py`):

```
curl -X POST https://<HOSTNAME>/api/1.0/command \
    -H 'Authorization: Bearer <TOKEN>' \
    -H 'Content-Type: application/json' \
    -d '{"imei":"123453325235","command":"int=3600"}'
```

Commands are comma-separated in a single string (e.g. `"int=900,movealarm=1"`) and ride as the trailing field of the encrypted UDP response envelope after each telemetry send. The firmware processes them via `cmd_run()` — including during STOP2 wake cycles.

| Command | Parameters | Condition | Description |
|---------|-----------|-----------|-------------|
| `int=` | 0 or 10–2147483647 | always | Set engine-off telemetry interval (seconds). Values 1–9 are clamped to 10. 0 disables periodic telemetry. Sends confirmation alert with old → new value. |
| `movealarm=` | 0 or 1 | `ALWAYS_ON_POWER` | Enable/disable accelerometer movement alarm. Sends "movement alarm ON/OFF" alert. |
| `alwayson=` | 0 or 1 | `RELAY_CONNECTED` | Switch power mode. 0=ignition-only, 1=always-on. Triggers relay switch after next successful send. Sends confirmation alert. |
| `movereset` | — | `ALWAYS_ON_POWER` | Reset movement alarm state: clears escalating cooldown, idle timer, and alert level. Restores original `loop_interval` if it was temporarily shortened by movement. |
| `locatenow` | — | always | Collect fresh GPS fix + sensor data, send telemetry, then send coordinates as alert (`google: lat,lon`). |
| `locate` | — | always | Send last known coordinates as alert (`google: lat,lon`). No new GPS fix. |
| `tomtomnow` | — | always | Collect fresh GPS fix + sensor data, send telemetry, then send coordinates as alert (`tomtom: lat,lon`). |
| `tomtom` | — | always | Send last known coordinates as alert (`tomtom: lat,lon`). No new GPS fix. |
| `config` | — | always | Send device status snapshot as alert. Includes: `int`, `ao` (if relay), `ma` (if always-on power), battery voltage, ignition state, uptime. |
| `reboot` | — | always | Sets `power_reboot` flag. System reboots after the current send cycle completes. |
| `poweroff` | — | `RELAY_CONNECTED` | Sets `power_off_relay` flag. After next successful send, switches relay to ignition-only. Does not change `config.always_on` — on next ignition boot, always-on is restored from saved config. |

