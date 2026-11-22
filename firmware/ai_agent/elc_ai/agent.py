"""Runs the assistant: the serial link to the card, the backends from the config, the ALIVE line, the shutdown."""
from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from typing import Optional

from . import __version__
from . import link as L
from .config import Config
from .connectivity import Connectivity
from .context import VehicleContext
from .dialog import Assistant
from .llm import AnthropicOnline, LlamaServer
from .retrieval import Manual
from .stt import WhisperServerStt
from .tts import FakeTts, PiperTts


class SerialLink:
    """The card's UART. pyserial when it is installed, else the tty with termios (Linux)."""

    def __init__(self, port: str, baud: int):
        try:
            import serial  # type: ignore
            self._s = serial.Serial(port, baud, timeout=0.2)
            self._read = lambda: self._s.read(512)
            self._write = self._s.write
        except ImportError:
            import termios
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
            attrs = termios.tcgetattr(fd)
            speed = getattr(termios, f"B{baud}")
            attrs[0] = attrs[1] = attrs[3] = 0
            attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            attrs[4] = attrs[5] = speed
            attrs[6][termios.VMIN], attrs[6][termios.VTIME] = 0, 2
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            self._read = lambda: os.read(fd, 512)
            self._write = lambda b: os.write(fd, b)

    def read(self) -> bytes:
        return self._read()

    def write(self, b: bytes) -> None:
        self._write(b)


def run_cmd(cmd: str) -> None:
    if cmd:
        subprocess.run(shlex.split(cmd), check=False, timeout=10)


def soc_temp_c() -> int:
    try:
        with open("/sys/class/thermal/thermal_zone0/temp") as f:
            return int(f.read().strip()) // 1000
    except (OSError, ValueError):
        return 0


def http_probe(url: str):
    def probe() -> bool:
        try:
            urllib.request.urlopen(urllib.request.Request(url, method="HEAD"), timeout=1.5)
            return True
        except urllib.error.HTTPError:
            return True  # any HTTP answer means the network works
        except Exception:
            return False
    return probe


def build(cfg: Config, send, on_shutdown) -> Assistant:
    ctx = VehicleContext()
    tts = PiperTts(cfg.tts_model, cfg.tts_rate, cfg.tts_binary) if cfg.tts_model else FakeTts(16000)
    online = None
    if cfg.online_enabled:
        online = AnthropicOnline(model=cfg.online_model, effort=cfg.online_effort, web_search=cfg.online_web_search)
    manual = None
    if cfg.manual_dir:
        manual = Manual()
        manual.add_dir(cfg.manual_dir)
    return Assistant(send, ctx, WhisperServerStt(cfg.stt_url, prompt=cfg.stt_prompt), tts,
                     LlamaServer(cfg.local_url, cfg.local_model), online,
                     Connectivity(http_probe(cfg.online_probe_url)), cfg, manual, soc_temp_c, on_shutdown)


def main(argv: Optional[list] = None) -> int:
    ap = argparse.ArgumentParser(prog="elc-ai", description="The AI card's assistant (runs on the compute module).")
    ap.add_argument("--config", help="config.toml (see config.example.toml)")
    ap.add_argument("--bench", metavar="WAV", help="time each stage once (speech to text on a 16 kHz mono WAV, the model, the voice), then exit")
    ap.add_argument("--version", action="version", version=__version__)
    a = ap.parse_args(argv)
    cfg = Config.load(a.config)
    if a.bench:
        return bench(cfg, a.bench)

    link = SerialLink(cfg.port, cfg.baud)
    stopping = threading.Event()

    def on_shutdown(reason: int) -> None:
        stopping.set()
        run_cmd(cfg.shutdown_cmd)

    asst = build(cfg, link.write, on_shutdown)
    parser = L.Parser()
    run_cmd(cfg.alive_on_cmd)  # ALIVE high: the card's sequencer waits for it
    asst.start()

    def tick() -> None:
        while not stopping.is_set():
            asst.conn.check()
            time.sleep(1.0)
    threading.Thread(target=tick, daemon=True).start()
    try:
        while not stopping.is_set():
            data = link.read()
            for f in parser.feed(data):
                asst.on_frame(f)
    except KeyboardInterrupt:
        pass
    finally:
        asst.stop()
        run_cmd(cfg.alive_off_cmd)
    return 0


def bench(cfg: Config, wav_path: str) -> int:
    import wave
    from .router import LOCAL
    with wave.open(wav_path, "rb") as w:
        if w.getframerate() != 16000 or w.getnchannels() != 1 or w.getsampwidth() != 2:
            print("the WAV must be 16 kHz, mono, 16-bit", file=sys.stderr)
            return 2
        pcm = w.readframes(w.getnframes())
    secs = len(pcm) / 32000
    stt = WhisperServerStt(cfg.stt_url, prompt=cfg.stt_prompt)
    t0 = time.monotonic()
    text = stt.transcribe(pcm)
    dt = time.monotonic() - t0
    print(f"speech to text: {dt:.2f} s for {secs:.1f} s of audio (x{dt / secs:.2f} real time): {text!r}")
    llm = LlamaServer(cfg.local_url, cfg.local_model)
    t0 = time.monotonic()
    first, n = None, 0
    for piece in llm.stream("Answer in one short sentence.", [], text or "What is an El Camino?", cfg.local_max_tokens):
        if first is None:
            first = time.monotonic() - t0
        n += 1
    tot = time.monotonic() - t0
    print(f"local model: first piece after {first or 0:.2f} s, {n} pieces in {tot:.2f} s ({n / max(tot - (first or 0), 1e-3):.1f} pieces/s)")
    if cfg.tts_model:
        tts = PiperTts(cfg.tts_model, cfg.tts_rate, cfg.tts_binary)
        t0 = time.monotonic()
        out = tts.synth("The oil pressure is forty two psi.")
        dt = time.monotonic() - t0
        print(f"voice: {dt:.2f} s for {len(out) / 2 / cfg.tts_rate:.1f} s of speech")
    return 0


if __name__ == "__main__":
    sys.exit(main())
