from __future__ import annotations

from typing import Any

from dashboard.integrations.si4737 import FUNC_FM
from dashboard.integrations.si4737 import FUNC_WB
from dashboard.integrations.si4737 import Si4737


class Si4737RadioAdapter:
    def __init__(self, config: dict[str, Any]) -> None:
        self._cfg = config
        self._radio: Si4737 | None = None
        self._status = "disabled"
        self._mode = "fm"
        self._muted = False

    def start(self) -> None:
        if not bool(self._cfg.get("enabled", False)):
            self._status = "disabled"
            return
        try:
            self._radio = Si4737(
                i2c_bus=int(self._cfg.get("i2c_bus", 1)),
                address=int(self._cfg.get("address", 0x11)),
                rst_pin=int(self._cfg.get("rst_pin", 17)),
            )
            startup_mode = str(self._cfg.get("startup_mode", "fm")).strip().lower()
            startup_volume = int(float(self._cfg.get("startup_volume", 45)))
            if startup_mode == "wb":
                channel = int(self._cfg.get("wb_channel", 1))
                self._radio.power_up_wb()
                self._radio.wb_tune(channel)
                self._mode = "wb"
                self._status = f"wb:{channel}"
            else:
                startup_freq = float(self._cfg.get("startup_freq_mhz", 101.3))
                self._radio.power_up_fm()
                self._radio.fm_tune(startup_freq)
                self._mode = "fm"
                self._status = f"fm:{self._radio.get_freq_mhz():.1f}"
            self._radio.set_volume(startup_volume)
            self._radio.unmute()
            self._muted = False
        except Exception as exc:
            self._status = f"error:{exc}"
            if self._radio is not None:
                try:
                    self._radio.close()
                except Exception:
                    pass
            self._radio = None

    def stop(self) -> None:
        if self._radio is not None:
            try:
                self._radio.close()
            except Exception:
                pass
        self._radio = None
        if bool(self._cfg.get("enabled", False)):
            self._status = "stopped"
        else:
            self._status = "disabled"

    def status(self) -> str:
        if not bool(self._cfg.get("enabled", False)):
            return "disabled"
        return self._status

    def fm_mode(self) -> str:
        radio = self._require_radio()
        if not radio:
            return self._status
        try:
            radio.power_up_fm()
            freq = float(self._cfg.get("startup_freq_mhz", 101.3))
            radio.fm_tune(freq)
            self._mode = "fm"
            self._status = f"fm:{radio.get_freq_mhz():.1f}"
        except Exception as exc:
            self._status = f"error:{exc}"
        return self._status

    def wb_mode(self, channel: int | None = None) -> str:
        radio = self._require_radio()
        if not radio:
            return self._status
        if channel is None:
            channel = int(self._cfg.get("wb_channel", 1))
        channel = max(1, min(7, int(channel)))
        try:
            radio.power_up_wb()
            radio.wb_tune(channel)
            self._mode = "wb"
            self._status = f"wb:{channel}"
        except Exception as exc:
            self._status = f"error:{exc}"
        return self._status

    def seek_up(self) -> str:
        radio = self._require_radio()
        if not radio:
            return self._status
        try:
            if self._mode != "fm":
                self.fm_mode()
            radio.fm_seek_up()
            self._status = f"fm:{radio.get_freq_mhz():.1f}"
        except Exception as exc:
            self._status = f"error:{exc}"
        return self._status

    def seek_down(self) -> str:
        radio = self._require_radio()
        if not radio:
            return self._status
        try:
            if self._mode != "fm":
                self.fm_mode()
            radio.fm_seek_down()
            self._status = f"fm:{radio.get_freq_mhz():.1f}"
        except Exception as exc:
            self._status = f"error:{exc}"
        return self._status

    def tune_step(self, delta_mhz: float) -> str:
        radio = self._require_radio()
        if not radio:
            return self._status
        try:
            if self._mode != "fm":
                self.fm_mode()
            current = radio.get_freq_mhz()
            next_freq = max(76.0, min(108.0, current + float(delta_mhz)))
            radio.fm_tune(next_freq)
            self._status = f"fm:{radio.get_freq_mhz():.1f}"
        except Exception as exc:
            self._status = f"error:{exc}"
        return self._status

    def set_volume(self, volume: int) -> str:
        radio = self._require_radio()
        if not radio:
            return self._status
        try:
            clamped = max(0, min(63, int(volume)))
            radio.set_volume(clamped)
            self._cfg["startup_volume"] = clamped
            self._status = f"{self._mode}:vol={clamped}"
        except Exception as exc:
            self._status = f"error:{exc}"
        return self._status

    def toggle_mute(self) -> str:
        radio = self._require_radio()
        if not radio:
            return self._status
        try:
            self._muted = not self._muted
            if self._muted:
                radio.mute()
            else:
                radio.unmute()
            state = "muted" if self._muted else "unmuted"
            self._status = f"{self._mode}:{state}"
        except Exception as exc:
            self._status = f"error:{exc}"
        return self._status

    def _require_radio(self) -> Si4737 | None:
        if not bool(self._cfg.get("enabled", False)):
            self._status = "disabled"
            return None
        if self._radio is None:
            self._status = "not-started"
            return None
        if self._radio.current_func == FUNC_WB:
            self._mode = "wb"
        elif self._radio.current_func == FUNC_FM:
            self._mode = "fm"
        return self._radio
