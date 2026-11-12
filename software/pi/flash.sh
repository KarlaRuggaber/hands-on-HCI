#!/usr/bin/env bash
# On the Pi: compile and upload the ESP32 firmware, the service is paused meanwhile.
# Once (already done on the box):
#   curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh
#   arduino-cli config init && arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
#   arduino-cli core update-index && arduino-cli core install esp32:esp32@3.2.1     # keep 3.2.1: 3.3 has a new touch driver
#   arduino-cli lib install "Adafruit NeoPixel" ArduinoJson
set -e
export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")"
PORT=$(python3 -c "import yaml; print(yaml.safe_load(open('config.yaml'))['serial']['port'])")
systemctl --user stop spicetracks || true
arduino-cli compile --upload -p "$PORT" --fqbn esp32:esp32:esp32s3:UploadSpeed=115200 ../firmware/spicetracks_esp32
systemctl --user start spicetracks || true
echo "Fertig. Jetzt 30 s die Pflanzen nicht berühren (Touch kalibriert)."
