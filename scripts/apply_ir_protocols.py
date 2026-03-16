#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
from typing import Any

import yaml


_ALIASES = {
    "ned": "nec",
}


def _normalize_protocol(value: Any) -> str:
    text = str(value).strip().lower()
    if not text:
        return ""
    return _ALIASES.get(text, text)


def _requested_protocols(ir_cfg: dict[str, Any]) -> list[str]:
    raw_protocols = ir_cfg.get("protocols")
    values: list[str]
    if isinstance(raw_protocols, str):
        values = [raw_protocols]
    elif isinstance(raw_protocols, list):
        values = [str(item) for item in raw_protocols]
    else:
        single = ir_cfg.get("protocol", "")
        values = [str(single)] if single else []
    normalized: list[str] = []
    for value in values:
        protocol = _normalize_protocol(value)
        if protocol and protocol not in normalized:
            normalized.append(protocol)
    return normalized


def main() -> int:
    parser = argparse.ArgumentParser(description="Apply IR decoder protocols from dashboard config")
    parser.add_argument(
        "--config",
        default="/home/pi/gauge-cluster/config/dashboard.hdmi5.yaml",
        help="Path to dashboard YAML config",
    )
    args = parser.parse_args()

    cfg_path = Path(args.config)
    if not cfg_path.exists():
        print(f"[ir-protocol] config not found: {cfg_path}", file=sys.stderr)
        return 1

    try:
        raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    except Exception as exc:
        print(f"[ir-protocol] failed reading config: {exc}", file=sys.stderr)
        return 1
    if not isinstance(raw, dict):
        print("[ir-protocol] invalid config root", file=sys.stderr)
        return 1

    ir_cfg = raw.get("inputs", {}).get("ir_remote", {})
    if not isinstance(ir_cfg, dict):
        print("[ir-protocol] no inputs.ir_remote section")
        return 0
    if not bool(ir_cfg.get("enabled", False)):
        print("[ir-protocol] ir_remote disabled")
        return 0
    if not bool(ir_cfg.get("apply_protocol", True)):
        print("[ir-protocol] apply_protocol disabled")
        return 0

    protocols = _requested_protocols(ir_cfg)
    if not protocols:
        print("[ir-protocol] no protocols requested")
        return 0

    cmd = ["ir-keytable"]
    rc_device = str(ir_cfg.get("rc_device", "")).strip()
    if rc_device:
        cmd.extend(["-s", rc_device])
    for protocol in protocols:
        cmd.extend(["-p", protocol])
    result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        stderr = result.stderr.strip() or "(no stderr)"
        print(f"[ir-protocol] ir-keytable failed: {stderr}", file=sys.stderr)
        return result.returncode
    stdout = result.stdout.strip()
    if stdout:
        print(stdout)
    print(f"[ir-protocol] applied protocols: {', '.join(protocols)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
