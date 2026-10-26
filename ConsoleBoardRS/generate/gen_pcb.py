#!/usr/bin/env python3
"""Build ConsoleBoardRS.kicad_pcb from the schematic's netlist with KiCad's pcbnew module (KiCad 10).

Run with KiCad's python:  $KIPY generate/gen_pcb.py [--route FREEROUTING_JAR]
  macOS: KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3

The console for the RetroSound knobs: the board fits a housing 111.5 x 38.5 mm outside (1.5 mm walls with 0.25 mm
clearance). The OLED module (100 x 33.5 mm, 2 x 8 header on its right edge) sits on standoffs, centred left to right and
flush with the board's top edge, so the housing's bezel has its widest band below the screen. Nothing else is on the
front: the ESP32-S3 module, USB-C, the CAN cable, the two RJ9 knob jacks (top entry) and the passives are on the back. Two layers since 2026-10-26 (ground pours on both; it was four with a GND plane: the 2-layer board
routes completely and costs a third as much).
ICs and connectors are placed by hand below; every passive is then put at the free spot closest to the pins it
connects to. With --route it runs Freerouting on a Specctra export and imports the result, then fills the ground planes.
"""
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

import numpy as np
import pcbnew
from pcbnew import FromMM, VECTOR2I, EDA_ANGLE

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
KICAD = '/Applications/KiCad/KiCad.app/Contents'
FPDIR = os.environ.get('KICAD_FOOTPRINTS', next((d for d in (KICAD + '/SharedSupport/footprints', '/usr/share/kicad/footprints')
                                                  if os.path.isdir(d)), ''))
CLI = os.environ.get('KICAD_CLI', KICAD + '/MacOS/kicad-cli' if os.path.exists(KICAD + '/MacOS/kicad-cli') else 'kicad-cli')
TMP = os.environ.get('TMPDIR', '/tmp')
SCH = os.path.join(ROOT, 'ConsoleBoardRS.kicad_sch')
PCB = os.path.join(ROOT, 'ConsoleBoardRS.kicad_pcb')
PRO = os.path.join(ROOT, 'ConsoleBoardRS.kicad_pro')
mm = FromMM

# ---- the OLED module (PARAMETERS: measure the module and edit these, then regenerate) ----------------------------------
SCREEN_W, SCREEN_H = 100.0, 33.5  # the module's PCB outline (Waveshare-style 3.12" OLED, 2 x 8 header on the right edge)
W, H = 108.0, 35.0                 # the board: the housing is 111.5 x 38.5 outside, 1.5 mm walls and 0.25 mm clearance all round
MX, MY = (W - SCREEN_W) / 2, 0.0   # the module's top-left corner: centred left to right, flush with the top edge (room for
                                   # text on the bezel below the screen)
HDR_SHIFT = 0.5                    # the socket's centre line (between its columns) is on the right holes' x, then this much closer to the edge
HDR_PITCH = 2.54
HOLE_DX, HOLE_DY = 94.0, 28.0      # the module's four mounting holes, centre to centre (measured), centred on the module; M2 standoffs
HOLE_X = (SCREEN_W - HOLE_DX) / 2        # 3.0 mm in from the sides
HOLE_Y = (SCREEN_H - HOLE_DY) / 2                # 2.75 mm in from the top and from the module's bottom edge
R_HOLES_DX = 1.0                   # checked against the module: H2 and H4 sit this much further right (J1 stays put)
H4_DY = 1.0                        # and H4 this much further down

OX, OY = 100.0, 80.0               # board origin on the sheet
HDR_X0 = MX + SCREEN_W - HOLE_X + HDR_SHIFT + HDR_PITCH / 2   # outer pin column (KiCad's socket has pad 1 in the outer column, pad 2 inside it)
HDR_Y = MY + SCREEN_H / 2 - 3.5 * HDR_PITCH                    # 8 rows, centred between the two right holes (= on the module)


def P(x, y):
    return VECTOR2I(mm(OX + x), mm(OY + y))


def netlist():
    xml = os.path.join(TMP, 'consoleboardrs_netlist.xml')
    subprocess.check_call([CLI, 'sch', 'export', 'netlist', '--format', 'kicadxml', '-o', xml, SCH],
                          stderr=subprocess.DEVNULL)
    r = ET.parse(xml).getroot()
    comps = {}
    for c in r.find('components'):
        f = {e.get('name'): (e.text or '') for e in c.find('fields')} if c.find('fields') is not None else {}
        comps[c.get('ref')] = (c.findtext('value'), c.findtext('footprint'), f.get('Datasheet', ''), f.get('Description', ''))
    nets = {}
    for n in r.find('nets'):
        for node in n:
            nets[(node.get('ref'), node.get('pin'))] = n.get('name')
    return comps, nets


def load(fpid):
    lib, name = fpid.split(':')
    path = os.path.join(ROOT, lib + '.pretty') if lib == 'ConsoleBoardRS' else os.path.join(FPDIR, lib + '.pretty')
    fp = pcbnew.FootprintLoad(path, name)
    assert fp, fpid
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


# ---------------------------------------------------------------------------------------------- placement plan
# (ref: (x, y, rotation, side)); side 'B' parts are flipped to the bottom (mirrored left-right, so a connector's origin
# is at the right end of its pins).
BOT = H                             # bottom edge
RJ_X = (57.0, 75.0)                # the two top-entry RJ9 jacks (plugs go in from behind the board): centre lines,
                                   # 18 mm apart (the A-2014-3-4-N-T-R bodies are 11.43 wide, 6.6 mm between them)
RJ_BODY = (-3.65, 12.85)           # the A-2014-3-4-N-T-R body from its near contact row (see gen_footprints.py)
RJ_Y = H - 1.0 - RJ_BODY[1]        # first contact row: the bodies end 1 mm above the bottom edge
U1_X = 26.0                     # the ESP32-S3 module: clear of the left M2 holes
EDGE_CU = 0.2                   # copper to board edge (the design rule below)
IR_PAD = 1.8                    # the Vishay MOLD footprint's pads
IR_INSET = 1.0                  # the IR receiver moved this far in from the corner, right and up
FIXED = {
    'J1': (HDR_X0, HDR_Y, 0, 'F'),         # socket pad 1 top right of the pair (module pin 1 = pad 2, see gen_sch.py)
    'U1': (U1_X, 12.8, 0, 'B'),            # antenna end on the top edge
    'J3': (23.5, BOT - 4.8, 0, 'B'),       # CAN cable, JST PH B4B-PH-K vertical (top entry): the plug goes in from behind
    'J2': (42.0, BOT - 3.7, 0, 'B'),       # USB-C
    'J4': (RJ_X[0], RJ_Y, 0, 'B'),        # Select knob, RJ9 4P4C, top entry
    'J5': (RJ_X[1], RJ_Y, 0, 'B'),        # Volume knob
    # the IR receiver (TSOP4838, on the front): pins 1-2-3 left to right seen from the front, in the corner where H3
    # was, IR_INSET in from where pin 3 under H3's centre and the row at the copper-to-edge limit would be (the body
    # hangs past the edge)
    'J6': (MX + HOLE_X - 2 * HDR_PITCH + IR_INSET, BOT - IR_PAD / 2 - EDGE_CU - 0.05 - IR_INSET, 0, 'F'),
    'SW1': (44.0, 5.5, 0, 'B'),            # RESET
    'SW2': (44.0, 11.0, 0, 'B'),           # BOOT
    'H1': (MX + HOLE_X, MY + HOLE_Y, 0, 'F'),
    'H2': (MX + SCREEN_W - HOLE_X + R_HOLES_DX, MY + HOLE_Y, 0, 'F'),
    'H4': (MX + SCREEN_W - HOLE_X + R_HOLES_DX, MY + SCREEN_H - HOLE_Y + H4_DY, 0, 'F'),
}
SIDE = {'oled': 'B', 'esp': 'B', 'boot': 'B', 'power': 'B', 'usb': 'B', 'can': 'B', 'led': 'B', 'ui': 'B', 'mount': 'F', 'misc': 'B'}
ANCHOR = {'esp': ['U1'], 'boot': ['U1', 'SW1', 'SW2'], 'can': ['J3'], 'power': ['J3', 'U1'], 'usb': ['J2'], 'oled': ['J1'],
          'ui': ['J4', 'J5', 'J6']}
GROUP_ORDER = ['esp', 'usb', 'can', 'power', 'boot', 'oled', 'led', 'ui', 'mount', 'misc']
POWER_NETS = {'+5V', '+3V3', 'VBUS', 'CAB_5V'}
NOT_FITTED = set()                        # every part is fitted by the assembler, the IR receiver J6 too
KEEP_SILK = {'J1', 'SW1', 'SW2', 'U1'}     # J2-J6 carry captions instead (J6's outline would be past the edge)
HOLES = [FIXED[h][:2] for h in ('H1', 'H2', 'H4')]   # no H3: the IR receiver has that corner
HOLE_KEEP = 3.0                          # radius kept free of parts around them

GRID = 0.2
NX, NY = int(round(W / GRID)), int(round(H / GRID))


class Occ:
    """Occupancy grid of one board side (True = taken)."""

    def __init__(self):
        self.a = np.zeros((NY, NX), bool)
        m = int(round(0.3 / GRID))
        self.a[:m, :] = self.a[-m:, :] = True
        self.a[:, :m] = self.a[:, -m:] = True

    def rect(self, x1, y1, x2, y2, pad=0.0):
        i1, i2 = max(0, int(np.floor((x1 - pad) / GRID))), min(NX, int(np.ceil((x2 + pad) / GRID)))
        j1, j2 = max(0, int(np.floor((y1 - pad) / GRID))), min(NY, int(np.ceil((y2 + pad) / GRID)))
        self.a[j1:j2, i1:i2] = True

    def circle(self, cx, cy, r):
        ys, xs = np.mgrid[0:NY, 0:NX]
        self.a |= ((xs + 0.5) * GRID - cx) ** 2 + ((ys + 0.5) * GRID - cy) ** 2 <= r * r

    def free_mask(self, w, h):
        """free[j, i]: a w x h (cells) box with its top-left cell at (i, j) touches nothing."""
        s = np.zeros((NY + 1, NX + 1), np.int32)
        s[1:, 1:] = self.a.cumsum(0).cumsum(1)
        out = np.zeros((NY, NX), bool)
        if w > NX or h > NY:
            return out
        tot = s[h:, w:] - s[:-h, w:] - s[h:, :-w] + s[:-h, :-w]
        out[:NY - h + 1, :NX - w + 1] = tot == 0
        return out


def bb_mm(box):
    return (box.GetLeft() / 1e6 - OX, box.GetTop() / 1e6 - OY, box.GetRight() / 1e6 - OX, box.GetBottom() / 1e6 - OY)


def courtyard(fp, side):
    fp.BuildCourtyardCaches()
    c = fp.GetCourtyard(pcbnew.F_CrtYd if side == 'F' else pcbnew.B_CrtYd)
    if c.OutlineCount() == 0:          # J1 has only a B.CrtYd (its socket): use its pins, not the mounting holes
        bx = [bb_mm(p.GetBoundingBox()) for p in fp.Pads() if not p.GetNumber().startswith('MP')]
        return (min(v[0] for v in bx) - 0.25, min(v[1] for v in bx) - 0.25,
                max(v[2] for v in bx) + 0.25, max(v[3] for v in bx) + 0.25)
    return bb_mm(c.BBox())


def main():
    route = sys.argv[sys.argv.index('--route') + 1] if '--route' in sys.argv else None
    comps, nets = netlist()
    groups = json.load(open(os.path.join(HERE, 'refs.json')))
    b = pcbnew.BOARD()
    b.SetCopperLayerCount(2)
    ds = b.GetDesignSettings()
    ds.m_CopperEdgeClearance = mm(0.2)
    netobj = {}

    def net(name):
        if name not in netobj:
            n = pcbnew.NETINFO_ITEM(b, name)
            b.Add(n)
            netobj[name] = n
        return netobj[name]

    fps = {}
    side_of = {}
    for ref in sorted(r for r in comps if not r.startswith('#')):
        value, fpid, sheet, descr = comps[ref]
        fp = load(fpid)
        fp.SetReference(ref)
        fp.SetValue(value)
        fp.GetField(pcbnew.FIELD_T_DATASHEET).SetText(sheet)     # as in the schematic, so the parity check is clean
        fp.GetField(pcbnew.FIELD_T_DESCRIPTION).SetText(descr)
        fp.SetAttributes(fp.GetAttributes() & ~pcbnew.FP_EXCLUDE_FROM_BOM)
        if ref in NOT_FITTED:  # fitted by hand after delivery: not in the BOM or the placement files
            fp.SetDNP(True)
            fp.SetAttributes(fp.GetAttributes() | pcbnew.FP_EXCLUDE_FROM_BOM | pcbnew.FP_EXCLUDE_FROM_POS_FILES)
        b.Add(fp)
        for pad in fp.Pads():
            n = nets.get((ref, pad.GetNumber()))
            if pad.GetNumber() in ('MP1', 'MP2'):
                n = 'GND'
            if n:
                pad.SetNet(net(n))
        fps[ref] = fp
        g = groups.get(ref, {}).get('group', 'misc')
        side_of[ref] = FIXED[ref][3] if ref in FIXED else SIDE[g]

    # the module's courtyard includes a 48 x 41 mm antenna area, and its keep-out zone is 48 mm wide: shrink both. The
    # console does not use Wi-Fi or Bluetooth (CAN only), so the antenna keep-out stays only under and beside the antenna.
    u1 = fps['U1']
    strip_epad_vias(u1)
    for g in list(u1.GraphicalItems()):
        if g.GetLayer() in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
            u1.Remove(g)
    r_ = pcbnew.PCB_SHAPE(u1)
    r_.SetShape(pcbnew.SHAPE_T_RECT)
    r_.SetLayer(pcbnew.F_CrtYd)
    r_.SetWidth(mm(0.05))
    r_.SetStart(VECTOR2I(mm(-9.4), mm(-13.1)))
    r_.SetEnd(VECTOR2I(mm(9.4), mm(13.1)))
    u1.Add(r_)
    for z in u1.Zones():
        o = z.Outline()
        o.RemoveAllContours()
        o.NewOutline()
        for (x_, y_) in [(-9.8, -27.75), (9.8, -27.75), (9.8, -6.75), (-9.8, -6.75)]:
            o.Append(mm(x_), mm(y_))
    occ = {'F': Occ(), 'B': Occ()}
    for hx, hy in HOLES:
        occ['F'].circle(hx, hy, HOLE_KEEP)
        occ['B'].circle(hx, hy, HOLE_KEEP)
    for s_ in ('F', 'B'):
        occ[s_].rect(U1_X - 9.8, 0, U1_X + 9.8, 6.6)      # nothing near the antenna (the module's own keep-out is 6.75 mm deep)

    def put(ref, x, y, rot, side):
        fp = fps[ref]
        if side == 'B' and not fp.IsFlipped():
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        # a flipped footprint's orientation is measured the other way round: +180 puts rotation 0 as on the front
        fp.SetOrientation(EDA_ANGLE(rot + (180 if side == 'B' else 0), pcbnew.DEGREES_T))
        fp.SetPosition(P(x, y))

    def mark(ref):
        """Occupy every courtyard outline of the part (separately: J1's are two hole circles), and for through-hole
        parts their pins on both sides plus the other side's courtyard."""
        fp = fps[ref]
        side = side_of[ref]
        if ref == 'U1':
            x, y = FIXED['U1'][0], FIXED['U1'][1]
            occ[side].rect(x - 9.4, y - 13.1, x + 9.4, y + 13.1)
            return
        fp.BuildCourtyardCaches()
        tht = fp.GetAttributes() & pcbnew.FP_THROUGH_HOLE
        for s_, lay in (('F', pcbnew.F_CrtYd), ('B', pcbnew.B_CrtYd)):
            if s_ != side and not tht:
                continue
            c = fp.GetCourtyard(lay)
            for k in range(c.OutlineCount()):
                occ[s_].rect(*bb_mm(c.Outline(k).BBox()), pad=0.15)
        if tht:
            for pad in fp.Pads():       # plated pins and locating pegs (non-plated holes) go through both sides
                if pad.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH) and not pad.GetNumber().startswith('MP'):
                    for s_ in ('F', 'B'):
                        occ[s_].rect(*bb_mm(pad.GetBoundingBox()), pad=0.4)

    for ref, (x, y, rot, side) in FIXED.items():
        put(ref, x, y, rot, side)
        mark(ref)
    occ['B'].rect(FIXED['J3'][0] - 9.5, BOT - 8.7, FIXED['J3'][0] + 3.5, BOT - 7.3)   # the CAN caption, between the ESP32 and J3
    for cx in RJ_X:                       # the knob captions, above the RJ9 jacks
        occ['B'].rect(cx - 5.0, RJ_Y + RJ_BODY[0] - 3.4, cx + 5.0, RJ_Y + RJ_BODY[0] - 1.8)
    occ['B'].rect(FIXED['J2'][0] - 3.0, BOT - 10.3, FIXED['J2'][0] + 3.0, BOT - 8.9)
    occ['F'].rect(FIXED['J6'][0] - 1.0, FIXED['J6'][1] - 3.2, FIXED['J6'][0] + 6.1, FIXED['J6'][1] - 1.6)   # its caption
    for swref in ('SW1', 'SW2'):          # the RST / BOOT captions beside the two buttons
        occ['B'].rect(FIXED[swref][0] + 4.6, FIXED[swref][1] - 0.8, FIXED[swref][0] + 8.4, FIXED[swref][1] + 0.8)

    # ------------------------------------------------------------------ passives: closest free spot to their pins
    def placed_pads():
        out = {}
        for r in placed:
            for pad in fps[r].Pads():
                n = pad.GetNetname()
                if n:
                    p = pad.GetPosition()
                    out.setdefault(n, []).append((r, p.x / 1e6 - OX, p.y / 1e6 - OY))
        return out

    placed = set(FIXED)
    rest = [r for r in fps if r not in FIXED]

    def prio(r):
        g = groups.get(r, {}).get('group', 'misc')
        nn = {p.GetNetname() for p in fps[r].Pads()}
        return (GROUP_ORDER.index(g), 0 if r.startswith('U') else 1, 0 if (nn & POWER_NETS and 'GND' in nn) else 1,
                0 if fps[r].GetValue() == '100n' else 1, r)

    rest.sort(key=prio)
    xs = (np.arange(NX) + 0.5) * GRID
    ys = (np.arange(NY) + 0.5) * GRID
    for ref in rest:
        fp = fps[ref]
        side = side_of[ref]
        g = groups.get(ref, {}).get('group', 'misc')
        anchors = set(ANCHOR.get(g, []))
        pads_by_net = placed_pads()
        best = None
        for rot in (0, 90, 180, 270):
            put(ref, 0, 0, rot, side)
            x1, y1, x2, y2 = courtyard(fp, side)
            x1 -= 0.1
            y1 -= 0.1
            x2 += 0.1
            y2 += 0.1
            wc, hc = int(np.ceil((x2 - x1) / GRID)), int(np.ceil((y2 - y1) / GRID))
            free = occ[side].free_mask(wc, hc)
            # candidate origin for top-left cell (i, j): x = i*GRID - x1
            cx = xs[None, :] - GRID / 2 - x1
            cy = ys[:, None] - GRID / 2 - y1
            cost = np.zeros((NY, NX))
            for pad in fp.Pads():
                n = pad.GetNetname()
                if not n:
                    continue
                px, py = pad.GetPosition().x / 1e6 - OX, pad.GetPosition().y / 1e6 - OY
                tg = pads_by_net.get(n, [])
                wgt = 1.0
                if n == 'GND':
                    tg = [t for t in tg if side_of[t[0]] == side or fps[t[0]].GetAttributes() & pcbnew.FP_THROUGH_HOLE]
                    wgt = 0.25
                elif n in POWER_NETS:
                    near = [t for t in tg if t[0] in anchors]
                    tg = near or tg
                else:
                    near = [t for t in tg if t[0] in anchors]
                    if near:
                        tg = near
                if not tg:
                    continue
                d = None
                for (_, tx, ty) in tg:
                    dd = np.hypot(cx + px - tx, cy + py - ty)
                    d = dd if d is None else np.minimum(d, dd)
                cost += wgt * d
            cost = np.where(free, cost, np.inf)
            j, i = np.unravel_index(np.argmin(cost), cost.shape)
            if np.isfinite(cost[j, i]) and (best is None or cost[j, i] < best[0]):
                best = (cost[j, i], float(cx[0, i]), float(cy[j, 0]), rot)
        if best is None:
            a = occ[side].a
            for jj in range(0, NY, 3): print("".join("#" if a[jj, ii] else "." for ii in range(0, NX, 2)))
            raise SystemExit("no room for %s (%s) %s" % (ref, side, courtyard(fp, side)))
        _, x, y, rot = best
        put(ref, round(x, 2), round(y, 2), rot, side)
        mark(ref)
        placed.add(ref)
        print(ref, side, round(x, 2), round(y, 2), rot)

    # ------------------------------------------------------------------ text
    for ref, fp in fps.items():
        t = fp.Reference()
        side = side_of[ref]
        if ref in KEEP_SILK:
            t.SetLayer(pcbnew.F_SilkS if side == 'F' else pcbnew.B_SilkS)
        else:
            t.SetLayer(pcbnew.F_Fab if side == 'F' else pcbnew.B_Fab)
        t.SetTextSize(VECTOR2I(mm(0.7), mm(0.7)))
        t.SetTextThickness(mm(0.12))
        fp.Value().SetVisible(False)
    for item in list(fps['J6'].GraphicalItems()):  # the IR receiver's outline lies past the bottom edge: its caption only
        if item.GetLayer() == pcbnew.F_SilkS:
            fps['J6'].Remove(item)

    def silk(s, x, y, size=0.7, layer=pcbnew.F_SilkS, rot=0):
        t = pcbnew.PCB_TEXT(b)
        t.SetText(s)
        t.SetLayer(layer)
        t.SetPosition(P(x, y))
        t.SetTextSize(VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(0.12 if size < 1 else 0.15))
        t.SetTextAngle(EDA_ANGLE(rot, pcbnew.DEGREES_T))
        if layer == pcbnew.B_SilkS:
            t.SetMirrored(True)
        b.Add(t)

    BS = pcbnew.B_SilkS
    silk('OLED', W / 2, MY + SCREEN_H + 0.75, 0.8)
    silk('CONSOLE RS', 70.0, 1.6, 0.8, BS)
    silk('CAN 5V H L G', FIXED['J3'][0] - 3.0, BOT - 8.0, 0.7, BS)
    silk('SELECT KNOB', RJ_X[0], RJ_Y + RJ_BODY[0] - 2.6, 0.7, BS)
    silk('VOLUME KNOB', RJ_X[1], RJ_Y + RJ_BODY[0] - 2.6, 0.7, BS)
    silk('USB', FIXED['J2'][0], BOT - 9.6, 0.7, BS)
    silk('S G V', FIXED['J6'][0] + 2.54, FIXED['J6'][1] - 2.4, 0.7)   # the IR receiver's pins, read from the front
    silk('RST', FIXED['SW1'][0] + 6.5, 5.5, 0.7, BS)
    silk('BOOT', FIXED['SW2'][0] + 6.5, 11.0, 0.7, BS)

    # ------------------------------------------------------------------ outline, zones
    corners = [(0, 0), (W, 0), (W, H), (0, H)]
    for a_, e_ in (((0, 0), (W, 0)), ((W, 0), (W, H)), ((W, H), (0, H)), ((0, H), (0, 0))):
        sh = pcbnew.PCB_SHAPE(b)
        sh.SetShape(pcbnew.SHAPE_T_SEGMENT)
        sh.SetLayer(pcbnew.Edge_Cuts)
        sh.SetWidth(mm(0.05))
        sh.SetStart(P(*a_))
        sh.SetEnd(P(*e_))
        b.Add(sh)
    for lay in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(b)
        z.SetLayer(lay)
        z.SetNet(net('GND'))
        z.SetMinThickness(mm(0.2))
        z.SetLocalClearance(mm(0.2))
        z.SetThermalReliefGap(mm(0.25))
        z.SetThermalReliefSpokeWidth(mm(0.3))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        z.SetAssignedPriority(0)
        o = z.Outline()
        o.NewOutline()
        for (x, y) in corners:
            o.Append(mm(OX + x), mm(OY + y))
        b.Add(z)

    # a ring inside the outline with no tracks or vias (Freerouting only keeps its trace clearance from the edge)
    k = pcbnew.ZONE(b)
    k.SetIsRuleArea(True)
    k.SetDoNotAllowTracks(True)
    k.SetDoNotAllowVias(True)
    k.SetDoNotAllowZoneFills(False)
    k.SetDoNotAllowPads(False)
    k.SetDoNotAllowFootprints(False)
    k.SetLayerSet(pcbnew.LSET.AllCuMask(2))
    o = k.Outline()
    o.NewOutline()
    for (x, y) in corners:
        o.Append(mm(OX + x), mm(OY + y))
    o.NewHole()
    e = 0.35
    for (x, y) in [(e, e), (W - e, e), (W - e, H - e), (e, H - e)]:
        o.Append(mm(OX + x), mm(OY + y), 0, 0)
    b.Add(k)

    rules(b)
    pcbnew.SaveBoard(PCB, b)
    print('placed', len(fps), 'footprints; wrote', PCB)
    if route:
        freeroute(route)


def fab_origin(b):
    """Fab outputs (Gerbers, drill, centroid) measure from the board's bottom-left corner, as assemblers expect."""
    b.GetDesignSettings().SetAuxOrigin(VECTOR2I(mm(OX), mm(OY + H)))


def rules(b):
    fab_origin(b)
    """Net classes and constraints, set on the board (SaveBoard writes them to the project file)."""
    ds = b.GetDesignSettings()
    ds.m_CopperEdgeClearance = mm(0.2)
    ds.m_MinClearance = mm(0.13)
    ds.m_TrackMinWidth = mm(0.15)
    ds.m_MinSilkTextHeight = mm(0.7)
    ds.m_MinThroughDrill = mm(0.3)
    ds.m_MinResolvedSpokes = 1          # header pins also reach the In1 plane through their holes
    ns = ds.m_NetSettings
    d = ns.GetDefaultNetclass()
    pwr = pcbnew.NETCLASS('Power')
    for nc, w in ((d, 0.2), (pwr, 0.3)):
        nc.SetClearance(mm(0.13))
        nc.SetTrackWidth(mm(w))
        nc.SetViaDiameter(mm(0.6))
        nc.SetViaDrill(mm(0.3))
    pwr.SetPriority(0)
    ns.SetNetclass('Power', pwr)
    ns.ClearNetclassPatternAssignments()
    for n in ('GND', '+5V', '+3V3', '+5VA', '+3V3A'):
        ns.SetNetclassPatternAssignment(n, 'Power')
    ns.RecomputeEffectiveNetclasses()
    b.SynchronizeNetsAndNetClasses(True)


def stitch(b, pitch=2.0, fanout=None):
    """GND vias so every GND pad and every piece of the F / B / In2 pours reaches the In1 plane: a grid of vias
    wherever they clear everything (not under the antenna, the screw heads or a part), then, for each GND pad the
    filled pours still leave off the plane, a via beside it with a short track.
    With fanout=[refs] (before routing) it only gives every GND pin of those parts its own via, escaping inward under
    the body first, locked so the router works around them: fine-pitch GND pins between signals get boxed in otherwise.
    Two layers (since 2026-10-26): there is no plane, Freerouting routes GND itself and the pours join afterwards, so
    this does nothing."""
    return
    gnd = b.FindNet('GND')
    rv, gap, tw = 0.3, 0.2, 0.25
    items = []                          # (kind, geometry, net)
    for t in b.GetTracks():
        if t.GetClass() == 'PCB_VIA':
            p = t.GetPosition()
            items.append(('c', (p.x / 1e6, p.y / 1e6, t.GetWidth(pcbnew.F_Cu) / 2e6), t.GetNetname()))
        else:
            a, e = t.GetStart(), t.GetEnd()
            items.append(('s', (a.x / 1e6, a.y / 1e6, e.x / 1e6, e.y / 1e6, t.GetWidth() / 2e6), t.GetNetname()))
    parts, keepouts = [], []
    for fp in b.GetFootprints():
        for pad in fp.Pads():
            bb = pad.GetBoundingBox()
            items.append(('r', (bb.GetLeft() / 1e6, bb.GetTop() / 1e6, bb.GetRight() / 1e6, bb.GetBottom() / 1e6), pad.GetNetname()))
        fp.BuildCourtyardCaches()
        for lay in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
            c = fp.GetCourtyard(lay)
            for k in range(c.OutlineCount()):
                bb = c.Outline(k).BBox()
                parts.append((bb.GetLeft() / 1e6, bb.GetTop() / 1e6, bb.GetRight() / 1e6, bb.GetBottom() / 1e6))
        for z in fp.Zones():
            bb = z.GetBoundingBox()
            keepouts.append((bb.GetLeft() / 1e6 - 0.5, bb.GetTop() / 1e6 - 0.5, bb.GetRight() / 1e6 + 0.5, bb.GetBottom() / 1e6 + 0.5))
    for z in b.Zones():
        if z.GetIsRuleArea() and z.GetBoundingBox().GetWidth() < mm(W - 1):
            bb = z.GetBoundingBox()
            keepouts.append((bb.GetLeft() / 1e6 - 0.5, bb.GetTop() / 1e6 - 0.5, bb.GetRight() / 1e6 + 0.5, bb.GetBottom() / 1e6 + 0.5))

    pads = pad_boxes(b)

    def clear(x, y, r=rv, skip=None, gnd_ok=False):
        if r >= rv and not off_pads(pads, x, y):
            return False                # a via never touches a pad, whatever its net
        for it in items:
            if it is skip or (gnd_ok and it[2] == 'GND'):
                continue
            kind, g, n = it
            if n == 'GND' and (kind == 's' or (kind == 'r' and (g[2] - g[0]) * (g[3] - g[1]) > 1.0)):
                continue                # GND on GND: tracks, and a track across the big pads (the module's ground field)
            if kind == 'c':
                if np.hypot(x - g[0], y - g[1]) < r + g[2] + gap:
                    return False
            elif kind == 's':
                ax, ay, ex, ey, hw = g
                dx, dy = ex - ax, ey - ay
                L = dx * dx + dy * dy
                u = 0 if L == 0 else max(0, min(1, ((x - ax) * dx + (y - ay) * dy) / L))
                if np.hypot(x - ax - u * dx, y - ay - u * dy) < r + hw + gap:
                    return False
            elif g[0] - r - gap < x < g[2] + r + gap and g[1] - r - gap < y < g[3] + r + gap:
                return False
        return True

    def spot_ok(x, y):
        if not (OX + 0.8 < x < OX + W - 0.8 and OY + 0.8 < y < OY + H - 0.8):
            return False
        if any(np.hypot(x - OX - hx, y - OY - hy) < HOLE_KEEP + 0.2 for hx, hy in HOLES):
            return False
        return not any(x1 < x < x2 and y1 < y < y2 for x1, y1, x2, y2 in keepouts)

    def via(x, y):
        v = pcbnew.PCB_VIA(b)
        v.SetPosition(VECTOR2I(mm(x), mm(y)))
        v.SetWidth(mm(0.6))
        v.SetDrill(mm(0.3))
        v.SetNet(gnd)
        v.SetLocked(bool(fanout))
        b.Add(v)
        items.append(('c', (x, y, rv), 'GND'))

    n = 0
    y = OY + 1.0
    while fanout is None and y < OY + H - 0.9:
        x = OX + 1.0
        while x < OX + W - 0.9:
            if spot_ok(x, y) and not any(x1 < x < x2 and y1 < y < y2 for x1, y1, x2, y2 in parts) and clear(x, y):
                via(x, y)
                n += 1
            x += pitch
        y += pitch

    def seg_clear(ax, ay, ex, ey, skip, gnd_ok=False):
        steps = max(2, int(np.hypot(ex - ax, ey - ay) / 0.1))
        return all(clear(ax + (ex - ax) * k / steps, ay + (ey - ay) * k / steps, tw / 2, skip, gnd_ok)
                   for k in range(1, steps + 1))

    def off_plane():
        """GND pads not reachable from the In1 plane through copper (after a fill)."""
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        b.BuildConnectivity()
        c = b.GetConnectivity()
        plane = [z for z in b.Zones() if not z.GetIsRuleArea() and z.GetLayer() == pcbnew.In1_Cu][0]
        seen, todo, pads = {plane.m_Uuid.AsString()}, [plane], set()
        while todo:
            for o in c.GetConnectedItems(todo.pop()):
                k = o.m_Uuid.AsString()
                if k not in seen:
                    seen.add(k)
                    todo.append(o)
                    if o.GetClass() == 'PAD':
                        pads.add(k)
        return [p for fp in b.GetFootprints() for p in fp.Pads()
                if p.GetNetname() == 'GND' and p.m_Uuid.AsString() not in pads]

    def drc_gnd_pads():
        """GND pads KiCad's DRC still finds unconnected (a pour island can hold one pad and nothing else)."""
        tmp = os.path.join(TMP, 'stitch_check.kicad_pcb')
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        pcbnew.SaveBoard(tmp, b)
        rpt = os.path.join(TMP, 'stitch_check.json')
        subprocess.call([CLI, 'pcb', 'drc', '--format', 'json', '-o', rpt, tmp], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        out = []
        unc = json.load(open(rpt)).get('unconnected_items', [])
        if any(all(it.get('description', '').startswith('Zone') for it in v.get('items', [])) for v in unc):
            # a pour island tied to the rest only through one pad's spokes: give the pads on via-less islands a link
            gv = [t.GetPosition() for t in b.GetTracks() if t.GetClass() == 'PCB_VIA' and t.GetNetname() == 'GND']
            for z in b.Zones():
                if z.GetIsRuleArea():
                    continue
                for L in (pcbnew.F_Cu, pcbnew.B_Cu, pcbnew.In2_Cu):
                    if not z.IsOnLayer(L):
                        continue
                    ps = z.GetFilledPolysList(L)
                    for i in range(ps.OutlineCount()):
                        o = ps.Outline(i)
                        if any(o.PointInside(v) for v in gv):
                            continue
                        bb = o.BBox()
                        bb.Inflate(mm(0.5))
                        out += [p for f in b.GetFootprints() for p in f.Pads() if p.GetNetname() == 'GND'
                                and p.IsOnLayer(L) and p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD
                                and bb.Intersects(p.GetBoundingBox())]
        for v in unc:
            for it in v.get('items', []):
                mm_ = re.match(r'(?:PTH )?[Pp]ad (\S+) \[GND\] of (\S+)', it.get('description', ''))
                if mm_:
                    fp = b.FindFootprintByReference(mm_.group(2))
                    out += [p for p in fp.Pads() if p.GetNumber() == mm_.group(1) and p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD]
        return out

    m = 0
    todo = off_plane() if fanout is None else [p for r in fanout for p in b.FindFootprintByReference(r).Pads()
                                                 if p.GetNetname() == 'GND' and p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD]
    for rnd in range(4):
      if rnd:
          if fanout:
              break
          todo = drc_gnd_pads()
          if not todo:
              break
      for pad in todo:
          fp = pad.GetParentFootprint()
          p, c = pad.GetPosition(), fp.GetPosition()
          px, py = p.x / 1e6, p.y / 1e6
          me = [it for it in items if it[0] == 'r' and it[1][0] <= px <= it[1][2] and it[1][1] <= py <= it[1][3]][:1]
          me = me[0] if me else None
          # leave the pad straight out, or first inward under the part's body (fine-pitch pins between signals)
          d = np.hypot(c.x / 1e6 - px, c.y / 1e6 - py) or 1
          ux, uy = (c.x / 1e6 - px) / d, (c.y / 1e6 - py) / d
          inward = [[(px, py), (px + ux * k, py + uy * k)] for k in (1.3, 0.9, 1.8)]
          starts = inward + [[(px, py)]] if fanout else [[(px, py)]] + inward
          best = None
          for path in starts:
              hx, hy = path[-1]
              if len(path) > 1 and not seg_clear(px, py, hx, hy, me):
                  continue
              for r in np.arange(0.9, 3.0, 0.1):
                  for a in np.arange(0, 2 * np.pi, np.pi / 24):
                      x, y = float(hx + r * np.cos(a)), float(hy + r * np.sin(a))
                      if spot_ok(x, y) and clear(x, y) and seg_clear(hx, hy, x, y, me):
                          best = path + [(x, y)]
                          break
                  if best:
                      break
              if best:
                  break
          new_via, width = True, tw
          if not best:                  # layer by layer: a track on the pad's layer to a GND via, old or new
              lay = pcbnew.B_Cu if pad.IsOnLayer(pcbnew.B_Cu) and not pad.IsOnLayer(pcbnew.F_Cu) else pcbnew.F_Cu
              lg = geo(b)
              cand = [(np.hypot(g[0] - px, g[1] - py), g[0], g[1], False) for kind, g, nn in items if kind == 'c' and nn == 'GND']
              stuck = {pad.m_Uuid.AsString()}
              for f2 in b.GetFootprints():
                  for q in f2.Pads():
                      if q.GetNetname() == 'GND' and q.IsOnLayer(lay) and q.m_Uuid.AsString() not in stuck:
                          qp = q.GetPosition()
                          cand.append((np.hypot(qp.x / 1e6 - px, qp.y / 1e6 - py), qp.x / 1e6, qp.y / 1e6, False))
              for r in np.arange(0.8, 3.5, 0.2):
                  for ang in np.arange(0, 2 * np.pi, np.pi / 12):
                      x, y = float(px + r * np.cos(ang)), float(py + r * np.sin(ang))
                      if spot_ok(x, y) and off_pads(pads, x, y) and all(seg_ok(lg, l, 'GND', x, y, x, y, rv, gap) for l in CU) and \
                              all(np.hypot(x - g[0], y - g[1]) > 0.9 for kind, g, nn in items if kind == 'c'):
                          cand.append((r + 0.5, x, y, True))
              for w in (tw, 0.15):       # a narrower track squeezes between fine-pitch pins
                  for dist, x, y, nv in sorted(cand):
                      if dist > 6:
                          break
                      for path in shapes((px, py), (x, y)):
                          if all(seg_ok(lg, lay, 'GND', *p0, *p1, w / 2) for p0, p1 in zip(path, path[1:])):
                              best, new_via, width = path, nv, w
                              break
                      if best:
                          break
                  if best:
                      break
          if not best:                  # never a via in the pad: the pour has to reach it
              print('  no via spot for GND pad %s-%s' % (fp.GetReference(), pad.GetNumber()))
              continue
          if new_via:
              via(*best[-1])
          lay = pcbnew.B_Cu if pad.IsOnLayer(pcbnew.B_Cu) and not pad.IsOnLayer(pcbnew.F_Cu) else pcbnew.F_Cu
          for (ax, ay), (ex, ey) in zip(best, best[1:]):
              t = pcbnew.PCB_TRACK(b)
              t.SetStart(VECTOR2I(mm(float(ax)), mm(float(ay))))
              t.SetEnd(VECTOR2I(mm(float(ex)), mm(float(ey))))
              t.SetWidth(mm(width))
              t.SetLayer(lay)
              t.SetNet(gnd)
              t.SetLocked(bool(fanout))
              b.Add(t)
              items.append(('s', (ax, ay, ex, ey, tw / 2), 'GND'))
          m += 1
    if fanout:
        print('GND fanout: %d vias' % m)
        return
    # pour islands that still have no via of their own (a pad's thermal spokes can be their only tie)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    k = 0
    lg = geo(b)
    for z in b.Zones():
        if z.GetIsRuleArea():
            continue
        for L in (pcbnew.F_Cu, pcbnew.B_Cu, pcbnew.In2_Cu):
            if not z.IsOnLayer(L):
                continue
            ps = z.GetFilledPolysList(L)
            for i in range(ps.OutlineCount()):
                o = ps.Outline(i)
                gv = [t for t in b.GetTracks() if t.GetClass() == 'PCB_VIA' and t.GetNetname() == 'GND']
                if any(o.PointInside(t.GetPosition()) for t in gv):
                    continue
                bb = o.BBox()
                done = False
                for yy in np.arange(bb.GetTop() / 1e6 + 0.3, bb.GetBottom() / 1e6 - 0.3, 0.1):
                    for xx in np.arange(bb.GetLeft() / 1e6 + 0.3, bb.GetRight() / 1e6 - 0.3, 0.1):
                        x, y = float(xx), float(yy)
                        if o.PointInside(VECTOR2I(mm(x), mm(y))) and spot_ok(x, y) and off_pads(pads, x, y) and \
                                all(seg_ok(lg, l, 'GND', x, y, x, y, rv, gap) for l in CU) and \
                                all(np.hypot(x - t.GetPosition().x / 1e6, y - t.GetPosition().y / 1e6) > 0.9
                                    for t in b.GetTracks() if t.GetClass() == 'PCB_VIA'):
                            via(x, y)
                            lg = geo(b)
                            k += 1
                            done = True
                            break
                    if done:
                        break
    if k:
        print('island vias:', k)
    left = off_plane()
    print('stitching vias: %d on the grid, %d with a track to a GND pad; %d GND pads off the plane' % (n, m, len(left)))


CU = [pcbnew.F_Cu, pcbnew.B_Cu]
PAD_GAP = 0.2                       # via copper to any pad's copper, same net or not: no via ever sits in or on a pad
VIA_R = 0.3


def pad_boxes(b):
    return [(bb.GetLeft() / 1e6, bb.GetTop() / 1e6, bb.GetRight() / 1e6, bb.GetBottom() / 1e6)
            for fp in b.GetFootprints() for p in fp.Pads() for bb in [p.GetBoundingBox()]]


def off_pads(pads, x, y, r=VIA_R, gap=PAD_GAP):
    """A via at (x, y) clears every pad (a via in or touching a pad wicks the solder away, or has to be plugged)."""
    e = r + gap
    return not any(x1 - e < x < x2 + e and y1 - e < y < y2 + e for x1, y1, x2, y2 in pads)


def vias_on_pads(b, gap=0.13):
    """(via, pad) for every via whose copper comes within gap (the board's clearance) of a pad's copper, any layer,
    same net or not."""
    out = []
    for v in b.GetTracks():
        if v.GetClass() != 'PCB_VIA':
            continue
        r = v.GetWidth(pcbnew.F_Cu) // 2 + mm(gap)
        for fp in b.GetFootprints():
            for p in fp.Pads():
                if any(p.IsOnLayer(l) and p.GetEffectiveShape(l).Collide(v.GetPosition(), r) for l in CU):
                    out.append((v, p))
    return out


_removed = []


def strip_epad_vias(fp):
    """The ESP32 footprint's ground pad holds a grid of 0.2 mm thermal vias (12 small plated pads numbered 41).
    Take them out: vias beside the pad tie it to the plane instead (epad_vias)."""
    for p in [p for p in fp.Pads() if p.GetNumber() == '41' and p.GetDrillSize().x > 0]:
        fp.Remove(p)
        _removed.append(p)              # keep the Python proxy: freeing it crashes pcbnew


def epad_vias(b, ref='U1'):
    """Locked GND vias around the module's ground pad, PAD_GAP clear of it, each with a short track to the pad edge."""
    fp = b.FindFootprintByReference(ref)
    ep = max((p for p in fp.Pads() if p.GetNumber() == '41'), key=lambda p: p.GetSize().x)
    lay = pcbnew.B_Cu if ep.IsOnLayer(pcbnew.B_Cu) else pcbnew.F_Cu
    bb = ep.GetBoundingBox()
    x1, y1, x2, y2 = bb.GetLeft() / 1e6, bb.GetTop() / 1e6, bb.GetRight() / 1e6, bb.GetBottom() / 1e6
    cx, cy, d = (x1 + x2) / 2, (y1 + y2) / 2, VIA_R + PAD_GAP + 0.05
    pads, lg, n = pad_boxes(b), geo(b), 0
    for (vx, vy), (ex, ey) in (((x1 - d, cy), (x1, cy)), ((x2 + d, cy), (x2, cy)), ((cx, y1 - d), (cx, y1)), ((cx, y2 + d), (cx, y2))):
        if not (off_pads(pads, vx, vy) and all(seg_ok(lg, l, 'GND', vx, vy, vx, vy, VIA_R) for l in CU)):
            continue
        v = pcbnew.PCB_VIA(b)
        v.SetPosition(VECTOR2I(mm(vx), mm(vy)))
        v.SetWidth(mm(0.6))
        v.SetDrill(mm(0.3))
        v.SetNet(ep.GetNet())
        v.SetLocked(True)
        b.Add(v)
        t = pcbnew.PCB_TRACK(b)
        t.SetStart(VECTOR2I(mm(vx), mm(vy)))
        t.SetEnd(VECTOR2I(mm(ex), mm(ey)))
        t.SetWidth(mm(0.4))
        t.SetLayer(lay)
        t.SetNet(ep.GetNet())
        t.SetLocked(True)
        b.Add(t)
        n += 1
    print('ground-pad vias beside %s-41: %d' % (ref, n))


def unpad(b):
    """Take out every GND via that sits on or touches a pad, with the tracks that led only to it; stitch() then ties
    the pads and pours to the plane again with vias clear of the pads. A signal via on a pad is an error."""
    bad = {v.m_Uuid.AsString(): v for v, p in vias_on_pads(b)}
    sig = [v for v in bad.values() if v.GetNetname() != 'GND']
    if sig:
        raise SystemExit('signal vias on pads: ' + ', '.join('%s at %.2f, %.2f' % (v.GetNetname(), v.GetPosition().x / 1e6 - OX,
                                                                                  v.GetPosition().y / 1e6 - OY) for v in sig))
    gone = [v.GetPosition() for v in bad.values()]
    for v in bad.values():
        b.Delete(v)
    # stubs: GND tracks left with a loose end, repeatedly (a track that ended on a removed via, then its neighbours)
    pads = [p for fp in b.GetFootprints() for p in fp.Pads()]
    k = 0
    while True:
        tr = [t for t in b.GetTracks() if t.GetNetname() == 'GND']
        vias = [t.GetPosition() for t in tr if t.GetClass() == 'PCB_VIA']
        segs = [t for t in tr if t.GetClass() == 'PCB_TRACK']

        def held(t, q):
            return any(q == w for w in vias) or \
                any(o is not t and o.GetLayer() == t.GetLayer() and (o.GetStart() == q or o.GetEnd() == q) for o in segs) or \
                any(p.GetNetname() == 'GND' and p.IsOnLayer(t.GetLayer()) and p.GetEffectiveShape(t.GetLayer()).Collide(q, 0) for p in pads)
        loose = [t for t in segs if not (held(t, t.GetStart()) and held(t, t.GetEnd()))]
        if not loose:
            break
        for t in loose:
            b.Delete(t)
            k += 1
    print('vias on pads taken out: %d (and %d stub tracks)' % (len(gone), k))


def prune(b):
    """A via too close to a through-hole pin's drill is redundant (the pin reaches its plane through its own hole)."""
    return                      # two layers: no stitching grid to prune
    holes = [(p.GetPosition(), p.GetDrillSize().x) for f in b.GetFootprints() for p in f.Pads() if p.GetDrillSize().x > 0]
    for v in [v for v in b.GetTracks() if v.GetClass() == 'PCB_VIA' and any(
            np.hypot((v.GetPosition().x - q.x) / 1e6, (v.GetPosition().y - q.y) / 1e6) < (v.GetDrill() + d) / 2e6 + 0.26
            for q, d in holes)]:
        b.Delete(v)


def geo(b):
    out = []                        # (layers, kind, g, net)
    for t in b.GetTracks():
        if t.GetClass() == 'PCB_VIA':
            p = t.GetPosition()
            out.append((set(CU), 'c', (p.x / 1e6, p.y / 1e6, t.GetWidth(pcbnew.F_Cu) / 2e6), t.GetNetname()))
        else:
            a, e = t.GetStart(), t.GetEnd()
            out.append(({t.GetLayer()}, 's', (a.x / 1e6, a.y / 1e6, e.x / 1e6, e.y / 1e6, t.GetWidth() / 2e6), t.GetNetname()))
    for fp in b.GetFootprints():
        for pad in fp.Pads():
            bb = pad.GetBoundingBox()
            out.append(({l for l in CU if pad.IsOnLayer(l)}, 'r',
                        (bb.GetLeft() / 1e6, bb.GetTop() / 1e6, bb.GetRight() / 1e6, bb.GetBottom() / 1e6), pad.GetNetname()))
    return out

def seg_ok(items, lay, net, ax, ay, ex, ey, hw, gap=0.14):
    steps = max(2, int(np.hypot(ex - ax, ey - ay) / 0.05))
    for k in range(steps + 1):
        x, y = ax + (ex - ax) * k / steps, ay + (ey - ay) * k / steps
        if not (OX + 0.4 < x < OX + W - 0.4 and OY + 0.4 < y < OY + H - 0.4):
            return False
        for lays, kind, g, n in items:
            if n == net or lay not in lays:
                continue
            if kind == 'c':
                if np.hypot(x - g[0], y - g[1]) < hw + g[2] + gap:
                    return False
            elif kind == 's':
                sx, sy, tx, ty, w = g
                dx, dy = tx - sx, ty - sy
                L = dx * dx + dy * dy
                u = 0 if L == 0 else max(0, min(1, ((x - sx) * dx + (y - sy) * dy) / L))
                if np.hypot(x - sx - u * dx, y - sy - u * dy) < hw + w + gap:
                    return False
            elif g[0] - hw - gap < x < g[2] + hw + gap and g[1] - hw - gap < y < g[3] + hw + gap:
                return False
    return True



def shapes(a, e):
    """Candidate paths from a to e: straight, the two L shapes and the two 45-degree dog-legs."""
    (ax, ay), (ex, ey) = a[:2], e[:2]
    dx, dy = ex - ax, ey - ay
    k = min(abs(dx), abs(dy))
    sx, sy = np.sign(dx), np.sign(dy)
    return [[(ax, ay), (ex, ey)], [(ax, ay), (ex, ay), (ex, ey)], [(ax, ay), (ax, ey), (ex, ey)],
            [(ax, ay), (ax + sx * k, ay + sy * k), (ex, ey)], [(ax, ay), (ex - sx * k, ey - sy * k), (ex, ey)]]


def finish(b):
    """Join what Freerouting left of a net in separate pieces: the closest pair of pads / track ends between two
    pieces, by a straight or L-shaped track on a layer both ends are on, if it clears everything else."""
    b.BuildConnectivity()
    c = b.GetConnectivity()
    fixed = 0
    for ni in list(b.GetNetInfo().NetsByName().values()):
        net = ni.GetNetname()
        if not net or net == 'GND' or net.startswith('unconnected'):
            continue
        members = [t for t in b.GetTracks() if t.GetNetname() == net] + \
                  [p for fp in b.GetFootprints() for p in fp.Pads() if p.GetNetname() == net]
        clusters, seen = [], set()
        for m in members:
            k = m.m_Uuid.AsString()
            if k in seen:
                continue
            cl = {k: m}
            for o in c.GetConnectedItems(m):
                cl[o.m_Uuid.AsString()] = o
            seen |= set(cl)
            clusters.append(list(cl.values()))
        if len(clusters) < 2:
            continue

        def ends(cl):
            out = []
            for it in cl:
                if it.GetClass() == 'PAD':
                    p = it.GetPosition()
                    out.append((p.x / 1e6, p.y / 1e6, {l for l in CU if it.IsOnLayer(l)}))
                elif it.GetClass() == 'PCB_VIA':
                    p = it.GetPosition()
                    out.append((p.x / 1e6, p.y / 1e6, set(CU)))
                elif it.GetClass() == 'PCB_TRACK':     # its ends and points along it
                    a_, e_ = it.GetStart(), it.GetEnd()
                    n_ = max(1, int(it.GetLength() / 1e6 / 0.5))
                    for k in range(n_ + 1):
                        out.append(((a_.x + (e_.x - a_.x) * k / n_) / 1e6, (a_.y + (e_.y - a_.y) * k / n_) / 1e6, {it.GetLayer()}))
            return out

        hw = (0.3 if net in POWER_NETS else 0.2) / 2
        items = geo(b)
        pads = pad_boxes(b)
        main_ = ends(clusters[0])
        for cl in clusters[1:]:
            pairs = sorted(((np.hypot(a[0] - e[0], a[1] - e[1]), a, e) for a in main_ for e in ends(cl)), key=lambda t: t[0])
            done = False
            for d, a, e in pairs[:60]:
                if d > 10:
                    break
                for lay in CU:
                    if lay not in a[2] or lay not in e[2]:
                        continue
                    for path in shapes(a, e):
                        if all(seg_ok(items, lay, net, *p0, *p1, hw) for p0, p1 in zip(path, path[1:])):
                            for p0, p1 in zip(path, path[1:]):
                                if p0 == p1:
                                    continue
                                t = pcbnew.PCB_TRACK(b)
                                t.SetStart(VECTOR2I(mm(float(p0[0])), mm(float(p0[1]))))
                                t.SetEnd(VECTOR2I(mm(float(p1[0])), mm(float(p1[1]))))
                                t.SetWidth(mm(hw * 2))
                                t.SetLayer(lay)
                                t.SetNet(ni)
                                b.Add(t)
                            done = True
                            break
                    if done:
                        break
                if done:
                    break
            if not done:                # through one via beside either end
                for d, a, e in pairs[:40]:
                    for s0, s1 in ((a, e), (e, a)):
                        for r in np.arange(0.7, 3.6, 0.15):
                            for ang in np.arange(0, 2 * np.pi, np.pi / 12):
                                vx, vy = float(s0[0] + r * np.cos(ang)), float(s0[1] + r * np.sin(ang))
                                if not off_pads(pads, vx, vy) or not all(seg_ok(items, l, net, vx, vy, vx, vy, 0.3) for l in CU):
                                    continue
                                l0 = next((l for l in CU if l in s0[2] and seg_ok(items, l, net, s0[0], s0[1], vx, vy, hw)), None)
                                if l0 is None:
                                    continue
                                for l1 in CU:
                                    if l1 not in s1[2]:
                                        continue
                                    for path in shapes((vx, vy), s1):
                                        if all(seg_ok(items, l1, net, *p0, *p1, hw) for p0, p1 in zip(path, path[1:])):
                                            for lay, (p0, p1) in [(l0, (s0[:2], (vx, vy)))] + [(l1, q) for q in zip(path, path[1:])]:
                                                if p0 == p1:
                                                    continue
                                                t = pcbnew.PCB_TRACK(b)
                                                t.SetStart(VECTOR2I(mm(float(p0[0])), mm(float(p0[1]))))
                                                t.SetEnd(VECTOR2I(mm(float(p1[0])), mm(float(p1[1]))))
                                                t.SetWidth(mm(hw * 2))
                                                t.SetLayer(lay)
                                                t.SetNet(ni)
                                                b.Add(t)
                                            v = pcbnew.PCB_VIA(b)
                                            v.SetPosition(VECTOR2I(mm(vx), mm(vy)))
                                            v.SetWidth(mm(0.6))
                                            v.SetDrill(mm(0.3))
                                            v.SetNet(ni)
                                            b.Add(v)
                                            done = True
                                            break
                                    if done:
                                        break
                                if done:
                                    break
                            if done:
                                break
                        if done:
                            break
                    if done:
                        break
            if done:
                fixed += 1
                main_ += ends(cl)
            else:
                print('  could not join a piece of', net)
    print('finisher: %d links added' % fixed)


def bridge_usb_vbus(b, locked=False):
    """The USB-C connector's two VBUS pad pairs (A4/B9, A9/B4) sit 4.8 mm apart with CC and D+/D- between them;
    Freerouting sometimes leaves them apart. Join them behind the pad row, under the connector body (B.Cu), as the
    first routed version of this board did. With locked=True (before routing) the join is drawn whatever the board
    holds and locked, so Freerouting routes around it instead of dropping a via where it goes."""
    j2 = next(f for f in b.GetFootprints() if f.GetReference() == 'J2')
    vb = [p for p in j2.Pads() if 'VBUS' in p.GetNetname()]
    b.BuildConnectivity()
    joined = {o.m_Uuid.AsString() for o in b.GetConnectivity().GetConnectedItems(vb[0])} | {vb[0].m_Uuid.AsString()}
    if all(q.m_Uuid.AsString() in joined for q in vb) and not locked:
        return
    ox, oy = j2.GetPosition().x / 1e6, j2.GetPosition().y / 1e6
    pts = [(-2.4, -3.68), (-2.4, -3.3), (-1.58, -2.48), (1.59, -2.48), (2.4, -3.29), (2.4, -3.68)]
    for a, e in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(b)
        t.SetStart(VECTOR2I(mm(ox + a[0]), mm(oy + a[1])))
        t.SetEnd(VECTOR2I(mm(ox + e[0]), mm(oy + e[1])))
        t.SetWidth(mm(0.2))
        t.SetLayer(pcbnew.B_Cu)
        t.SetNet(vb[0].GetNet())
        t.SetLocked(locked)
        b.Add(t)
    print('  joined the USB-C VBUS pad pairs behind the pad row')


def unrouted(b):
    b.BuildConnectivity()
    return b.GetConnectivity().GetUnconnectedCount(False)


def freeroute(jar):
    """Freerouting 1.9.0 (Java 17+; opens its window while it works). 2.1 does not finish this board, and 1.9 hangs
    when the DSN already has wiring, so it is one run from an unrouted board."""
    dsn, ses = os.path.join(TMP, 'consoleboardrs.dsn'), os.path.join(TMP, 'consoleboardrs.ses')
    for rnd in range(1):
        b = pcbnew.LoadBoard(PCB)
        rules(b)
        epad_vias(b)
        stitch(b, fanout=['U2', 'U3', 'U4'] +      # every GND pad but the module's has its own via
               sorted(f.GetReference() for f in b.GetFootprints() if re.match(r'(C|R|JP|SW)\d', f.GetReference())))
        bridge_usb_vbus(b, locked=True)    # before routing, so no via lands where it goes
        pcbnew.SaveBoard(PCB, b)
        for z in list(b.Zones()):
            if not z.GetIsRuleArea():
                b.Remove(z)             # no plane: Freerouting routes GND like any net; the pours come back after
        pcbnew.ExportSpecctraDSN(b, dsn)
        if os.path.exists(ses):
            os.remove(ses)
        java = os.environ.get('JAVA', 'java')
        subprocess.check_call([java, '-jar', jar, '-de', dsn, '-do', ses, '-mp', os.environ.get('FR_PASSES', '120'), '-mt', '1'], cwd=TMP,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        b = pcbnew.LoadBoard(PCB)
        rules(b)
        assert pcbnew.ImportSpecctraSES(b, ses)
        finish(b)
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        left = unrouted(b)
        pcbnew.SaveBoard(PCB, b)
        print('round %d: %d tracks and vias, %d unrouted' % (rnd + 1, len(b.GetTracks()), left))
        if left == 0:
            break
    b = pcbnew.LoadBoard(PCB)
    rules(b)
    unpad(b)                    # Freerouting and the finisher may still leave a via on a pad
    stitch(b)
    prune(b)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    left = vias_on_pads(b)
    pcbnew.SaveBoard(PCB, b)
    assert not left, 'vias on pads: %d' % len(left)


if __name__ == '__main__':
    if '--fill' in sys.argv or '--stitch' in sys.argv or '--unpad' in sys.argv:     # refill (and stitch) the routed board
        bb = pcbnew.LoadBoard(PCB)
        rules(bb)
        if '--unpad' in sys.argv:          # move the vias off the pads of the routed board, without routing it again
            strip_epad_vias(bb.FindFootprintByReference('U1'))
            unpad(bb)
            epad_vias(bb)
        if '--stitch' in sys.argv or '--unpad' in sys.argv:
            stitch(bb)
            prune(bb)
        pcbnew.ZONE_FILLER(bb).Fill(bb.Zones())
        left = vias_on_pads(bb)
        for v, p in left:
            print('  via on pad %s-%s at %.2f, %.2f' % (p.GetParentFootprint().GetReference(), p.GetNumber(),
                                                        v.GetPosition().x / 1e6 - OX, v.GetPosition().y / 1e6 - OY))
        pcbnew.SaveBoard(PCB, bb)
    else:
        main()
