"""The agent's settings: a TOML file (config.example.toml), read with the standard library."""
from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from typing import Optional


@dataclass
class Config:
    port: str = "/dev/ttyS3"
    baud: int = 1500000
    vehicle: str = "a 1964 Chevrolet El Camino"
    stt_url: str = "http://127.0.0.1:8081"
    stt_prompt: str = "El Camino, oil pressure, volume, next song, radio, Bluetooth."
    local_url: str = "http://127.0.0.1:8080"
    local_model: str = "local"
    local_max_tokens: int = 160
    online_enabled: bool = False
    online_model: str = "claude-opus-5-5"
    online_effort: str = "low"
    online_web_search: bool = True
    online_probe_url: str = "https://api.anthropic.com"
    first_token_s: float = 4.0
    tts_model: str = ""
    tts_rate: int = 22050
    tts_binary: str = "piper"
    manual_dir: str = ""
    alive_on_cmd: str = ""
    alive_off_cmd: str = ""
    shutdown_cmd: str = "systemctl poweroff"
    history_turns: int = 6
    silence_ms: int = 800

    @staticmethod
    def load(path: Optional[str]) -> "Config":
        c = Config()
        if not path:
            return c
        with open(path, "rb") as f:
            d = tomllib.load(f)
        m = {
            "link": {"port": "port", "baud": "baud"},
            "vehicle": {"description": "vehicle"},
            "stt": {"url": "stt_url", "prompt": "stt_prompt"},
            "local": {"url": "local_url", "model": "local_model", "max_tokens": "local_max_tokens"},
            "online": {"enabled": "online_enabled", "model": "online_model", "effort": "online_effort",
                       "web_search": "online_web_search", "probe_url": "online_probe_url", "first_token_s": "first_token_s"},
            "tts": {"model": "tts_model", "rate": "tts_rate", "binary": "tts_binary"},
            "manual": {"dir": "manual_dir"},
            "gpio": {"alive_on_cmd": "alive_on_cmd", "alive_off_cmd": "alive_off_cmd"},
            "system": {"shutdown_cmd": "shutdown_cmd"},
            "dialog": {"history_turns": "history_turns", "silence_ms": "silence_ms"},
        }
        for section, fields in m.items():
            for key, attr in fields.items():
                if key in d.get(section, {}):
                    setattr(c, attr, d[section][key])
        return c
