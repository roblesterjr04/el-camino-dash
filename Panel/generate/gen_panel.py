#!/usr/bin/env python3
"""Build Panel/Panel.kicad_pcb: every current board on one 4-layer panel, mouse-bite tabs between them, rails top and
bottom with fiducials and tooling holes. Run with KiCad's Python plus KiKit (see ../README.md):

    Panel/.venv/bin/python Panel/generate/gen_panel.py

References get a per-board prefix (AU-R1); the silkscreen keeps the board's own (R1). Nets get the same prefix.
Through-hole parts are fitted by the assembler like the SMD ones: in the BOM and the placement files, unless the
board itself marks them not fitted. Pads-only footprints (solder jumpers, mounting holes, net ties) leave the BOM and
placement files.
"""
import os
import sys
import tempfile

import pcbnew
from shapely.geometry import LineString, MultiLineString, box as sbox
from shapely.ops import unary_union
from kikit import panelize
from kikit.panelize import Origin, maxTabCount, TabAnnotation
from kikit.panelize_ui_impl import polygonToSubstrate
from kikit.defs import Layer
from kikit.units import mm
from kikit.common import fromDegrees, toKiCADPoint

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
OUT = os.path.join(ROOT, 'Panel', 'Panel.kicad_pcb')

GAP = 2.5          # mm between boards (the mill path)
RAIL = 5.0         # mm, top and bottom rails (the assembler's conveyor edges)
TAB_W = 5.0        # mm, tab width
TAB_PITCH = 30.0   # mm, about one tab per this much shared edge (60 mm card edges get two)
MAX_REACH = 3 + 27.5 + 1.5    # mm: fillet + the widest board-to-rail gap (ConsoleBoardRS) + slack
KEEPOUT = 1.5      # mm around a placed part that hangs past its board edge: no tab there
BITE_DRILL, BITE_PITCH, BITE_OFFSET = 0.5, 0.8, 0.0

# (directory, ref / net prefix, centre in the panel (mm, y down from DigiDash2's top edge), rotation (deg, KiCad's
# sense: + is counter-clockwise on screen)).
#
#   +------------------------------------- DigiDash2 329 x 113 ---------------------------------------+
#   +-- GPS 80x60 --+ +-- Audio 80x60 --+ +-- Sensor 80x60 --+         (air: the SMA's 12 mm)
#   +---------------+ +-----------------+ +------------------+               ^ SMA
#                             +---- ConsoleBoardRS 108x35 ----+ +-- Radio 80x60 --+
#
# The Radio card's edge-mount SMA (top edge) overhangs 12 mm: the card sits in the bottom row, its SMA under the free
# corner to the right of the Sensor card, its top-left corner still under the Sensor card so a tab holds that edge.
#
# The through-hole parts are fitted on the panel, so two gaps are wider: the Sensor and Radio cards' left-edge headers
# (J6-J8, J3 / J4) stand 6.5 mm past the edge, and would meet the Audio card's 3.5 mm jack (J6) and the console's OLED
# socket (J1), each 3.3 mm in from its own edge. At 5 mm they clear by about 1.8 mm.
DD_W, DD_H = 329.05, 113.05       # DigiDash2's outline (329 x 113, plus the edge line)
WIDE_GAP = 5.0                     # mm: Audio | Sensor, Console | Radio, and DigiDash2 / the card row
ROW_B = DD_H + WIDE_GAP            # top of the card row (5 mm: the Audio card's mic jack nose stands 2.5 mm past its top edge)
ROW_C = ROW_B + 60 + GAP           # top of the bottom row
SENSOR_X = 2 * 80 + GAP + WIDE_GAP      # Sensor card's left edge
RADIO_X = SENSOR_X + 80 - 15.75         # Radio card's left edge: under the Sensor card's last 15.75 mm (a tab there)
BOARDS = [
    ('DigiDash2',      'DD', (DD_W / 2, DD_H / 2),                            0),
    ('GPSCard',        'GP', (40, ROW_B + 30),                                0),
    ('AudioCarrier',   'AU', (80 + GAP + 40, ROW_B + 30),                     0),
    ('SensorCard',     'SE', (SENSOR_X + 40, ROW_B + 30),                     0),
    ('RadioCard',      'RA', (RADIO_X + 40, ROW_C + 30),                      0),
    ('ConsoleBoardRS', 'CO', (RADIO_X - WIDE_GAP - 54, ROW_C + 17.5),         0),
]
# Left out: DigiDash/ (rev 1, kept as the reference), ConsoleBoard/ (the keys-and-encoders console, replaced by
# ConsoleBoardRS), LTECard/ and BenchBoard/.


def main():
    panel = panelize.Panel(OUT)
    first = pcbnew.LoadBoard(os.path.join(ROOT, 'DigiDash2', 'DigiDash2.kicad_pcb'))
    panel.inheritDesignSettings(first)
    panel.inheritProperties(first)
    panel.inheritPageSize(first)
    panel.inheritCopperLayers(first)

    tmp = tempfile.mkdtemp(prefix='elc-panel-')
    for d, tag, (cx, cy), rot in BOARDS:
        path = os.path.join(ROOT, d, d + '.kicad_pcb')
        b = pcbnew.LoadBoard(path)
        if b.GetCopperLayerCount() != 4:
            # KiKit wants one layer count: a 2-layer board goes in as 4 layers with empty inner layers.
            b.SetCopperLayerCount(4)
            path = os.path.join(tmp, d + '.kicad_pcb')
            pcbnew.SaveBoard(path, b)
        panel.appendBoard(path, toKiCADPoint((cx * mm, cy * mm)), origin=Origin.Center,
                          rotationAngle=fromDegrees(rot), tolerance=int(15 * mm),
                          netRenamer=lambda n, orig, t=tag: f'{t}-{orig}' if orig else orig,
                          refRenamer=lambda n, orig, t=tag: f'{t}-{orig}',
                          bakeRef=True, inheritDrc=False)

    # Keep-outs for tabs, read off the panel itself (already moved and turned).
    keepouts = []
    for f in panel.board.GetFootprints():
        # SMD parts only (antennas, USB-C, SMA): a through-hole header's pins may cross a tab, it breaks clear of them
        if not f.GetAttributes() & pcbnew.FP_SMD:
            continue
        p = f.GetPosition()
        for s in panel.substrates:
            x0, y0, x1, y1 = s.bounds()
            if x0 <= p.x <= x1 and y0 <= p.y <= y1:
                eb = pcbnew.BOX2I(pcbnew.VECTOR2I(int(x0), int(y0)), pcbnew.VECTOR2I(int(x1 - x0), int(y1 - y0)))
                cy = f.GetCourtyard(pcbnew.F_CrtYd if f.GetLayer() == pcbnew.F_Cu else pcbnew.B_CrtYd).BBox()
                if cy.GetWidth() == 0:
                    cy = f.GetBoundingBox(False)
                if (cy.GetLeft() < eb.GetLeft() or cy.GetRight() > eb.GetRight() or
                        cy.GetTop() < eb.GetTop() or cy.GetBottom() > eb.GetBottom()):
                    k = int(KEEPOUT * mm)
                    keepouts.append((f.GetReference(),
                                     sbox(cy.GetLeft() - k, cy.GetTop() - k, cy.GetRight() + k, cy.GetBottom() + k)))
                break

    # Rails as ghosts first, so the boards facing them get tabs toward them.
    minx, miny, maxx, maxy = panel.boardsBBox()
    g, r = GAP * mm, RAIL * mm
    ghosts = [polygonToSubstrate(sbox(minx, miny - g - r, maxx, miny - g)),
              polygonToSubstrate(sbox(minx, maxy + g, maxx, maxy + g + r))]

    panel.clearTabsAnnotations()
    w = TAB_W * mm

    def count_fn(edge_len, _dir):
        return max(1, min(round(edge_len / (TAB_PITCH * mm)), maxTabCount(edge_len, w, 8 * mm)))

    panel._buildTabAnnotations(count_fn, lambda _l, _d: w, ghosts)

    dropped = []
    for s in panel.substrates:
        keep = []
        for a in s.annotations:
            if not isinstance(a, TabAnnotation):
                keep.append(a)
                continue
            (ox, oy), (dx, dy) = a.origin, a.direction
            half = a.width / 2
            # The whole tab, a few mm into this board and across the gap into the facing one, so that the facing
            # board's half of the same tab is dropped with it.
            into, out = 3 * mm, (WIDE_GAP + 3) * mm
            if dx:   # left / right edge (direction points into the board): tab spans y
                xa, xb = (ox - out, ox + into) if dx > 0 else (ox - into, ox + out)
                tb = sbox(xa, oy - half, xb, oy + half)
            else:
                ya, yb = (oy - out, oy + into) if dy > 0 else (oy - into, oy + out)
                tb = sbox(ox - half, ya, ox + half, yb)
            hit = [ref for ref, k in keepouts if k.intersects(tb)]
            if hit:
                dropped.append((hit, (ox / mm, oy / mm)))
            else:
                keep.append(a)
        s.annotations = keep

    # Partition lines: the middle of every gap between two facing pieces (boards and rails). A tab runs from its
    # board to the first one it meets, where the facing board's tab at the same place meets it. Only pieces with
    # nothing between them face each other.
    pieces = [sub.bounds() for sub in panel.substrates] + [gh.bounds() for gh in ghosts]

    def rail(j):
        # Rails grow no tabs of their own: a board's tab runs all the way to the rail.
        return j >= len(panel.substrates)

    def clear(i, j, gap):
        # Nothing else in the gap between pieces i and j.
        return not any(k not in (i, j) and sbox(*pieces[k]).intersection(gap).area > 0 for k in range(len(pieces)))

    for i, sub in enumerate(panel.substrates):
        x0, y0, x1, y1 = pieces[i]
        lines = []
        for j, (u0, v0, u1, v1) in enumerate(pieces):
            if j == i:
                continue
            lo, hi = max(x0, u0), min(x1, u1)          # above / below
            if hi > lo:
                if v0 >= y1 and clear(i, j, sbox(lo, y1, hi, v0)):
                    y = v0 if rail(j) else (y1 + v0) / 2
                    lines.append(LineString([(lo, y), (hi, y)]))
                elif v1 <= y0 and clear(i, j, sbox(lo, v1, hi, y0)):
                    y = v1 if rail(j) else (y0 + v1) / 2
                    lines.append(LineString([(lo, y), (hi, y)]))
            lo, hi = max(y0, v0), min(y1, v1)          # left / right
            if hi > lo:
                if u0 >= x1 and clear(i, j, sbox(x1, lo, u0, hi)):
                    lines.append(LineString([((x1 + u0) / 2, lo), ((x1 + u0) / 2, hi)]))
                elif u1 <= x0 and clear(i, j, sbox(u1, lo, x0, hi)):
                    lines.append(LineString([((x0 + u1) / 2, lo), ((x0 + u1) / 2, hi)]))
        sub.partitionLine = MultiLineString(lines)
    # Build the tabs one at a time and keep only the ones that hold something. KiKit runs each tab to the partition
    # line, half way across the gap, where it meets the facing board's tab; where the facing board has no tab at
    # that place (a keep-out dropped it), the half tab would hold nothing.
    eps = 0.05 * mm
    built = []    # (substrate index, tab polygons, cuts)
    long_tabs = 0
    for i, s in enumerate(panel.substrates):
        for a in s.annotations:
            t, c = panelize.buildTabs(panel, s, s.partitionLine, [a], int(1 * mm))
            if not t:
                continue
            # A tab reaches half way across its gap to a board, or all the way to a rail (at most 27.5 mm, under
            # ConsoleBoardRS: 25 mm). One that runs on along a narrow gap between two other boards would fill the mill
            # path: drop it.
            x0, y0, x1, y1 = t[0].bounds
            reach = (x1 - x0) if a.direction[0] else (y1 - y0)
            if reach > MAX_REACH * mm:
                long_tabs += 1
                continue
            built.append((i, t, c))
    rails = unary_union([gh.substrates for gh in ghosts]).buffer(eps)
    tabs, cuts, dangling = [], [], 0
    held_by = [0] * len(panel.substrates)
    for i, t, c in built:
        others = [sub.substrates for j, sub in enumerate(panel.substrates) if j != i]
        others += [p for j, tt, _ in built if j != i for p in tt]
        reach = unary_union(others).buffer(eps)
        if all(p.intersects(reach) or p.intersects(rails) for p in t):
            tabs.extend(t)
            cuts.extend(c)
            held_by[i] += 1
        else:
            dangling += 1
    panel.forwardTabs.extend(tabs)
    panel.boardSubstrate.union(tabs)
    panel.makeRailsTb(int(r), vspace=int(g))

    # Every gap between two facing boards must stay mostly open (the mill path): tabs only.
    whole = panel.boardSubstrate.substrates
    for i, (x0, y0, x1, y1) in enumerate(pieces[:len(panel.substrates)]):
        for j, (u0, v0, u1, v1) in enumerate(pieces[:len(panel.substrates)]):
            lo, hi = max(y0, v0), min(y1, v1)
            if j != i and hi > lo and 0 < u0 - x1 < 30 * mm:
                gap = sbox(x1 + eps, lo, u0 - eps, hi)
                fill = whole.intersection(gap).area / gap.area
                print('gap %s | %s: %.0f %% filled' % (BOARDS[i][0], BOARDS[j][0], 100 * fill))
            lo, hi = max(x0, u0), min(x1, u1)
            if j != i and hi > lo and 0 < v0 - y1 < 30 * mm:
                gap = sbox(lo, y1 + eps, hi, v0 - eps)
                fill = whole.intersection(gap).area / gap.area
                print('gap %s / %s: %.0f %% filled' % (BOARDS[i][0], BOARDS[j][0], 100 * fill))
    panel.makeMouseBites(cuts, int(BITE_DRILL * mm), int(BITE_PITCH * mm), int(BITE_OFFSET * mm), prolongation=int(0.5 * mm))
    panel.addMillFillets(int(1 * mm))

    # Fiducials and tooling holes on the rails (JLCPCB / PCBWay assembly panel: 4 x 1.152 mm holes, 3 fiducials).
    panel.addCornerTooling(4, int(5 * mm), int(r / 2), int(1.152 * mm))
    panel.addCornerFiducials(3, int(15 * mm), int(r / 2), int(1 * mm), int(2 * mm))
    pminx, pminy, pmaxx, pmaxy = panel.panelBBox()
    panel.addText('EL CAMINO DASH - PANEL', toKiCADPoint(((pminx + pmaxx) / 2, pminy + r / 2)),
                  width=int(1.5 * mm), height=int(1.5 * mm), thickness=int(0.25 * mm), layer=Layer.F_SilkS)
    panel.setAuxiliaryOrigin(toKiCADPoint((pminx, pmaxy)))
    panel.setGridOrigin(toKiCADPoint((pminx, pmaxy)))

    # Through-hole parts are placed like the SMD ones; only a board's own not-fitted parts stay out.
    tht, padsonly = [], []
    for f in panel.board.GetFootprints():
        a = f.GetAttributes()
        if f.GetReference().startswith('KiKit_'):    # mouse bites, tooling holes, fiducials
            f.SetExcludedFromBOM(True)
            f.SetExcludedFromPosFiles(True)
        elif a & pcbnew.FP_THROUGH_HOLE:
            if not f.IsDNP():
                f.SetExcludedFromBOM(False)
                f.SetExcludedFromPosFiles(False)
                tht.append(f.GetReference())
        elif not a & pcbnew.FP_SMD:
            f.SetExcludedFromBOM(True)
            f.SetExcludedFromPosFiles(True)
            padsonly.append(f.GetReference())

    # Keep each board's own zone fills: a refill here would use DigiDash2's rules (0.2 mm), not the card's.
    panel.save(reconstructArcs=True, refillAllZones=False)

    print('panel %.2f x %.2f mm' % ((pmaxx - pminx) / mm, (pmaxy - pminy) / mm))
    pieces = list(getattr(panel.boardSubstrate.substrates, 'geoms', [panel.boardSubstrate.substrates]))
    print('substrate pieces: %d (must be 1)' % len(pieces))
    if len(pieces) > 1:
        for k, piece in enumerate(pieces):
            held = [BOARDS[i][0] for i, sub in enumerate(panel.substrates) if piece.contains(sub.substrates.centroid)]
            print('  piece %d: %s' % (k, ', '.join(held) or 'rail / scrap'))
    print('tabs: %d built, %d dropped (would not reach a neighbour), %d dropped (ran along a gap)' % (len(tabs), dangling, long_tabs))
    print('through-hole, placed: %d' % len(tht))
    print('pads-only, out of BOM / placement: %d' % len(padsonly))
    for hit, at in dropped:
        print('tab dropped at (%.1f, %.1f) mm: clear of %s' % (at[0], at[1], ', '.join(hit)))
    for (d, *_), n in zip(BOARDS, held_by):
        print('  %-15s %2d tabs' % (d, n))


if __name__ == '__main__':
    sys.exit(main())
