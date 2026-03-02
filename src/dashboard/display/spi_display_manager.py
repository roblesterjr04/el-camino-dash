from __future__ import annotations

from dataclasses import dataclass
import logging
import threading
import time
from typing import Any

import pygame
from PIL import Image, ImageDraw

from dashboard.display.gc9a01_driver import GC9A01


LOG = logging.getLogger(__name__)


@dataclass
class _DisplayThreadState:
    index: int
    label: str
    width: int
    height: int
    shape: str
    driver: GC9A01
    worker: threading.Thread
    event: threading.Event
    lock: threading.Lock
    back_buffer: Image.Image | None = None
    should_stop: bool = False
    initialized: bool = False
    reinit_attempted: bool = False


class SPIDisplayManager:
    def __init__(self, displays: list[dict[str, Any]], speed_hz: int = 8_000_000) -> None:
        self._speed_hz = int(speed_hz)
        self._states: list[_DisplayThreadState] = []
        self._mask_cache: dict[tuple[int, int], Image.Image] = {}
        self._black_cache: dict[tuple[int, int], Image.Image] = {}
        self._build_states(displays)

    def _build_states(self, displays: list[dict[str, Any]]) -> None:
        for idx, display in enumerate(displays):
            width = int(display["width"])
            height = int(display["height"])
            label = str(display.get("label", f"Display {idx + 1}"))
            shape = str(display.get("shape", "round")).strip().lower()
            driver = GC9A01(
                spi_bus=int(display["spi_bus"]),
                spi_device=int(display["spi_device"]),
                dc_pin=int(display["dc_pin"]),
                rst_pin=int(display["rst_pin"]),
                speed_hz=int(display.get("speed_hz", self._speed_hz)),
                width=width,
                height=height,
                rotation=int(display.get("rotation", 0)),
                madctl=display.get("madctl"),
                colmod=int(display.get("colmod", 0x55)),
            )
            event = threading.Event()
            lock = threading.Lock()
            worker = threading.Thread(
                target=self._worker_loop,
                args=(idx,),
                name=f"spi-display-{idx}",
                daemon=True,
            )
            state = _DisplayThreadState(
                index=idx,
                label=label,
                width=width,
                height=height,
                shape=shape,
                driver=driver,
                worker=worker,
                event=event,
                lock=lock,
            )
            self._states.append(state)

        for state in self._states:
            self._try_init_display(state)
            if state.initialized:
                try:
                    # Pre-clear to avoid startup residual artifacts on some GC9A01 modules.
                    state.driver.write_frame(Image.new("RGB", (state.width, state.height), (0, 0, 0)))
                except Exception as exc:
                    LOG.debug("[spi] pre-clear failed for %s: %s", state.label, exc)
            state.worker.start()

    def _try_init_display(self, state: _DisplayThreadState) -> None:
        try:
            state.driver.init()
            state.initialized = True
            state.reinit_attempted = False
            LOG.info("[spi] initialized %s", state.label)
        except Exception as exc:
            state.initialized = False
            LOG.warning("[spi] failed to initialize %s: %s", state.label, exc)

    def _worker_loop(self, idx: int) -> None:
        state = self._states[idx]
        while not state.should_stop:
            state.event.wait(timeout=0.25)
            if state.should_stop:
                break
            if not state.event.is_set():
                continue
            frame = None
            with state.lock:
                frame = state.back_buffer
                state.back_buffer = None
                state.event.clear()
            if frame is None:
                continue
            if not state.initialized:
                self._try_init_display(state)
                if not state.initialized:
                    continue

            started = time.monotonic()
            try:
                state.driver.write_frame(frame)
                state.reinit_attempted = False
            except Exception as exc:
                LOG.warning("[spi] frame write failed on %s: %s", state.label, exc)
                state.initialized = False
                continue
            elapsed = time.monotonic() - started
            if elapsed > 2.0 and not state.reinit_attempted:
                LOG.warning("[spi] slow frame write on %s (%.2fs), attempting re-init", state.label, elapsed)
                state.reinit_attempted = True
                try:
                    state.driver.cleanup()
                except Exception:
                    pass
                self._try_init_display(state)

        try:
            state.driver.cleanup()
        except Exception as exc:
            LOG.debug("[spi] cleanup failed for %s: %s", state.label, exc)

    def _mask_for(self, size: tuple[int, int]) -> Image.Image:
        if size in self._mask_cache:
            return self._mask_cache[size]
        width, height = size
        mask = Image.new("L", size, 0)
        draw = ImageDraw.Draw(mask)
        draw.ellipse([0, 0, width - 1, height - 1], fill=255)
        self._mask_cache[size] = mask
        return mask

    def _black_for(self, size: tuple[int, int]) -> Image.Image:
        if size in self._black_cache:
            return self._black_cache[size]
        black = Image.new("RGB", size, (0, 0, 0))
        self._black_cache[size] = black
        return black

    def update(self, display_index: int, pygame_surface: pygame.Surface) -> None:
        if display_index < 0 or display_index >= len(self._states):
            return
        state = self._states[display_index]
        size = (state.width, state.height)
        try:
            rgb = pygame.image.tostring(pygame_surface, "RGB")
            frame = Image.frombytes("RGB", pygame_surface.get_size(), rgb)
            if frame.size != size:
                frame = frame.resize(size, Image.BILINEAR)
            if state.shape == "round":
                frame = Image.composite(frame, self._black_for(size), self._mask_for(size))
        except Exception as exc:
            LOG.warning("[spi] failed to convert frame for %s: %s", state.label, exc)
            return

        with state.lock:
            state.back_buffer = frame
            state.event.set()

    def shutdown(self) -> None:
        for state in self._states:
            state.should_stop = True
            state.event.set()
        for state in self._states:
            if state.worker.is_alive():
                state.worker.join(timeout=1.0)
