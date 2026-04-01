#!/usr/bin/env python3
"""
Solid-color flash on each X11 screen (requires pygame + running X with RandR).

  DISPLAY=:0 SDL_VIDEODRIVER=x11 python3 /opt/el-camino-dash/scripts/test_hdmi_pygame_x11.py

Stop gauge-cluster-hdmi first if it owns the tty; use x11-extended or a manual xinit session.
"""
from __future__ import annotations

import os
import sys
import time

os.environ.setdefault("DISPLAY", ":0")
os.environ["SDL_VIDEODRIVER"] = "x11"

DISPLAY_SEC = 4.0


def main() -> int:
    try:
        import pygame
    except ImportError:
        print("pygame not available", file=sys.stderr)
        return 1

    pygame.init()
    try:
        sizes = pygame.display.get_desktop_sizes()
    except Exception:
        sizes = [(640, 480)]
    n = len(sizes) if sizes else 1
    print(f"[elc-test] pygame sees {n} desktop(s): {sizes}", flush=True)

    colors = [(220, 50, 50), (50, 200, 50), (50, 80, 220)]
    for idx in range(n):
        w, h = sizes[idx][0], sizes[idx][1]
        w = max(320, min(w, 1280))
        h = max(240, min(h, 1080))
        try:
            pygame.display.set_mode((w, h), pygame.NOFRAME, display=idx)
        except TypeError:
            pygame.display.set_mode((w, h), pygame.NOFRAME)
        except pygame.error as e:
            print(f"[elc-test] display {idx}: {e}", flush=True)
            continue
        surf = pygame.display.get_surface()
        surf.fill(colors[idx % len(colors)])
        pygame.display.flip()
        print(f"[elc-test] display {idx}: {w}x{h} color {colors[idx % len(colors)]} ({DISPLAY_SEC}s)", flush=True)
        t0 = time.monotonic()
        while time.monotonic() - t0 < DISPLAY_SEC:
            for ev in pygame.event.get():
                if ev.type == pygame.QUIT:
                    pygame.quit()
                    return 0
            time.sleep(0.05)

    pygame.quit()
    print("[elc-test] done", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
