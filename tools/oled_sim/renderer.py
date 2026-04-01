"""
256×64 SSD1322 OLED render model matching `arduino/sketches/encoder_controller/encoder_controller.ino`.

Uses the same u8g2 BDFs (via FreeType) as the firmware: 6x10, 10x20, helvB08.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import freetype

SCREEN_W = 256
SCREEN_H = 64
PI_SILENCE_MS = 3000


@dataclass
class OledState:
    oled_mode: str = "drive"
    oled_title: str = ""
    oled_l1: str = ""
    oled_l2: str = ""
    oled_m0: str = ""
    oled_m1: str = ""
    oled_m2: str = ""
    oled_v0: str = ""
    oled_v1: str = ""
    oled_v2: str = ""
    oled_mt: int = 0
    oled_mo: int = 0
    oled_mg: int = 0
    oled_wl: str = "OUTSIDE"
    oled_wt: str = "--"
    oled_st: str = ""
    oled_sa: str = ""
    oled_pr: int = 0
    oled_pe: str = "0:00"
    oled_pt: str = "0:00"
    oled_tm: str = "12:00"
    oled_ap: str = "AM"
    ever_received_pi: bool = False
    last_pi_frame_ms: int = 0


@dataclass
class FontSet:
    f6: freetype.Face
    f10: freetype.Face
    fhb: freetype.Face


def load_fonts(font_dir: Path) -> FontSet:
    f6 = freetype.Face(str(font_dir / "6x10.bdf"))
    f10 = freetype.Face(str(font_dir / "10x20.bdf"))
    fhb = freetype.Face(str(font_dir / "helvB08.bdf"))
    f6.set_pixel_sizes(0, 10)
    f10.set_pixel_sizes(0, 20)
    fhb.set_pixel_sizes(11, 11)
    return FontSet(f6=f6, f10=f10, fhb=fhb)


def sanitize_ascii(s: str) -> str:
    out: list[str] = []
    for c in s:
        o = ord(c)
        if 0x20 <= o <= 0x7E:
            out.append(c)
        else:
            out.append(" ")
    return "".join(out)


def parse_downlink_line(line: str, state: OledState, now_ms: int) -> bool:
    """Returns True if line was accepted as a valid OLED frame (matches firmware gate)."""
    if not line or "MODE=" not in line or "TITLE=" not in line or "|" not in line:
        return False
    state.oled_mt = 0
    state.oled_mo = 0
    state.oled_mg = 0
    state.oled_m0 = ""
    state.oled_m1 = ""
    state.oled_m2 = ""
    state.oled_v0 = ""
    state.oled_v1 = ""
    state.oled_v2 = ""
    for part in line.split("|"):
        if "=" not in part:
            continue
        key, _, val = part.partition("=")
        key = key.strip()
        val = sanitize_ascii(val)
        if key == "MODE":
            state.oled_mode = val[:7]
        elif key == "TITLE":
            state.oled_title = val[:31]
        elif key == "L1":
            state.oled_l1 = val[:39]
        elif key == "L2":
            state.oled_l2 = val[:39]
        elif key == "M0":
            state.oled_m0 = val[:31]
        elif key == "M1":
            state.oled_m1 = val[:31]
        elif key == "M2":
            state.oled_m2 = val[:31]
        elif key == "V0":
            state.oled_v0 = val[:31]
        elif key == "V1":
            state.oled_v1 = val[:31]
        elif key == "V2":
            state.oled_v2 = val[:31]
        elif key == "MT":
            try:
                state.oled_mt = max(0, int(val))
            except ValueError:
                state.oled_mt = 0
        elif key == "MO":
            try:
                state.oled_mo = max(0, int(val))
            except ValueError:
                state.oled_mo = 0
        elif key == "MG":
            try:
                state.oled_mg = max(0, int(val))
            except ValueError:
                state.oled_mg = 0
        elif key == "WL":
            state.oled_wl = val[:15]
        elif key == "WT":
            state.oled_wt = val[:11]
        elif key == "ST":
            state.oled_st = val[:31]
        elif key == "SA":
            state.oled_sa = val[:31]
        elif key == "PR":
            try:
                v = int(val)
                state.oled_pr = max(0, min(100, v))
            except ValueError:
                state.oled_pr = 0
        elif key == "PE":
            state.oled_pe = val[:9]
        elif key == "PT":
            state.oled_pt = val[:9]
        elif key == "TM":
            state.oled_tm = val[:9]
        elif key == "AP":
            state.oled_ap = val[:3]
    state.ever_received_pi = True
    state.last_pi_frame_ms = now_ms
    return True


def pi_link_live(state: OledState, now_ms: int) -> bool:
    if not state.ever_received_pi:
        return False
    return (now_ms - state.last_pi_frame_ms) < PI_SILENCE_MS


def _blit_glyph(buf: list[list[int]], bitmap: freetype.Bitmap, x0: int, y0: int, color: int) -> None:
    """Blit FreeType glyph bitmap (BDF strikes are FT_PIXEL_MODE_MONO packed rows)."""
    w, h = bitmap.width, bitmap.rows
    pitch = abs(bitmap.pitch)
    raw = bytes(bitmap.buffer)
    # 2 = FT_PIXEL_MODE_GRAY (8 bits per pixel); 1 = FT_PIXEL_MODE_MONO (1 bit per pixel, MSB first)
    if bitmap.pixel_mode == 2:
        for row in range(h):
            for col in range(w):
                idx = row * pitch + col
                if idx < len(raw) and raw[idx] > 0:
                    px, py = x0 + col, y0 + row
                    if 0 <= px < SCREEN_W and 0 <= py < SCREEN_H:
                        buf[py][px] = color
        return
    for row in range(h):
        for col in range(w):
            byte_idx = row * pitch + col // 8
            if byte_idx >= len(raw):
                continue
            bit = 7 - (col & 7)
            if (raw[byte_idx] >> bit) & 1:
                px, py = x0 + col, y0 + row
                if 0 <= px < SCREEN_W and 0 <= py < SCREEN_H:
                    buf[py][px] = color


def truncate_to_width(face: freetype.Face, text: str, max_px: int) -> str:
    if max_px <= 0 or not text:
        return ""
    lo, hi = 0, len(text)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if str_width(face, text[:mid]) <= max_px:
            lo = mid
        else:
            hi = mid - 1
    return text[:lo]


def str_width(face: freetype.Face, text: str) -> int:
    x = 0
    for ch in text:
        face.load_char(ord(ch), freetype.FT_LOAD_DEFAULT)
        x += face.glyph.advance.x >> 6
    return int(x)


def draw_str(
    buf: list[list[int]],
    face: freetype.Face,
    x: int,
    y_baseline: int,
    text: str,
    color: int = 1,
) -> None:
    px = x
    for ch in text:
        try:
            face.load_char(ord(ch), freetype.FT_LOAD_RENDER)
        except Exception:
            face.load_char(ord("?"), freetype.FT_LOAD_RENDER)
        g = face.glyph
        b = g.bitmap
        if b.width and b.rows:
            bx = px + g.bitmap_left
            by = y_baseline - g.bitmap_top
            _blit_glyph(buf, b, bx, by, color)
        px += g.advance.x >> 6


def draw_rect_outline(buf: list[list[int]], x: int, y: int, w: int, h: int, color: int = 1) -> None:
    if w <= 0 or h <= 0:
        return
    draw_hline(buf, x, y, w, color)
    draw_hline(buf, x, y + h - 1, w, color)
    draw_line(buf, x, y, x, y + h - 1, color)
    draw_line(buf, x + w - 1, y, x + w - 1, y + h - 1, color)


def draw_hline(buf: list[list[int]], x: int, y: int, w: int, color: int = 1) -> None:
    for i in range(w):
        xi = x + i
        if 0 <= xi < SCREEN_W and 0 <= y < SCREEN_H:
            buf[y][xi] = color


def draw_line(buf: list[list[int]], x0: int, y0: int, x1: int, y1: int, color: int = 1) -> None:
    """Bresenham line (matches single-pixel u8g2 lines)."""
    dx = abs(x1 - x0)
    dy = -abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    x, y = x0, y0
    while True:
        if 0 <= x < SCREEN_W and 0 <= y < SCREEN_H:
            buf[y][x] = color
        if x == x1 and y == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x += sx
        if e2 <= dx:
            err += dx
            y += sy


def thick_edge(buf: list[list[int]], x0: int, y0: int, x1: int, y1: int) -> None:
    draw_line(buf, x0, y0, x1, y1, 1)
    draw_line(buf, x0 + 1, y0, x1 + 1, y1, 1)
    draw_line(buf, x0, y0 + 1, x1, y1 + 1, 1)


def draw_box(buf: list[list[int]], x: int, y: int, w: int, h: int, color: int) -> None:
    for yy in range(y, y + h):
        if yy < 0 or yy >= SCREEN_H:
            continue
        row = buf[yy]
        for xx in range(x, x + w):
            if 0 <= xx < SCREEN_W:
                row[xx] = color


def fill_disc(buf: list[list[int]], cx: int, cy: int, r: int, color: int = 1) -> None:
    """Filled disc; u8g2 drawDisc uses integer rasterization."""
    r2 = r * r
    for y in range(cy - r, cy + r + 1):
        if y < 0 or y >= SCREEN_H:
            continue
        for x in range(cx - r, cx + r + 1):
            if x < 0 or x >= SCREEN_W:
                continue
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy <= r2:
                buf[y][x] = color


def draw_chevy_bowtie(buf: list[list[int]]) -> None:
    vx = [15, 30, 103, 103, 153, 153, 241, 226, 153, 153, 103, 103]
    vy = [45, 19, 19, 9, 9, 19, 19, 45, 45, 55, 55, 45]
    for i in range(12):
        j = (i + 1) % 12
        thick_edge(buf, vx[i], vy[i], vx[j], vy[j])
    ix = [22, 34, 107, 107, 149, 149, 234, 221, 149, 149, 107, 107]
    iy = [41, 23, 23, 15, 15, 23, 23, 41, 41, 51, 51, 41]
    for i in range(12):
        j = (i + 1) % 12
        draw_line(buf, ix[i], iy[i], ix[j], iy[j], 1)


def draw_idle_bowtie_screen(buf: list[list[int]]) -> None:
    draw_box(buf, 0, 0, SCREEN_W, SCREEN_H, 0)
    draw_chevy_bowtie(buf)


def draw_thermometer_icon(buf: list[list[int]], x: int, y: int) -> None:
    draw_line(buf, x + 3, y + 2, x + 3, y + 14, 1)
    fill_disc(buf, x + 3, y + 18, 4, 1)


def draw_eighth_note_icon(buf: list[list[int]], x: int, y: int) -> None:
    fill_disc(buf, x + 3, y + 5, 3, 1)
    fill_disc(buf, x + 3, y + 14, 2, 1)
    draw_box(buf, x + 6, y + 2, 2, 12, 1)
    draw_line(buf, x + 8, y + 2, x + 14, y + 6, 1)


def draw_seg_progress(buf: list[list[int]], x: int, y: int, w: int, h: int, pct: int) -> None:
    gap = 1
    seg_w = 2
    n = (w + gap) // (seg_w + gap)
    if n < 1:
        return
    filled = (n * pct) // 100
    for i in range(n):
        if i < filled:
            draw_box(buf, x + i * (seg_w + gap), y, seg_w, h, 1)


def draw_menu_screen(buf: list[list[int]], fonts: FontSet, state: OledState) -> None:
    if state.oled_mt > 0:
        draw_menu_scroll(buf, fonts, state)
        return
    draw_box(buf, 0, 0, SCREEN_W, SCREEN_H, 0)
    draw_box(buf, 0, 0, SCREEN_W, 14, 1)
    draw_str(buf, fonts.f6, 6, 11, " MENU ", color=0)
    draw_hline(buf, 0, 15, SCREEN_W, 1)
    ty = 26
    draw_str(buf, fonts.f6, 0, ty, state.oled_title if state.oled_title else "-", color=1)
    ty += 12
    draw_str(buf, fonts.f6, 0, ty, state.oled_l1 if state.oled_l1 else "-", color=1)
    ty += 12
    draw_str(buf, fonts.f6, 0, ty, state.oled_l2 if state.oled_l2 else "-", color=1)


def draw_menu_scroll(buf: list[list[int]], fonts: FontSet, state: OledState) -> None:
    """Match encoder_controller drawMenuScroll (title bar + 3 rows + scrollbar)."""
    draw_box(buf, 0, 0, SCREEN_W, SCREEN_H, 0)
    draw_box(buf, 0, 0, SCREEN_W, 14, 1)
    draw_str(buf, fonts.f6, 4, 11, (state.oled_title or "Menu")[:31], color=0)
    draw_hline(buf, 0, 15, SCREEN_W, 1)
    row_h = 12
    body_top = 16
    text_x = 16
    track_x = 250
    track_y = 17
    track_w = 4
    track_h = 46
    max_rows = 3
    gap = 4
    right_edge = track_x - 4
    labs = (state.oled_m0, state.oled_m1, state.oled_m2)
    vals = (state.oled_v0, state.oled_v1, state.oled_v2)
    f6 = fonts.f6
    for i in range(max_rows):
        idx = state.oled_mo + i
        if state.oled_mt == 0 or idx >= state.oled_mt:
            break
        lab = labs[i] or ""
        val = (vals[i] or "").strip()
        y_top = body_top + i * row_h
        baseline = y_top + 10
        if state.oled_mg == idx:
            draw_rect_outline(buf, 0, max(0, y_top - 1), track_x - 2, row_h + 1, 1)
        cy = y_top + row_h // 2
        fill_disc(buf, 6, cy, 5, 1)
        if lab:
            ch = lab[0].upper()
            draw_str(buf, fonts.f6, 3, baseline, ch, color=0)
        if val:
            vw = str_width(f6, val)
            val_x = right_edge - vw
            if val_x < text_x + 4:
                val_x = text_x + 4
            label_max_px = max(0, val_x - gap - text_x)
            lab_draw = truncate_to_width(f6, lab, label_max_px)
            draw_str(buf, f6, text_x, baseline, lab_draw, color=1)
            draw_str(buf, f6, val_x, baseline, val[:31], color=1)
        else:
            lab_max = right_edge - text_x
            lab_draw = truncate_to_width(f6, lab, lab_max)
            draw_str(buf, f6, text_x, baseline, lab_draw, color=1)
    draw_line(buf, track_x, track_y, track_x + track_w - 1, track_y, 1)
    draw_line(buf, track_x, track_y + track_h - 1, track_x + track_w - 1, track_y + track_h - 1, 1)
    draw_line(buf, track_x, track_y, track_x, track_y + track_h - 1, 1)
    draw_line(buf, track_x + track_w - 1, track_y, track_x + track_w - 1, track_y + track_h - 1, 1)
    if state.oled_mt > max_rows:
        thumb_h = max(4, (max_rows * track_h) // state.oled_mt)
        max_first = state.oled_mt - max_rows
        ty = track_y + 1
        if max_first > 0:
            ty += (state.oled_mo * (track_h - 2 - thumb_h)) // max_first
        draw_box(buf, track_x + 1, ty, track_w - 2, thumb_h, 1)


def draw_drive_dashboard(buf: list[list[int]], fonts: FontSet, state: OledState) -> None:
    draw_box(buf, 0, 0, SCREEN_W, SCREEN_H, 0)
    draw_thermometer_icon(buf, 4, 14)
    draw_str(buf, fonts.f6, 18, 12, state.oled_wl if state.oled_wl else "OUT", color=1)
    draw_str(buf, fonts.f10, 18, 32, state.oled_wt if state.oled_wt else "--", color=1)
    draw_eighth_note_icon(buf, 58, 6)
    draw_str(buf, fonts.fhb, 76, 20, state.oled_st if state.oled_st else "-", color=1)
    draw_str(buf, fonts.f6, 58, 34, state.oled_sa if state.oled_sa else "-", color=1)
    draw_hline(buf, 58, 40, 132, 1)
    draw_seg_progress(buf, 58, 44, 130, 6, state.oled_pr)
    draw_str(buf, fonts.f6, 58, 62, state.oled_pe if state.oled_pe else "0:00", color=1)
    tail = f"/ {state.oled_pt if state.oled_pt else '0:00'}"
    tw = str_width(fonts.f6, tail)
    draw_str(buf, fonts.f6, 186 - tw, 62, tail, color=1)
    tm = state.oled_tm if state.oled_tm else "--:--"
    draw_str(buf, fonts.f10, 196, 28, tm, color=1)
    cw = str_width(fonts.f10, tm)
    draw_str(buf, fonts.f6, 196 + cw + 2, 28, state.oled_ap if state.oled_ap else "", color=1)


def draw_pi_screen(buf: list[list[int]], fonts: FontSet, state: OledState) -> None:
    if state.oled_mode == "menu":
        draw_menu_screen(buf, fonts, state)
    else:
        draw_drive_dashboard(buf, fonts, state)


def render_frame(state: OledState, fonts: FontSet, now_ms: int) -> list[list[int]]:
    buf = [[0] * SCREEN_W for _ in range(SCREEN_H)]
    if not pi_link_live(state, now_ms):
        draw_idle_bowtie_screen(buf)
    else:
        draw_pi_screen(buf, fonts, state)
    return buf


def buf_to_pygame_surface(buf: list[list[int]], scale: int):
    import pygame

    # Warm amber OLED: lit pixels yellow-leaning; dim pixels brown-green (not cold gray).
    off = (28, 26, 18)
    on = (255, 220, 115)
    raw = bytearray(SCREEN_W * SCREEN_H * 3)
    i = 0
    for y in range(SCREEN_H):
        for x in range(SCREEN_W):
            c = off if buf[y][x] == 0 else on
            raw[i] = c[0]
            raw[i + 1] = c[1]
            raw[i + 2] = c[2]
            i += 3
    surf = pygame.image.frombytes(bytes(raw), (SCREEN_W, SCREEN_H), "RGB")
    if scale != 1:
        surf = pygame.transform.scale(surf, (SCREEN_W * scale, SCREEN_H * scale))
    return surf
