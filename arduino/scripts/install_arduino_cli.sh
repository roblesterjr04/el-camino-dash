#!/usr/bin/env bash
set -euo pipefail

# Install arduino-cli (Homebrew on macOS), import project board indexes, install AVR + ESP32 + RP2040 cores,
# and common libraries used by repo sketches.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CONFIG_FILE="${PROJECT_ROOT}/arduino/config/arduino-cli.yaml"
ESP32_INDEX_URL="https://espressif.github.io/arduino-esp32/package_esp32_index.json"
# Earle Philhower RP2040 (recommended for encoder_controller; avoids Mbed RTOS crash LED pattern on Pico).
RP2040_PHILHOWER_INDEX_URL="https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json"

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew is required but was not found." >&2
  exit 1
fi

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "Installing arduino-cli via Homebrew..."
  brew install arduino-cli
else
  echo "arduino-cli already installed."
fi

if [[ -f "${CONFIG_FILE}" ]]; then
  echo "Project config reference: ${CONFIG_FILE}"
fi
if ! arduino-cli config dump 2>/dev/null | grep -Fq "${ESP32_INDEX_URL}"; then
  echo "Registering Espressif board index URL for ESP32 core..."
  arduino-cli config add board_manager.additional_urls "${ESP32_INDEX_URL}"
else
  echo "Espressif board index URL already in arduino-cli config."
fi
if ! arduino-cli config dump 2>/dev/null | grep -Fq "${RP2040_PHILHOWER_INDEX_URL}"; then
  echo "Registering Earle Philhower RP2040 board index (arduino-pico)..."
  arduino-cli config add board_manager.additional_urls "${RP2040_PHILHOWER_INDEX_URL}"
else
  echo "RP2040 (Philhower) board index URL already in arduino-cli config."
fi
echo "Updating board index..."
arduino-cli core update-index || echo "WARN: core update-index failed (offline? continuing.)" >&2

core_installed() {
  local c="$1"
  arduino-cli core list | awk -v core="$c" 'NR > 1 && $1 == core {found=1} END {exit found ? 0 : 1}'
}

install_core_if_missing() {
  local c="$1"
  if core_installed "${c}"; then
    echo "Arduino core already installed: ${c}"
  else
    echo "Installing Arduino core: ${c}"
    arduino-cli core install "${c}"
  fi
}

if [[ $# -eq 0 ]]; then
  DEFAULT_CORES=(arduino:avr esp32:esp32 rp2040:rp2040 arduino:mbed_rp2040)
else
  DEFAULT_CORES=("$@")
fi

for c in "${DEFAULT_CORES[@]}"; do
  install_core_if_missing "${c}"
done

# encoder_controller depends on Paul Stoffregen's Encoder (Arduino Library Manager).
if ! arduino-cli lib list | awk 'NR > 1 && $1 == "Encoder" {found=1} END {exit found ? 0 : 1}'; then
  echo 'Installing library: Encoder (for encoder_controller sketch)'
  arduino-cli lib install "Encoder"
else
  echo "Library already installed: Encoder"
fi

# encoder_controller: U8g2 (SSD1322 256x64 SPI page buffer; fits ATmega328P SRAM).
lib_line_contains() {
  arduino-cli lib list 2>/dev/null | grep -Fq "$1"
}
if lib_line_contains "U8g2"; then
  echo "Library already installed: U8g2"
else
  echo "Installing library: U8g2"
  arduino-cli lib install "U8g2"
fi

# encoder_controller IR (Arduino-IRremote v4, NEC decode). Match exact name, not IRremoteESP8266.
if arduino-cli lib list 2>/dev/null | awk 'NR > 1 && $1 == "IRremote" {found=1} END {exit found ? 0 : 1}'; then
  echo "Library already installed: IRremote"
else
  echo "Installing library: IRremote"
  arduino-cli lib install "IRremote"
fi

# esp32_audio_bridge: A2DP + AVRCP (git; not in Library Manager index).
if ! arduino-cli lib list 2>/dev/null | grep -Fq "ESP32-A2DP"; then
  echo "Installing ESP32-A2DP + arduino-audio-tools (for esp32_audio_bridge; enables unsafe git install)..."
  arduino-cli config set library.enable_unsafe_install true
  arduino-cli lib install --git-url https://github.com/pschatzmann/ESP32-A2DP.git
  arduino-cli lib install --git-url https://github.com/pschatzmann/arduino-audio-tools.git
else
  echo "ESP32-A2DP already listed in arduino-cli lib list (skip git install)."
fi

echo "Arduino CLI setup complete."
echo "Cores: ${DEFAULT_CORES[*]}"
echo "List ports: arduino-cli board list"
