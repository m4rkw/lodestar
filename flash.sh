#!/bin/bash
set -e

CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
FQBN="fortebit_openiot:stm32:polaris_1:hwrev=V10,modem=UG96,upload_method=STM32CubeDFU,usb=CDC,opt=osstd"
SKETCH="/Users/mark/Library/Mobile Documents/com~apple~CloudDocs/code/lodestar"
PORT="/dev/cu.usbmodemPOLARIS10V3G1"
PROGRAMMER="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI"

HEX=$(find ~/Library/Caches/arduino/sketches/ -name "lodestar.ino.hex" -newer "$SKETCH/lodestar.ino" 2>/dev/null | head -1)
if [ -z "$HEX" ]; then
  HEX=$(find ~/Library/Caches/arduino/sketches/ -name "lodestar.ino.hex" 2>/dev/null | head -1)
fi
echo "Hex: $HEX"

echo "=== Triggering DFU bootloader ==="
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
sleep 4

echo "=== Flashing ==="
"$PROGRAMMER" -c port=usb1 -e all -d "$HEX" -v -ob BOR_LEV=2 -g 2>&1 | grep -E "verified|Error|Start operation|FAILED"

echo "=== Done ==="
