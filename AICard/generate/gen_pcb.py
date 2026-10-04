#!/usr/bin/env python3
"""Build AICard.kicad_pcb from the schematic's netlist with KiCad's pcbnew module (KiCad 7 or later).
Adapted from LTECard/generate/gen_pcb.py (itself from AudioCarrier/generate/gen_pcb.py).

Run with a Python that has pcbnew and numpy:  python3 generate/gen_pcb.py [--route FREEROUTING_JAR]

The board is an 80 x 60 mm slot card (docs/hardware/accessory_ports.md, docs/hardware/ai_card.md): it stands on edge in a
DigiDash slot, its right-angle 2x15 header centred on the bottom edge. Four layers (F signal, In1 GND plane, In2 signal /
pours, B signal). F is the parts side (20 mm limit); B faces the next card and has no parts.

The compute module (a Pi-header board, 65 x 30 mm, PROVISIONAL) plugs into a 2 x 20 socket across the middle of the card
and covers its upper half 8.5 mm above it. Everything under it is low (the ESP32-S3-1U, buttons, LEDs); the 470 uF bulk is
outside it. Left edge: USB-C and the debug header. Bottom: the module's 5 V buck (by the AUX pins 25-30), the rail buffer
(by the I2S pins 15-18), the CAN transceiver and the 3.3 V regulator (by pins 1-6).

Grounds: AUX_GND (the AUX 12 V return, pins 28-30) is a separate net. It meets GND only on the main board (NT1), so on
this card it has its own pours on F / In2 / B over the buck area (AUX_REGION); In1 is a solid GND plane under everything.
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
sys.path.insert(0, HERE)
from sexp import parse  # noqa: E402
ROOT = os.path.normpath(os.path.join(HERE, '..'))
KICAD = '/Applications/KiCad/KiCad.app/Contents'
FPDIR = os.environ.get('KICAD_FOOTPRINTS', next((d for d in (KICAD + '/SharedSupport/footprints', '/usr/share/kicad/footprints')
                                                  if os.path.isdir(d)), ''))
CLI = os.environ.get('KICAD_CLI', KICAD + '/MacOS/kicad-cli' if os.path.exists(KICAD + '/MacOS/kicad-cli') else 'kicad-cli')
TMP = os.environ.get('TMPDIR', '/tmp')
SCH = os.path.join(ROOT, 'AICard.kicad_sch')
PCB = os.path.join(ROOT, 'AICard.kicad_pcb')
PRO = os.path.join(ROOT, 'AICard.kicad_pro')
mm = FromMM

W, H = 80.0, 60.0          # a slot card: the bottom edge (y = H) goes into the slot

# ---- compute module (PROVISIONAL: a Pi-Zero-size board, 65 x 30 mm; see AICard/README.md "Measure the module before ordering")
MOD_X0, MOD_Y0, MOD_W, MOD_H = 7.5, 5.0, 65.0, 30.0   # its outline on the card (it hangs 8.5 mm over the card)
J2_PIN1 = (14.6, 28.73)    # socket pin 1: the header's left end; odd pins in the upper row, pins counting to the right
MOUNT_HOLES = [(11.0, 8.5), (69.0, 8.5)]               # the module's two far corners (3.5 mm in from its edges): M2.5
HOLE_KEEP = 3.3                                        # radius kept free of parts around them

USB_Y = 36.5               # USB-C centre on the left edge (the plug faces out of the card)
# Slot header: pin 1 at 17.78 mm right of the centre (the pins count to the left), its plastic body's outer face on
# the bottom edge, so the pins stand 6 mm past the edge into the socket.
J1_PIN1 = (W / 2 + 17.78, H - 6.58)
# the AUX domain: AUX_GND pours (F, In2, B) over the buck, the bulk and the AUX pins (25-30, x 22-27); GND everywhere else
AUX_REGION = (9.0, 33.6, 41.0, H)
OX, OY = 100.0, 80.0       # board origin on the sheet


def P(x, y):
    return VECTOR2I(mm(OX + x), mm(OY + y))


def netlist():
    xml = os.path.join(TMP, 'aicard_netlist.xml')
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


def set_fields(fp, datasheet, descr):
    """Datasheet and description: GetField() on KiCad 8+, properties / SetDescription() on 7."""
    if hasattr(fp, 'GetField'):
        fp.GetField(pcbnew.FIELD_T_DATASHEET).SetText(datasheet)
        fp.GetField(pcbnew.FIELD_T_DESCRIPTION).SetText(descr)
    else:
        fp.SetDescription(descr)
        if datasheet:
            fp.SetProperty('Datasheet', datasheet)


def load(fpid):
    lib, name = fpid.split(':')
    fp = pcbnew.FootprintLoad(os.path.join(FPDIR, lib + '.pretty'), name)
    assert fp, fpid
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


# ---------------------------------------------------------------------------------------------- placement plan
# (ref: (x, y, rotation, side)). J1 and J2 (the headers) are placed in main(); the 470 uF bulk is found by its value.
FIXED = {
    'U1': (24.0, 16.5, 0, 'F'),          # ESP32-S3-1U under the module, clear of the mounting hole and the socket
    'J3': (4.04, 52.0, 180, 'F'),        # debug serial, right angle: plastic body on the left edge, pins past it
    'J4': (3.1, USB_Y, 270, 'F'),        # USB-C: the footprint's board-edge line on x = 0, the plug side off the edge
    'SW1': (40.0, 3.4, 0, 'F'),          # RESET, top edge (low parts under the module)
    'SW2': (46.0, 3.4, 0, 'F'),          # BOOT
    'D1': (2.6, 12.0, 0, 'F'),           # module LED, left edge
    'D2': (2.6, 15.6, 0, 'F'),           # microphone LED
    'RT1': (46.0, 37.5, 0, 'F'),         # thermistor, just outside the module's shadow
    'U5': (25.0, 44.0, 0, 'F'),          # module buck, above the AUX pins (25-30)
    'L1': (34.0, 40.0, 0, 'F'),
    'D3': (34.3, 47.6, 0, 'F'),
    'BULK': (13.5, 40.2, 0, 'F'),        # 470 uF, near the socket's 5 V pins; 10 mm tall, so outside the module's shadow
    'U2': (55.4, 46.5, 0, 'F'),          # CAN transceiver above the header's CAN pins (5 / 6)
    'U3': (66.0, 46.0, 0, 'F'),          # 3.3 V regulator beside the 5 V pins (1 / 2)
    'U6': (47.3, 46.5, 0, 'F'),          # RET buffer, right of the AUX region, near the rail pins (15-18)
    'H1': (MOUNT_HOLES[0][0], MOUNT_HOLES[0][1], 0, 'F'),
    'H2': (MOUNT_HOLES[1][0], MOUNT_HOLES[1][1], 0, 'F'),
}
SIDE = {g: 'F' for g in ('port', 'power', 'esp', 'boot', 'usb', 'can', 'led', 'buck', 'mod', 'rail', 'therm', 'mech', 'misc')}
ANCHOR = {'power': ['U3'], 'esp': ['U1'], 'boot': ['U1', 'SW1', 'SW2', 'J3'], 'usb': ['J4', 'U4'], 'can': ['U2'],
          'led': ['D1', 'D2', 'U1'], 'buck': ['U5', 'L1', 'D3', 'BULK'], 'mod': ['J2', 'U1', 'BULK'], 'rail': ['U6', 'U1'],
          'therm': ['RT1', 'U1'], 'port': ['J1', 'U1'], 'mech': ['H1']}
GROUP_ORDER = ['buck', 'esp', 'usb', 'can', 'power', 'rail', 'boot', 'mod', 'therm', 'led', 'port', 'mech', 'misc']
POWER_NETS = {'+5V', '+3V3', '/AUX12', '/MOD_5V'}
GND_NETS = {'GND', '/AUX_GND'}
BUCK_SMALL = {'/BUCK_BOOT', '/BUCK_RT', '/BUCK_COMP', '/BUCK_CZ', '/BUCK_FB', '/BUCK_EN', '/EN_SRC', '/Q1_G', '/Q2_D', '/BUCK_ON_G'}
KEEP_SILK = {'J1', 'J2', 'J3', 'SW1', 'SW2', 'U1', 'J4'}
HOLES = list(MOUNT_HOLES)
EXTRA_KEEP = {'U5': 0.6}                   # extra margin (mm) the placer leaves round a part: room for the router to leave its pins

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


def zone(b, net, layer, pts, prio=0):
    z = pcbnew.ZONE(b)
    z.SetLayer(layer)
    z.SetNet(net)
    z.SetMinThickness(mm(0.2))
    z.SetLocalClearance(mm(0.2))
    z.SetThermalReliefGap(mm(0.25))
    z.SetThermalReliefSpokeWidth(mm(0.3))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    z.SetAssignedPriority(prio)
    o = z.Outline()
    o.NewOutline()
    for (x, y) in pts:
        o.Append(mm(OX + x), mm(OY + y))
    b.Add(z)
    return z


def rule_area(b, pts, hole=None):
    k = pcbnew.ZONE(b)
    k.SetIsRuleArea(True)
    k.SetDoNotAllowTracks(True)
    k.SetDoNotAllowVias(True)
    (k.SetDoNotAllowZoneFills if hasattr(k, 'SetDoNotAllowZoneFills') else k.SetDoNotAllowCopperPour)(False)
    k.SetDoNotAllowPads(False)
    k.SetDoNotAllowFootprints(False)
    k.SetLayerSet(pcbnew.LSET.AllCuMask(4))
    o = k.Outline()
    o.NewOutline()
    for (x, y) in pts:
        o.Append(mm(OX + x), mm(OY + y))
    if hole:
        o.NewHole()
        for (x, y) in hole:
            o.Append(mm(OX + x), mm(OY + y), 0, 0)
    b.Add(k)


def rect_pts(x1, y1, x2, y2):
    return [(x1, y1), (x2, y1), (x2, y2), (x1, y2)]


def main():
    route = sys.argv[sys.argv.index('--route') + 1] if '--route' in sys.argv else None
    comps, nets = netlist()
    groups = json.load(open(os.path.join(HERE, 'refs.json')))
    bulk = [r for r, v in comps.items() if v[0] == '470u 10V']
    assert len(bulk) == 1, bulk
    FIXED[bulk[0]] = FIXED.pop('BULK')
    for g in ANCHOR.values():
        g[:] = [bulk[0] if r == 'BULK' else r for r in g]
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
        set_fields(fp, sheet, descr)     # as in the schematic, so the parity check is clean
        fp.SetAttributes(fp.GetAttributes() & ~pcbnew.FP_EXCLUDE_FROM_BOM)
        b.Add(fp)
        for pad in fp.Pads():
            n = nets.get((ref, pad.GetNumber()))
            if n:
                pad.SetNet(net(n))
            if pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH and pad.GetDrillSize().x < mm(0.3):
                pad.SetDrillSize(VECTOR2I(mm(0.3), mm(0.3)))    # the module's ground-pad vias: 0.6 / 0.3 like the rest
        fps[ref] = fp
        g = groups.get(ref, {}).get('group', 'misc')
        side_of[ref] = FIXED[ref][3] if ref in FIXED else SIDE[g]

    occ = {'F': Occ(), 'B': Occ()}
    occs = [occ]

    def put(ref, x, y, rot, side):
        fp = fps[ref]
        if side == 'B' and not fp.IsFlipped():
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        fp.SetOrientation(EDA_ANGLE(rot, pcbnew.DEGREES_T))
        fp.SetPosition(P(x, y))

    def mark(ref):
        """Occupy every courtyard outline of the part, and for through-hole parts their pins on both sides plus the other
        side's courtyard."""
        fp = fps[ref]
        side = side_of[ref]
        fp.BuildCourtyardCaches()
        tht = fp.GetAttributes() & pcbnew.FP_THROUGH_HOLE
        for s_, lay in (('F', pcbnew.F_CrtYd), ('B', pcbnew.B_CrtYd)):
            if s_ != side and not tht:
                continue
            c = fp.GetCourtyard(lay)
            for k in range(c.OutlineCount()):
                for o_ in occs:
                    o_[s_].rect(*bb_mm(c.Outline(k).BBox()), pad=0.15 + EXTRA_KEEP.get(ref, 0.0))
        for pad in fp.Pads():       # plated pins and locating pegs (non-plated holes) go through both sides
            if pad.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH):
                for s_ in ('F', 'B'):
                    for o_ in occs:
                        o_[s_].rect(*bb_mm(pad.GetBoundingBox()), pad=0.4)

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
    # the compute module's socket: pad 1 at the left end, pad 2 below it, pad 3 to its right
    for rot in (0, 90, 180, 270):
        put('J2', 0, 0, rot, 'F')
        q = {p.GetNumber(): p.GetPosition() for p in fps['J2'].Pads()}
        d2 = (q['2'].x - q['1'].x, q['2'].y - q['1'].y)
        d3 = (q['3'].x - q['1'].x, q['3'].y - q['1'].y)
        if d2[1] > 0 and abs(d2[0]) < 1000 and d3[0] > 0 and abs(d3[1]) < 1000:
            break
    else:
        raise SystemExit('no orientation of J2 puts pad 2 below pad 1 and pad 3 to its right')
    side_of['J2'] = 'F'
    put('J2', J2_PIN1[0], J2_PIN1[1], rot, 'F')
    mark('J2')
    for ref, (x, y, rot, side) in FIXED.items():
        put(ref, x, y, rot, side)
        mark(ref)
    q = {p.GetNumber(): p.GetPosition() for p in fps['J4'].Pads()}
    assert q['A1'].x / 1e6 - OX > 4, 'USB-C turned the wrong way (its pins must be on the card side, the plug off the edge)'
    q = {p.GetNumber(): p.GetPosition() for p in fps['J2'].Pads()}
    assert q['2'].x / 1e6 - OX < 20 and q['40'].x / 1e6 - OX > 60, 'socket not spanning the card'
    # AUX-domain parts go in the AUX region, and nothing else does; nothing on the back (it faces the next card)
    occ_aux = {'F': Occ(), 'B': Occ()}
    for s_ in ('F', 'B'):
        occ_aux[s_].a = occ[s_].a.copy()
        occ[s_].rect(*AUX_REGION)          # other parts keep out of the AUX region
        outside = np.ones((NY, NX), bool)
        x1_, y1_, x2_, y2_ = AUX_REGION
        outside[int(np.ceil(y1_ / GRID)):int(np.floor(y2_ / GRID)), int(np.ceil(x1_ / GRID)):int(np.floor(x2_ / GRID))] = False
        occ_aux[s_].a |= outside           # and the buck's parts stay inside it (their ground is the AUX_GND pour)
    occ['B'].rect(0, 0, W, H)
    occ_aux['B'].rect(0, 0, W, H)
    occs.append(occ_aux)

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

    placed = set(FIXED) | {'J1', 'J2'}
    rest = [r for r in fps if r not in placed]

    def prio(r):
        g = groups.get(r, {}).get('group', 'misc')
        nn = {p.GetNetname() for p in fps[r].Pads()}
        # in the buck: the input caps first (the hot loop), then its small-signal parts (BOOT, RT, EN, FB, COMP) next to
        # its pins, then the output caps
        buck = 0 if {'/AUX12', '/AUX_GND'} <= nn else 1 if nn & BUCK_SMALL else 2
        return (GROUP_ORDER.index(g), 0 if r.startswith('U') else 1, buck if g == 'buck' else 0,
                0 if (nn & POWER_NETS and nn & GND_NETS) else 1, 0 if fps[r].GetValue() == '100n' else 1, r)

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
            free = (occ_aux if g == 'buck' else occ)[side].free_mask(wc, hc)
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
            for jj in range(0, NY, 3):
                print("".join("#" if a[jj, ii] else "." for ii in range(0, NX, 2)))
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
    for ref in ('J1', 'J3', 'J4'):       # the footprints' own notes ('PCB EDGE', ...) run off the board
        for item in fps[ref].GraphicalItems():
            if item.GetClass() == 'PCB_TEXT' and item.GetLayer() in (pcbnew.F_SilkS, pcbnew.B_SilkS):
                item.SetVisible(False)
    for ref, (x, y) in {'J1': (J1_PIN1[0] + 3.5, J1_PIN1[1]), 'U1': (24.0, 6.4), 'SW1': (40.0, 6.0), 'SW2': (46.0, 6.0),
                        'D1': (6.0, 12.0), 'D2': (6.0, 15.6), 'J4': (9.0, 42.5), 'J3': (2.5, 56.4),
                        'J2': (J2_PIN1[0] - 3.0, J2_PIN1[1] - 3.5)}.items():
        t = fps[ref].Reference()
        t.SetTextAngle(EDA_ANGLE(0, pcbnew.DEGREES_T))
        t.SetPosition(P(x, y))

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

    def line(x1, y1, x2, y2, layer, w=0.12):
        sh = pcbnew.PCB_SHAPE(b)
        sh.SetShape(pcbnew.SHAPE_T_SEGMENT)
        sh.SetLayer(layer)
        sh.SetWidth(mm(w))
        sh.SetStart(P(x1, y1))
        sh.SetEnd(P(x2, y2))
        b.Add(sh)

    # the compute module's outline (provisional size) on the fab layer, with its name on the silkscreen
    cx1, cy1, cx2, cy2 = MOD_X0, MOD_Y0, MOD_X0 + MOD_W, MOD_Y0 + MOD_H
    for a_, e_ in (((cx1, cy1), (cx2, cy1)), ((cx2, cy1), (cx2, cy2)), ((cx2, cy2), (cx1, cy2)), ((cx1, cy2), (cx1, cy1))):
        line(*a_, *e_, pcbnew.F_Fab, 0.1)
    silk('COMPUTE MODULE (Pi 40-pin)', MOD_X0 + MOD_W / 2 + 4, MOD_Y0 + 6.0, 1.0)
    silk('OUTLINE + HOLES PROVISIONAL', MOD_X0 + MOD_W / 2 + 4, MOD_Y0 + 8.0, 0.8)
    silk('1', J2_PIN1[0] - 1.0, J2_PIN1[1] - 2.2, 0.7)
    silk('RST', 40.0, 1.2, 0.7)
    silk('BOOT', 46.0, 1.2, 0.7)
    silk('MOD', 5.6, 10.0, 0.7)
    silk('MIC', 5.6, 17.4, 0.7)
    tx, ty = FIXED['J3'][0], FIXED['J3'][1]
    for k, s_ in enumerate(['G', 'TX', 'RX']):
        silk(s_, tx + 2.3, ty + 2.54 - 2.54 * k, 0.7)
    silk('AI VOICE', 66.0, 38.0, 1.0)
    silk('AUX GND', (AUX_REGION[0] + AUX_REGION[2]) / 2, 57.0, 0.7, pcbnew.B_SilkS)

    # ------------------------------------------------------------------ outline, zones
    for a_, e_ in (((0, 0), (W, 0)), ((W, 0), (W, H)), ((W, H), (0, H)), ((0, H), (0, 0))):
        line(*a_, *e_, pcbnew.Edge_Cuts, 0.05)
    corners = rect_pts(0, 0, W, H)
    zone(b, net('GND'), pcbnew.In1_Cu, corners)
    for lay in (pcbnew.F_Cu, pcbnew.B_Cu, pcbnew.In2_Cu):
        zone(b, net('GND'), lay, corners, 0)
        zone(b, net('/AUX_GND'), lay, rect_pts(*AUX_REGION), 1)

    # a ring inside the outline with no tracks or vias (Freerouting only keeps its trace clearance from the edge)
    e = 0.35
    rule_area(b, corners, rect_pts(e, e, W - e, H - e))

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
    """Net classes and constraints, set on the board where the Python API allows, and in the project file for the net
    classes (KiCad 7 does not expose their assignment to Python)."""
    ds = b.GetDesignSettings()
    for name, val in (('m_CopperEdgeClearance', 0.2), ('m_MinClearance', 0.13), ('m_TrackMinWidth', 0.15),
                      ('m_MinSilkTextHeight', 0.7)):
        if hasattr(ds, name):
            setattr(ds, name, mm(val))
    if hasattr(ds, 'm_MinResolvedSpokes'):
        ds.m_MinResolvedSpokes = 1          # header pins also reach the In1 plane through their holes
    d = ds.m_NetSettings.m_DefaultNetClass
    d.SetClearance(mm(0.13))
    d.SetTrackWidth(mm(0.2))
    d.SetViaDiameter(mm(0.6))
    d.SetViaDrill(mm(0.3))
    write_project()


def write_project():
    """AICard.kicad_pro: the net classes (name, track width, clearance, vias) and which nets are in them, plus the rules
    the generators rely on. Merged into an existing file, created if there is none."""
    try:
        pro = json.load(open(PRO))
    except (OSError, ValueError):
        pro = {}
    pro.setdefault('meta', {'filename': os.path.basename(PRO), 'version': 1})
    ns = pro.setdefault('net_settings', {})
    base = {'bus_width': 12, 'diff_pair_gap': 0.25, 'diff_pair_via_gap': 0.25, 'diff_pair_width': 0.2, 'line_style': 0,
            'microvia_diameter': 0.3, 'microvia_drill': 0.1, 'pcb_color': 'rgba(0, 0, 0, 0.000)',
            'schematic_color': 'rgba(0, 0, 0, 0.000)', 'via_diameter': 0.6, 'via_drill': 0.3, 'wire_width': 6}
    classes = [dict(base, name='Default', clearance=0.13, track_width=0.2, priority=2147483647)]
    patterns = []
    for i, (cname, (w, clr, members)) in enumerate(CLASSES.items()):
        classes.append(dict(base, name=cname, clearance=clr, track_width=w, priority=i + 1))
        patterns += [{'netclass': cname, 'pattern': m} for m in members]
    ns['classes'] = classes
    ns['netclass_patterns'] = patterns
    ns['meta'] = {'version': 3}
    ds_ = pro.setdefault('board', {}).setdefault('design_settings', {}).setdefault('rules', {})
    ds_.update({'min_clearance': 0.13, 'min_copper_edge_clearance': 0.2, 'min_track_width': 0.15, 'min_via_diameter': 0.5,
                'min_through_hole_diameter': 0.3, 'min_hole_clearance': 0.19, 'min_hole_to_hole': 0.25, 'min_text_height': 0.7,
                'min_resolved_spokes': 1})
    json.dump(pro, open(PRO, 'w'), indent=2)


CLASSES = {      # name: (track width, clearance, [nets]), as on the main board (DigiDash2/generate/gen_pcb.py)
    'Power': (0.3, 0.13, ['GND', '+5V', '+3V3']),
    'Aux': (0.8, 0.2, ['/AUX12']),                    # AUX 12 V in (about 1.5 A; the buck's input pins are narrow)
    'Mod5V': (1.0, 0.2, ['/MOD_5V']),                 # module 5 V out (1.5 A typical, 3 A peak)
    'AuxGnd': (0.8, 0.2, ['/AUX_GND']),               # the AUX return, outside its pours
    'Switch': (1.0, 0.2, ['/BUCK_SW']),               # the buck's switch node: short and wide
}
WIDTH = {m: w for (w, _, ms) in CLASSES.values() for m in ms}


def via_width(v):
    """A via's diameter: GetWidth(layer) on KiCad 8+, GetWidth() on 7."""
    try:
        return v.GetWidth(pcbnew.F_Cu)
    except TypeError:
        return v.GetWidth()


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
            items.append(('c', (p.x / 1e6, p.y / 1e6, via_width(t) / 2e6), t.GetNetname()))
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

    def clear(x, y, r=rv, skip=None, gnd_ok=False, net='GND'):
        for it in items:
            if it is skip or (gnd_ok and it[2] == net):
                continue
            kind, g, n = it
            if n == net and (kind == 's' or (kind == 'r' and (g[2] - g[0]) * (g[3] - g[1]) > 1.0)):
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

    def via(x, y, net=None):
        v = pcbnew.PCB_VIA(b)
        v.SetPosition(VECTOR2I(mm(x), mm(y)))
        v.SetWidth(mm(0.6))
        v.SetDrill(mm(0.3))
        v.SetNet(net or gnd)
        v.SetLocked(bool(fanout))
        b.Add(v)
        items.append(('c', (x, y, rv), (net or gnd).GetNetname()))

    def in_aux(x, y, m=0.0):
        x1, y1, x2, y2 = AUX_REGION
        return x1 - m < x - OX < x2 + m and y1 - m < y - OY < y2 + m

    # the grid: GND vias (to the In1 plane) outside the AUX region, AUX_GND vias (joining its F / In2 / B pours) inside
    n = 0
    aux = b.FindNet('/AUX_GND')
    y = OY + 1.0
    while fanout is None and y < OY + H - 0.9:
        x = OX + 1.0
        while x < OX + W - 0.9:
            if spot_ok(x, y) and not any(x1 < x < x2 and y1 < y < y2 for x1, y1, x2, y2 in parts):
                if in_aux(x, y, -0.8):
                    if clear(x, y, net='/AUX_GND'):
                        via(x, y, aux)
                        n += 1
                elif not in_aux(x, y, 0.8) and clear(x, y):
                    via(x, y)
                    n += 1
            x += pitch
        y += pitch

    def seg_clear(ax, ay, ex, ey, skip, gnd_ok=False):
        steps = max(2, int(np.hypot(ex - ax, ey - ay) / 0.1))
        return all(clear(ax + (ex - ax) * k / steps, ay + (ey - ay) * k / steps, tw / 2, skip, gnd_ok)
                   for k in range(1, steps + 1))

    def off_plane():
        """GND pads not reachable from the In1 plane through copper: what the DRC reports as unconnected (KiCad 7's Python
        API cannot walk the connectivity graph)."""
        return drc_gnd_pads()

    def drc_gnd_pads():
        """GND pads KiCad's DRC still finds unconnected (a pour island can hold one pad and nothing else)."""
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        out = []
        unc = unconnected_report(b)
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
        if z.GetIsRuleArea() or z.GetNetname() != 'GND':      # the AUX_GND pours have their own grid
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
            out.append((set(CU), 'c', (p.x / 1e6, p.y / 1e6, via_width(t) / 2e6), t.GetNetname()))
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
    try:
        b.BuildConnectivity()
        c = b.GetConnectivity()
        c.GetConnectedItems(next(iter(b.GetTracks())), [])
    except TypeError:
        print('finisher skipped: this KiCad cannot walk the connectivity graph from Python (the DRC report says what is left)')
        return
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

        hw = WIDTH.get(net, 0.2) / 2
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
                        if all(seg_ok(items, lay, net, *p0, *p1, hw, 0.21) for p0, p1 in zip(path, path[1:])):
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
                                if not all(seg_ok(items, l, net, vx, vy, vx, vy, 0.3, 0.21) for l in CU):
                                    continue
                                l0 = next((l for l in CU if l in s0[2] and seg_ok(items, l, net, s0[0], s0[1], vx, vy, hw, 0.21)), None)
                                if l0 is None:
                                    continue
                                for l1 in CU:
                                    if l1 not in s1[2]:
                                        continue
                                    for path in shapes((vx, vy), s1):
                                        if all(seg_ok(items, l1, net, *p0, *p1, hw, 0.21) for p0, p1 in zip(path, path[1:])):
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


def drc_text(b, path):
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    b.BuildConnectivity()
    pcbnew.WriteDRCReport(b, path, pcbnew.EDA_UNITS_MILLIMETRES, True)
    return open(path).read()


def unconnected_report(b):
    """What KiCad's DRC calls unconnected, as [{'items': [{'description': ...}]}] (the shape of the JSON report of 8+)."""
    txt = drc_text(b, os.path.join(TMP, 'aicard_stitch_check.rpt'))
    out = []
    for blk in re.split(r'\n(?=\[)', txt):
        if blk.startswith('[unconnected_items]'):
            out.append({'items': [{'description': m.group(1)} for m in re.finditer(r'@\([^)]*\): (.*)', blk)]})
    return out


def unrouted(b):
    b.BuildConnectivity()
    return b.GetConnectivity().GetUnconnectedCount(False)


def patch_dsn_classes(dsn):
    """KiCad 7's DSN export puts every net in one default class (it does not read the project's net classes from Python).
    Split them as CLASSES says, so Freerouting uses the wide tracks and the 0.2 mm clearances."""
    txt = open(dsn).read()
    i = txt.index('    (class kicad_default')
    j = txt.index('      (circuit', i)
    names = re.findall(r'"[^"]*"|\S+', txt[i + len('    (class kicad_default'):j])
    names = [n for n in names if n != '""']
    k = txt.index('\n    )\n', j) + len('\n    )\n')
    via = re.search(r'\(use_via (\S+)\)', txt[j:k]).group(1)
    out, used = [], set()
    for cname, (w, clr, members) in CLASSES.items():
        mem = [n for n in names if n in members]
        used |= set(mem)
        if mem:
            out.append('    (class %s %s\n      (circuit\n        (use_via %s)\n      )\n      (rule\n        (width %g)\n        (clearance %g)\n      )\n    )\n'
                       % (cname, ' '.join(mem), via, w * 1000, clr * 1000))
    rest = [n for n in names if n not in used]
    out.append('    (class kicad_default "" %s\n      (circuit\n        (use_via %s)\n      )\n      (rule\n        (width 200)\n        (clearance 130.1)\n      )\n    )\n'
               % (' '.join(rest), via))
    open(dsn, 'w').write(txt[:i] + ''.join(out) + txt[k:])


def cshapes(a, e):
    """Detours around what sits between two points: out sideways (or up / down) by d, along, and back in."""
    (ax, ay), (ex, ey) = a[:2], e[:2]
    out = []
    for d in (0.9, 1.3, 1.8, 2.5, 3.2):
        for sx in (-1, 1):
            out.append([(ax, ay), (ax + sx * d, ay), (ax + sx * d, ey), (ex, ey)])
            out.append([(ax, ay), (ex + sx * d, ay), (ex + sx * d, ey), (ex, ey)])
        for sy in (-1, 1):
            out.append([(ax, ay), (ax, ay + sy * d), (ex, ay + sy * d), (ex, ey)])
            out.append([(ax, ay), (ax, ey + sy * d), (ex, ey + sy * d), (ex, ey)])
    return out


def join_unconnected(b):
    """What the router left (the DRC's 'unconnected items' with two ends): a straight F.Cu track between the two ends when
    nothing of another net is in the way. KiCad 7 cannot walk the connectivity graph from Python, so this works from the report."""
    txt = drc_text(b, os.path.join(TMP, 'aicard_join_check.rpt'))
    done = 0
    for blk in re.split(r'\n(?=\[)', txt):
        if not blk.startswith('[unconnected_items]'):
            continue
        items = re.findall(r'@\(([-\d.]+) mm, ([-\d.]+) mm\): (.*)', blk)
        if len(items) != 2:
            continue
        (x1, y1, d1), (x2, y2, d2) = items
        m = re.search(r'\[([^\]]+)\]', d1)
        if not m:
            continue
        net = m.group(1)
        ni = b.FindNet(net)
        if ni is None:
            continue
        lg = geo(b)
        def exact(x, y, d):
            """A pad's own centre (the report's position can be a corner of it), else the reported point."""
            m_ = re.search(r'[Pp]ad (\S+) \[[^\]]*\] of (\S+)', d)
            if m_:
                fp_ = b.FindFootprintByReference(m_.group(2))
                for p_ in fp_.Pads() if fp_ else []:
                    if p_.GetNumber() == m_.group(1) and p_.GetNetname() == net:
                        return (p_.GetPosition().x / 1e6, p_.GetPosition().y / 1e6)
            return (float(x), float(y))
        a_, e_ = exact(x1, y1, d1), exact(x2, y2, d2)

        def snap(pt, d):
            """A track end that sits on a through-hole pad of this net: that pad's centre, on every layer."""
            best = None
            for fp_ in b.GetFootprints():
                for p_ in fp_.Pads():
                    if p_.GetNetname() == net and p_.GetAttribute() == pcbnew.PAD_ATTRIB_PTH:
                        c_ = (p_.GetPosition().x / 1e6, p_.GetPosition().y / 1e6)
                        dd = np.hypot(c_[0] - pt[0], c_[1] - pt[1])
                        if dd < 1.2 and (best is None or dd < best[0]):
                            best = (dd, c_)
            return best[1] if best else None
        sa, se = snap(a_, d1), snap(e_, d2)
        thru_a, thru_e = sa is not None, se is not None
        if thru_a:
            a_ = sa
        if thru_e:
            e_ = se
        w0 = WIDTH.get(net, 0.2)
        placed_ = False
        lnames = {'F.Cu': pcbnew.F_Cu, 'In2.Cu': pcbnew.In2_Cu, 'B.Cu': pcbnew.B_Cu}

        def layers_of(desc):
            m_ = re.search(r'on (F\.Cu|In\d\.Cu|B\.Cu)', desc)
            if m_:
                return {lnames[m_.group(1)]} if m_.group(1) in lnames else set()
            return set(lnames.values()) if 'PTH' in desc else set()   # a through-hole pad is on every layer
        common = [l for l in (pcbnew.F_Cu, pcbnew.B_Cu, pcbnew.In2_Cu)
                  if (thru_a or l in layers_of(d1)) and (thru_e or l in layers_of(d2))]
        for w in dict.fromkeys((w0, 0.5, 0.3, 0.2)):          # the class width first; a narrower track fits a small pad
            for lay in common:
                for path in shapes(a_, e_) + cshapes(a_, e_):
                    if all(seg_ok(lg, lay, net, *p0, *p1, w / 2, 0.2) for p0, p1 in zip(path, path[1:])):
                        for p0, p1 in zip(path, path[1:]):
                            if p0 == p1:
                                continue
                            t = pcbnew.PCB_TRACK(b)
                            t.SetStart(VECTOR2I(mm(float(p0[0])), mm(float(p0[1]))))
                            t.SetEnd(VECTOR2I(mm(float(p1[0])), mm(float(p1[1]))))
                            t.SetWidth(mm(w))
                            t.SetLayer(lay)
                            t.SetNet(ni)
                            b.Add(t)
                        placed_ = True
                        break
                if placed_:
                    break
            if placed_:
                break
        if placed_:
            done += 1
        else:
            print('  could not join', net, (x1, y1), (x2, y2))
    print('joined %d unconnected pairs' % done)


def import_ses(b, path):
    """Freerouting's Specctra session (.ses) onto the board. KiCad 7's ImportSpecctraSES works on the GUI's board only,
    so the wires and vias are read here: units of 0.1 um, y up. Wires and vias already on the board (the locked GND
    fan-out the DSN carried) are not added twice."""
    tree = parse(open(path).read())

    def find(node, key):
        return [c for c in node if isinstance(c, list) and c and c[0] == key]
    routes = find(tree, 'routes')[0]
    net_out = find(routes, 'network_out')[0]
    layers = {'F.Cu': pcbnew.F_Cu, 'In1.Cu': pcbnew.In1_Cu, 'In2.Cu': pcbnew.In2_Cu, 'B.Cu': pcbnew.B_Cu}
    have_v = [(t.GetPosition().x, t.GetPosition().y, t.GetNetname()) for t in b.GetTracks() if t.GetClass() == 'PCB_VIA']
    have_t = {(t.GetLayer(), t.GetStart().x, t.GetStart().y, t.GetEnd().x, t.GetEnd().y) for t in b.GetTracks()
              if t.GetClass() != 'PCB_VIA'}
    tracks = vias = 0

    def pt(x, y):
        return VECTOR2I(int(round(float(x) / 10.0 * 1000.0)), int(round(-float(y) / 10.0 * 1000.0)))
    for net in find(net_out, 'net'):
        name = net[1]
        ni = b.FindNet(name)
        if ni is None:
            raise SystemExit('the session has a net the board does not: %s' % name)
        for item in net[2:]:
            if not isinstance(item, list):
                continue
            if item[0] == 'wire':
                for path_ in find(item, 'path'):
                    lay, w, coords = layers[path_[1]], float(path_[2]) / 10.0 / 1000.0, path_[3:]
                    pts = [pt(coords[i], coords[i + 1]) for i in range(0, len(coords) - 1, 2)]
                    for a_, e_ in zip(pts, pts[1:]):
                        if a_ == e_ or (lay, a_.x, a_.y, e_.x, e_.y) in have_t or (lay, e_.x, e_.y, a_.x, a_.y) in have_t:
                            continue
                        t = pcbnew.PCB_TRACK(b)
                        t.SetStart(a_ + VECTOR2I(0, 0) if False else a_)
                        t.SetEnd(e_)
                        t.SetWidth(mm(w))
                        t.SetLayer(lay)
                        t.SetNet(ni)
                        b.Add(t)
                        tracks += 1
            elif item[0] == 'via':
                m = re.search(r'_(\d+):(\d+)_um', item[1])
                dia, drill = (int(m.group(1)) / 1000.0, int(m.group(2)) / 1000.0) if m else (0.6, 0.3)
                p_ = pt(item[2], item[3])
                if any(abs(p_.x - vx) < 2000 and abs(p_.y - vy) < 2000 and vn == name for vx, vy, vn in have_v):
                    continue
                v = pcbnew.PCB_VIA(b)
                v.SetPosition(p_)
                v.SetWidth(mm(dia))
                v.SetDrill(mm(drill))
                v.SetNet(ni)
                b.Add(v)
                vias += 1
    print('imported %d tracks and %d vias from %s' % (tracks, vias, os.path.basename(path)))


def preroute(b):
    """The module's 5 V, drawn before the router runs and locked: socket pins 2 and 4 joined, and down to the 470 uF bulk's
    + pad (the first thing the supply meets). Freerouting then routes round it. Skipped, with a message, if something is in the way."""
    def pad(ref, num=None, net=None):
        fp = b.FindFootprintByReference(ref)
        for p_ in fp.Pads():
            if (num is None or p_.GetNumber() == num) and (net is None or p_.GetNetname() == net):
                return p_.GetPosition().x / 1e6, p_.GetPosition().y / 1e6
    bulk = [f.GetReference() for f in b.GetFootprints() if f.GetValue() == '470u 10V']
    if len(bulk) != 1:
        return
    p2, p4, pb = pad('J2', '2'), pad('J2', '4'), pad(bulk[0], net='/MOD_5V')
    ni = b.FindNet('/MOD_5V')
    w = WIDTH['/MOD_5V']
    path = [p2, p4] + ([(p4[0], pb[1])] if abs(p4[0] - pb[0]) > 0.05 or True else []) + [pb]
    lg = geo(b)
    if not all(seg_ok(lg, pcbnew.F_Cu, '/MOD_5V', *a_, *e_, w / 2, 0.2) for a_, e_ in zip(path, path[1:]) if a_ != e_):
        print('  5 V pre-route blocked: left to the router')
        return
    for a_, e_ in zip(path, path[1:]):
        if a_ == e_:
            continue
        t = pcbnew.PCB_TRACK(b)
        t.SetStart(VECTOR2I(mm(a_[0]), mm(a_[1])))
        t.SetEnd(VECTOR2I(mm(e_[0]), mm(e_[1])))
        t.SetWidth(mm(w))
        t.SetLayer(pcbnew.F_Cu)
        t.SetNet(ni)
        t.SetLocked(True)
        b.Add(t)
    print('5 V pre-routed: socket pins 2 / 4 to the bulk capacitor')


def freeroute(jar):
    """Freerouting (the LTE card used 1.9.0; 2.4.1 needs Java 25 and runs headless with --gui.enabled=false). It is one run from
    an unrouted board: with wiring already in the DSN some versions hang."""
    dsn, ses = os.path.join(TMP, 'aicard.dsn'), os.path.join(TMP, 'aicard.ses')
    for rnd in range(1):
        b = pcbnew.LoadBoard(PCB)
        rules(b)
        preroute(b)
        stitch(b, fanout=['U2', 'U3', 'U4', 'U6'] +      # every GND pad but the module's has its own via
               sorted(f.GetReference() for f in b.GetFootprints() if re.match(r'(C|R|D|SW)\d', f.GetReference())))
        pcbnew.SaveBoard(PCB, b)
        for z in list(b.Zones()):
            if not z.GetIsRuleArea() and z.GetLayer() != pcbnew.In1_Cu:
                b.Remove(z)             # the In1 GND plane stays: Freerouting reaches it with vias
        pcbnew.ExportSpecctraDSN(b, dsn)
        patch_dsn_classes(dsn)
        java = os.environ.get('JAVA', 'java')
        if os.environ.get('SKIP_ROUTE') and os.path.exists(ses):
            print('reusing', ses)
        else:
          if os.path.exists(ses):
            os.remove(ses)
          subprocess.check_call([java, '-jar', jar, '--gui.enabled=false', '-de', dsn, '-do', ses, '-mp', os.environ.get('FR_PASSES', '120'), '-mt', '1'], cwd=TMP,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        b = pcbnew.LoadBoard(PCB)
        rules(b)
        import_ses(b, ses)
        finish(b)
        join_unconnected(b)
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        left = unrouted(b)
        pcbnew.SaveBoard(PCB, b)
        print('round %d: %d tracks and vias, %d unrouted' % (rnd + 1, len(b.GetTracks()), left))
        if left == 0:
            break
    b = pcbnew.LoadBoard(PCB)
    rules(b)
    stitch(b)
    # a via too close to a through-hole pin's drill is redundant (the pin reaches its plane through its own hole)
    holes = [(p.GetPosition(), p.GetDrillSize().x) for f in b.GetFootprints() for p in f.Pads() if p.GetDrillSize().x > 0]
    for v in [v for v in b.GetTracks() if v.GetClass() == 'PCB_VIA' and v.GetNetname() in GND_NETS and any(
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
            stitch(bb)
        pcbnew.ZONE_FILLER(bb).Fill(bb.Zones())
        pcbnew.SaveBoard(PCB, bb)
    else:
        main()
