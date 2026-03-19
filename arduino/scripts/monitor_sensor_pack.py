#!/usr/bin/env python3
from __future__ import annotations

import argparse
import glob
import math
import os
import shutil
import subprocess
import sys
import time

try:
    import serial  # type: ignore
except Exception as exc:  # pragma: no cover
    print(f"pyserial import failed: {exc}", file=sys.stderr)
    sys.exit(1)


SYNC = b"\xAA\x55"
LEGACY_PAYLOAD_LEN = 19
EXTENDED_PAYLOAD_LEN = 21
LEGACY_FRAME_LEN = 2 + LEGACY_PAYLOAD_LEN + 1
EXTENDED_FRAME_LEN = 2 + EXTENDED_PAYLOAD_LEN + 1

FAULT_BITS: list[tuple[int, str]] = [
    (0, "POT_OPEN"),
    (1, "POT_SHORT"),
    (2, "TEMP_OPEN"),
    (3, "TEMP_SHORT"),
    (4, "PRESS_OPEN"),
    (5, "PRESS_SHORT"),
    (6, "VOLT_OPEN"),
    (7, "VOLT_HIGH"),
    (8, "HALL_MISSING"),
    (9, "COIL_MISSING"),
    (10, "ALT_STUCK"),
    (11, "POT_POWER"),
    (12, "TEMP_POWER"),
    (13, "PRESS_POWER"),
    (14, "VOLT_POWER"),
]

APP_SERVICE_UNITS: tuple[str, ...] = (
    "gauge-cluster.service",
    "gauge-cluster-spi.service",
    "gauge-cluster-hdmi.service",
    "gauge-hide-cursor.service",
)

APP_PROCESS_PATTERNS: tuple[str, ...] = (
    "src/main.py",
    "scripts/run_hw.sh",
    "scripts/run_sim.sh",
)

MAC_PORT_GLOBS: tuple[str, ...] = (
    "/dev/cu.usbmodem*",
    "/dev/cu.usbserial*",
    "/dev/cu.SLAB_USBtoUART*",
    "/dev/cu.wchusbserial*",
)


def parse_frame(payload: bytes) -> dict[str, int | None]:
    if len(payload) == LEGACY_PAYLOAD_LEN:
        return {
            "pot": int.from_bytes(payload[0:2], "big"),
            "therm": int.from_bytes(payload[2:4], "big"),
            "pressure": int.from_bytes(payload[4:6], "big"),
            "voltage": int.from_bytes(payload[6:8], "big"),
            "ambient_therm": None,
            "speed_rpm": int.from_bytes(payload[8:12], "big"),
            "engine_rpm": int.from_bytes(payload[12:16], "big"),
            "flags": payload[16],
            "faults": int.from_bytes(payload[17:19], "big"),
        }

    if len(payload) == EXTENDED_PAYLOAD_LEN:
        return {
            "pot": int.from_bytes(payload[0:2], "big"),
            "therm": int.from_bytes(payload[2:4], "big"),
            "pressure": int.from_bytes(payload[4:6], "big"),
            "voltage": int.from_bytes(payload[6:8], "big"),
            "ambient_therm": int.from_bytes(payload[8:10], "big"),
            "speed_rpm": int.from_bytes(payload[10:14], "big"),
            "engine_rpm": int.from_bytes(payload[14:18], "big"),
            "flags": payload[18],
            "faults": int.from_bytes(payload[19:21], "big"),
        }

    raise ValueError(f"Unexpected payload length: {len(payload)}")


def to_system_voltage(raw_adc: int) -> float:
    vdiv_r1 = 27000.0
    vdiv_r2 = 10000.0
    vref = 5.0
    ratio = vdiv_r2 / (vdiv_r1 + vdiv_r2)
    return ((float(raw_adc) / 1023.0) * vref) / ratio


def thermistor_temp_c(raw_adc: int) -> float:
    # Match sensor_pack.ino conversion constants (100k NTC, B3950).
    therm_series_r = 100000.0
    therm_nominal_r = 100000.0
    therm_nominal_t_c = 25.0
    therm_beta = 3950.0

    if raw_adc <= 0 or raw_adc >= 1023:
        return float("nan")
    resistance = therm_series_r * ((1023.0 / float(raw_adc)) - 1.0)
    if resistance <= 0.0:
        return float("nan")

    steinhart = math.log(resistance / therm_nominal_r) / therm_beta
    steinhart += 1.0 / (therm_nominal_t_c + 273.15)
    return (1.0 / steinhart) - 273.15


def temp_c_to_f(temp_c: float) -> float:
    return (temp_c * 9.0 / 5.0) + 32.0


def fmt_temp(raw_adc: int | None) -> str:
    if raw_adc is None:
        return "n/a"
    temp_c = thermistor_temp_c(raw_adc)
    if math.isnan(temp_c):
        return "invalid"
    temp_f = temp_c_to_f(temp_c)
    return f"{temp_c:5.1f}C / {temp_f:5.1f}F"


def decode_faults(fault_word: int) -> list[str]:
    active: list[str] = []
    for bit, name in FAULT_BITS:
        if fault_word & (1 << bit):
            active.append(name)
    return active


def list_serial_ports() -> list[str]:
    ports: list[str] = []
    try:
        from serial.tools import list_ports  # type: ignore

        ports = sorted(p.device for p in list_ports.comports() if p.device)
    except Exception:
        # Fall back to a direct /dev glob on hosts where list_ports is unavailable.
        if sys.platform == "darwin":
            for pattern in MAC_PORT_GLOBS:
                ports.extend(glob.glob(pattern))
            ports = sorted(set(ports))
    return ports


def resolve_port(port_arg: str | None) -> str | None:
    if port_arg:
        return port_arg

    ports = list_serial_ports()
    if not ports:
        return None

    if len(ports) == 1:
        return ports[0]

    # Prefer common Arduino-style ports first when multiple are available.
    preferred = [p for p in ports if "usbmodem" in p or "usbserial" in p]
    if preferred:
        return preferred[0]
    return ports[0]


def stop_app_services() -> None:
    if not sys.platform.startswith("linux"):
        return

    if shutil.which("systemctl"):
        use_sudo = os.geteuid() != 0 and shutil.which("sudo") is not None
        for unit in APP_SERVICE_UNITS:
            cmd = ["systemctl", "stop", unit]
            if use_sudo:
                cmd = ["sudo", "-n"] + cmd
            subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    if shutil.which("pkill"):
        for pattern in APP_PROCESS_PATTERNS:
            subprocess.run(
                ["pkill", "-f", pattern],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )


def render_dashboard(fields: dict[str, int | None]) -> None:
    flags = int(fields["flags"] or 0)
    faults_word = int(fields["faults"] or 0)
    voltage_raw = int(fields["voltage"] or 0)
    therm_raw = int(fields["therm"] or 0)
    ambient_raw = fields["ambient_therm"]
    speed_rpm = int(fields["speed_rpm"] or 0)
    engine_rpm = int(fields["engine_rpm"] or 0)
    alt_ok = bool(flags & 0x01)
    ign_on = bool(flags & 0x02)
    volts = to_system_voltage(voltage_raw)
    faults = decode_faults(faults_word)

    lines: list[str] = []
    lines.append("sensor_pack live monitor (press Ctrl+C to stop)")
    lines.append("")
    lines.append("ADC raw:")
    lines.append(
        "  pot={pot:4d}  therm={therm:4d}  ambient={ambient:>4}  pressure={pressure:4d}  voltage={voltage:4d}".format(
            pot=int(fields["pot"] or 0),
            therm=therm_raw,
            ambient=("n/a" if ambient_raw is None else f"{int(ambient_raw):4d}"),
            pressure=int(fields["pressure"] or 0),
            voltage=voltage_raw,
        )
    )
    lines.append("")
    lines.append("Computed:")
    lines.append(
        "  speed_rpm={speed_rpm:5d}  engine_rpm={engine_rpm:5d}  system_voltage={volts:5.2f}V".format(
            speed_rpm=speed_rpm,
            engine_rpm=engine_rpm,
            volts=volts,
        )
    )
    lines.append(f"  engine_temp={fmt_temp(therm_raw)}  ambient_temp={fmt_temp(ambient_raw)}")
    lines.append(
        "  flags=0x{flags:02X}  alt_ok={alt_ok}  ign_on={ign_on}".format(
            flags=flags,
            alt_ok=int(alt_ok),
            ign_on=int(ign_on),
        )
    )
    lines.append("")
    lines.append("Faults:")
    if faults:
        lines.append(f"  fault_word=0x{faults_word:04X}")
        for name in faults:
            lines.append(f"  - {name}")
    else:
        lines.append("  none (fault_word=0x0000)")

    # Redraw in-place without scrolling.
    sys.stdout.write("\x1b[H\x1b[2J")
    sys.stdout.write("\n".join(lines) + "\n")
    sys.stdout.flush()


def main() -> int:
    parser = argparse.ArgumentParser(description="Decode sensor_pack.ino binary serial stream.")
    parser.add_argument(
        "--port",
        help="Serial port, e.g. /dev/cu.usbmodem101. If omitted, auto-detect is attempted.",
    )
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--timeout", type=float, default=0.1, help="Read timeout seconds")
    parser.add_argument(
        "--list-ports",
        action="store_true",
        help="List detected serial ports and exit",
    )
    parser.add_argument(
        "--refresh-hz",
        type=float,
        default=10.0,
        help="Dashboard refresh rate in Hz (default: 10)",
    )
    parser.add_argument(
        "--no-stop-services",
        action="store_true",
        help="Do not stop dashboard services/processes before opening serial",
    )
    args = parser.parse_args()

    if args.list_ports:
        ports = list_serial_ports()
        if ports:
            print("Detected serial ports:")
            for p in ports:
                print(f"  {p}")
            return 0
        print("No serial ports detected.")
        return 1

    port = resolve_port(args.port)
    if not port:
        print(
            "No serial port specified and auto-detect failed.\n"
            "Use --list-ports to discover available devices, then pass --port.",
            file=sys.stderr,
        )
        return 1

    if not args.no_stop_services:
        stop_app_services()
        # Give systemd/process teardown a brief moment to release serial handles.
        time.sleep(0.5)

    try:
        ser = serial.Serial(port, baudrate=args.baud, timeout=args.timeout)
    except Exception as exc:
        print(f"Failed to open serial port {port}: {exc}", file=sys.stderr)
        return 1

    print(f"Listening on {port} @ {args.baud}...")
    buf = bytearray()
    last_render = 0.0
    refresh_interval = 1.0 / max(1.0, args.refresh_hz)
    latest_fields: dict[str, int | None] | None = None

    try:
        while True:
            chunk = ser.read(256)
            if chunk:
                buf.extend(chunk)
            while len(buf) >= LEGACY_FRAME_LEN:
                sync_idx = buf.find(SYNC)
                if sync_idx < 0:
                    buf.clear()
                    break
                if sync_idx > 0:
                    del buf[:sync_idx]
                if len(buf) < LEGACY_FRAME_LEN:
                    break

                frame_consumed = False
                for payload_len in (EXTENDED_PAYLOAD_LEN, LEGACY_PAYLOAD_LEN):
                    frame_len = 2 + payload_len + 1
                    if len(buf) < frame_len:
                        continue
                    payload = bytes(buf[2 : 2 + payload_len])
                    recv_crc = buf[2 + payload_len]

                    calc_crc = 0
                    for b in payload:
                        calc_crc ^= b
                    if recv_crc != calc_crc:
                        continue

                    del buf[:frame_len]
                    latest_fields = parse_frame(payload)
                    frame_consumed = True
                    break

                if not frame_consumed:
                    # Bad sync candidate; drop one byte and keep searching.
                    del buf[0]

            now = time.time()
            if latest_fields is not None and (now - last_render) >= refresh_interval:
                render_dashboard(latest_fields)
                last_render = now
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    raise SystemExit(main())
