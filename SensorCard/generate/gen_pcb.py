#!/usr/bin/env python3
"""Build SensorCard.kicad_pcb from the schematic's netlist with KiCad's pcbnew module (KiCad 10).

Run with KiCad's python:  $KIPY generate/gen_pcb.py [--route FREEROUTING_JAR]
  macOS: KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3

Copied from GPSCard/generate/gen_pcb.py and adapted. The board is an 80 x 60 mm slot card
(docs/hardware/accessory_ports.md): it stands on edge in a DigiDash slot, its right-angle 2x15 header centred on the
bottom edge; the sender (J2), speed (J3) and tach (J4) connectors on the top edge; the sensor serial out (J8), FTDI
(J7) and ISP (J6) headers right angle on the left edge. Four layers (F signal, In1 GND plane, In2 signal / GND fill,
B signal), the GPS card's rules, plus a wide-clearance class for the tach input. Every part is on F (the parts side);
B is empty. ICs and connectors are placed by hand below; every passive is then put at the free spot closest to the
pins it connects to. Not-fitted parts (DNP in the schematic) are placed and marked DNP. With --route it runs
Freerouting on a Specctra export and imports the result, then fills the ground planes and stitches them.
"""
import json
import os
import re
import shutil
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
SCH = os.path.join(ROOT, 'SensorCard.kicad_sch')
PCB = os.path.join(ROOT, 'SensorCard.kicad_pcb')
PRO = os.path.join(ROOT, 'SensorCard.kicad_pro')
mm = FromMM

W, H = 80.0, 60.0          # a slot card: the bottom edge (y = H) goes into the slot
J2_X = 27.0                # top edge: the sender connector's centre (JST PH 12, side entry)
J3_X = 50.0                # the speed connector's centre (JST PH 4)
J4_X = 71.5                # the tach connector's centre (Micro-Fit 3.0, 2 pin): the far end, away from the analog side
PH_ROW = 6.3               # JST PH side entry turned 180: pin row 6.3 mm in puts the body's face on the edge
MF_ROW = 9.07              # Micro-Fit 43650 (turned 0): pin row 9.07 mm in puts the mating face on the edge
J8_Y = 17.0                # left edge, right angle, pin 1 the lowest pin: sensor serial out (1x3)
J7_Y = 35.0                # FTDI (1x6)
J6_Y = 45.0                # ISP (2x3)
HDR_X = 4.04               # right-angle 2.54 mm headers: pad row 4.04 mm in, so the body's outer face is on the edge
HDR2_X = 6.58              # a 2-row right-angle header: pin 1's column (the inner one) 6.58 mm in
# Slot header: pin 1 at 17.78 mm right of the centre (the pins count to the left), its plastic body's outer face on
# the bottom edge, so the pins stand 6 mm past the edge into the socket.
J1_PIN1 = (W / 2 + 17.78, H - 6.58)
OX, OY = 100.0, 80.0       # board origin on the sheet


def P(x, y):
    return VECTOR2I(mm(OX + x), mm(OY + y))


def netlist():
    xml = os.path.join(TMP, 'sensorcard_netlist.xml')
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
            name = n.get('name')
            if name.startswith('unconnected-('):   # the schematic spells '/' inside these as {slash}
                name = 'unconnected-(' + name[len('unconnected-('):].replace('/', '{slash}')
            nets[(node.get('ref'), node.get('pin'))] = name
    return comps, nets


def load(fpid):
    lib, name = fpid.split(':')
    fp = pcbnew.FootprintLoad(os.path.join(FPDIR, lib + '.pretty'), name)
    assert fp, fpid
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


# ---------------------------------------------------------------------------------------------- placement plan
# (ref: (x, y, rotation, side)). J1 (the slot header) is placed in main().
FIXED = {
    # top edge (JST PH side entry turned 180: pad 1 on the right, seen from the parts side)
    'J2': (J2_X + 11.0, PH_ROW, 180, 'F'),
    'J3': (J3_X + 3.0, PH_ROW, 180, 'F'),
    'J4': (J4_X - 1.5, MF_ROW, 0, 'F'),       # Micro-Fit turned 0: the mating face toward the top edge, pad 1 left
    'JP1': (J3_X, 13.2, 0, 'F'),              # speed sensor supply: pad 1 (5V) left, pad 3 (12V) right
    'U5': (J4_X, 21.0, 90, 'F'),              # tach optocoupler right behind J4 (the high-voltage side stays up here)
    # left edge, right angle, pins off the edge (rotation 180: pin 1 lowest)
    'J8': (HDR_X, J8_Y, 180, 'F'),
    'J7': (HDR_X, J7_Y, 180, 'F'),
    'J6': (HDR2_X, J6_Y, 180, 'F'),
    # chips
    'U1': (32.0, 30.0, 0, 'F'),               # ATmega2560: ADC pins (top side) toward J2, ICP / USART1 / TWI (bottom) toward the slot
    'Y1': (21.0, 41.5, 90, 'F'),              # 2560 crystal beside XTAL1 / XTAL2 (pins 33 / 34, bottom-left corner)
    'U2': (66.6, 33.0, 0, 'F'),               # MCP2515 (east of the sender config block)
    'Y2': (66.6, 42.5, 0, 'F'),               # its crystal (OSC1 / OSC2, pins 7 / 8)
    'U3': (53.0, H - 15.0, 0, 'F'),           # MCP2562 above the slot header's CAN pins (5 / 6)
    'U4': (43.5, H - 15.0, 0, 'F'),           # FRAM
    # jumpers and the spare header (vertical, parts side)
    'J5': (11.0, 50.0, 0, 'F'),               # RPM select (2x2)
    'J9': (18.5, 50.0, 0, 'F'),               # demo (1x2)
    'J10': (74.6, 30.0, 0, 'F'),              # spare I/O (2x5), below the optocoupler
}
SIDE = {'port': 'F', 'mcu': 'F', 'prog': 'F', 'sensors': 'F', 'speed': 'F', 'tach': 'F', 'misc': 'F'}
ANCHOR = {'port': ['J1'], 'mcu': ['U1'], 'prog': ['J6', 'J7', 'J8'], 'sensors': ['J2', 'U1'], 'speed': ['J3', 'JP1'],
          'tach': ['U5', 'J4']}
GROUP_ORDER = ['tach', 'speed', 'mcu', 'sensors', 'port', 'prog', 'misc']
POWER_NETS = {'+5V', '/AVCC', '/SLOT_12V', '/SLOT_BATT', '/ENGINE_TEMP_VIN', '/OIL_PRESSURE_VIN', '/SPEED_VIN',
              '/SPEED_VSEL'}       # local labels carry the sheet path
HV_NETS = ['/TACH', '/TACH_R']     # the coil input up to the two series resistors: wide clearance
KEEP_SILK = {'J1', 'J2', 'J3', 'J4', 'J5', 'J6', 'J7', 'J8', 'J9', 'J10', 'JP1'}
# The sender config block: one column per channel between the 2560 and the MCP2515, the topology jumper (pull-up /
# pull-down) on top and the value jumpers below it, each labelled on the silk with its value (* = bridged as delivered).
CFG_X0, CFG_Y0, CFG_DX = 42.4, 21.0, 4.7
CFG = [('FUEL', 'JP2', [('JP6', '100'), ('JP7', '220*'), ('JP8', '470')]),
       ('TEMP', 'JP3', [('JP9', '220'), ('JP10', '1k*'), ('JP11', '10k')]),
       ('OIL', 'JP4', [('JP12', '220*'), ('JP13', '1k'), ('JP14', '100k')]),
       ('AMB', 'JP5', [('JP15', '10k'), ('JP16', '100k*')])]
CFG_TOPO_DY, CFG_VAL_DY0, CFG_VAL_PITCH = 4.0, 9.0, 3.6
DESC_ANCHOR = re.compile(r'^(U\d+|Y\d+)\b')   # 'U2 ...', 'Y1 load': a decoupling or load part sits by that chip
HOLES = []                                # a slot card has no mounting holes: the slot and the case hold it
HOLE_KEEP = 3.3                          # radius kept free of parts around them

GRID = 0.2
NX, NY = int(round(W / GRID)), int(round(H / GRID))
STITCH_PITCH = 3.0          # ground stitching grid


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
    if c.OutlineCount() == 0:          # no courtyard on this side: use the pins
        bx = [bb_mm(p.GetBoundingBox()) for p in fp.Pads() if not p.GetNumber().startswith('MP')]
        return (min(v[0] for v in bx) - 0.25, min(v[1] for v in bx) - 0.25,
                max(v[2] for v in bx) + 0.25, max(v[3] for v in bx) + 0.25)
    return bb_mm(c.BBox())


def main():
    route = sys.argv[sys.argv.index('--route') + 1] if '--route' in sys.argv else None
    comps, nets = netlist()
    groups = json.load(open(os.path.join(HERE, 'refs.json')))
    b = pcbnew.BOARD()
    b.SetCopperLayerCount(4)
    b.SetLayerType(pcbnew.In1_Cu, pcbnew.LT_POWER)      # solid GND plane: the router only drops vias into it
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
        b.Add(fp)
        for pad in fp.Pads():
            n = nets.get((ref, pad.GetNumber()))
            if pad.GetNumber() in ('MP1', 'MP2'):
                n = 'GND'
            if n:
                pad.SetNet(net(n))
        fps[ref] = fp
        if groups.get(ref, {}).get('dnp'):
            fp.SetDNP(True)             # not fitted as delivered (the schematic's DNP flag)
        g = groups.get(ref, {}).get('group', 'misc')
        side_of[ref] = FIXED[ref][3] if ref in FIXED else SIDE[g]

    occ = {'F': Occ(), 'B': Occ()}
    for hx, hy in HOLES:
        occ['F'].circle(hx, hy, HOLE_KEEP)
        occ['B'].circle(hx, hy, HOLE_KEEP)

    def put(ref, x, y, rot, side):
        fp = fps[ref]
        if side == 'B' and not fp.IsFlipped():
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        fp.SetOrientation(EDA_ANGLE(rot, pcbnew.DEGREES_T))
        fp.SetPosition(P(x, y))

    def mark(ref):
        """Occupy every courtyard outline of the part (separately: J1's are two hole circles), and for through-hole
        parts their pins on both sides plus the other side's courtyard."""
        fp = fps[ref]
        side = side_of[ref]
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

    # the slot header: pins off the bottom edge, pad 2 below pad 1 (the row nearer the edge, the lower row of pins) and
    # pad 3 left of pad 1. Turned this way each pin meets the same-numbered pad of the main board's slot socket.
    for rot in (0, 90, 180, 270):
        put('J1', 0, 0, rot, 'F')
        q = {p.GetNumber(): p.GetPosition() for p in fps['J1'].Pads()}
        d2 = (q['2'].x - q['1'].x, q['2'].y - q['1'].y)
        d3 = (q['3'].x - q['1'].x, q['3'].y - q['1'].y)
        if d2[1] > 0 and abs(d2[0]) < 1000 and d3[0] < 0 and abs(d3[1]) < 1000:
            break
    else:
        raise SystemExit('no orientation of J1 puts pad 2 below pad 1 and pad 3 to its left')
    side_of['J1'] = 'F'
    put('J1', J1_PIN1[0], J1_PIN1[1], rot, 'F')
    mark('J1')
    for ref, (x, y, rot, side) in FIXED.items():
        put(ref, x, y, rot, side)
        mark(ref)
    # the sender config block: jumpers stand vertically, pad 1 at the top (topology: pad 1 = +5 V = pull-up)
    cfg_refs = []

    def put_vertical(ref, x, y):
        side_of[ref] = 'F'
        for rot in (270, 90):
            put(ref, x, y, rot, 'F')
            q = {p.GetNumber(): p.GetPosition().y for p in fps[ref].Pads()}
            if q['1'] < q['2']:
                break
        mark(ref)
        cfg_refs.append(ref)

    for i, (name, topo, vals) in enumerate(CFG):
        x = CFG_X0 + i * CFG_DX
        put_vertical(topo, x, CFG_Y0 + CFG_TOPO_DY)
        for k, (jp, _) in enumerate(vals):
            put_vertical(jp, x, CFG_Y0 + CFG_VAL_DY0 + CFG_VAL_PITCH * k)
    occ['F'].rect(CFG_X0 - 2.0, CFG_Y0 - 0.6, CFG_X0 + 3 * CFG_DX + 3.6,
                  CFG_Y0 + CFG_VAL_DY0 + 2 * CFG_VAL_PITCH + 1.8)   # the block and its labels
    occ['F'].rect(8.4, J8_Y - 6.0, 12.0, J8_Y + 1.0)         # room for the left-edge header pin legends
    occ['F'].rect(8.4, J7_Y - 13.6, 12.0, J7_Y + 1.0)
    occ['F'].rect(8.9, J6_Y - 6.0, 15.4, J6_Y + 1.0)
    occ['F'].rect(J2_X - 12.0, 7.9, J2_X + 12.0, 11.9)       # J2's pin legends
    occ['F'].rect(J3_X - 4.0, 7.9, J3_X + 4.0, 11.9)         # J3's
    occ['F'].rect(J3_X - 3.2, 14.4, J3_X + 3.2, 16.0)        # JP1's
    occ['F'].rect(J4_X - 5.0, 10.8, J4_X + 5.0, 12.0)        # J4's
    occ['F'].rect(0.6, 2.0, 13.5, 8.6)                       # the card's name
    occ['F'].rect(J2_X + 13.6, 2.8, J2_X + 16.0, 5.2)        # J2's reference
    occ['F'].rect(74.2, 26.4, 77.6, 28.8)                    # J10's reference
    occ['F'].rect(J3_X - 4.4, 12.6, J3_X - 1.6, 14.6)        # JP1's reference

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

    placed = set(FIXED) | {'J1'} | set(cfg_refs)
    rest = [r for r in fps if r not in placed]

    def prio(r):
        g = groups.get(r, {}).get('group', 'misc')
        nn = {p.GetNetname() for p in fps[r].Pads()}
        return (GROUP_ORDER.index(g), 0 if r.startswith('U') else 1, 0 if (nn & POWER_NETS and 'GND' in nn) else 1,
                0 if fps[r].GetValue() == '100n' else 1, r)

    rest.sort(key=prio)
    descr_of = {r: comps[r][3] for r in comps}
    xs = (np.arange(NX) + 0.5) * GRID
    ys = (np.arange(NY) + 0.5) * GRID
    for ref in rest:
        fp = fps[ref]
        side = side_of[ref]
        g = groups.get(ref, {}).get('group', 'misc')
        anchors = set(ANCHOR.get(g, []))
        m_ = DESC_ANCHOR.match(descr_of.get(ref, ''))
        if m_ and m_.group(1) in fps:
            anchors = {m_.group(1)}     # a chip's own decoupling / load part: next to that chip
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
    for item in fps['J1'].GraphicalItems():        # its 'socket on bottom' note: not wanted here
        if item.GetClass() == 'PCB_TEXT':
            item.SetVisible(False)
    for ref, dx, dy in [('J1', 3.5, 0), ('J2', 3.6, -2.3), ('J3', 3.6, -2.3), ('J4', 8.0, -5.0), ('JP1', -3.0, 0.6),
                        ('J8', 9.5, -2.54), ('J7', 9.5, -6.35), ('J6', 6.8, -5.08), ('J5', 1.27, -2.4), ('J9', 0, -2.4),
                        ('J10', 1.27, -2.4)]:
        fp = fps[ref]
        t = fps[ref].Reference()
        t.SetTextAngle(EDA_ANGLE(90 if ref in ('J4', 'J7', 'J8') else 0, pcbnew.DEGREES_T))
        t.SetPosition(P(fp.GetPosition().x / 1e6 - OX + dx, fp.GetPosition().y / 1e6 - OY + dy))

    def silk(s, x, y, size=0.7, layer=pcbnew.F_SilkS, rot=0):
        t = pcbnew.PCB_TEXT(b)
        t.SetText(s)
        t.SetLayer(layer)
        t.SetPosition(P(x, y))
        t.SetTextSize(VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(0.12 if size < 1 else 0.15))
        t.SetTextAngle(EDA_ANGLE(rot, pcbnew.DEGREES_T))
        t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
        if layer == pcbnew.B_SilkS:
            t.SetMirrored(True)
            t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_RIGHT)
        b.Add(t)

    # pin legends beside the left-edge headers (their pin 1 is the lowest pin)
    for x0, y0, names in ((8.6, J8_Y, ['GND', 'TX', 'RX']), (8.6, J7_Y, ['GND', 'CTS', '--', 'RXI', 'TXO', 'DTR'])):
        for k, s in enumerate(names):
            silk(s, x0, y0 - 2.54 * k, 0.8)
    silk('ISP', 9.1, J6_Y - 5.08, 0.8)
    silk('1 MISO 2 5V', 9.1, J6_Y - 2.9, 0.7)
    silk('3 SCK 4 MOSI', 9.1, J6_Y - 1.5, 0.7)
    silk('5 RST 6 GND', 9.1, J6_Y - 0.1, 0.7)
    # top-edge connectors' pins, below them (pad 1 is on the right for the JST PH ones, seen from the parts side)
    for k, s in enumerate(['FUEL', 'G', 'TEMP', 'TV', 'G', 'OIL', 'OV', 'G', 'AMB', 'G', 'ALT', 'G']):
        silk(s, J2_X + 11.0 - 2.0 * k, 11.7, 0.7, rot=90)
    for k, s in enumerate(['PWR', 'SPD', 'G', 'G']):
        silk(s, J3_X + 3.0 - 2.0 * k, 11.7, 0.7, rot=90)
    silk('TACH', J4_X - 4.4, 11.0, 0.7)
    silk('G', J4_X + 1.2, 11.0, 0.7)
    silk('5V', J3_X - 2.9, 14.7, 0.7)
    silk('12V', J3_X + 1.2, 14.7, 0.7)
    # the sender config block's labels
    silk('SENDER CONFIG', CFG_X0 - 1.6, CFG_Y0 - 0.2, 0.7)
    for i, (name, topo, vals) in enumerate(CFG):
        x = CFG_X0 + i * CFG_DX
        silk(name, x - 1.6, CFG_Y0 + 1.1, 0.7)
        silk('PU', x + 1.0, CFG_Y0 + CFG_TOPO_DY - 1.6, 0.7)
        silk('PD', x + 1.0, CFG_Y0 + CFG_TOPO_DY + 1.6, 0.7)
        for k, (_, lab) in enumerate(vals):
            silk(lab, x + 1.0, CFG_Y0 + CFG_VAL_DY0 + CFG_VAL_PITCH * k, 0.7)
    silk('SENSOR CARD', 1.0, 3.2, 1.0)
    silk('rev A  node 0x04', 1.0, 4.9, 0.8)
    silk('not in slot 1', 1.0, 6.5, 0.8)
    silk('SENSOR CARD  rev A', 46.0, 15.0, 1.0, pcbnew.B_SilkS)   # mirrored: the text runs to +x from here
    # the configuration table on the back (it carries no parts): what each front-side jumper does
    for k, s in enumerate(['SENDER CONFIG (block on the front)',
                           'One column per channel. Top jumper:',
                           'PU (pull-up, as delivered) or PD',
                           'Below: bridge ONE value (ohms)',
                           '(two bridged = in parallel)',
                           '* = bridged as delivered',
                           'FUEL JP2: 100 JP6 220 JP7 470 JP8',
                           'TEMP JP3: 220 JP9 1k JP10 10k JP11',
                           'OIL JP4: 220 JP12 1k JP13 100k JP14',
                           'AMB JP5: 10k JP15 100k JP16',
                           'SPEED PWR JP1: 1-2 5V*  2-3 12V',
                           'RPM SEL J5: none 8  A 4  B 6 pulses/rev',
                           'DEMO J9: fitted = fake sweep (bench)',
                           'CAN node 0x04. Not slot 1']):
        silk(s, 46.0, 18.0 + 1.4 * k, 0.75, pcbnew.B_SilkS)

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
    for lay in (pcbnew.In1_Cu, pcbnew.F_Cu, pcbnew.B_Cu, pcbnew.In2_Cu):
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
    k.SetLayerSet(pcbnew.LSET.AllCuMask(4))
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


ROUTE_MARGIN = 0.01         # extra clearance for Freerouting only: its rounding can land a hair under the DRC rule


def rules(b, margin=0.0):
    fab_origin(b)
    """Net classes and constraints, set on the board (SaveBoard writes them to the project file)."""
    ds = b.GetDesignSettings()
    ds.m_CopperEdgeClearance = mm(0.2)
    ds.m_MinClearance = mm(0.13)
    ds.m_TrackMinWidth = mm(0.15)
    ds.m_MinSilkTextHeight = mm(0.7)
    ds.m_MinResolvedSpokes = 1          # header pins also reach the In1 plane through their holes
    ns = ds.m_NetSettings
    d = ns.GetDefaultNetclass()
    pwr = pcbnew.NETCLASS('Power')
    hv = pcbnew.NETCLASS('HV')          # the coil input: kickback of a few hundred volts before the series resistors
    for nc, w, clr in ((d, 0.2, 0.13), (pwr, 0.3, 0.13), (hv, 0.3, 1.0)):
        nc.SetClearance(mm(clr + margin))
        nc.SetTrackWidth(mm(w))
        nc.SetViaDiameter(mm(0.6))
        nc.SetViaDrill(mm(0.3))
    pwr.SetPriority(1)
    hv.SetPriority(0)
    ns.SetNetclass('Power', pwr)
    ns.SetNetclass('HV', hv)
    ns.ClearNetclassPatternAssignments()
    for n in ['GND'] + sorted(POWER_NETS):
        ns.SetNetclassPatternAssignment(n, 'Power')
    for n in HV_NETS:
        ns.SetNetclassPatternAssignment(n, 'HV')
    ns.RecomputeEffectiveNetclasses()
    b.SynchronizeNetsAndNetClasses(True)


def stitch(b, pitch=2.0, fanout=None):
    """GND vias so every GND pad and every piece of the F / B / In2 pours reaches the In1 plane: a grid of vias
    wherever they clear everything (not under the antenna, the screw heads or a part), then, for each GND pad the
    filled pours still leave off the plane, a via beside it with a short track.
    With fanout=[refs] (before routing) it only gives every GND pin of those parts its own via, escaping inward under
    the body first, locked so the router works around them: fine-pitch GND pins between signals get boxed in otherwise."""
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

    def clear(x, y, r=rv, skip=None, gnd_ok=False):
        for it in items:
            if it is skip or (gnd_ok and it[2] == 'GND'):
                continue
            kind, g, n = it
            if n == 'GND' and (kind == 's' or (kind == 'r' and (g[2] - g[0]) * (g[3] - g[1]) > 1.0)):
                continue                # GND on GND: tracks, and the big pads (the module's ground field)
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
        tmp = os.path.join(TMP, 'sensorcard_stitch_check.kicad_pcb')
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        pcbnew.SaveBoard(tmp, b)
        rpt = os.path.join(TMP, 'sensorcard_stitch_check.json')
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
                      if spot_ok(x, y) and all(seg_ok(lg, l, 'GND', x, y, x, y, rv, gap) for l in CU) and \
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
          if not best and all(seg_ok(geo(b), l, 'GND', px, py, px, py, rv, gap) for l in CU):
              via(px, py)               # last resort: via in the pad (hand-solderable; plug it if paste-reflowing)
              print('  via in pad for GND pad %s-%s' % (fp.GetReference(), pad.GetNumber()))
              m += 1
              continue
          if not best:
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
                bx1, by1, bx2, by2 = bb.GetLeft() / 1e6, bb.GetTop() / 1e6, bb.GetRight() / 1e6, bb.GetBottom() / 1e6
                # only the copper near this island: a dense card has thousands of items and many small islands
                near = [it for it in lg if _bbox(it)[0] < bx2 + 1 and _bbox(it)[2] > bx1 - 1 and
                        _bbox(it)[1] < by2 + 1 and _bbox(it)[3] > by1 - 1]
                vias_xy = [(t.GetPosition().x / 1e6, t.GetPosition().y / 1e6) for t in b.GetTracks() if t.GetClass() == 'PCB_VIA']
                vias_xy = [(vx, vy) for vx, vy in vias_xy if bx1 - 1 < vx < bx2 + 1 and by1 - 1 < vy < by2 + 1]
                done = False
                for yy in np.arange(by1 + 0.3, by2 - 0.3, 0.2):
                    for xx in np.arange(bx1 + 0.3, bx2 - 0.3, 0.2):
                        x, y = float(xx), float(yy)
                        if o.PointInside(VECTOR2I(mm(x), mm(y))) and spot_ok(x, y) and \
                                all(seg_ok(near, l, 'GND', x, y, x, y, rv, gap) for l in CU) and \
                                all(np.hypot(x - vx, y - vy) > 0.9 for vx, vy in vias_xy):
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


CU = [pcbnew.F_Cu, pcbnew.In2_Cu, pcbnew.B_Cu]


def _bbox(it):
    """(x1, y1, x2, y2) of a geo() item."""
    _, kind, g, _ = it
    if kind == 'c':
        return (g[0] - g[2], g[1] - g[2], g[0] + g[2], g[1] + g[2])
    if kind == 's':
        return (min(g[0], g[2]) - g[4], min(g[1], g[3]) - g[4], max(g[0], g[2]) + g[4], max(g[1], g[3]) + g[4])
    return g


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
                    out.append((p.x / 1e6, p.y / 1e6, {l for l in CU if it.IsOnLayer(l)}, 'pad'))
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
        main_ = ends(clusters[0])
        all_items = items

        def local(a, e, m=5.0):
            """The copper within m mm of the box around a and e (checking the whole dense board per step is too slow)."""
            x1, x2 = min(a[0], e[0]) - m, max(a[0], e[0]) + m
            y1, y2 = min(a[1], e[1]) - m, max(a[1], e[1]) + m
            return [it for it in all_items if _bbox(it)[0] < x2 and _bbox(it)[2] > x1 and _bbox(it)[1] < y2 and _bbox(it)[3] > y1]

        for cl in clusters[1:]:
            pairs = sorted(((np.hypot(a[0] - e[0], a[1] - e[1]), a, e) for a in main_ for e in ends(cl)), key=lambda t: t[0])
            done = False
            for d, a, e in pairs[:60]:
                if d > 10:
                    break
                items = local(a, e)
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
            if not done:                # through one via beside either end: the closest pairs, then every pad-to-pad pair
                # (the closest pairs are often all points along tracks, never the two pads the link should join)
                pad_pairs = [p_ for p_ in pairs if len(p_[1]) > 3 and len(p_[2]) > 3 and p_[0] < 12]
                for d, a, e in pairs[:12] + pad_pairs:
                    items = local(a, e)
                    for s0, s1 in ((a, e), (e, a)):
                        for r in np.arange(0.7, 3.6, 0.15):
                            for ang in np.arange(0, 2 * np.pi, np.pi / 12):
                                vx, vy = float(s0[0] + r * np.cos(ang)), float(s0[1] + r * np.sin(ang))
                                if not all(seg_ok(items, l, net, vx, vy, vx, vy, 0.3) for l in CU):
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


def unrouted(b):
    b.BuildConnectivity()
    return b.GetConnectivity().GetUnconnectedCount(False)


def freeroute(jar):
    """Freerouting 1.9.0 (Java 17+; opens its window while it works). 1.9 hangs when the DSN already has wiring, so
    a round is one run from the unrouted (placed) board. One round by default (the placement routes completely); with
    FR_TRIES > 1 a round that leaves something unrouted is thrown away and run again with the next FR_PASSES limit."""
    dsn, ses = os.path.join(TMP, 'sensorcard.dsn'), os.path.join(TMP, 'sensorcard.ses')
    placed = os.path.join(TMP, 'sensorcard_placed.kicad_pcb')
    passes = [int(p) for p in os.environ.get('FR_PASSES', '120').split(',')]   # with FR_TRIES > 1, a different limit each round
    shutil.copyfile(PCB, placed)
    for rnd in range(int(os.environ.get('FR_TRIES', '1'))):     # default one round, as the audio card; FR_TRIES > 1 retries from the placed board
        shutil.copyfile(placed, PCB)
        b = pcbnew.LoadBoard(PCB)
        rules(b)
        stitch(b, fanout=['U1', 'U2', 'U3', 'U4', 'U5', 'Y1', 'Y2'] +      # every small GND pad has its own via before routing
               sorted(f.GetReference() for f in b.GetFootprints() if re.match(r'(C|R|JP|D|F|FB)\d', f.GetReference())))
        pcbnew.SaveBoard(PCB, b)
        for z in list(b.Zones()):
            if not z.GetIsRuleArea() and z.GetLayer() != pcbnew.In1_Cu:
                b.Remove(z)             # the In1 GND plane stays: Freerouting reaches it with vias
        rules(b, margin=ROUTE_MARGIN)   # route with a little extra clearance; the saved board keeps the real rules
        pcbnew.ExportSpecctraDSN(b, dsn)
        if os.path.exists(ses):
            os.remove(ses)
        java = os.environ.get('JAVA', 'java')
        subprocess.check_call([java, '-jar', jar, '-de', dsn, '-do', ses, '-mp', str(passes[rnd % len(passes)]), '-mt', '1'], cwd=TMP,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        left = import_ses(ses)
        print('round %d: %d unrouted' % (rnd + 1, left))
        if left == 0:
            break
    finish_board()


def import_ses(ses):
    """Load the Freerouting session into the board (which has the fan-out vias), join leftovers, fill."""
    b = pcbnew.LoadBoard(PCB)
    rules(b)
    assert pcbnew.ImportSpecctraSES(b, ses)
    finish(b)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    left = unrouted(b)
    pcbnew.SaveBoard(PCB, b)
    return left


def finish_board():
    """Ground stitching, then drop vias that crowd a through-hole pin, fill."""
    b = pcbnew.LoadBoard(PCB)
    rules(b)
    stitch(b, pitch=STITCH_PITCH)
    # a via too close to a through-hole pin's drill is redundant (the pin reaches its plane through its own hole)
    holes = [(p.GetPosition(), p.GetDrillSize().x) for f in b.GetFootprints() for p in f.Pads() if p.GetDrillSize().x > 0]
    for v in [v for v in b.GetTracks() if v.GetClass() == 'PCB_VIA' and any(
            np.hypot((v.GetPosition().x - q.x) / 1e6, (v.GetPosition().y - q.y) / 1e6) < (v.GetDrill() + d) / 2e6 + 0.26
            for q, d in holes)]:
        b.Delete(v)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(PCB, b)


if __name__ == '__main__':
    if '--fill' in sys.argv or '--stitch' in sys.argv:     # refill (and stitch) the routed board
        bb = pcbnew.LoadBoard(PCB)
        rules(bb)
        if '--stitch' in sys.argv:
            stitch(bb, pitch=STITCH_PITCH)
        pcbnew.ZONE_FILLER(bb).Fill(bb.Zones())
        pcbnew.SaveBoard(PCB, bb)
    elif '--from-ses' in sys.argv:        # rebuild the pre-route board from the placed copy, then resume from the last session
        shutil.copyfile(os.path.join(TMP, 'sensorcard_placed.kicad_pcb'), PCB)
        bb = pcbnew.LoadBoard(PCB)
        rules(bb)
        stitch(bb, fanout=['U1', 'U2', 'U3', 'U4', 'U5', 'Y1', 'Y2'] +
               sorted(f.GetReference() for f in bb.GetFootprints() if re.match(r'(C|R|JP|D|F|FB)\d', f.GetReference())))
        pcbnew.SaveBoard(PCB, bb)
        print('%d unrouted after the import' % import_ses(os.path.join(TMP, 'sensorcard.ses')))
        finish_board()
    else:
        main()
