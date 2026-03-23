"""NetworkManager (nmcli) helpers for Wi-Fi status and control on Linux targets."""

from __future__ import annotations

import subprocess
import sys
from typing import Any


def _run_nmcli(args: list[str], timeout: float = 15.0) -> tuple[int, str, str]:
    try:
        proc = subprocess.run(
            ["nmcli", *args],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except FileNotFoundError:
        return 127, "", "nmcli not found"
    except subprocess.TimeoutExpired:
        return 124, "", "nmcli timed out"
    except OSError as exc:
        return 1, "", str(exc)
    out = (proc.stdout or "").strip()
    err = (proc.stderr or "").strip()
    return int(proc.returncode), out, err


def _parse_device_table(stdout: str) -> list[tuple[str, str, str, str]]:
    rows: list[tuple[str, str, str, str]] = []
    for line in stdout.splitlines():
        parts = line.split(":")
        if len(parts) < 4:
            continue
        device, dev_type, state, connection = parts[0], parts[1], parts[2], ":".join(parts[3:])
        rows.append((device.strip(), dev_type.strip(), state.strip(), connection.strip()))
    return rows


def _first_wifi_device(rows: list[tuple[str, str, str, str]]) -> tuple[str, str, str, str] | None:
    for row in rows:
        if row[1] == "wifi":
            return row
    return None


def _wifi_ipv4(device: str) -> str:
    code, out, _ = _run_nmcli(["-g", "IP4.ADDRESS", "device", "show", device], timeout=6.0)
    if code != 0 or not out:
        return "-"
    for line in out.splitlines():
        line = line.strip()
        if line and line != "--":
            # Often "192.168.1.5/24"
            return line.split("/", 1)[0]
    return "-"


def _radio_wifi_enabled() -> str | None:
    code, out, _ = _run_nmcli(["radio", "wifi"], timeout=4.0)
    if code != 0 or not out:
        return None
    first = out.strip().splitlines()[0].strip().lower()
    if first in {"enabled", "disabled"}:
        return first
    return None


def wifi_status_dict(config: dict[str, Any]) -> dict[str, str]:
    """Return short strings for menu status keys."""
    if not sys.platform.startswith("linux"):
        return {
            "wifi_summary": "Wi-Fi (non-Linux)",
            "wifi_connection": "-",
            "wifi_ip": "-",
            "wifi_radio": "-",
            "wifi_hint": "Use Pi + NetworkManager (nmcli)",
        }

    code, out, err = _run_nmcli(["-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device"], timeout=8.0)
    if code != 0:
        msg = err or out or f"exit {code}"
        short = msg.replace("\n", " ")[:72]
        return {
            "wifi_summary": short or "nmcli error",
            "wifi_connection": "-",
            "wifi_ip": "-",
            "wifi_radio": "-",
            "wifi_hint": "Install network-manager / nmcli",
        }

    row = _first_wifi_device(_parse_device_table(out))
    radio = _radio_wifi_enabled()
    radio_s = radio if radio else "?"

    if not row:
        return {
            "wifi_summary": f"no wifi device · radio {radio_s}",
            "wifi_connection": "-",
            "wifi_ip": "-",
            "wifi_radio": radio_s,
            "wifi_hint": "New SSID: nmcli over SSH",
        }

    device, _wtype, state, conn = row
    if state.lower() == "connected" and conn and conn != "--":
        ip = _wifi_ipv4(device)
        summary = f"{conn[:28]} · {ip}"
        if len(summary) > 56:
            summary = summary[:53] + "..."
        return {
            "wifi_summary": summary,
            "wifi_connection": conn[:40],
            "wifi_ip": ip,
            "wifi_radio": radio_s,
            "wifi_hint": "New SSID: nmcli over SSH",
        }

    return {
        "wifi_summary": f"{device} {state} · radio {radio_s}",
        "wifi_connection": "-",
        "wifi_ip": "-",
        "wifi_radio": radio_s,
        "wifi_hint": "New SSID: nmcli over SSH",
    }


def _wifi_connection_names() -> list[str]:
    code, out, _ = _run_nmcli(["-t", "-f", "NAME,TYPE", "connection"], timeout=10.0)
    if code != 0 or not out:
        return []
    names: list[str] = []
    for line in out.splitlines():
        parts = line.split(":")
        if len(parts) < 2:
            continue
        name, ctype = parts[0].strip(), parts[1].strip()
        if ctype == "802-11-wireless" and name:
            names.append(name)
    return names


def wifi_rescan() -> str:
    if not sys.platform.startswith("linux"):
        return "Wi-Fi rescan needs Linux"
    code, out, err = _run_nmcli(["dev", "wifi", "rescan"], timeout=20.0)
    if code == 0:
        return "Wi-Fi scan requested"
    return (err or out or f"rescan failed ({code})")[:120]


def wifi_connect(config: dict[str, Any]) -> str:
    if not sys.platform.startswith("linux"):
        return "Wi-Fi connect needs Linux"
    system_cfg = config.get("system") if isinstance(config.get("system"), dict) else {}
    wifi_cfg = system_cfg.get("wifi") if isinstance(system_cfg.get("wifi"), dict) else {}
    preferred = str(wifi_cfg.get("connection_name", "elc-wifi")).strip()
    err_first = ""

    if preferred:
        code, out, err = _run_nmcli(["connection", "up", preferred], timeout=45.0)
        if code == 0:
            return f"Connected ({preferred})"
        err_first = (err or out or "")[:200]

    names = _wifi_connection_names()
    last_err = err_first
    for name in names:
        if preferred and name == preferred:
            continue
        code, out, err = _run_nmcli(["connection", "up", name], timeout=45.0)
        if code == 0:
            return f"Connected ({name})"
        last_err = (err or out or "")[:200]
    if not names and not preferred:
        return "No saved Wi-Fi profile (set system.wifi.connection_name)"
    return (last_err or "Connect failed")[:120]


def wifi_disconnect(config: dict[str, Any]) -> str:
    if not sys.platform.startswith("linux"):
        return "Wi-Fi disconnect needs Linux"
    code, out, _ = _run_nmcli(["-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device"], timeout=8.0)
    if code != 0:
        return "Could not list devices"
    row = _first_wifi_device(_parse_device_table(out))
    if not row:
        return "No Wi-Fi device"
    device = row[0]
    code, out, err = _run_nmcli(["device", "disconnect", device], timeout=20.0)
    if code == 0:
        return f"Disconnected {device}"
    return (err or out or f"disconnect failed ({code})")[:120]


def wifi_radio_toggle() -> str:
    if not sys.platform.startswith("linux"):
        return "Wi-Fi radio toggle needs Linux"
    state = _radio_wifi_enabled()
    if state == "enabled":
        code, _, err = _run_nmcli(["radio", "wifi", "off"], timeout=8.0)
        return "Wi-Fi radio off" if code == 0 else (err or "radio off failed")[:120]
    if state == "disabled":
        code, _, err = _run_nmcli(["radio", "wifi", "on"], timeout=8.0)
        return "Wi-Fi radio on" if code == 0 else (err or "radio on failed")[:120]
    code, out, _ = _run_nmcli(["radio", "wifi"], timeout=4.0)
    text = (out or "").strip().lower()
    if "disable" in text:
        code, _, err = _run_nmcli(["radio", "wifi", "on"], timeout=8.0)
        return "Wi-Fi radio on" if code == 0 else (err or "radio on failed")[:120]
    code, _, err = _run_nmcli(["radio", "wifi", "off"], timeout=8.0)
    return "Wi-Fi radio off" if code == 0 else (err or "radio off failed")[:120]
