#!/usr/bin/env python3
"""Build AudioCarrier.kicad_pcb from the schematic's netlist with KiCad's pcbnew module (KiCad 10).

Run with KiCad's python:  $KIPY generate/gen_pcb.py [--route FREEROUTING_JAR]
  macOS: KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3

The board is the 50 x 26 mm envelope of one DigiDash header position (docs/hardware/accessory_ports.md), centred on
the two M3 holes of the carrier header. Four layers (F signal, In1 GND plane, In2 signal / power, B signal), parts
on both sides: the module, power, CAN and the connectors on top (away from the DigiDash board), the DACs, ADC and mic
preamp on the bottom (the 11 mm standoff gap). ICs and connectors are placed by hand below; every passive is then
put at the free spot closest to the pins it connects to. With --route it runs Freerouting on a Specctra export and
imports the result, then fills the ground planes.
"""
import json
import os
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
SCH = os.path.join(ROOT, 'AudioCarrier.kicad_sch')
PCB = os.path.join(ROOT, 'AudioCarrier.kicad_pcb')
PRO = os.path.join(ROOT, 'AudioCarrier.kicad_pro')
mm = FromMM

W, H = 50.0, 26.0          # the header-position envelope
OX, OY = 100.0, 80.0       # board origin on the sheet


def P(x, y):
    return VECTOR2I(mm(OX + x), mm(OY + y))


def netlist():
    xml = os.path.join(TMP, 'audiocarrier_netlist.xml')
    subprocess.check_call([CLI, 'sch', 'export', 'netlist', '--format', 'kicadxml', '-o', xml, SCH],
                          stderr=subprocess.DEVNULL)
    r = ET.parse(xml).getroot()
    comps = {}
    for c in r.find('components'):
        comps[c.get('ref')] = (c.findtext('value'), c.findtext('footprint'))
    nets = {}
    for n in r.find('nets'):
        for node in n:
            nets[(node.get('ref'), node.get('pin'))] = n.get('name').lstrip('/')
    return comps, nets


def load(fpid):
    lib, name = fpid.split(':')
    path = {'AudioCarrier': os.path.join(ROOT, 'AudioCarrier.pretty'),
            'DigiDash_Accessory': os.path.join(ROOT, '..', 'DigiDash', 'DigiDash_Accessory.pretty')}.get(
        lib, os.path.join(FPDIR, lib + '.pretty'))
    fp = pcbnew.FootprintLoad(path, name)
    assert fp, fpid
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


# ---------------------------------------------------------------------------------------------- placement plan
# Carrier header J1 at 90 degrees: footprint (fx, fy) -> board (19.92 + fy, 18.08 - fx), so the envelope centre
# (5.08, 5.08) is the board centre, MP1 lands at (4.68, 16.81) and MP2 at (45.32, 9.19).
# (ref: (x, y, rotation, side)); side 'B' parts are flipped to the bottom.
FIXED = {
    'J1': (19.92, 18.08, 90, 'F'),
    'U1': (11.2, 6.5, 90, 'F'),          # antenna end on the left edge (x 0.2 ... 5.6), clear of MP1's screw head
    'J2': (29.8, 22.4, 0, 'F'),          # JST PH 9, vertical, along the bottom edge
    'J10': (9.9, 24.0, 90, 'F'),         # programming header along the bottom edge
    'SW1': (3.6, 23.6, 0, 'F'),
    'SW2': (12.4, 19.3, 0, 'F'),
    'U2': (24.6, 10.6, 0, 'F'),          # CAN transceiver between the module and the header
    'U7': (35.2, 4.3, 0, 'F'),           # 3.3 V regulator
    'U5': (11.4, 6.6, 0, 'B'),           # front DAC, under the module
    'U6': (23.2, 6.6, 0, 'B'),           # rear DAC
    'U4': (34.6, 6.2, 0, 'B'),           # ADC
    'U3': (38.6, 16.4, 0, 'B'),          # mic preamp
}
SIDE = {'port': 'F', 'power': 'F', 'esp': 'F', 'boot': 'F', 'can': 'F', 'misc': 'F',
        'dac_f': 'B', 'dac_r': 'B', 'adc': 'B', 'mic': 'B', 'audio_io': 'B'}
ANCHOR = {'power': ['U7'], 'esp': ['U1'], 'boot': ['U1', 'SW1', 'SW2', 'J10'], 'can': ['U2'], 'dac_f': ['U5'],
          'dac_r': ['U6'], 'adc': ['U4'], 'mic': ['U3'], 'audio_io': ['J2'], 'port': ['J1']}
GROUP_ORDER = ['esp', 'dac_f', 'dac_r', 'adc', 'mic', 'can', 'power', 'boot', 'port', 'audio_io', 'misc']
POWER_NETS = {'+5V', '+3V3', '+5VA', '+3V3A'}
KEEP_SILK = {'J1', 'J2', 'J10', 'SW1', 'SW2', 'JP1', 'JP2', 'U1'}
HOLES = [(4.68, 16.81), (45.32, 9.19)]   # MP1 / MP2 of J1: M3 screw heads on top, standoffs underneath
HOLE_KEEP = 3.3                          # radius kept free of parts around them

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
        value, fpid = comps[ref]
        fp = load(fpid)
        fp.SetReference(ref)
        fp.SetValue(value)
        b.Add(fp)
        for pad in fp.Pads():
            n = nets.get((ref, pad.GetNumber()))
            if pad.GetNumber() in ('MP1', 'MP2'):
                n = 'GND'
            if n and not n.startswith('unconnected-'):
                pad.SetNet(net(n))
        fps[ref] = fp
        g = groups.get(ref, {}).get('group', 'misc')
        side_of[ref] = FIXED[ref][3] if ref in FIXED else SIDE[g]

    occ = {'F': Occ(), 'B': Occ()}
    for hx, hy in HOLES:
        occ['F'].circle(hx, hy, HOLE_KEEP)
        occ['B'].circle(hx, hy, HOLE_KEEP)
    occ['B'].rect(0, 0, 6.0, 13.6)       # nothing metal under the antenna

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
            for pad in fp.Pads():
                if pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH and not pad.GetNumber().startswith('MP'):
                    for s_ in ('F', 'B'):
                        occ[s_].rect(*bb_mm(pad.GetBoundingBox()), pad=0.4)

    for ref, (x, y, rot, side) in FIXED.items():
        put(ref, x, y, rot, side)
        mark(ref)

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
        return (GROUP_ORDER.index(g), 0 if (nn & POWER_NETS and 'GND' in nn) else 1,
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
    for ref, dx, dy, rot in [('J1', -4.0, 1.27, 0), ('J2', -3.4, 0, 0), ('J10', 2.0, -2.2, 0), ('SW1', 0, -2.6, 0),
                             ('SW2', 0, -2.6, 0), ('U1', 0, 0, 0)]:
        fp = fps[ref]
        c = courtyard(fp, 'F')
        t = fp.Reference()
        t.SetTextAngle(EDA_ANGLE(rot, pcbnew.DEGREES_T))
        if ref == 'U1':
            t.SetPosition(P(11.2, 6.5))
        else:
            t.SetPosition(P(fp.GetPosition().x / 1e6 - OX + dx, fp.GetPosition().y / 1e6 - OY + dy))

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

    jx, jy = FIXED['J2'][0], FIXED['J2'][1]
    for k, s in enumerate(['FL', 'FR', 'G', 'RL', 'RR', 'G', 'MIC', 'AUX', 'G']):
        silk(s, jx + 2 * k, jy - 1.6, 0.7)
    tx, ty = FIXED['J10'][0], FIXED['J10'][1]
    for k, s in enumerate(['G', 'TX', 'RX', 'IO0', 'EN']):
        silk(s, tx + 2.54 * k, ty - 2.0, 0.8)
    silk('AUDIO CARRIER', 44.6, 2.0, 0.7, pcbnew.B_SilkS)

    # ------------------------------------------------------------------ outline, zones
    corners = [(0, 0), (W, 0), (W, H), (0, H)]
    for k in range(4):
        sh = pcbnew.PCB_SHAPE(b)
        sh.SetShape(pcbnew.SHAPE_T_SEGMENT)
        sh.SetLayer(pcbnew.Edge_Cuts)
        sh.SetWidth(mm(0.05))
        sh.SetStart(P(*corners[k]))
        sh.SetEnd(P(*corners[(k + 1) % 4]))
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
        z.SetAssignedPriority(0)
        o = z.Outline()
        o.NewOutline()
        for (x, y) in corners:
            o.Append(mm(OX + x), mm(OY + y))
        b.Add(z)

    pcbnew.SaveBoard(PCB, b)
    rules()
    print('placed', len(fps), 'footprints; wrote', PCB)
    if route:
        freeroute(route)


def rules():
    """Net classes and constraints (they live in the project file)."""
    d = json.load(open(PRO))
    base = dict(d['net_settings']['classes'][0])
    base.update({'name': 'Default', 'clearance': 0.15, 'track_width': 0.2, 'via_diameter': 0.6, 'via_drill': 0.3})
    pwr = dict(base)
    pwr.update({'name': 'Power', 'track_width': 0.4, 'priority': 0})
    d['net_settings']['classes'] = [base, pwr]
    d['net_settings']['netclass_patterns'] = [{'netclass': 'Power', 'pattern': n}
                                               for n in ('GND', '+5V', '+3V3', '+5VA', '+3V3A')]
    r = d['board']['design_settings']['rules']
    r.update({'min_copper_edge_clearance': 0.2, 'min_text_height': 0.7, 'min_track_width': 0.15, 'min_via_diameter': 0.5,
              'min_through_hole_diameter': 0.3, 'min_hole_clearance': 0.2, 'min_clearance': 0.15})
    json.dump(d, open(PRO, 'w'), indent=2)


def freeroute(jar):
    dsn, ses = os.path.join(TMP, 'audiocarrier.dsn'), os.path.join(TMP, 'audiocarrier.ses')
    b = pcbnew.LoadBoard(PCB)
    for z in list(b.Zones()):
        if not z.GetIsRuleArea() and z.GetLayer() != pcbnew.In1_Cu:
            b.Remove(z)                 # the In1 GND plane stays: Freerouting reaches it with vias
    pcbnew.ExportSpecctraDSN(b, dsn)
    java = os.environ.get('JAVA', '/opt/homebrew/opt/openjdk@21/bin/java')
    subprocess.check_call([java, '-jar', jar, '-de', dsn, '-do', ses, '-mp', '200', '-mt', '1', '--gui.enabled=false'])
    b = pcbnew.LoadBoard(PCB)
    ok = pcbnew.ImportSpecctraSES(b, ses)
    print('SES import', ok)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(PCB, b)


if __name__ == '__main__':
    if '--fill' in sys.argv:
        bb = pcbnew.LoadBoard(PCB)
        pcbnew.ZONE_FILLER(bb).Fill(bb.Zones())
        pcbnew.SaveBoard(PCB, bb)
    else:
        main()
