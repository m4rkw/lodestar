#!/bin/bash
set -e

CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
FQBN="fortebit_openiot:stm32:polaris_1:hwrev=V10,modem=UG96,upload_method=STM32CubeDFU,usb=CDC,opt=osstd"
SKETCH="/Users/mark/Library/Mobile Documents/com~apple~CloudDocs/code/lodestar"
PORT="/dev/cu.usbmodemPOLARIS10V3G1"
PROGRAMMER="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI"

# -- Compile -----------------------------------------------------------------

echo "=== Compiling (UG96 3G) ==="
"$CLI" compile --fqbn "$FQBN" "$SKETCH"

# find the hex file from the build
HEX=$(find ~/Library/Caches/arduino/sketches/ -name "firmware.ino.hex" -newer "$SKETCH/firmware.ino" 2>/dev/null | head -1)
if [ -z "$HEX" ]; then
  HEX=$(find ~/Library/Caches/arduino/sketches/ -name "firmware.ino.hex" 2>/dev/null | head -1)
fi
echo "Hex: $HEX"
