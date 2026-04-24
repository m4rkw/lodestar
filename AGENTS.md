# Polaris firmware project

## Overview

This repo contains Arduino firmware for Polaris vehicle trackers.

## Important considerations

The single most important design factor in this software is power conservation.
The device may be connected to 12v live and detects engine-off movement, sending
telemetry as appropriate. When the engine isn't running this uses battery power
so great care needs to be taken to ensure we don't waste power.

## Functionality

The device sends telemetry (gps coords, speed, battery voltage etc) to a server
using UDP. UDP is used in order to get a high resolution of data, allowing a
transmission rate of one event per second (the maximum rate at which we can poll
the GPS device).

- When the ignition is on the device sends telemetry at a rate of one event
  every 30s (approx)
- When the ignition is on and the battery voltage is above 13v this means the
  engine is running and so telemetry is sent at the maximum rate that we get GPS
  updates (one event per second)
- When the ignition is turned off the device goes into STOP2 sleep mode, drawing
  only 2.5mA of power. It uses an interrupt to wake again from the ignition
  signal.
- In addition to the ignition signal wakes it also has a configurable engine-off
  wake interval, if set this will wake the device once every {n} seconds, send a
  single telemetry point and then go back to sleep.
- There is also engine-off movement detection via the accelerometer, if movement
  is detected an alert is sent via the server. If the engine-off wake interval
  is disabled or very long it is temporarily reduced to 4h to ensure telemetry
  points are sent in the event that the vehicle is being stolen on a
  transporter.
- An escalating cooldown time is used to avoid spamming alerts in the event the
  the car is moving continuously.
- Remote commands can be used to adjust the configuration
- Uses the onboard IWDG watchdog with a 32s timeout to detect a hung process and
  reset the device

## Webserver

The webserver component is in web/

## Tests

The tests are in the tests/ directory and can be executed with "make test".

## Development rules

These rules MUST be adhered to when making changes.

1. Be especially careful with changes to modem flows. Do not arbitrarily remove
delays under the premise of saving power if there's any chance they might cause
the modem to go into a bad state.

2. Be conscious of the power implications of changes. It's very important to
conserve power when the engine isn't running.

3. There is a hardware watchdog (IWDG) in place with a 32s timeout. It's
therefore critical that loops are constructed carefully and reset the IWDG timer
at appropriate points.

4. Test must be updated as necessary, new functionality should be covered by
tests.

5. The web components should be updated as necessary for changes or new
functionality.
