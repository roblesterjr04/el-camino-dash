#!/usr/bin/env python3
"""p4_emulator.py — ESP32-P4 Round Gauge Emulator

Renders three 480×480 round displays side-by-side, faithfully matching the firmware.

  Left:   Speedometer      firmware/esp32_p4_speedo/
  Center: Tach/Clock/Media firmware/esp32_p4_center/
  Right:  TSPS gauges      firmware/esp32_p4_tsps/

Rendering order matches firmware:
  bg → dial → needle(s) → overlay
  (speedo_empty.png is 100% opaque, so needle must go on top of it)

Pivot notes
-----------
  Speedometer needle uses needle.png rotated about the gauge centre (50%×50% of
  the 480×480 buffer), matching ``gauge_screen`` and firmware speedo_cluster.

Clock hands
-----------
  The sprite clock (needle.png, 112×654 px) causes distortion at clock-hand scales.
  The emulator uses the firmware's line-hand fallback instead, which is cleaner and
  lets the clock_face.png overlay do its job without interference.

Controls
--------
  Mouse click on center gauge     → short tap (open / close overlay menu)
  Hold  T  (keyboard):
        0.5 s release             → cycle Center view (Tach→Clock→Media)   [≈ 3 s hold on device]
        1.5 s release             → toggle Classic / Fallout theme          [≈ 10 s hold on device]
  R                               → toggle TSPS Classic 1 / Classic 2
  1 / 2 / 3                       → set Center to Tach / Clock / Media directly
  F                               → force Fallout theme toggle (instant)
  Q / Esc                         → quit

Run
---
  .venv/bin/python p4_emulator.py
"""

from __future__ import annotations

import json
import math
import sys
from datetime import datetime
from pathlib import Path
from time import monotonic
from typing import NamedTuple

import pygame
import pygame.math

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
ROOT      = Path(__file__).resolve().parent
ASSETS    = ROOT / "assets" / "gauges"
STATE_FILE = ROOT / ".p4_emulator_state.json"

# ---------------------------------------------------------------------------
# Firmware constants
# ---------------------------------------------------------------------------
CANVAS     = 480          # SPEEDO/CENTER/TSPS_ASSET_EDGE
LAYOUT_REF = 1080
LS         = CANVAS / LAYOUT_REF   # ≈ 0.4444

# Pivot Y: firmware uses 0.42*CANVAS≈201, but dial PNGs centre at 50%.
# Use 50% here so the needle aligns visually with the dial artwork.
# FIRMWARE FIX: change 0.42f → 0.5f in speedo_cluster.cpp / center_cluster.cpp.
PIVOT_Y = CANVAS // 2   # 240

# BG matches firmware BG_565 = 0x0000 (#000)
BG = (0, 0, 0)

# Needle colours decoded from firmware 565 constants
NEEDLE_SPEEDO = (int(31*255//31), int(10*255//63), int(8*255//31))   # 0xF948 ≈ (255,81,66)
NEEDLE_TACH   = (int(31*255//31), int(6*255//63),  int(5*255//31))   # (255,48,41)
NEEDLE_TSPS   = (255, 52, 52)

# Clock hand colour decoded from firmware wcol
# ((224>>3)<<11)|((76>>2)<<5)|(56>>3) — rgb888 components preserved well
CLOCK_COLOR   = (224, 76, 56)   # warm reddish orange

# Fallout palette
FO_GREEN = (0, 255, 60)
FO_DIM   = (0, 90, 30)

# Overlay menu colours  (p4_overlay_menu.cpp)
MENU_PANEL    = (0, 0, 0)
MENU_BORDER   = (120, 120, 120)
MENU_BTN_BASE = (44,  44,  44)
MENU_BTN_HI   = (72,  72,  72)
MENU_TXT_DIM  = (200, 200, 200)
MENU_TXT_HI   = (255, 255, 255)

# kLayoutInset=120 at 720 px → scaled to our 480 canvas
MENU_INSET = round(120 / 720 * CANVAS)   # ≈ 80 px

# Window layout
GAP    = 18
MARGIN = 20
WIN_W  = MARGIN * 2 + CANVAS * 3 + GAP * 2
WIN_H  = MARGIN * 2 + CANVAS + 56

# Key-hold thresholds (seconds) — compressed from device's 3 s / 10 s
HOLD_VIEW_S  = 0.5   # → cycle center view
HOLD_THEME_S = 1.5   # → toggle theme

# ---------------------------------------------------------------------------
# State persistence
# ---------------------------------------------------------------------------

def _load_state() -> dict:
    if STATE_FILE.exists():
        try:
            return json.loads(STATE_FILE.read_text())
        except Exception:
            pass
    return {}


def _save_state(theme: str, center_view: str, tsps_mode: str) -> None:
    try:
        STATE_FILE.write_text(json.dumps(
            {"theme": theme, "center_view": center_view, "tsps_mode": tsps_mode},
            indent=2))
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Angle math — identical to firmware
# ---------------------------------------------------------------------------

def _map_angle(ratio: float, a0: float, a1: float) -> float:
    ratio = max(0.0, min(1.0, ratio))
    start = a0 % 360.0;
    if start < 0: start += 360.0
    end = a1 % 360.0;
    if end   < 0: end   += 360.0
    span = (end - start) % 360.0;
    if span  < 0: span  += 360.0
    return (start + span * ratio) % 360.0


def _user_to_math_rad(user: float) -> float:
    """User angle (0=bottom/6-o'clock, increases CW) → math radians for trig."""
    return math.radians((270.0 - user) % 360.0)


def _needle_thick(line_w: float, cap: int = 6) -> int:
    return max(1, min(cap, math.ceil(line_w * LS / 3.0)))


# ---------------------------------------------------------------------------
# Drawing helpers
# ---------------------------------------------------------------------------

def _thick_line(surf: pygame.Surface, color: tuple,
                p0: tuple[int, int], p1: tuple[int, int], thick: int) -> None:
    """Rounded-cap thick line matching firmware draw_line_thick."""
    w = max(1, thick * 2 + 1)
    pygame.draw.line(surf, color, p0, p1, width=w)
    pygame.draw.circle(surf, color, p0, thick)
    pygame.draw.circle(surf, color, p1, thick)


def _sample_bilinear_rgba(sprite: pygame.Surface, sx: float, sy: float) -> tuple[int, int, int, int]:
    sw, sh = sprite.get_size()
    if sx < -0.5 or sy < -0.5 or sx > sw - 0.5 or sy > sh - 0.5:
        return (0, 0, 0, 0)
    sx = max(0.0, min(float(sw - 1), sx))
    sy = max(0.0, min(float(sh - 1), sy))
    x0 = int(math.floor(sx))
    y0 = int(math.floor(sy))
    x1 = min(x0 + 1, sw - 1)
    y1 = min(y0 + 1, sh - 1)
    fx = sx - x0
    fy = sy - y0
    w00 = (1.0 - fx) * (1.0 - fy)
    w10 = fx * (1.0 - fy)
    w01 = (1.0 - fx) * fy
    w11 = fx * fy
    c00 = sprite.get_at((x0, y0))
    c10 = sprite.get_at((x1, y0))
    c01 = sprite.get_at((x0, y1))
    c11 = sprite.get_at((x1, y1))
    r = w00 * c00.r + w10 * c10.r + w01 * c01.r + w11 * c11.r
    g = w00 * c00.g + w10 * c10.g + w01 * c01.g + w11 * c11.g
    b = w00 * c00.b + w10 * c10.b + w01 * c01.b + w11 * c11.b
    a = w00 * c00.a + w10 * c10.a + w01 * c01.a + w11 * c11.a
    if a < 4.0:
        return (0, 0, 0, 0)
    return (int(r + 0.5), int(g + 0.5), int(b + 0.5), int(a + 0.5))


def _blend_pixel_rgba_over(surf: pygame.Surface, x: int, y: int, sr: int, sg: int, sb: int, sa: int) -> None:
    if sa < 4:
        return
    dc = surf.get_at((x, y))
    dr, dg, db = int(dc[0]), int(dc[1]), int(dc[2])
    if sa > 250:
        surf.set_at((x, y), (sr, sg, sb))
        return
    ia = 255 - sa
    surf.set_at(
        (x, y),
        ((sr * sa + dr * ia) // 255, (sg * sa + dg * ia) // 255, (sb * sa + db * ia) // 255),
    )


def _blit_rotated_about_pivot(
    surf: pygame.Surface,
    sprite: pygame.Surface,
    world_pivot: tuple[int, int],
    local_pivot: tuple[float, float],
    angle_deg: float,
) -> None:
    """Inverse map + bilinear; same convention as GaugeScreen._blit_rotated_about_pivot (center_cluster wy)."""
    sw, sh = sprite.get_size()
    if sw < 2 or sh < 2:
        return
    px, py = local_pivot
    hub_x, hub_y = float(world_pivot[0]), float(world_pivot[1])
    th = math.radians(angle_deg)
    co, si = math.cos(th), math.sin(th)
    wlim, hlim = surf.get_size()
    corners_ix = (0.0, float(sw - 1), 0.0, float(sw - 1))
    corners_iy = (0.0, 0.0, float(sh - 1), float(sh - 1))
    min_wx = min_wy = 1e9
    max_wx = max_wy = -1e9
    for ix, iy in zip(corners_ix, corners_iy):
        dx, dy = ix - px, iy - py
        wx = hub_x + dx * co - dy * si
        wy = hub_y - (dx * si + dy * co)
        min_wx = min(min_wx, wx)
        max_wx = max(max_wx, wx)
        min_wy = min(min_wy, wy)
        max_wy = max(max_wy, wy)
    x0 = max(0, int(math.floor(min_wx)) - 1)
    x1 = min(wlim - 1, int(math.ceil(max_wx)) + 1)
    y0 = max(0, int(math.floor(min_wy)) - 1)
    y1 = min(hlim - 1, int(math.ceil(max_wy)) + 1)
    for wy in range(y0, y1 + 1):
        for wx in range(x0, x1 + 1):
            vxf = float(wx) + 0.5 - hub_x
            vyf = float(wy) + 0.5 - hub_y
            dx = co * vxf - si * vyf
            dyf = -si * vxf - co * vyf
            sx = dx + px
            sy = dyf + py
            r, g, b, a = _sample_bilinear_rgba(sprite, sx, sy)
            if a >= 4:
                _blend_pixel_rgba_over(surf, wx, wy, r, g, b, a)


def _blit_rotated_speedo_needle_fw(
    surf: pygame.Surface,
    sprite: pygame.Surface,
    world_pivot: tuple[int, int],
    local_pivot: tuple[float, float],
    angle_deg: float,
) -> None:
    """Matches firmware speedo_cluster: inverse map + bilinear (no diagonal holes)."""
    sw, sh = sprite.get_size()
    if sw < 2 or sh < 2:
        return
    px, py = local_pivot
    hub_x, hub_y = float(world_pivot[0]), float(world_pivot[1])
    th = math.radians(angle_deg)
    co, si = math.cos(th), math.sin(th)
    wlim, hlim = surf.get_size()
    corners_ix = (0.0, float(sw - 1), 0.0, float(sw - 1))
    corners_iy = (0.0, 0.0, float(sh - 1), float(sh - 1))
    min_wx = min_wy = 1e9
    max_wx = max_wy = -1e9
    for ix, iy in zip(corners_ix, corners_iy):
        dx, dy = ix - px, iy - py
        wx = hub_x + dx * co - dy * si
        wy = hub_y + dx * si + dy * co
        min_wx = min(min_wx, wx)
        max_wx = max(max_wx, wx)
        min_wy = min(min_wy, wy)
        max_wy = max(max_wy, wy)
    x0 = max(0, int(math.floor(min_wx)) - 1)
    x1 = min(wlim - 1, int(math.ceil(max_wx)) + 1)
    y0 = max(0, int(math.floor(min_wy)) - 1)
    y1 = min(hlim - 1, int(math.ceil(max_wy)) + 1)
    for wy in range(y0, y1 + 1):
        for wx in range(x0, x1 + 1):
            vxf = float(wx) + 0.5 - hub_x
            vyf = float(wy) + 0.5 - hub_y
            dx = vxf * co + vyf * si
            dyf = -vxf * si + vyf * co
            sx = dx + px
            sy = dyf + py
            r, g, b, a = _sample_bilinear_rgba(sprite, sx, sy)
            if a >= 4:
                _blend_pixel_rgba_over(surf, wx, wy, r, g, b, a)


def _draw_gauge_needle(surf: pygame.Surface, color: tuple,
                       cx: int, cy: int, length: int,
                       ratio: float, a0: float, a1: float, thick: int,
                       invert: bool = False) -> None:
    if invert:
        ratio = 1.0 - ratio
    ang  = _map_angle(ratio, a0, a1)
    r    = _user_to_math_rad(ang)
    tip  = (int(cx + math.cos(r) * length),
            int(cy - math.sin(r) * length))
    _thick_line(surf, color, (cx, cy), tip, thick)


# Pre-built circular corner mask (black outside inscribed circle, transparent inside).
_CORNER_MASK: pygame.Surface | None = None


def _ensure_corner_mask() -> pygame.Surface:
    global _CORNER_MASK
    if _CORNER_MASK is None:
        s = pygame.Surface((CANVAS, CANVAS))
        s.fill((0, 0, 0))
        pygame.draw.circle(s, (255, 0, 255), (CANVAS // 2, CANVAS // 2), CANVAS // 2)
        s.set_colorkey((255, 0, 255))
        _CORNER_MASK = s
    return _CORNER_MASK


def _apply_round_mask(surf: pygame.Surface) -> None:
    """Black out corners outside inscribed circle (matches p4_round_mask)."""
    surf.blit(_ensure_corner_mask(), (0, 0))


def _seg_bar(surf: pygame.Surface, x: int, y: int, n: int, pct: float,
             on_c: tuple, off_c: tuple, seg_w: int = 14, gap: int = 3,
             seg_h: int = 10) -> None:
    fi = max(0, min(n, int(pct / 100.0 * n)))
    for i in range(n):
        pygame.draw.rect(surf, on_c if i < fi else off_c,
                         (x + i * (seg_w + gap), y, seg_w, seg_h))


# ---------------------------------------------------------------------------
# Simulated sensor data
# ---------------------------------------------------------------------------

class SimData:
    def __init__(self) -> None:
        self._t = 0.0
        self._odo_miles = 12345.0

    def tick(self, dt: float) -> None:
        mph_now = max(0.0, 70.0 + 62.0 * math.sin(self._t * 0.28))
        self._odo_miles += mph_now * (dt / 3600.0)
        self._t += dt

    @property
    def mph(self) -> float:
        return max(0.0, 70.0 + 62.0 * math.sin(self._t * 0.28))

    @property
    def odo_miles(self) -> float:
        return self._odo_miles

    @property
    def rpm(self) -> float:
        return max(600.0, 2500.0 + 1800.0 * math.sin(self._t * 0.38 + 1.0))

    @property
    def fuel_pct(self) -> float:
        return max(4.0, 55.0 + 34.0 * math.sin(self._t * 0.11 + 0.5))

    @property
    def oil_psi(self) -> float:
        return max(5.0, 42.0 + 14.0 * math.sin(self._t * 0.19 + 2.0))

    @property
    def coolant_f(self) -> float:
        return 195.0 + 18.0 * math.sin(self._t * 0.10 + 1.5)

    @property
    def amps(self) -> float:
        return 4.0 + 9.0 * math.sin(self._t * 0.31 + 3.0)

    @property
    def gear(self) -> int:
        return min(3, max(1, int(self.mph / 47) + 1))

    @property
    def track(self) -> str:
        return "The Allman Brothers Band - Ramblin' Man"

    @property
    def media_elapsed_s(self) -> int:
        return int(self._t) % 212

    @property
    def media_duration_s(self) -> int:
        return 212

    @property
    def media_vol_u4(self) -> int:
        return 10


# ---------------------------------------------------------------------------
# Asset loading
# ---------------------------------------------------------------------------

class GaugeAssets:
    def __init__(self) -> None:
        self._cache: dict[str, pygame.Surface | None] = {}

    def _load(self, path: Path, size: int = CANVAS) -> pygame.Surface | None:
        key = f"{path}@{size}"
        if key in self._cache:
            return self._cache[key]
        surf = None
        if path.exists():
            try:
                img  = pygame.image.load(str(path)).convert_alpha()
                surf = pygame.transform.smoothscale(img, (size, size))
            except Exception as exc:
                print(f"[assets] {path.name}: {exc}", file=sys.stderr)
        else:
            print(f"[assets] not found: {path}", file=sys.stderr)
        self._cache[key] = surf
        return surf

    def _first(self, *paths: Path, size: int = CANVAS) -> pygame.Surface | None:
        for p in paths:
            s = self._load(p, size)
            if s is not None:
                return s
        return None

    # ── Speedo ──
    @property
    def speedo_dial(self):
        return self._load(ASSETS / "classic_2_rect" / "speedo_empty.png")

    @property
    def speedo_overlay(self):
        return self._load(ASSETS / "classic_2_rect" / "speedo_mask.png")

    @property
    def speedo_needle(self) -> pygame.Surface | None:
        """Scaled like firmware export (native_scale ~0.32 × min(buffer))."""
        key = "speedo_needle_scaled"
        if key in self._cache:
            return self._cache[key]
        path = ASSETS / "classic_2_rect" / "needle.png"
        if not path.exists():
            print(f"[assets] not found: {path}", file=sys.stderr)
            self._cache[key] = None
            return None
        try:
            img = pygame.image.load(str(path)).convert_alpha()
        except Exception as exc:
            print(f"[assets] {path.name}: {exc}", file=sys.stderr)
            self._cache[key] = None
            return None
        nw, nh = img.get_size()
        target = max(8, int(round(CANVAS * 0.32)))
        if nw >= nh:
            new_w = target
            new_h = max(1, int(round(nh * target / nw)))
        else:
            new_h = target
            new_w = max(1, int(round(nw * target / nh)))
        out = pygame.transform.smoothscale(img, (new_w, new_h))
        self._cache[key] = out
        return out

    # ── Center – tach ──
    @property
    def tach_dial(self):
        return self._first(ASSETS / "classic_1" / "tach_base.png",
                           ASSETS / "classic_2_rect" / "tach_base.png")

    # ── Center – clock ──
    # Note: needle.png is 112×654 px — too distorted as clock hand sprite.
    # The emulator uses firmware's line-hand fallback instead.
    @property
    def clock_dial_cover(self):
        half = CANVAS // 2
        return self._first(ASSETS / "classic_1" / "clock-dial-cover.png",
                           ASSETS / "classic_2_rect" / "clock-dial-cover.png",
                           size=half)

    @property
    def clock_face(self):
        return self._first(ASSETS / "classic_1" / "clock-face.png",
                           ASSETS / "classic_2_rect" / "clock-face.png")

    # ── TSPS Classic 2 ──
    @property
    def tsps2_dial(self):
        return self._load(ASSETS / "classic_2_rect" / "full_gauges.png")

    @property
    def tsps2_overlay(self):
        return self._load(ASSETS / "classic_2_rect" / "full_guage_mask.png")

    # ── TSPS Classic 1 ──
    @property
    def tsps1_dial(self):
        return self._load(ASSETS / "classic_1_rect" / "indicator_fuel.png")

    @property
    def tsps1_overlay(self):
        return self._load(ASSETS / "classic_1_rect" / "gauge_mask.png")


# ---------------------------------------------------------------------------
# ── LEFT: Speedometer ────────────────────────────────────────────────────────
#  Render order: bg → dial (speedo_empty, scaled) → needle → overlay (mask)
#  Matches firmware: dial 75% of buffer, mask full size; needle between them.
# ---------------------------------------------------------------------------

SPEEDO_DIAL_SCALE = 0.75


def render_speedo_classic(assets: GaugeAssets, mph: float, odo_miles: float = 0.0) -> pygame.Surface:
    """firmware: speedo_cluster.cpp speedo_render()"""
    surf = pygame.Surface((CANVAS, CANVAS))
    surf.fill(BG)

    dial_s = max(2, int(round(CANVAS * SPEEDO_DIAL_SCALE)))
    mx = (CANVAS - dial_s) // 2
    my = (CANVAS - dial_s) // 2
    rat = max(0.0, min(1.0, mph / 140.0))
    hub_x, hub_y = CANVAS // 2, CANVAS // 2

    # 1. Dial (speedo_empty — smaller than full gauge)
    if assets.speedo_dial:
        scaled = pygame.transform.smoothscale(assets.speedo_dial, (dial_s, dial_s))
        surf.blit(scaled, (mx, my))

    # 2. Needle sprite above dial, below mask (hub = gauge centre = 50% buffer width / height)
    needle = assets.speedo_needle
    if needle:
        user_ang = _map_angle(rat, 60.0, 300.0)
        math_deg = (270.0 - user_ang) % 360.0
        angle_deg = math_deg + (-90.0)
        lw, lh = needle.get_size()
        _blit_rotated_speedo_needle_fw(surf, needle, (hub_x, hub_y), (lw * 0.5, lh * 0.91), angle_deg)
    else:
        cx = mx + dial_s // 2
        cy = my + int(0.42 * dial_s) + int(-5 * LS)
        lng = int(round(CANVAS * 0.36 * (dial_s / CANVAS)))
        tw = _needle_thick(12)
        _draw_gauge_needle(surf, NEEDLE_SPEEDO, cx, cy, lng, rat, 60.0, 300.0, tw)

    # 3. Mask / bezel (full size)
    if assets.speedo_overlay:
        surf.blit(assets.speedo_overlay, (0, 0))

    # 4. Odometer (matches firmware: ~50 px below needle hub, on top of mask)
    odo_fnt = pygame.font.SysFont("monospace", max(12, round(CANVAS * 0.042)))
    odo_txt = odo_fnt.render(f"{odo_miles:.1f}", True, (236, 236, 226))
    odo_rect = odo_txt.get_rect(center=(hub_x, hub_y + 50))
    surf.blit(odo_txt, odo_rect)

    return surf


def render_speedo_fallout(data: SimData, fnt: pygame.font.Font) -> pygame.Surface:
    """firmware: speedo_fallout.cpp speedo_draw_fallout()"""
    surf = pygame.Surface((CANVAS, CANVAS))
    surf.fill((0, 0, 0))
    sw, sh = CANVAS, CANVAS

    def tx(s: str, x: int, y: int, color: tuple = FO_GREEN) -> None:
        surf.blit(fnt.render(s, True, color), (x, y))

    tx("- STATUS -", sw // 2 - 56, 28)
    tx("[ vehicle outline ]", 40, 80, FO_DIM)
    pygame.draw.rect(surf, FO_GREEN, (80, 100, sw - 160, 100), 1)
    tx("FUEL", 32, 230)
    _seg_bar(surf, 120, 226, 10, data.fuel_pct, FO_GREEN, FO_DIM)
    tx(f"{data.fuel_pct:.0f}%", sw - 100, 230)
    tx("RADIO", 32, 276)
    tx("-- MHz", sw - 140, 276)
    tx("TEMP", 32, 320)
    tx(f"{data.coolant_f:.0f} F", sw - 120, 320)
    tx(".. DATA . MENU . STAT", 24, sh - 36, FO_DIM)
    return surf


# ---------------------------------------------------------------------------
# ── CENTER: Tachometer / Clock / Media ───────────────────────────────────────
# ---------------------------------------------------------------------------

def render_center_tach(assets: GaugeAssets, rpm: float) -> pygame.Surface:
    """firmware: center_cluster.cpp draw_tach_needle()"""
    surf = pygame.Surface((CANVAS, CANVAS))
    surf.fill((0, 0, 0))

    cx  = CANVAS // 2
    cy  = PIVOT_Y
    lng = int(CANVAS * 0.35)
    rat = min(1.0, max(0.0, min(rpm, 8000.0) / 6000.0))

    # 1. Tach dial
    if assets.tach_dial:
        surf.blit(assets.tach_dial, (0, 0))

    # 2. Needle on top
    _draw_gauge_needle(surf, NEEDLE_TACH, cx, cy, lng, rat, 115.0, 250.0, 4)

    return surf


def _clock_wall_angles() -> tuple[float, float]:
    """Wall time → (minute_user_deg, hour_user_deg).

    In firmware user-angle convention, 0° points to 6-o'clock.
    Adding +180° maps 12-o'clock (ratio=0) to user=180° → math=90° → tip UP.
    This correction is needed when using wall time (not boot millis).
    """
    now = datetime.now()
    sec = now.second + now.microsecond / 1_000_000
    min_of_hour = now.minute + sec / 60.0
    hr12 = now.hour % 12
    min_ratio = min_of_hour / 60.0
    hr_ratio  = (hr12 + min_ratio) / 12.0
    min_user = (180.0 + min_ratio * 360.0) % 360.0
    hr_user  = (180.0 + hr_ratio  * 360.0) % 360.0
    return min_user, hr_user


def _draw_clock_line_hand(surf: pygame.Surface,
                          cx: int, cy: int,
                          user_deg: float, length: int,
                          color: tuple, thick: int) -> None:
    r   = _user_to_math_rad(user_deg)
    tip = (int(cx + math.cos(r) * length),
           int(cy - math.sin(r) * length))
    _thick_line(surf, color, (cx, cy), tip, thick)


def render_center_clock(assets: GaugeAssets) -> pygame.Surface:
    """firmware: center_cluster.cpp clock mode.
    Line hands matching firmware draw_clock_hands_line() colours and lengths.
    Render order: bg → dial_cover (hub) → hands → clock_face (face overlay).
    """
    surf = pygame.Surface((CANVAS, CANVAS))
    surf.fill((0, 0, 0))
    # Firmware clock pivot is at dead centre (0.5, not 0.42).
    cx, cy = CANVAS // 2, CANVAS // 2

    # 1. Clock dial cover (half-size, centred) — decorative hub background
    if assets.clock_dial_cover:
        r = assets.clock_dial_cover.get_rect(center=(cx, cy))
        surf.blit(assets.clock_dial_cover, r.topleft)

    # 2. Clock hands — firmware dimensions: mlen=0.30*dim, hlen=0.20*dim, thick 3/4
    min_user, hr_user = _clock_wall_angles()
    mlen = int(0.30 * CANVAS)   # 144 px
    hlen = int(0.20 * CANVAS)   # 96 px
    _draw_clock_line_hand(surf, cx, cy, min_user, mlen, CLOCK_COLOR, 3)
    _draw_clock_line_hand(surf, cx, cy, hr_user,  hlen, CLOCK_COLOR, 4)

    # 3. Clock face overlay (numbers, markings — on top of hands)
    if assets.clock_face:
        surf.blit(assets.clock_face, (0, 0))

    return surf


def render_center_media(data: SimData) -> pygame.Surface:
    """firmware: center_media_ui.cpp — gauge-style media (art placeholder, progress, clock)."""
    surf = pygame.Surface((CANVAS, CANVAS))
    surf.fill((8, 8, 10))
    sw, sh = CANVAS, CANVAS
    pad = 20
    max_w = sw - 2 * pad
    hdr_h = 56

    f_head = pygame.font.SysFont("sans", 22, bold=True)
    f_sm = pygame.font.SysFont("sans", 15)
    f_combo = pygame.font.SysFont("sans", 19)
    f_big = pygame.font.SysFont("sans", 36, bold=True)
    f_clock = pygame.font.SysFont("sans", 20)

    hdr_s = f_head.render("BLUETOOTH", True, (248, 244, 220))
    surf.blit(hdr_s, ((sw - hdr_s.get_width()) // 2, (hdr_h - hdr_s.get_height()) // 2))

    arc_cx, arc_cy = int(sw * 0.15), int(hdr_h * 0.55)
    arc_r = sw // 7
    vol = int(data.media_vol_u4)
    nseg = 10
    filled = min(nseg, vol * nseg // 15)
    for i in range(nseg):
        t = math.radians(195 + (80 * (i + 0.5) / nseg))
        sx = int(arc_cx + math.cos(t) * (arc_r - 3))
        sy = int(arc_cy - math.sin(t) * (arc_r - 3))
        col = (255, 140, 40) if i < filled else (55, 48, 40)
        pygame.draw.rect(surf, col, (sx - 3, sy - 4, 6, 9))
    vl = f_sm.render("VOLUME", True, (200, 195, 180))
    surf.blit(vl, (arc_cx - vl.get_width() // 2 - 2, arc_cy + arc_r + 4))
    vn = f_sm.render(str(vol * 10 // 15), True, (200, 195, 180))
    surf.blit(vn, (arc_cx - vn.get_width() // 2, arc_cy + arc_r + 18))

    cool_q = int(round(data.coolant_f / 2.0)) * 2
    tmp_s = f_big.render(str(cool_q), True, (248, 244, 220))
    deg_s = f_sm.render("F", True, (248, 244, 220))
    rtx = sw - pad - tmp_s.get_width() - deg_s.get_width() - 6
    rty = hdr_h // 2 - tmp_s.get_height() // 2
    surf.blit(tmp_s, (rtx, rty))
    surf.blit(deg_s, (rtx + tmp_s.get_width(), rty + tmp_s.get_height() - deg_s.get_height()))

    y_art = hdr_h + 10
    art_side = 168 if sw > 400 else int(sw * 0.36)
    ax = (sw - art_side) // 2
    border = (235, 228, 200)
    pygame.draw.rect(surf, border, (ax, y_art, art_side, art_side), 1)
    for i in range(8):
        yy = y_art + 2 + (i * (art_side - 4)) // 8
        c = (48, 52, 58) if i & 1 else (22, 24, 28)
        pygame.draw.line(surf, c, (ax + 2, yy), (ax + art_side - 3, yy))
    pygame.draw.circle(surf, (90, 200, 110), (ax + art_side // 2, y_art + art_side // 2), art_side // 10)
    pygame.draw.circle(surf, (18, 22, 24), (ax + art_side // 2, y_art + art_side // 2), art_side // 16)

    raw = (data.track or "").strip()
    have_meta = bool(raw)
    artist, title = "", "Nothing playing"
    if have_meta:
        if " - " in raw:
            a, t = raw.split(" - ", 1)
            artist, title = a.strip(), t.strip()
        else:
            title = raw
    combo = f"{title} - {artist}" if artist and title else title
    while combo and f_combo.size(combo)[0] > max_w and len(combo) > 1:
        combo = combo[:-1]
    combo_img = f_combo.render(combo, True, (150, 230, 155) if have_meta else (130, 170, 200))
    ty = y_art + art_side + 10
    surf.blit(combo_img, ((sw - combo_img.get_width()) // 2, ty))

    el, du = data.media_elapsed_s, data.media_duration_s
    prog_y = y_art + art_side + 34
    prog_h = 10
    prog_x, prog_w = pad, sw - 2 * pad
    pygame.draw.rect(surf, (90, 88, 85), (prog_x, prog_y, prog_w, prog_h), 1, border_radius=4)
    ratio = (el / du) if du > 0 else 0.0
    ratio = max(0.0, min(1.0, ratio))
    fw = int((prog_w - 4) * ratio)
    if fw > 0:
        pygame.draw.rect(surf, (245, 238, 215), (prog_x + 2, prog_y + 2, fw, prog_h - 4), border_radius=3)
    pygame.draw.rect(surf, (248, 242, 220), (prog_x + 2 + max(0, fw - 2), prog_y + 1, 4, prog_h - 2))

    def mmss(sec: int) -> str:
        return f"{sec // 60}:{sec % 60:02d}"

    el_t = f_sm.render(mmss(el) if du > 0 else "--:--", True, (230, 225, 210))
    rem_s = max(0, du - el) if du > 0 else 0
    rem_t = f_sm.render(f"-{rem_s // 60}:{rem_s % 60:02d}" if du > 0 else "--:--", True, (230, 225, 210))
    tty = prog_y + prog_h + 6
    surf.blit(el_t, (prog_x + 2, tty))
    surf.blit(rem_t, (prog_x + prog_w - rem_t.get_width() - 2, tty))

    now = datetime.now()
    h = now.hour % 12 or 12
    clk = f_clock.render(f"{h}:{now.minute:02d} {'PM' if now.hour >= 12 else 'AM'}", True, (240, 235, 215))
    surf.blit(clk, ((sw - clk.get_width()) // 2, sh - pad - clk.get_height()))
    return surf


def render_center_fallout(data: SimData,
                          fnt_sm: pygame.font.Font,
                          fnt_lg: pygame.font.Font) -> pygame.Surface:
    """firmware: center_fallout.cpp center_draw_fallout()"""
    surf = pygame.Surface((CANVAS, CANVAS))
    surf.fill((0, 0, 0))
    sw, sh = CANVAS, CANVAS

    def tx(s: str, x: int, y: int, color: tuple = FO_GREEN,
           f: pygame.font.Font | None = None) -> None:
        surf.blit((f or fnt_sm).render(s, True, color), (x, y))

    tx("- SPEED -", sw // 2 - 54, 28)

    gn = data.gear
    tx("1  2  3", 28, 108, FO_DIM)
    if 1 <= gn <= 3:
        bx = 24 + (gn - 1) * 28 - 2
        pygame.draw.rect(surf, FO_GREEN, (bx, 96, 22, 26), 1)

    mph_i = max(0, min(999, int(data.mph + 0.5)))
    tx(f"{mph_i:03d}", sw // 2 - 8, 96, FO_GREEN, fnt_lg)
    tx("MPH", sw // 2 + 52, 104)

    tx("RPM", 28, 200)
    tx(f"{int(data.rpm)}", sw - 120, 200)

    tx("AP", 28, 236)
    ap = max(0, min(10, int((data.amps + 100.0) / 200.0 * 10.0)))
    _seg_bar(surf, 110, 232, 10, ap * 10, FO_GREEN, FO_DIM)

    tx("TRIP", 28, 276);  tx("42.7 MI",  sw - 120, 276)
    tx("ODO",  28, 312);  tx("73841 MI", sw - 140, 312)
    tx(".. DATA . MENU . STAT", 24, sh - 36, FO_DIM)
    return surf


# ---------------------------------------------------------------------------
# ── RIGHT: TSPS ──────────────────────────────────────────────────────────────
#  Render order: bg → dial → needles → overlay
# ---------------------------------------------------------------------------

def render_tsps_classic2(assets: GaugeAssets, data: SimData) -> pygame.Surface:
    """firmware: tsps_cluster.cpp tsps_render_classic2()"""
    surf = pygame.Surface((CANVAS, CANVAS))
    surf.fill(BG)

    base_cx = CANVAS // 2
    base_cy = PIVOT_Y   # 240; firmware uses 0.42*CANVAS but PNG centres at 50%

    def sub(v0, v1, a0, a1, ox, oy, lr, lw, invert, value):
        cx  = base_cx + int(ox * LS)
        cy  = base_cy + int(oy * LS)
        lng = int(CANVAS * lr)
        tw  = max(1, min(5, math.ceil(lw * LS / 3.0)))
        rat = max(0.0, min(1.0, (value - v0) / (v1 - v0))) if v1 > v0 else 0.0
        _draw_gauge_needle(surf, NEEDLE_TSPS, cx, cy, lng, rat, a0, a1, tw, invert)

    # 1. Dial (37% opaque)
    if assets.tsps2_dial:
        surf.blit(assets.tsps2_dial, (0, 0))

    # 2. All four needles — SubGauge params from tsps_cluster.cpp
    sub(0,    60,   250, 290,  0, 40, 0.41, 10, False, data.oil_psi)    # Oil PSI
    sub(0,   100,   152, 208,  0,  0, 0.35, 10, False, data.fuel_pct)   # Fuel %
    sub(40,  250,   -20,  20,  0, 50, 0.40, 10, True,  data.coolant_f)  # Coolant (inv)
    sub(-100, 100,   68, 110,  0, 40, 0.41, 10, False, data.amps)       # Amps

    # 3. Overlay (hub/label layer) with (ox, oy) = (5*LS, 6*LS) ≈ (2, 3) shift
    # blend_overlay_shift reads source at (x+ox, y+oy) → pygame blit at (-ox, -oy)
    if assets.tsps2_overlay:
        surf.blit(assets.tsps2_overlay, (-round(5 * LS), -round(6 * LS)))

    return surf


def render_tsps_classic1(assets: GaugeAssets, data: SimData) -> pygame.Surface:
    """firmware: tsps_cluster.cpp tsps_render_classic1()"""
    surf = pygame.Surface((CANVAS, CANVAS))
    surf.fill(BG)

    cx  = CANVAS // 2
    cy  = PIVOT_Y
    lng = int(CANVAS * 0.2)
    tw  = max(1, min(5, math.ceil(10 * LS / 3.0)))
    rat = max(0.0, min(1.0, data.fuel_pct / 100.0))

    # 1. Dial
    if assets.tsps1_dial:
        surf.blit(assets.tsps1_dial, (0, 0))

    # 2. Fuel needle — SubGauge {v0=0,v1=100, a0=145,a1=215, ox=0,oy=0, len=0.2, lw=10}
    _draw_gauge_needle(surf, NEEDLE_TSPS, cx, cy, lng, rat, 145.0, 215.0, tw)

    # 3. Overlay — classic1 shift: (0, -5*LS) → blit at (0, +round(5*LS))
    if assets.tsps1_overlay:
        surf.blit(assets.tsps1_overlay, (0, round(5 * LS)))

    return surf


def render_tsps_fallout(data: SimData, fnt: pygame.font.Font) -> pygame.Surface:
    """firmware: tsps_fallout.cpp tsps_draw_fallout()"""
    surf = pygame.Surface((CANVAS, CANVAS))
    surf.fill((0, 0, 0))
    sw, sh = CANVAS, CANVAS

    def tx(s: str, x: int, y: int, color: tuple = FO_GREEN) -> None:
        surf.blit(fnt.render(s, True, color), (x, y))

    tx("- HP -", sw // 2 - 40, 28)
    hp = max(0.0, min(100.0, 100.0 - data.oil_psi * 0.15))
    _seg_bar(surf, 40, 120, 10, hp, FO_GREEN, FO_DIM)
    tx(f"{hp:.0f}%", sw - 100, 118)
    tx("RAD", 32, 200);   tx("0.00 R",       sw - 120, 200)
    tx("NAV", 32, 250);   tx("> NW",          120,      250)
    tx("WASTELAND RD.",    32, 300)
    tx("MAP . DATA . MENU . STAT", 24, sh - 36, FO_DIM)
    return surf


# ---------------------------------------------------------------------------
# ── Overlay menu ─────────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------

class MenuPage:
    CLOSED = "closed"
    HOME   = "home"     # 2×2: TACH/CLOCK/WIFI/MORE
    WIFI   = "wifi"     # stub
    MORE   = "more"     # stub


class ButtonRect(NamedTuple):
    label: str
    rect:  pygame.Rect
    tag:   str


def _home_buttons() -> list[ButtonRect]:
    m, g = MENU_INSET, 14
    iw = (CANVAS - 2 * m - g) // 2
    ih = iw
    return [
        ButtonRect("TACH",  pygame.Rect(m,       m,      iw, ih), "tach"),
        ButtonRect("CLOCK", pygame.Rect(m+iw+g,  m,      iw, ih), "clock"),
        ButtonRect("WIFI",  pygame.Rect(m,       m+ih+g, iw, ih), "wifi"),
        ButtonRect("MORE",  pygame.Rect(m+iw+g,  m+ih+g, iw, ih), "more"),
    ]


def render_overlay(page: str, center_view: str,
                   fnt: pygame.font.Font) -> pygame.Surface:
    """firmware: p4_overlay_menu.cpp P4OverlayMenu::paintMenu()"""
    surf = pygame.Surface((CANVAS, CANVAS))
    surf.fill((0, 0, 0))
    cx, cy = CANVAS // 2, CANVAS // 2
    r_panel = CANVAS // 2 - 14

    pygame.draw.circle(surf, MENU_PANEL,  (cx, cy), r_panel)
    pygame.draw.circle(surf, MENU_BORDER, (cx, cy), r_panel, width=2)

    if page == MenuPage.HOME:
        for btn in _home_buttons():
            hi   = (btn.tag == center_view)
            fill = MENU_BTN_HI if hi else MENU_BTN_BASE
            pygame.draw.rect(surf, fill,        btn.rect, border_radius=12)
            pygame.draw.rect(surf, MENU_BORDER, btn.rect, width=1, border_radius=12)
            t = fnt.render(btn.label, True, MENU_TXT_HI if hi else MENU_TXT_DIM)
            surf.blit(t, t.get_rect(center=btn.rect.center))
    else:
        # Stub pages
        for i, line in enumerate(["N/A in emulator", "", "[tap to close]"]):
            t = fnt.render(line, True, MENU_TXT_DIM)
            surf.blit(t, t.get_rect(center=(cx, cy - 18 + i * 22)))

    return surf


def _overlay_hit(page: str, lx: int, ly: int) -> str | None:
    if page == MenuPage.HOME:
        for btn in _home_buttons():
            if btn.rect.collidepoint(lx, ly):
                return btn.tag
    return None


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

def _gauge_x(idx: int) -> int:
    return MARGIN + idx * (CANVAS + GAP)


def _in_circle(wx: int, wy: int, gauge_idx: int) -> bool:
    cx = _gauge_x(gauge_idx) + CANVAS // 2
    cy = MARGIN + CANVAS // 2
    r  = CANVAS // 2
    return (wx - cx) ** 2 + (wy - cy) ** 2 <= r * r


def _draw_hold_progress(screen: pygame.Surface, fnt: pygame.font.Font,
                        held_s: float) -> None:
    """Visual indicator while T is held."""
    bar_w = 300
    bar_h = 8
    bx = (WIN_W - bar_w) // 2
    by = MARGIN + CANVAS + 36
    pygame.draw.rect(screen, (40, 40, 40), (bx, by, bar_w, bar_h), border_radius=4)
    pct  = min(1.0, held_s / HOLD_THEME_S)
    fill = round(bar_w * pct)
    if held_s >= HOLD_THEME_S:
        color = (255, 80, 40)    # theme toggle ready
        label = "Release → Toggle FALLOUT/CLASSIC"
    elif held_s >= HOLD_VIEW_S:
        color = (80, 200, 120)   # view cycle ready
        label = "Release → Cycle Center View  |  keep holding for theme"
    else:
        color = (80, 120, 200)
        label = "Hold T…"
    if fill > 0:
        pygame.draw.rect(screen, color, (bx, by, fill, bar_h), border_radius=4)
    t = fnt.render(label, True, color)
    screen.blit(t, (bx, by - 18))


def main() -> None:
    pygame.init()
    pygame.display.set_caption("ESP32-P4 Gauge Emulator")
    screen = pygame.display.set_mode((WIN_W, WIN_H))

    mono_sm  = pygame.font.SysFont("monospace", 14)
    mono_lg  = pygame.font.SysFont("monospace", 26, bold=True)
    ui_fnt   = pygame.font.SysFont("sans", 15, bold=True)
    help_fnt = pygame.font.SysFont("monospace", 12)

    assets = GaugeAssets()
    data   = SimData()

    # Load persisted state
    saved       = _load_state()
    theme       = saved.get("theme",       "classic")   # "classic" | "fallout"
    center_view = saved.get("center_view", "tach")      # "tach" | "clock" | "media"
    tsps_mode   = saved.get("tsps_mode",   "classic2")  # "classic2" | "classic1"
    menu_page   = MenuPage.CLOSED

    # Key-hold tracking (for long-press simulation)
    t_hold_start: float | None = None  # monotonic() when T was pressed

    clock    = pygame.time.Clock()
    last_t   = monotonic()

    while True:
        now    = monotonic()
        dt     = min(now - last_t, 0.1)
        last_t = now
        data.tick(dt)

        # ── Events ──────────────────────────────────────────────────────────
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                _save_state(theme, center_view, tsps_mode)
                pygame.quit()
                return

            # T key — long-press gesture simulation
            if event.type == pygame.KEYDOWN and event.key == pygame.K_t:
                t_hold_start = monotonic()

            if event.type == pygame.KEYUP and event.key == pygame.K_t:
                if t_hold_start is not None:
                    held_s = monotonic() - t_hold_start
                    t_hold_start = None
                    if held_s >= HOLD_THEME_S:
                        # ≥10 s on device → toggle Classic / Fallout
                        theme     = "fallout" if theme == "classic" else "classic"
                        menu_page = MenuPage.CLOSED
                        _save_state(theme, center_view, tsps_mode)
                    elif held_s >= HOLD_VIEW_S and theme == "classic":
                        # ≥3 s on device → cycle center view
                        views       = ["tach", "clock", "media"]
                        center_view = views[(views.index(center_view) + 1) % len(views)]
                        menu_page   = MenuPage.CLOSED
                        _save_state(theme, center_view, tsps_mode)

            if event.type == pygame.KEYDOWN:
                k = event.key
                if k in (pygame.K_q, pygame.K_ESCAPE):
                    _save_state(theme, center_view, tsps_mode)
                    pygame.quit()
                    return

                # Instant shortcuts for convenience
                elif k == pygame.K_f:
                    theme     = "fallout" if theme == "classic" else "classic"
                    menu_page = MenuPage.CLOSED
                    _save_state(theme, center_view, tsps_mode)
                elif k == pygame.K_r:
                    tsps_mode = "classic1" if tsps_mode == "classic2" else "classic2"
                    _save_state(theme, center_view, tsps_mode)
                elif k == pygame.K_1 and theme == "classic":
                    center_view = "tach";  menu_page = MenuPage.CLOSED
                    _save_state(theme, center_view, tsps_mode)
                elif k == pygame.K_2 and theme == "classic":
                    center_view = "clock"; menu_page = MenuPage.CLOSED
                    _save_state(theme, center_view, tsps_mode)
                elif k == pygame.K_3 and theme == "classic":
                    center_view = "media"; menu_page = MenuPage.CLOSED
                    _save_state(theme, center_view, tsps_mode)

            # Mouse click — short tap on center gauge
            if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
                wx, wy = event.pos
                if _in_circle(wx, wy, 1):
                    lx = wx - _gauge_x(1)
                    ly = wy - MARGIN
                    if menu_page == MenuPage.CLOSED:
                        menu_page = MenuPage.HOME
                    else:
                        tag = _overlay_hit(menu_page, lx, ly)
                        if tag == "tach" and theme == "classic":
                            center_view = "tach";  menu_page = MenuPage.CLOSED
                            _save_state(theme, center_view, tsps_mode)
                        elif tag == "clock" and theme == "classic":
                            center_view = "clock"; menu_page = MenuPage.CLOSED
                            _save_state(theme, center_view, tsps_mode)
                        elif tag == "wifi":
                            menu_page = MenuPage.WIFI
                        elif tag == "more":
                            menu_page = MenuPage.MORE
                        else:
                            menu_page = MenuPage.CLOSED   # tap outside → close

        # ── Render gauges ───────────────────────────────────────────────────
        screen.fill((8, 8, 8))

        if theme == "classic":
            left_surf   = render_speedo_classic(assets, data.mph, data.odo_miles)
            center_surf = {
                "tach":  lambda: render_center_tach(assets, data.rpm),
                "clock": lambda: render_center_clock(assets),
                "media": lambda: render_center_media(data),
            }.get(center_view, lambda: render_center_tach(assets, data.rpm))()
            right_surf = (render_tsps_classic2(assets, data)
                          if tsps_mode == "classic2"
                          else render_tsps_classic1(assets, data))
        else:
            left_surf   = render_speedo_fallout(data, mono_sm)
            center_surf = render_center_fallout(data, mono_sm, mono_lg)
            right_surf  = render_tsps_fallout(data, mono_sm)

        # Round mask on all gauges
        for surf in (left_surf, center_surf, right_surf):
            _apply_round_mask(surf)

        # Center menu overlay
        if menu_page != MenuPage.CLOSED and theme == "classic":
            ov = render_overlay(menu_page, center_view, ui_fnt)
            _apply_round_mask(ov)
            center_surf.blit(ov, (0, 0))

        # Blit gauges
        screen.blit(left_surf,   (_gauge_x(0), MARGIN))
        screen.blit(center_surf, (_gauge_x(1), MARGIN))
        screen.blit(right_surf,  (_gauge_x(2), MARGIN))

        # Circle bezels
        for idx in range(3):
            cx = _gauge_x(idx) + CANVAS // 2
            cy = MARGIN + CANVAS // 2
            pygame.draw.circle(screen, (55, 55, 55), (cx, cy), CANVAS // 2, width=2)

        # ── T key hold progress ──────────────────────────────────────────────
        if t_hold_start is not None:
            held_s = monotonic() - t_hold_start
            _draw_hold_progress(screen, help_fnt, held_s)
        else:
            # ── Status + help bar ────────────────────────────────────────────
            by   = MARGIN + CANVAS + 6
            fps  = clock.get_fps()
            lx   = MARGIN

            parts: list[tuple[str, tuple]] = []
            if theme == "classic":
                parts += [
                    (f"Hold T: 0.5s=view  1.5s=theme  (now: {center_view})", (120, 140, 160)),
                    (f"  R=TSPS({tsps_mode})  1/2/3=center  F=fallout  Q=quit",
                     (90, 100, 110)),
                ]
            else:
                parts += [
                    ("FALLOUT theme — Hold T 1.5s to return, or F", (0, 200, 80)),
                    ("  R=TSPS mode  Q=quit", (0, 130, 50)),
                ]

            for line, color in parts:
                t = help_fnt.render(line, True, color)
                screen.blit(t, (lx, by))
                by += 16

            # Live values
            vals = (f"{data.mph:.0f}MPH  {data.rpm:.0f}RPM  "
                    f"Fuel {data.fuel_pct:.0f}%  Oil {data.oil_psi:.0f}PSI  "
                    f"Coolant {data.coolant_f:.0f}°F  Amps {data.amps:+.1f}  "
                    f"{fps:.0f}fps")
            vs = help_fnt.render(vals, True, (55, 55, 55))
            screen.blit(vs, (MARGIN, MARGIN + CANVAS + 40))

        pygame.display.flip()
        clock.tick(60)


if __name__ == "__main__":
    main()
