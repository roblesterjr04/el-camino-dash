from __future__ import annotations

import time

try:
    import smbus2
except ImportError:  # pragma: no cover - hardware-specific dependency
    smbus2 = None  # type: ignore[assignment]

try:
    import RPi.GPIO as GPIO
except ImportError:  # pragma: no cover - hardware-specific dependency
    GPIO = None  # type: ignore[assignment]


CMD_POWER_UP = 0x01
CMD_GET_REV = 0x10
CMD_POWER_DOWN = 0x11
CMD_SET_PROPERTY = 0x12
CMD_FM_TUNE_FREQ = 0x20
CMD_FM_SEEK_START = 0x21
CMD_FM_TUNE_STATUS = 0x22
CMD_FM_RSQ_STATUS = 0x23
CMD_FM_RDS_STATUS = 0x24
CMD_WB_TUNE_FREQ = 0x50

PROP_FM_DEEMPHASIS = 0x1100
PROP_RX_VOLUME = 0x4000
PROP_RX_HARD_MUTE = 0x4001
PROP_FM_RDS_INT_SOURCE = 0x1500
PROP_FM_RDS_INT_FIFO_CNT = 0x1501
PROP_FM_RDS_CONFIG = 0x1502

WB_CHANNELS = {
    1: 162400,
    2: 162425,
    3: 162450,
    4: 162475,
    5: 162500,
    6: 162525,
    7: 162550,
}

FUNC_FM = 0
FUNC_WB = 3


class Si4737:
    def __init__(self, i2c_bus: int = 1, address: int = 0x11, rst_pin: int = 17) -> None:
        if smbus2 is None:
            raise RuntimeError("smbus2 not installed")
        if GPIO is None:
            raise RuntimeError("RPi.GPIO not installed")
        self.bus = smbus2.SMBus(i2c_bus)
        self.addr = address
        self.rst_pin = rst_pin
        self.current_freq = 0
        self.current_func: int | None = None
        self.volume = 45

        GPIO.setwarnings(False)
        GPIO.setmode(GPIO.BCM)
        GPIO.setup(self.rst_pin, GPIO.OUT)

    def _reset(self) -> None:
        GPIO.output(self.rst_pin, GPIO.LOW)
        time.sleep(0.1)
        GPIO.output(self.rst_pin, GPIO.HIGH)
        time.sleep(0.1)

    def _send_command(self, cmd: int, *args: int) -> None:
        data = [cmd, *list(args)]
        self.bus.write_i2c_block_data(self.addr, data[0], data[1:])
        time.sleep(0.01)

    def _read_response(self, count: int = 1) -> list[int]:
        return self.bus.read_i2c_block_data(self.addr, 0x00, count)

    def _wait_cts(self, timeout: float = 1.0) -> int:
        start = time.time()
        while time.time() - start < timeout:
            status = self._read_response(1)
            if status[0] & 0x80:
                return status[0]
            time.sleep(0.005)
        raise TimeoutError("Si4737 CTS timeout")

    def _wait_stc(self, timeout: float = 5.0) -> None:
        start = time.time()
        while time.time() - start < timeout:
            status = self._read_response(1)
            if status[0] & 0x01:
                return
            time.sleep(0.05)
        raise TimeoutError("Si4737 STC timeout")

    def _set_property(self, prop: int, value: int) -> None:
        self._send_command(
            CMD_SET_PROPERTY,
            0x00,
            (prop >> 8) & 0xFF,
            prop & 0xFF,
            (value >> 8) & 0xFF,
            value & 0xFF,
        )
        self._wait_cts()

    def power_up_fm(self) -> None:
        self._reset()
        time.sleep(0.1)
        self._send_command(CMD_POWER_UP, 0x10 | FUNC_FM, 0x05)
        self._wait_cts(timeout=2.0)
        self.current_func = FUNC_FM
        time.sleep(0.5)
        self._configure_fm()

    def power_up_wb(self) -> None:
        self._reset()
        time.sleep(0.1)
        self._send_command(CMD_POWER_UP, 0x10 | FUNC_WB, 0x05)
        self._wait_cts(timeout=2.0)
        self.current_func = FUNC_WB
        time.sleep(0.5)

    def power_down(self) -> None:
        self._send_command(CMD_POWER_DOWN)
        self._wait_cts()

    def _configure_fm(self) -> None:
        self._set_property(PROP_FM_DEEMPHASIS, 0x0002)
        self._set_property(PROP_RX_VOLUME, self.volume)
        self._set_property(PROP_FM_RDS_INT_SOURCE, 0x0001)
        self._set_property(PROP_FM_RDS_INT_FIFO_CNT, 0x0004)
        self._set_property(PROP_FM_RDS_CONFIG, 0xFF01)

    def fm_tune(self, freq_mhz: float) -> None:
        freq = int(freq_mhz * 100)
        self._send_command(
            CMD_FM_TUNE_FREQ,
            0x00,
            (freq >> 8) & 0xFF,
            freq & 0xFF,
            0x00,
        )
        self._wait_cts()
        self._wait_stc()
        self.current_freq = freq

    def fm_seek_up(self) -> None:
        self._send_command(CMD_FM_SEEK_START, 0x0C)
        self._wait_cts()
        self._wait_stc(timeout=5.0)
        self._update_freq()

    def fm_seek_down(self) -> None:
        self._send_command(CMD_FM_SEEK_START, 0x04)
        self._wait_cts()
        self._wait_stc(timeout=5.0)
        self._update_freq()

    def _update_freq(self) -> None:
        self._send_command(CMD_FM_TUNE_STATUS, 0x01)
        self._wait_cts()
        resp = self._read_response(8)
        self.current_freq = (resp[2] << 8) | resp[3]

    def wb_tune(self, channel: int = 1) -> None:
        freq = WB_CHANNELS.get(channel, WB_CHANNELS[1])
        self._send_command(CMD_WB_TUNE_FREQ, 0x00, (freq >> 8) & 0xFF, freq & 0xFF)
        self._wait_cts()
        self._wait_stc()

    def set_volume(self, vol: int) -> None:
        self.volume = max(0, min(63, int(vol)))
        self._set_property(PROP_RX_VOLUME, self.volume)

    def mute(self, left: bool = True, right: bool = True) -> None:
        val = (0x02 if left else 0) | (0x01 if right else 0)
        self._set_property(PROP_RX_HARD_MUTE, val)

    def unmute(self) -> None:
        self._set_property(PROP_RX_HARD_MUTE, 0x0000)

    def get_rsq(self) -> dict[str, bool | int | float]:
        self._send_command(CMD_FM_RSQ_STATUS, 0x01)
        self._wait_cts()
        resp = self._read_response(8)
        return {
            "stereo": bool(resp[3] & 0x80),
            "rssi": resp[4],
            "snr": resp[5],
            "freq_mhz": self.current_freq / 100.0,
        }

    def get_rds(self) -> dict[str, bool | int]:
        self._send_command(CMD_FM_RDS_STATUS, 0x01)
        self._wait_cts()
        resp = self._read_response(13)
        block_a = (resp[4] << 8) | resp[5]
        block_b = (resp[6] << 8) | resp[7]
        block_c = (resp[8] << 8) | resp[9]
        block_d = (resp[10] << 8) | resp[11]
        return {
            "sync": bool(resp[2] & 0x01),
            "block_a": block_a,
            "block_b": block_b,
            "block_c": block_c,
            "block_d": block_d,
        }

    def get_freq_mhz(self) -> float:
        return self.current_freq / 100.0

    def get_revision(self) -> dict[str, str | int]:
        self._send_command(CMD_GET_REV)
        self._wait_cts()
        resp = self._read_response(9)
        return {
            "part": f"Si47{resp[1]}",
            "firmware": f"{resp[2]}.{resp[3]}",
            "patch": (resp[4] << 8) | resp[5],
            "component": f"{resp[6]}.{resp[7]}",
            "chip_rev": chr(resp[8]) if resp[8] else "?",
        }

    def close(self) -> None:
        try:
            self.power_down()
        except Exception:
            pass
        self.bus.close()
