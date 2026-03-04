#!/usr/bin/env bash
set -euo pipefail

REQUIRED_CORE="${1:-arduino:avr}"

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

echo "Updating board index..."
arduino-cli core update-index

if ! arduino-cli core list | awk '{print $1}' | rg "^${REQUIRED_CORE}$" >/dev/null 2>&1; then
  echo "Installing Arduino core: ${REQUIRED_CORE}"
  arduino-cli core install "${REQUIRED_CORE}"
else
  echo "Arduino core already installed: ${REQUIRED_CORE}"
fi

echo "Arduino CLI setup complete."
