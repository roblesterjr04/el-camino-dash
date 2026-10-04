#!/usr/bin/env bash
# Host tests for the assistant (Python 3.11+, standard library only). The wire tests also build the ESP32 side's C++ and
# compare bytes with it (skipped when there is no C++ compiler).
set -euo pipefail
cd "$(dirname "$0")"
PYTHONPATH=.:tests python3 -m unittest discover -s tests -v "$@"
