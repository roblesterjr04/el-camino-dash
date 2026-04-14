#!/usr/bin/env python3
"""
Fullscreen X11 pattern for triple-round HDMI: left column | center overlap | right column.

On the Pi (SSH): do not stop gauge-cluster-hdmi-x11-extended — that tears down xinit/Xorg.
Allow user pi to use :0, then run as pi (same as the dashboard client):

  sudo DISPLAY=:0 xhost +SI:localuser:pi
  sudo runuser -u pi -- env DISPLAY=:0 SDL_VIDEODRIVER=x11 HOME=/home/pi \\
    ELC_TEST_TIMEOUT_S=30 python3 /opt/el-camino-dash/scripts/test_triple_hdmi_pattern.py

Optional: sudo DISPLAY=:0 xhost -SI:localuser:pi  # revoke after testing

Quit: Esc or Q, or wait for ELC_TEST_TIMEOUT_S (default 60).
"""
from __future__ import annotations

import os
import sys

# Must set before pygame.display
os.environ.setdefault("SDL_VIDEODRIVER", "x11")

import pygame


def main() -> None:
    dw = int(os.environ.get("ELC_TEST_WIDTH", "2160"))
    dh = int(os.environ.get("ELC_TEST_HEIGHT", "1080"))
    ref = int(os.environ.get("ELC_TEST_CANVAS_REF", "2880"))
    timeout_s = float(os.environ.get("ELC_TEST_TIMEOUT_S", "60"))

    center_x = (1080 * dw) // max(1, ref)

    pygame.init()
    pygame.display.set_caption("ELC triple HDMI test")
    flags = pygame.NOFRAME
    os.environ.setdefault("SDL_VIDEO_WINDOW_POS", "0,0")
    screen = pygame.display.set_mode((dw, dh), flags)
    font = pygame.font.Font(None, 72)
    small = pygame.font.Font(None, 36)

    t0 = pygame.time.get_ticks()
    clock = pygame.time.Clock()

    while True:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                pygame.quit()
                sys.exit(0)
            if event.type == pygame.KEYDOWN and event.key in (pygame.K_ESCAPE, pygame.K_q):
                pygame.quit()
                sys.exit(0)

        if timeout_s > 0 and (pygame.time.get_ticks() - t0) / 1000.0 >= timeout_s:
            pygame.quit()
            sys.exit(0)

        screen.fill((20, 20, 20))
        # Left 5" strip (splitter column 0–1079)
        pygame.draw.rect(screen, (140, 30, 30), (0, 0, 1080, dh))
        # Right 5" strip (splitter column 1080–2159)
        pygame.draw.rect(screen, (30, 30, 140), (1080, 0, 1080, dh))
        # Center 4" overlap (drawn on top)
        pygame.draw.rect(screen, (30, 120, 40), (center_x, 0, 720, dh))

        def label(txt: str, x: int, y: int, color: tuple[int, int, int]) -> None:
            surf = font.render(txt, True, color)
            screen.blit(surf, (x, y))

        label("LEFT 5\"", 40, dh // 2 - 80, (255, 255, 255))
        label(f"CENTER 4\" @x={center_x}", center_x + 20, dh // 2 - 80, (255, 255, 255))
        label("RIGHT 5\"", 1080 + 40, dh // 2 - 80, (255, 255, 255))

        info = small.render(f"{dw}x{dh}  ref={ref}  center_x={center_x}", True, (200, 200, 200))
        screen.blit(info, (20, 20))

        pygame.display.flip()
        clock.tick(30)

    pygame.quit()


if __name__ == "__main__":
    main()
