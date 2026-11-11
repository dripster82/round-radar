#!/bin/sh
# Build Round Radar and flash it to the board over USB, then show its log.
# Plug the board into the Mac by its port marked "USB" first.
set -e
cd "$(dirname "$0")"
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc,FlashMode=qio"
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
if [ -z "$PORT" ]; then echo "Board not found. Plug it in by the port marked USB."; exit 1; fi
arduino-cli compile --fqbn "$FQBN" --build-path build/work RoundRadar
cp build/work/RoundRadar.ino.merged.bin build/RoundRadar-merged.bin
arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir build/work RoundRadar
echo "Flashed. Log follows (Ctrl-C to stop):"
sleep 2
arduino-cli monitor -p "$(ls /dev/cu.usbmodem* | head -1)" -c baudrate=115200
