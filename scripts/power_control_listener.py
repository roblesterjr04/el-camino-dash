#!/usr/bin/env python3
"""ATtiny power-control listener for Raspberry Pi shutdown handshake.

PI_SIGNAL (GPIO16) is pulsed HIGH by the ATtiny when ignition-off shutdown is
requested. This service initiates OS shutdown. PI_CONFIRM (GPIO12) is held HIGH
during normal runtime and driven LOW during shutdown/exit so the ATtiny can cut
power after the Pi is down.
"""

from __future__ import annotations

import argparse
import signal
import subprocess
import sys
import time

try:
    import RPi.GPIO as GPIO  # type: ignore
except ImportError as exc:  # pragma: no cover - hardware-specific dependency
    print(f"[power-control] ERROR: RPi.GPIO unavailable: {exc}", flush=True)
    raise


class PowerControlListener:
    def __init__(
        self,
        signal_pin: int,
        confirm_pin: int,
        debounce_ms: int,
        poll_interval_s: float,
        startup_ignore_s: float,
    ) -> None:
        self.signal_pin = signal_pin
        self.confirm_pin = confirm_pin
        self.debounce_ms = debounce_ms
        self.poll_interval_s = poll_interval_s
        self.startup_ignore_s = startup_ignore_s
        self._stopping = False
        self._shutdown_requested = False
        self._last_signal_state = 0
        self._last_event_monotonic = 0.0
        self._startup_monotonic = 0.0

    def setup_gpio(self) -> None:
        GPIO.setwarnings(False)
        GPIO.setmode(GPIO.BCM)
        GPIO.setup(self.signal_pin, GPIO.IN, pull_up_down=GPIO.PUD_DOWN)
        GPIO.setup(self.confirm_pin, GPIO.OUT, initial=GPIO.HIGH)
        self._startup_monotonic = time.monotonic()
        self._last_signal_state = int(GPIO.input(self.signal_pin))
        print(
            f"[power-control] listening on GPIO{self.signal_pin}, "
            f"confirm on GPIO{self.confirm_pin}=HIGH",
            flush=True,
        )

    def _on_shutdown_signal(self, _channel: int) -> None:
        if self._shutdown_requested or self._stopping:
            return
        self._shutdown_requested = True
        print("[power-control] shutdown pulse detected; requesting poweroff", flush=True)
        try:
            subprocess.Popen(
                ["/bin/systemctl", "--no-block", "poweroff"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception as exc:  # pragma: no cover - system interaction
            print(f"[power-control] ERROR: failed to request poweroff: {exc}", flush=True)

    def stop(self) -> None:
        if self._stopping:
            return
        self._stopping = True
        print("[power-control] stopping; driving PI_CONFIRM LOW", flush=True)
        try:
            GPIO.output(self.confirm_pin, GPIO.LOW)
            # Give the ATtiny time to sample low before cleanup.
            time.sleep(0.15)
        except Exception as exc:  # pragma: no cover - hardware-specific
            print(f"[power-control] WARN: failed driving PI_CONFIRM low: {exc}", flush=True)
        finally:
            try:
                GPIO.cleanup()
            except Exception:
                pass

    def _poll_signal(self) -> None:
        current = int(GPIO.input(self.signal_pin))
        now = time.monotonic()
        # Ignore early boot transients from attached controller circuitry.
        if now - self._startup_monotonic < self.startup_ignore_s:
            self._last_signal_state = current
            return
        if (
            self._last_signal_state == 0
            and current == 1
            and (now - self._last_event_monotonic) * 1000.0 >= float(self.debounce_ms)
        ):
            self._last_event_monotonic = now
            self._on_shutdown_signal(self.signal_pin)
        self._last_signal_state = current

    def run(self) -> int:
        self.setup_gpio()
        while not self._stopping:
            self._poll_signal()
            time.sleep(self.poll_interval_s)
        return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ATtiny shutdown listener service")
    parser.add_argument("--signal-pin", type=int, default=16, help="BCM GPIO for PI_SIGNAL input")
    parser.add_argument("--confirm-pin", type=int, default=12, help="BCM GPIO for PI_CONFIRM output")
    parser.add_argument("--debounce-ms", type=int, default=80, help="Debounce for PI_SIGNAL pulse")
    parser.add_argument(
        "--poll-interval-ms",
        type=int,
        default=20,
        help="Main loop sleep in ms (must be well below ATtiny pulse width)",
    )
    parser.add_argument(
        "--startup-ignore-ms",
        type=int,
        default=45000,
        help="Ignore shutdown signal edges for N ms after service start",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    listener = PowerControlListener(
        signal_pin=args.signal_pin,
        confirm_pin=args.confirm_pin,
        debounce_ms=args.debounce_ms,
        poll_interval_s=max(0.02, args.poll_interval_ms / 1000.0),
        startup_ignore_s=max(0.0, args.startup_ignore_ms / 1000.0),
    )

    def _handle_stop(_signum: int, _frame: object) -> None:
        listener.stop()

    signal.signal(signal.SIGINT, _handle_stop)
    signal.signal(signal.SIGTERM, _handle_stop)

    try:
        return listener.run()
    except KeyboardInterrupt:
        listener.stop()
        return 0
    except Exception as exc:  # pragma: no cover - defensive logging
        print(f"[power-control] ERROR: {exc}", flush=True)
        listener.stop()
        return 1


if __name__ == "__main__":
    sys.exit(main())
