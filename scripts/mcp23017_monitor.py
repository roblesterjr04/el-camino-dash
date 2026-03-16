#!/usr/bin/env python3
"""Monitor MCP23017 GPIO state and I2C read health.

This tool is intended for on-device diagnostics when encoder/buttons appear
noisy or unresponsive. It can optionally force known-safe MCP input/pull-up
configuration before sampling.
"""

from __future__ import annotations

import argparse
import sys
import time
from typing import Iterable

try:
    import smbus2  # type: ignore
except ImportError as exc:  # pragma: no cover - device dependency
    print(f"ERROR: smbus2 unavailable: {exc}", file=sys.stderr)
    sys.exit(2)


IODIRA = 0x00
IODIRB = 0x01
GPPUA = 0x0C
GPPUB = 0x0D
GPIOA = 0x12
GPIOB = 0x13

PIN_LABELS = {
    0: "menu_a",
    1: "menu_b",
    2: "menu_button",
    3: "aux_a",
    4: "aux_b",
    5: "aux_button",
    6: "menu_left",
    7: "menu_select",
}


def parse_int(value: str) -> int:
    return int(value, 0)


def configure_inputs_pullups(bus: smbus2.SMBus, address: int) -> None:
    bus.write_byte_data(address, IODIRA, 0xFF)
    bus.write_byte_data(address, IODIRB, 0xFF)
    bus.write_byte_data(address, GPPUA, 0xFF)
    bus.write_byte_data(address, GPPUB, 0xFF)


def read_gpio_word(
    bus: smbus2.SMBus,
    address: int,
    retries: int,
    retry_delay_s: float,
) -> tuple[int, bool]:
    last_error: Exception | None = None
    for _ in range(max(1, retries)):
        try:
            block = bus.read_i2c_block_data(address, GPIOA, 2)
            if len(block) >= 2:
                gpio_a = int(block[0]) & 0xFF
                gpio_b = int(block[1]) & 0xFF
                return gpio_a | (gpio_b << 8), True
            gpio_a = int(bus.read_byte_data(address, GPIOA)) & 0xFF
            gpio_b = int(bus.read_byte_data(address, GPIOB)) & 0xFF
            return gpio_a | (gpio_b << 8), True
        except Exception as exc:  # pragma: no cover - hardware I/O
            last_error = exc
            if retry_delay_s > 0:
                time.sleep(retry_delay_s)
    if last_error is not None:
        return 0, False
    return 0, False


def changed_pins(diff_mask: int, pins: Iterable[int]) -> list[int]:
    return [pin for pin in pins if (diff_mask >> pin) & 0x01]


def pin_state_text(word: int, pins: Iterable[int]) -> str:
    parts: list[str] = []
    for pin in pins:
        level = (word >> pin) & 0x01
        label = PIN_LABELS.get(pin, f"pin{pin}")
        parts.append(f"{label}= {level}")
    return " ".join(parts)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Monitor MCP23017 GPIO and I2C health")
    parser.add_argument("--bus", type=int, default=1, help="I2C bus number (default: 1)")
    parser.add_argument(
        "--address",
        type=parse_int,
        default=0x20,
        help="MCP23017 address (decimal or hex, default: 0x20)",
    )
    parser.add_argument(
        "--interval-ms",
        type=float,
        default=20.0,
        help="Sample interval in milliseconds (default: 20)",
    )
    parser.add_argument(
        "--samples",
        type=int,
        default=0,
        help="Number of samples, 0 means run forever",
    )
    parser.add_argument(
        "--retries",
        type=int,
        default=5,
        help="Read retries per sample (default: 5)",
    )
    parser.add_argument(
        "--retry-delay-ms",
        type=float,
        default=1.0,
        help="Delay between retries in milliseconds (default: 1)",
    )
    parser.add_argument(
        "--configure",
        dest="configure",
        action="store_true",
        default=True,
        help="Configure MCP as inputs with pull-ups before monitoring (default: on)",
    )
    parser.add_argument(
        "--no-configure",
        dest="configure",
        action="store_false",
        help="Do not write MCP configuration registers",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    interval_s = max(0.001, args.interval_ms / 1000.0)
    retry_delay_s = max(0.0, args.retry_delay_ms / 1000.0)

    try:
        bus = smbus2.SMBus(args.bus)
    except Exception as exc:
        print(f"ERROR: failed to open /dev/i2c-{args.bus}: {exc}", file=sys.stderr)
        return 2

    if args.configure:
        try:
            configure_inputs_pullups(bus, args.address)
            print("configured MCP: IODIR[A/B]=0xFF, GPPU[A/B]=0xFF")
        except Exception as exc:
            print(f"WARN: failed to configure MCP registers: {exc}")

    print(
        f"monitoring bus={args.bus} addr=0x{args.address:02X} "
        f"interval_ms={args.interval_ms} retries={args.retries}"
    )
    print("watch for menu_button (GPA2) toggles: level 1->0 on press, 0->1 on release")

    ok = 0
    err = 0
    sample_idx = 0
    last_word: int | None = None
    start = time.monotonic()
    last_stats = start

    while args.samples <= 0 or sample_idx < args.samples:
        word, success = read_gpio_word(bus, args.address, args.retries, retry_delay_s)
        now = time.monotonic()
        sample_idx += 1
        if not success:
            err += 1
            if (now - last_stats) >= 1.0:
                total = ok + err
                err_pct = (100.0 * err / total) if total else 0.0
                print(f"stats ok={ok} err={err} err_pct={err_pct:.1f}%")
                last_stats = now
            time.sleep(interval_s)
            continue

        ok += 1
        gpio_a = word & 0xFF
        gpio_b = (word >> 8) & 0xFF

        if last_word is None:
            print(f"t+0.000s GPIOA=0x{gpio_a:02X} GPIOB=0x{gpio_b:02X} {pin_state_text(word, range(8))}")
            last_word = word
        elif word != last_word:
            diff = word ^ last_word
            changed_a = changed_pins(diff, range(0, 8))
            changed_b = changed_pins(diff, range(8, 16))
            rel_s = now - start
            line = f"t+{rel_s:7.3f}s GPIOA=0x{gpio_a:02X} GPIOB=0x{gpio_b:02X}"
            if changed_a:
                labels = ",".join(PIN_LABELS.get(pin, f"gpa{pin}") for pin in changed_a)
                line += f" changed_A=[{labels}]"
            if changed_b:
                labels = ",".join(f"gpb{pin - 8}" for pin in changed_b)
                line += f" changed_B=[{labels}]"
            line += f" menu_button={(word >> 2) & 0x01}"
            print(line)
            last_word = word

        if (now - last_stats) >= 1.0:
            total = ok + err
            err_pct = (100.0 * err / total) if total else 0.0
            print(f"stats ok={ok} err={err} err_pct={err_pct:.1f}%")
            last_stats = now

        time.sleep(interval_s)

    total = ok + err
    err_pct = (100.0 * err / total) if total else 0.0
    print(f"done samples={total} ok={ok} err={err} err_pct={err_pct:.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
