#!/usr/bin/env python3
"""Build GPSCard.kicad_pcb from the schematic's netlist with KiCad's pcbnew module (KiCad 10).

Run with KiCad's python:  $KIPY generate/gen_pcb.py [--route FREEROUTING_JAR]
  macOS: KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3

Same pattern as AudioCarrier/generate/gen_pcb.py. The board is an 80 x 60 mm slot card
(docs/hardware/accessory_ports.md, docs/hardware/gps_card.md): it stands on edge in a DigiDash slot, its right-angle
2x15 header centred on the bottom edge, the GPS connector (J2, JST PH side entry) on the top edge, the SWD (J3) and
debug serial (J4) headers right angle on the left edge. Four layers (F signal, In1 GND plane, In2 signal / GND fill,
B signal), the audio card's rules. Every part is on F (the parts side); B is empty. ICs and connectors are placed by
hand below; every passive is then put at the free spot closest to the pins it connects to. With --route it runs
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
SCH = os.path.join(ROOT, 'GPSCard.kicad_sch')
PCB = os.path.join(ROOT, 'GPSCard.kicad_pcb')
PRO = os.path.join(ROOT, 'GPSCard.kicad_pro')
mm = FromMM

W, H = 80.0, 60.0          # a slot card: the bottom edge (y = H) goes into the slot
J2_X = 40.0                # the GPS connector's centre on the top edge
J3_Y = 26.0                # SWD header pin 1 (left edge; its pins count upward)
J4_Y = 36.0                # debug serial header pin 1 (left edge, below J3)
HDR_X = 4.04               # right-angle 2.54 mm headers: pad row 4.04 mm in, so the body's outer face is on the edge
# Slot header: pin 1 at 17.78 mm right of the centre (the pins count to the left), its plastic body's outer face on
# the bottom edge, so the pins stand 6 mm past the edge into the socket.
J1_PIN1 = (W / 2 + 17.78, H - 6.58)
OX, OY = 100.0, 80.0       # board origin on the sheet


def P(x, y):
    return VECTOR2I(mm(OX + x), mm(OY + y))


def netlist():
    xml = os.path.join(TMP, 'gpscard_netlist.xml')
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
    fp = pcbnew.FootprintLoad(os.path.join(FPDIR, lib + '.pretty'), name)
    assert fp, fpid
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


# ---------------------------------------------------------------------------------------------- placement plan
# (ref: (x, y, rotation, side)). J1 (the slot header) is placed in main().
FIXED = {
    'J2': (J2_X + 3.0, 6.3, 180, 'F'),   # JST PH side entry: rotation 180 puts the plug opening on the top edge (pad 1 right)
    'U4': (J2_X, 12.5, 0, 'F'),          # ESD array right behind J2
    'J3': (HDR_X, J3_Y, 180, 'F'),       # SWD, right angle, pins off the left edge (rotation 180: pin 1 lowest)
    'J4': (HDR_X, J4_Y, 180, 'F'),       # debug serial, right angle, below J3
    'JP1': (J2_X + 7.0, 13.0, 0, 'F'),   # puck supply select: pad 1 (3V3) left, pad 3 (5V) right
    'U1': (30.0, 30.0, 0, 'F'),          # MCU
    'R2': (29.75, 35.6, 270, 'F'),       # LED resistor straight below U1 pin 12 (PA5), pad 1 nearest the pin
    'D1': (29.75, 38.6, 90, 'F'),        # status LED below it
    'U2': (52.0, H - 14.0, 0, 'F'),      # CAN transceiver above the header's CAN pins (5 / 6)
    'U3': (63.0, H - 14.0, 0, 'F'),      # 3.3 V regulator beside the 5 V pins (1 / 2)
}
SIDE = {'port': 'F', 'power': 'F', 'mcu': 'F', 'prog': 'F', 'can': 'F', 'gps': 'F', 'misc': 'F'}
ANCHOR = {'power': ['U3'], 'mcu': ['U1'], 'can': ['U2'], 'gps': ['U4', 'J2'], 'port': ['J1'], 'prog': ['J3', 'J4']}
GROUP_ORDER = ['mcu', 'can', 'power', 'gps', 'prog', 'port', 'misc']
POWER_NETS = {'+5V', '+3V3', '/PUCK_VSEL', '/PUCK_PWR'}   # local labels carry the sheet path
KEEP_SILK = {'J1', 'J2', 'J3', 'J4', 'JP1'}
HOLES = []                                # a slot card has no mounting holes: the slot and the case hold it
HOLE_KEEP = 3.3                          # radius kept free of parts around them

STITCH_PITCH = 4.0          # ground stitching grid: the board is nearly empty, 2 mm (the audio card's) gives ~900 vias
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
    occ['F'].rect(6.0, J3_Y - 11.0, 9.8, J4_Y + 1.0)         # room for the header pin legends
    occ['F'].rect(J2_X - 4.0, 7.8, J2_X + 4.0, 10.6)         # and J2's
    occ['F'].rect(J2_X + 3.4, 10.4, J2_X + 10.6, 15.9)       # and JP1's
    occ['F'].rect(2.5, 3.0, 26.0, 6.6)                       # the card's name

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

    placed = set(FIXED) | {'J1'}
    rest = [r for r in fps if r not in placed]

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
    for item in fps['J1'].GraphicalItems():        # its 'socket on bottom' note: not wanted here
        if item.GetClass() == 'PCB_TEXT':
            item.SetVisible(False)
    for ref, dx, dy in [('J1', 3.5, 0), ('J2', 5.0, -2.3), ('JP1', 0, -2.0), ('J3', 8.5, -5.08), ('J4', 8.5, -2.54)]:
        fp = fps[ref]
        t = fps[ref].Reference()
        t.SetTextAngle(EDA_ANGLE(90 if ref in ('J3', 'J4') else 0, pcbnew.DEGREES_T))
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
    for y0, names in ((J3_Y, ['3V3', 'DIO', 'CLK', 'RST', 'GND']), (J4_Y, ['GND', 'TX', 'RX'])):
        for k, s in enumerate(names):
            silk(s, 6.3, y0 - 2.54 * k, 0.8)
    # the GPS connector's pins, below it (pad 1 is on the right, seen from the parts side)
    for k, s in enumerate(['PWR', 'TX', 'RX', 'G']):
        silk(s, J2_X + 3.0 - 2.0 * k, 10.1, 0.7, rot=90)
    silk('3V3', J2_X + 4.4, 15.4, 0.7)
    silk('5V', J2_X + 8.2, 15.4, 0.7)
    silk('GPS CARD  rev A', 3.0, 4.0, 1.0)
    silk('DigiDash slot card  node 0x05', 3.0, 5.8, 0.8)
    silk('GPS CARD  rev A', 77.0, 30.0, 1.0, pcbnew.B_SilkS)

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


def rules(b):
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
    for nc, w in ((d, 0.2), (pwr, 0.3)):
        nc.SetClearance(mm(0.13))
        nc.SetTrackWidth(mm(w))
        nc.SetViaDiameter(mm(0.6))
        nc.SetViaDrill(mm(0.3))
    pwr.SetPriority(0)
    ns.SetNetclass('Power', pwr)
    ns.ClearNetclassPatternAssignments()
    for n in ('GND', '+5V', '+3V3', '/PUCK_VSEL', '/PUCK_PWR'):
        ns.SetNetclassPatternAssignment(n, 'Power')
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
        tmp = os.path.join(TMP, 'gpscard_stitch_check.kicad_pcb')
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        pcbnew.SaveBoard(tmp, b)
        rpt = os.path.join(TMP, 'gpscard_stitch_check.json')
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
                done = False
                for yy in np.arange(bb.GetTop() / 1e6 + 0.3, bb.GetBottom() / 1e6 - 0.3, 0.1):
                    for xx in np.arange(bb.GetLeft() / 1e6 + 0.3, bb.GetRight() / 1e6 - 0.3, 0.1):
                        x, y = float(xx), float(yy)
                        if o.PointInside(VECTOR2I(mm(x), mm(y))) and spot_ok(x, y) and \
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


CU = [pcbnew.F_Cu, pcbnew.In2_Cu, pcbnew.B_Cu]


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
    dsn, ses = os.path.join(TMP, 'gpscard.dsn'), os.path.join(TMP, 'gpscard.ses')
    placed = os.path.join(TMP, 'gpscard_placed.kicad_pcb')
    passes = [int(p) for p in os.environ.get('FR_PASSES', '120').split(',')]   # with FR_TRIES > 1, a different limit each round
    shutil.copyfile(PCB, placed)
    for rnd in range(int(os.environ.get('FR_TRIES', '1'))):     # default one round, as the audio card; FR_TRIES > 1 retries from the placed board
        shutil.copyfile(placed, PCB)
        b = pcbnew.LoadBoard(PCB)
        rules(b)
        stitch(b, fanout=['U1', 'U2', 'U3', 'U4', 'Y1'] +      # every small GND pad has its own via before routing
               sorted(f.GetReference() for f in b.GetFootprints() if re.match(r'(C|R|JP|D|F)\d', f.GetReference())))
        pcbnew.SaveBoard(PCB, b)
        for z in list(b.Zones()):
            if not z.GetIsRuleArea() and z.GetLayer() != pcbnew.In1_Cu:
                b.Remove(z)             # the In1 GND plane stays: Freerouting reaches it with vias
        pcbnew.ExportSpecctraDSN(b, dsn)
        if os.path.exists(ses):
            os.remove(ses)
        java = os.environ.get('JAVA', 'java')
        subprocess.check_call([java, '-jar', jar, '-de', dsn, '-do', ses, '-mp', str(passes[rnd % len(passes)]), '-mt', '1'], cwd=TMP,
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
    else:
        main()
