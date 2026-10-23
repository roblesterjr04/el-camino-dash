#!/usr/bin/env python3
"""Build DigiDash2.kicad_pcb (the DigiDash main board, rev 2) from the schematic's netlist with KiCad's pcbnew (KiCad 10).

Run with KiCad's python:  $KIPY generate/gen_pcb.py [--route FREEROUTING_JAR]
  macOS: KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3

A 329 x 113 mm outline, grown from rev 1's (309.2 x 103.7) so that every screen's four M4 holes land on the board. The
three screen headers (J5 / J1 / J4) are at the spacing measured from the screens (2026-10-19). Four layers: F signal, In1 solid GND, In2 solid +5 V, B signal. Every SMD part
is on the back (single-sided assembly); only the screen headers are on the front. Through-hole parts are in the
placement files too: the assembler fits them. Six card slots in a row at the left
(cards stand on edge) with the AUX 12 V feed along the top edge (the sensor pack is a card), and the screen transceivers,
video decoder, power supply and harness connectors in the lower band. ICs and connectors are placed by hand below; every
passive then goes to the free spot closest to the pins it connects to. With --route: GND / +5 V fan-out vias, one
Freerouting run, leftover links joined, GND stitching, zone fill.
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
SCH = os.path.join(ROOT, 'DigiDash2.kicad_sch')
PCB = os.path.join(ROOT, 'DigiDash2.kicad_pcb')
mm = FromMM

W, H = 329.0, 113.0        # rev 1's 309.2 x 103.7, grown to take the screens' mounting holes
OX, OY = 20.0, 20.0        # board origin on the sheet

# Screen headers J5 / J1 / J4: 2x20, 2.54 mm, on the front, pin rows along x, pin 1 at the west end of the lower row.
# Measured from the screens (2026-10-19): 28 empty pin positions between neighbours (pin 1 to pin 1: 48 pitches,
# 121.92 mm). The side headers sit 12 mm above rev 1's height (moved 2026-10-19 for the housing); the center one is
# higher still, with one empty pin row between its rows and theirs (3 pitches, 7.62 mm). Each screen (4C at the sides, 3.4C in the center: the same carrier, placed the same
# on both) has four M4 threads on its back, a 75 mm square round the display's centre, which is 34.384 mm east and
# 25.745 mm north of header pin 1 (Waveshare's ESP32-P4-WIFI6-TOUCH-LCD-4C / -3_4C drawings, seen from the board's
# front). The screens, and so their holes, are centred across the board
PITCH = 2.54
SCREEN_STEP = (20 + 28) * PITCH
SCREEN_C = (34.384, -25.745)                                            # display centre from header pin 1
SCREEN_HOLE = 37.5                                                      # M4 holes at +/- this from the centre
SCREEN_HOLE_KEEP = 4.5                                                  # an M4 head on the back, the boss on the front
SCREEN_X0 = round(W / 2 - SCREEN_STEP - SCREEN_C[0], 3)                 # J5 pin 1
SCREEN_Y = 84.1                                                         # the side headers' pin-1 row
SCREENS = {'J5': (SCREEN_X0, SCREEN_Y), 'J1': (round(SCREEN_X0 + SCREEN_STEP, 3), round(SCREEN_Y - 3 * PITCH, 3)),
           'J4': (round(SCREEN_X0 + 2 * SCREEN_STEP, 3), SCREEN_Y)}
SCREEN_HOLES = {}                                                       # ref: (x, y), board-only M4 clearance holes
for _i, (_x, _y) in enumerate(SCREENS.values()):
    for _j, (_dx, _dy) in enumerate(((-1, -1), (1, -1), (-1, 1), (1, 1))):
        SCREEN_HOLES['H%d' % (5 + 4 * _i + _j)] = (round(_x + SCREEN_C[0] + _dx * SCREEN_HOLE, 3),
                                                   round(_y + SCREEN_C[1] + _dy * SCREEN_HOLE, 3))


def P(x, y):
    return VECTOR2I(mm(OX + x), mm(OY + y))


def netlist():
    xml = os.path.join(TMP, 'digidash2_netlist.xml')
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
    path = {'DigiDash2': os.path.join(ROOT, 'DigiDash2.pretty'),
            'DigiDash_Accessory': os.path.join(ROOT, '..', 'DigiDash', 'DigiDash_Accessory.pretty')}.get(
        lib, os.path.join(FPDIR, lib + '.pretty'))
    fp = pcbnew.FootprintLoad(path, name)
    assert fp, fpid
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


# ---------------------------------------------------------------------------------------------- placement plan
# Card slots (docs/hardware/accessory_ports.md): six 2x15 sockets in a row on the back, pin rows along y, pin 1 at the
# top. A card stands on edge in each, its parts facing -x: slot 1, at the left end, faces off the end of the row, so
# it is the slot for a card with tall parts. A card is 80 mm long, centred on its socket (y 13 .. 93).
SLOT_X0, SLOT_PITCH, SLOT_YC = 20.0, 25.4, 53.0       # slot 1's socket centre x, the pitch, the sockets' centre y
SLOTS = {'J%d' % (31 + i): SLOT_X0 + SLOT_PITCH * i for i in range(6)}       # ref: socket centre x
CARD_HALF = 40.0
# A card's parts may stand up to SLOT_PITCH - 5.4 mm (20 mm at 25.4: the pitch less the board, the 2.5 mm back side
# and a margin). Under a card only parts up to about 7 mm tall (the card's bottom edge sits about 8.5 mm up): each
# slot's envelope runs from that height on its parts side to 6 mm on the other (its own back and solder tails).
CARD_PARTS = SLOT_PITCH - 5.4
ENVELOPES = [(0.0 if i == 0 else x - CARD_PARTS, SLOT_YC - CARD_HALF, x + 6.0, SLOT_YC + CARD_HALF)
             for i, x in enumerate(SLOTS.values())]
TALL_RE = re.compile(r'CP_Elec|Fuseholder|L_Wuerth_HCI|L_12x12')
# board mounting holes (M3, the housing's four posts): in the gaps between the screens, above and below where they
# meet, each pair mirrored about the board's centre line, 5.25 mm in from the top and bottom edges (from the housing's
# model, 2026-10-19). 7 mm clear round each, both sides: the post on the front, the screw head on the back
HOLES_MH = {'H1': (W / 2 - 54.0, 5.25), 'H2': (W / 2 + 54.0, 5.25), 'H3': (W / 2 - 40.0, H - 5.25),
            'H4': (W / 2 + 40.0, H - 5.25)}
# connectors on an edge: ref -> (edge, position along it); the body is turned so the plug side faces out. None since
# 2026-10-19: every cable connector is vertical, in GROUP
EDGE = {}
# The cable connectors, all vertical on the back so every cable plugs in from the rear, in one group at the right end
# of the board, past the power section (the housing steps down from the deep card shell over the low power section to
# a shallower connector shell), with 5-8 mm between plugs for fingers and latches: ref -> (courtyard centre x, y,
# rotation). Top row (between the right screen's upper holes): trunk port, serial port, cable port 4. Second row:
# vehicle power J7, cable ports 1-3. Third row: J7's reverse FET Q2, the camera (RCA video, J9). Bottom row: the AUX
# input path, Q6 (reverse FET, westmost, toward the slots it feeds), J13, its fuse F1
GROUP = {
    'J25': (266.0, 16.0, 0), 'J6': (285.0, 16.0, 0), 'J24': (303.0, 16.0, 0),
    'J7': (243.4, 31.0, 0), 'J17': (266.0, 31.0, 0), 'J22': (284.0, 31.0, 0), 'J23': (302.0, 31.0, 0),
    'Q2': (243.4, 45.0, 0), 'J11': (263.0, 45.0, 0), 'J9': (285.0, 45.0, 0),
    'Q6': (237.0, 64.0, 0), 'J13': (252.5, 64.0, 90), 'F1': (272.0, 64.0, 0),
}
# ref: (x, y, rotation, side)
FIXED = {
    # the AUX return's one joint with GND, under the AUX input (J13, in GROUP): its AUX_GND pad right below J13's
    # pin 2 (AUX GND), its GND pad east
    'NT1': (252.25, 71.5, 180, 'B'),
    # sensor serial in (a jumper wire from the sensor card's J8): above the slot row (over slot 4), clear of the card
    # envelopes, within reach of a card's left edge (which is at this end of the row) in any slot
    'J27': (96.0, 6.0, 90, 'B'),
    # video, above the center header (J1), between slot 5's socket pins (J35, x 120.3 / 122.9) and slot 6's (J36,
    # x 145.7 / 148.3). On the back it is mirrored: the analog side (AIP1A, XTAL1 / XTAL2, pins 1-8) faces east, the data
    # pins (9-16) face west, toward J1 below. Its 1.8 V LDO above it. The RCA jack (tall) below J1, east of slot 6's
    # envelope
    'U8': (132.0, 64.0, 180, 'B'),
    'U11': (160.0, 60.0, 0, 'B'), 'U12': (132.0, 55.5, 0, 'B'),
    # power section, between the card cage and the connector group: the 5 V buck (its parts placed round it, below the
    # video line at y 37) and the switched-12 V FET
    'U2': (205.0, 50.0, 0, 'B'), 'Q5': (180.0, 48.0, 0, 'B'),
    # CAN transceivers beside their screen headers
    'U5': (64.0, 84.0, 0, 'B'), 'U9': (124.0, 79.0, 0, 'B'), 'U10': (244.0, 84.0, 0, 'B'),
    'J16': (190.0, 79.0, 90, 'B'), 'D2': (172.0, 82.0, 0, 'B'),
    # the serial port's 3.3 V PTC, inline on J6 pin 1's track below the connector
    'F32': (286.0, 24.0, 90, 'B'),
}
# video support parts, fixed by their exact description (so renumbering cannot move them). The input chain sits in the
# strip between Q2 and the RCA jack (J11 pin 1 at x 263, y 45), from the jack's level up: the ESD clamp and the
# anti-alias filter's C1, the filter's L (0 R as delivered) and C2, then the 75 R termination and 1/2 attenuation, so
# only the low-impedance, terminated signal runs across to the decoder. Beside U8's east side (pins 1-8): the coupling
# cap at AIP1A, the resistor across the crystal, the crystal and its load caps. The pixel-clock damping resistor right
# under SCLK (pin 9)
FIXED_DESCR = {
    'Video input ESD': (250.5, 47.0, 90, 'B'),
    'Anti-alias filter C1 (fit with C2 and a 2.7 uH in place of the 0 R)': (252.7, 47.0, 90, 'B'),
    'Anti-alias filter L: 0 R as delivered, 2.7 uH 0805 inductor to fit the filter': (250.5, 43.0, 90, 'B'),
    'Anti-alias filter C2 (fit with C1 and a 2.7 uH in place of the 0 R)': (252.7, 43.1, 90, 'B'),
    '75 R termination and 1/2 attenuation (R1)': (250.5, 39.0, 90, 'B'),
    '75 R termination and 1/2 attenuation (R2)': (252.7, 39.0, 90, 'B'),
    'Input coupling (C4)': (137.6, 58.8, 180, 'B'),
    'Across the crystal (TI note E)': (137.4, 64.5, 270, 'B'),
    '14.31818 MHz, +/-50 ppm, fundamental': (140.0, 64.5, 270, 'B'),
    'Y3 load (2 CL - Cstray)': (140.6, 61.0, 0, 'B'),
    'Y3 load': (140.0, 68.0, 0, 'B'),
    'Pixel clock series (edge damping)': (133.75, 70.0, 270, 'B'),
    # the center screen's sensor-serial divider right under its RX pin (J1 pin 15): left to the placer the top resistor
    # lands by J27, walled in by the slot buses between slots 4 and 5, and the link to J1 cannot be routed
    'Divider top, C screen': (143.4, 79.14, 0, 'B'),
    'Divider bottom, C screen': (147.4, 79.14, 0, 'B'),
}
# the video input and the pixel clock's first 2 mm, drawn by hand and locked before Freerouting runs (left to itself it
# detours round J1, 80-150 mm with up to 8 vias). VID_ATT leaves the termination (R32) through a via onto F.Cu, which is
# empty but for crossings and the AUX feed, and lies right over the In1 ground plane: west along y 37.1 (between the
# connector group's second and third rows, above the buck), over the top of slot 6's socket (y 29.6), south down the
# gap between slot 6's pins and the crystal (x 143), back down to B.Cu beside the
# coupling cap C27, and C27 straight into AIP1A (pin 1). (net, layer, width, points in board mm); a via wherever
# consecutive pieces change layer. Placement is fixed above, so these points follow the parts; re-check them if R32, C27,
# R34 or U8 move
PREROUTE = [
    ('/VID_ATT', 'B', 0.25, [(250.5, 38.18), (250.5, 37.1)]),
    ('/VID_ATT', 'F', 0.25, [(250.5, 37.1), (160.0, 37.1), (152.5, 29.6), (143.0, 29.6), (143.0, 58.8), (139.9, 58.8)]),
    ('/VID_ATT', 'B', 0.25, [(139.9, 58.8), (138.38, 58.8)]),
    ('/VID_AIP1A', 'B', 0.2, [(136.82, 58.8), (136.82, 61.4), (135.97, 62.25), (135.16, 62.25)]),
    # SCLK (pin 9) straight down into the 22 R's pad
    ('/VID_SCLK', 'B', 0.2, [(133.75, 67.16), (133.75, 69.17)]),
]
SIDE_DEFAULT = 'B'
ANCHOR = {'power': ['U2', 'Q2', 'Q5', 'U11', 'U12'], 'aux': ['Q6', 'J13', 'F1', 'NT1'],
          'screens': ['U5', 'U9', 'U10', 'J1', 'J4', 'J5', 'J9', 'J6', 'J27'],
          'video': ['U8', 'J11'], 'ports': ['J17', 'J22', 'J23', 'J24', 'J25'], 'slots': list(SLOTS) + ['NT2']}
GROUP_ORDER = ['video', 'power', 'aux', 'screens', 'ports', 'slots', 'misc']
POWER_NETS = {'+5V', '+3V3', '/+1V8', '/+1V8A', '/12V_BATT', '/SW12', '/AUX12'}
PLANES = {'GND': pcbnew.In1_Cu, '+5V': pcbnew.In2_Cu}
KEEP_SILK = {'J1', 'J4', 'J5', 'J6', 'J7', 'J9', 'J11', 'J13', 'J16', 'J17',
             'J22', 'J23', 'J24', 'J25', 'J27', 'SW1', 'U8'} | set(SLOTS)
HOLE_KEEP = 3.5
HOLES = []                 # filled with every hole: (x, y, keep-out radius)

GRID = 0.25
NX, NY = int(round(W / GRID)), int(round(H / GRID))


class Occ:
    """Occupancy grid of one board side (True = taken)."""

    def __init__(self):
        self.a = np.zeros((NY, NX), bool)
        m = int(round(0.4 / GRID))
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
    if c.OutlineCount() == 0:
        bx = [bb_mm(p.GetBoundingBox()) for p in fp.Pads() if not p.GetNumber().startswith('MP')]
        return (min(v[0] for v in bx) - 0.25, min(v[1] for v in bx) - 0.25,
                max(v[2] for v in bx) + 0.25, max(v[3] for v in bx) + 0.25)
    boxes = [bb_mm(c.Outline(k).BBox()) for k in range(c.OutlineCount())]
    if len(boxes) > 1:                  # port headers: their courtyard circles are the M3 holes; use the pins
        bx = [bb_mm(p.GetBoundingBox()) for p in fp.Pads() if not p.GetNumber().startswith('MP')]
        return (min(v[0] for v in bx) - 0.25, min(v[1] for v in bx) - 0.25,
                max(v[2] for v in bx) + 0.25, max(v[3] for v in bx) + 0.25)
    return boxes[0]


def main():
    route = sys.argv[sys.argv.index('--route') + 1] if '--route' in sys.argv else None
    comps, nets = netlist()
    groups = json.load(open(os.path.join(HERE, 'refs.json')))
    b = pcbnew.BOARD()
    b.SetCopperLayerCount(4)
    b.SetLayerType(pcbnew.In1_Cu, pcbnew.LT_POWER)      # solid GND
    b.SetLayerType(pcbnew.In2_Cu, pcbnew.LT_POWER)      # solid +5 V
    netobj = {}

    def net(name):
        if name not in netobj:
            n = pcbnew.NETINFO_ITEM(b, name)
            b.Add(n)
            netobj[name] = n
        return netobj[name]

    for ref in comps:
        if comps[ref][3] in FIXED_DESCR:
            FIXED[ref] = FIXED_DESCR[comps[ref][3]]
    assert sum(c[3] in FIXED_DESCR for c in comps.values()) == len(FIXED_DESCR), 'a FIXED_DESCR entry matches no part'
    fps, side_of = {}, {}
    for ref in sorted(r for r in comps if not r.startswith('#')):
        value, fpid, sheet, descr = comps[ref]
        fp = load(fpid)
        fp.SetReference(ref)
        fp.SetValue(value)
        fp.GetField(pcbnew.FIELD_T_DATASHEET).SetText(sheet)
        fp.GetField(pcbnew.FIELD_T_DESCRIPTION).SetText(descr)
        fp.SetAttributes(fp.GetAttributes() & ~pcbnew.FP_EXCLUDE_FROM_BOM)
        if groups.get(ref, {}).get('dnp'):
            fp.SetDNP(True)             # not fitted as delivered (the schematic's DNP flag)
        if ref == 'U11':                # the 3.3 V LDO's GND tab joins the pour solidly, to shed its heat
            for pad in fp.Pads():
                if pad.GetNumber() == '2':
                    pad.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
        b.Add(fp)
        for pad in fp.Pads():
            n = nets.get((ref, pad.GetNumber()))
            if pad.GetNumber() in ('MP1', 'MP2'):
                n = 'GND'
            if n:
                pad.SetNet(net(n))
        fps[ref] = fp
        side_of[ref] = 'F' if ref in ('J1', 'J4', 'J5') else FIXED.get(ref, (0, 0, 0, SIDE_DEFAULT))[3]

    occ = {'F': Occ(), 'B': Occ()}

    def put(ref, x, y, rot, side):
        fp = fps[ref]
        if (side == 'B') != fp.IsFlipped():
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        fp.SetOrientation(EDA_ANGLE(rot, pcbnew.DEGREES_T))
        fp.SetPosition(P(x, y))

    def mark(ref):
        fp = fps[ref]
        side = side_of[ref]
        fp.BuildCourtyardCaches()
        tht = fp.GetAttributes() & pcbnew.FP_THROUGH_HOLE
        for s_, lay in (('F', pcbnew.F_CrtYd), ('B', pcbnew.B_CrtYd)):
            if s_ != side and not tht:
                continue
            c = fp.GetCourtyard(lay)
            if c.OutlineCount() > 1:        # port headers (hole circles): the pins are marked below
                continue
            for k in range(c.OutlineCount()):
                occ[s_].rect(*bb_mm(c.Outline(k).BBox()), pad=0.3)
        bare = fp.GetCourtyard(pcbnew.F_CrtYd if side == 'F' else pcbnew.B_CrtYd).OutlineCount() == 0   # a net tie
        for pad in fp.Pads():
            if pad.GetNumber().startswith('MP'):
                continue
            # through-hole pins, SMD parts' pads on the far side (a PowerPAD's thermal vias and back pad), and every pad
            # of a part without a courtyard
            for s_, lay in (('F', pcbnew.F_Cu), ('B', pcbnew.B_Cu)):
                if pad.IsOnLayer(lay) and (tht or s_ != side or bare):
                    occ[s_].rect(*bb_mm(pad.GetBoundingBox()), pad=0.4)

    # screen headers (SCREENS): pin 1 at the given point, turned so the pin rows run along x
    for ref, (x, y) in SCREENS.items():
        put(ref, x, y, 90, 'F')
        mark(ref)
    # mounting holes
    for ref, (x, y) in HOLES_MH.items():
        side_of[ref] = 'B'
        put(ref, x, y, 0, 'B')
        HOLES.append((x, y, HOLE_KEEP))
    # the screens' M4 holes: board-only footprints (not in the schematic, not fitted, no copper)
    for ref, (x, y) in SCREEN_HOLES.items():
        fp = load('MountingHole:MountingHole_4.3mm_M4')
        fp.SetReference(ref)
        fp.SetValue('M4 screen')
        fp.SetAttributes(fp.GetAttributes() | pcbnew.FP_BOARD_ONLY | pcbnew.FP_EXCLUDE_FROM_BOM
                         | pcbnew.FP_EXCLUDE_FROM_POS_FILES)
        b.Add(fp)
        fps[ref] = fp
        side_of[ref] = 'F'
        put(ref, x, y, 0, 'F')
        HOLES.append((x, y, SCREEN_HOLE_KEEP))
    for x, y, r in HOLES:
        occ['F'].circle(x, y, r)
        occ['B'].circle(x, y, r)
    # card slots: turned so pad 2 sits at +x of pad 1 (the card's parts face -x) and pad 3 below pad 1 (pin 1 at the
    # top). A card standing in the slot, parts side toward -x, has its header's pin 1 at the top (its right end, seen
    # from its parts side, is the main board's top) and its pad-2 row (the lower row, nearer the card) toward +x, so
    # this is the only way round in which each pad meets the same-numbered pin of the card's header
    for ref, xc in SLOTS.items():
        for rot in (0, 90, 180, 270):
            put(ref, 0, 0, rot, 'B')
            q = {p.GetNumber(): p.GetPosition() for p in fps[ref].Pads()}
            d2 = (q['2'].x - q['1'].x, q['2'].y - q['1'].y)
            d3 = (q['3'].x - q['1'].x, q['3'].y - q['1'].y)
            if d2[0] > 0 and abs(d2[1]) < 1000 and d3[1] > 0 and abs(d3[0]) < 1000:
                break
        else:
            raise SystemExit('no orientation of %s puts pad 2 at +x and pad 3 below pad 1' % ref)
        ps = [p.GetPosition() for p in fps[ref].Pads()]
        mx = (min(v.x for v in ps) + max(v.x for v in ps)) / 2e6 - OX
        my = (min(v.y for v in ps) + max(v.y for v in ps)) / 2e6 - OY
        side_of[ref] = 'B'
        put(ref, xc - mx, SLOT_YC - my, rot, 'B')
        mark(ref)
    under = Occ()                       # the card envelopes: no part taller than about 7 mm under a card
    under.a[:] = False
    for e in ENVELOPES:
        under.rect(*e)

    def tall_part(ref):
        fp = fps[ref]
        return TALL_RE.search(fp.GetFPIDAsString()) is not None or (
            bool(fp.GetAttributes() & pcbnew.FP_THROUGH_HOLE) and ref not in SLOTS)

    def check_envelope(ref, side):
        c = courtyard(fps[ref], side)
        hit = [e for e in ENVELOPES if not (c[2] <= e[0] or c[0] >= e[2] or c[3] <= e[1] or c[1] >= e[3])]
        if hit and side == 'B' and tall_part(ref):
            print('check: %s (through-hole or tall) is under a card envelope: confirm it is under 7 mm tall' % ref, c)
    # connectors on an edge
    for ref, (edge, along) in EDGE.items():
        best = None
        for rot in (0, 90, 180, 270):
            put(ref, 0, 0, rot, 'B')
            x1, y1, x2, y2 = courtyard(fps[ref], 'B')
            pins = [p.GetPosition() for p in fps[ref].Pads() if p.GetNumber()]
            px = [q.x / 1e6 - OX for q in pins]
            py = [q.y / 1e6 - OY for q in pins]
            across = py if edge in ('top', 'bottom') else px        # the pin row must run along the edge
            if max(across) - min(across) > 0.01:
                continue
            row = across[0]
            # how far the body reaches past the pin row toward the edge (the mating face is at the far end)
            reach = {'top': row - y1, 'bottom': y2 - row, 'left': row - x1, 'right': x2 - row}[edge]
            back = {'top': y2 - row, 'bottom': row - y1, 'left': x2 - row, 'right': row - x1}[edge]
            if reach > back and (best is None or reach > best[0]):
                best = (reach, rot, (x1, y1, x2, y2))
        assert best, ('no orientation faces the edge', ref)
        _, rot, (x1, y1, x2, y2) = best
        over = 0.8                      # the plug side stands just past the edge
        if edge in ('top', 'bottom'):
            x = along - (x1 + x2) / 2
            y = (-over - y1) if edge == 'top' else (H + over - y2)
        else:
            y = along - (y1 + y2) / 2
            x = (-over - x1) if edge == 'left' else (W + over - x2)
        side_of[ref] = 'B'
        put(ref, x, y, rot, 'B')
        check_envelope(ref, 'B')
        mark(ref)
    for ref, (x, y, rot, side) in FIXED.items():
        put(ref, x, y, rot, side)
        check_envelope(ref, side)
        mark(ref)
    for ref, (cx, cy, rot) in GROUP.items():   # by courtyard centre
        side_of[ref] = 'B'
        put(ref, 0, 0, rot, 'B')
        x1, y1, x2, y2 = courtyard(fps[ref], 'B')
        put(ref, round(cx - (x1 + x2) / 2, 3), round(cy - (y1 + y2) / 2, 3), rot, 'B')
        check_envelope(ref, 'B')
        mark(ref)
    # keep the passives off the pre-drawn tracks on the back and the vias where they change layer
    for k, (net_, lay, w, pts) in enumerate(PREROUTE):
        if lay == 'B':
            for (ax, ay), (ex, ey) in zip(pts, pts[1:]):
                occ['B'].rect(min(ax, ex), min(ay, ey), max(ax, ex), max(ay, ey), pad=0.5)
        if k and PREROUTE[k - 1][0] == net_ and PREROUTE[k - 1][1] != lay:
            occ['B'].circle(pts[0][0], pts[0][1], 0.8)

    # ------------------------------------------------------------------ passives: closest free spot to their pins
    placed = set(FIXED) | set(EDGE) | set(GROUP) | set(SLOTS) | set(HOLES_MH) | set(SCREEN_HOLES) | {'J1', 'J4', 'J5'}

    def placed_pads():
        out = {}
        for r in placed:
            for pad in fps[r].Pads():
                n = pad.GetNetname()
                if n:
                    p = pad.GetPosition()
                    out.setdefault(n, []).append((r, p.x / 1e6 - OX, p.y / 1e6 - OY))
        return out

    rest = [r for r in fps if r not in placed]
    for r in rest:
        side_of[r] = SIDE_DEFAULT

    def prio(r):
        g = groups.get(r, {}).get('group', 'misc')
        nn = {p.GetNetname() for p in fps[r].Pads()}
        return (GROUP_ORDER.index(g) if g in GROUP_ORDER else 99, 0 if (nn & POWER_NETS and 'GND' in nn) else 1,
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
        # every part on the back (single-sided assembly); tall parts stay out from under the cards
        tall = TALL_RE.search(fp.GetFPIDAsString()) is not None
        for rot, side in [(r_, 'B') for r_ in (0, 90, 180, 270)]:
            put(ref, 0, 0, rot, side)
            x1, y1, x2, y2 = courtyard(fp, side)
            x1, y1, x2, y2 = x1 - 0.1, y1 - 0.1, x2 + 0.1, y2 + 0.1
            wc, hc = int(np.ceil((x2 - x1) / GRID)), int(np.ceil((y2 - y1) / GRID))
            if tall and side == 'B':        # tall parts stay out from under the cards
                saved = occ['B'].a
                occ['B'].a = saved | under.a
                free = occ['B'].free_mask(wc, hc)
                occ['B'].a = saved
            else:
                free = occ[side].free_mask(wc, hc)
            cx = xs[None, :] - GRID / 2 - x1
            cy = ys[:, None] - GRID / 2 - y1
            cost = np.zeros((NY, NX))
            for pad in fp.Pads():
                n = pad.GetNetname()
                if not n or n.startswith('unconnected'):
                    continue
                px, py = pad.GetPosition().x / 1e6 - OX, pad.GetPosition().y / 1e6 - OY
                tg = pads_by_net.get(n, [])
                wgt = 1.0
                if n in PLANES:
                    near = [t for t in tg if t[0] in anchors]
                    tg = near or tg
                    wgt = 0.25 if n == 'GND' else 0.6
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
                best = (cost[j, i], float(cx[0, i]), float(cy[j, 0]), rot, side)
        if best is None:
            raise SystemExit('no room for %s' % ref)
        _, x, y, rot, side = best
        side_of[ref] = side
        put(ref, round(x, 2), round(y, 2), rot, side)
        mark(ref)
        placed.add(ref)

    # ------------------------------------------------------------------ text
    for ref, fp in fps.items():
        t = fp.Reference()
        side = side_of[ref]
        on_silk = ref in KEEP_SILK
        t.SetLayer((pcbnew.F_SilkS if on_silk else pcbnew.F_Fab) if side == 'F' else (pcbnew.B_SilkS if on_silk else pcbnew.B_Fab))
        t.SetTextSize(VECTOR2I(mm(1.0 if on_silk else 0.7), mm(1.0 if on_silk else 0.7)))
        t.SetTextThickness(mm(0.15 if on_silk else 0.12))
        fp.Value().SetVisible(False)
        for item in fp.GraphicalItems():
            if item.GetClass() == 'PCB_TEXT' and item.GetText() not in ('${REFERENCE}',):
                item.SetVisible(False)

    def silk(s, x, y, size=1.0, layer=pcbnew.B_SilkS):
        t = pcbnew.PCB_TEXT(b)
        t.SetText(s)
        t.SetLayer(layer)
        t.SetPosition(P(x, y))
        t.SetTextSize(VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(0.15))
        if layer == pcbnew.B_SilkS:
            t.SetMirrored(True)
        b.Add(t)

    # the names of the connectors, and the board title, are added by add_labels() after routing

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
    for lay, n in ((pcbnew.In1_Cu, 'GND'), (pcbnew.In2_Cu, '+5V'), (pcbnew.F_Cu, 'GND'), (pcbnew.B_Cu, 'GND')):
        z = pcbnew.ZONE(b)
        z.SetLayer(lay)
        z.SetNet(net(n))
        z.SetMinThickness(mm(0.2))
        z.SetLocalClearance(mm(0.25))
        z.SetThermalReliefGap(mm(0.3))
        z.SetThermalReliefSpokeWidth(mm(0.4))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        z.SetAssignedPriority(0)
        o = z.Outline()
        o.NewOutline()
        for (x, y) in corners:
            o.Append(mm(OX + x), mm(OY + y))
        b.Add(z)
    # a net tie's copper bridge is not a pad, so Freerouting does not see it: keep other tracks and vias off it
    for fp in fps.values():
        if not fp.GetFPIDAsString().startswith('NetTie'):
            continue
        lay = pcbnew.B_Cu if fp.IsFlipped() else pcbnew.F_Cu
        k = pcbnew.ZONE(b)
        k.SetIsRuleArea(True)
        k.SetDoNotAllowTracks(True)
        k.SetDoNotAllowVias(True)
        k.SetDoNotAllowZoneFills(False)
        k.SetDoNotAllowPads(False)
        k.SetDoNotAllowFootprints(False)
        k.SetLayer(lay)
        o = k.Outline()
        o.NewOutline()
        ang = np.radians(fp.GetOrientationDegrees())
        cx, cy = fp.GetPosition().x, fp.GetPosition().y
        for (dx, dy) in ((-1.0, -1.1), (1.0, -1.1), (1.0, 1.1), (-1.0, 1.1)):   # between the pads (at +/-2 mm)
            rx, ry = dx * np.cos(ang) + dy * np.sin(ang), -dx * np.sin(ang) + dy * np.cos(ang)
            o.Append(int(cx + mm(float(rx))), int(cy + mm(float(ry))))
        b.Add(k)
    k = pcbnew.ZONE(b)                  # no tracks or vias right at the edge
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
    e = 0.4
    for (x, y) in [(e, e), (W - e, e), (W - e, H - e), (e, H - e)]:
        o.Append(mm(OX + x), mm(OY + y), 0, 0)
    b.Add(k)

    rules(b)
    pcbnew.SaveBoard(PCB, b)
    print('placed', len(fps), 'footprints; wrote', PCB)
    if route:
        freeroute(route)


CLASSES = {      # name: (track width, clearance, [nets])
    'Plane': (0.5, 0.15, ['GND', '+5V']),
    'Power': (0.4, 0.15, ['+3V3', '/+1V8', '/+1V8A', '/CAM_12V', '/TRUNK_12V', '/Illumination']),
    'Power12': (0.8, 0.2, ['/BATT_IN', '/12V_BATT', '/SW12', '/IGN_IN']),
    'Switch': (1.5, 0.2, ['/BUCK_SW']),
    'Video': (0.25, 0.2, ['/VIDEO_IN', '/VID_AAF', '/VID_ATT']),
    'Aux': (1.0, 0.3, ['/AUX_IN', '/AUX_IN_F', '/AUX12', '/AUX_GND']),     # about 3 A (5 A fuse): the LTE card's buck
}


def rules(b):
    ds = b.GetDesignSettings()
    ds.m_CopperEdgeClearance = mm(0.3)
    ds.m_MinClearance = mm(0.13)
    ds.m_TrackMinWidth = mm(0.15)
    ds.m_MinSilkTextHeight = mm(0.7)
    ds.m_MinResolvedSpokes = 1
    ds.m_TentViasFront = True           # vias under silkscreen are not clipped
    ds.m_TentViasBack = True
    ns = ds.m_NetSettings
    d = ns.GetDefaultNetclass()
    d.SetClearance(mm(0.13))
    d.SetTrackWidth(mm(0.2))
    d.SetViaDiameter(mm(0.6))
    d.SetViaDrill(mm(0.3))
    ns.ClearNetclassPatternAssignments()
    names = {n.GetNetname() for n in b.GetNetInfo().NetsByName().values()}
    for i, (cname, (w, clr, members)) in enumerate(CLASSES.items()):
        nc = pcbnew.NETCLASS(cname)
        nc.SetClearance(mm(clr))
        nc.SetTrackWidth(mm(w))
        nc.SetViaDiameter(mm(0.8 if w >= 0.8 else 0.6))
        nc.SetViaDrill(mm(0.4 if w >= 0.8 else 0.3))
        nc.SetPriority(i)
        ns.SetNetclass(cname, nc)
        for m in members:
            if m in names:
                ns.SetNetclassPatternAssignment(m, cname)
        for m in names:                 # the per-position 5 V / 12 V feeds
            if cname == 'Power' and re.match(r'^/(S\d_5V|S\d_3V3|S\d_BATT|C\d_5V)$', m):
                ns.SetNetclassPatternAssignment(m, cname)
            if cname == 'Power12' and re.match(r'^/S\d_12V$', m):
                ns.SetNetclassPatternAssignment(m, cname)
    ns.RecomputeEffectiveNetclasses()
    b.SynchronizeNetsAndNetClasses(True)


def stitch(b, pitch=2.0, fanout=None, netname='GND', grid=True):
    """GND vias so every GND pad and every piece of the F / B / In2 pours reaches the In1 plane: a grid of vias
    wherever they clear everything (not under the antenna, the screw heads or a part), then, for each GND pad the
    filled pours still leave off the plane, a via beside it with a short track.
    With fanout=[refs] (before routing) it only gives every GND pin of those parts its own via, escaping inward under
    the body first, locked so the router works around them: fine-pitch GND pins between signals get boxed in otherwise."""
    gnd = b.FindNet(netname)
    rv, gap, tw = 0.3, (0.2 if fanout else 0.3), 0.25
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
        for gi in fp.GraphicalItems():
            if gi.GetLayer() in (pcbnew.F_Cu, pcbnew.B_Cu):
                bb = gi.GetBoundingBox()
                items.append(('r', (bb.GetLeft() / 1e6, bb.GetTop() / 1e6, bb.GetRight() / 1e6, bb.GetBottom() / 1e6), ''))
        for pad in fp.Pads():           # drilled holes: never shared, whatever the net
            if pad.GetDrillSize().x > 0:
                q = pad.GetPosition()
                items.append(('c', (q.x / 1e6, q.y / 1e6, pad.GetDrillSize().x / 2e6 + 0.1), '__hole'))
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

    active = [items]                    # the items near the pad being worked on (all of them outside the pad loop)

    def clear(x, y, r=rv, skip=None, gnd_ok=False):
        for it in active[0]:
            if it is skip or (gnd_ok and it[2] == netname):
                continue
            kind, g, n = it
            if n == netname and (kind == 's' or (kind == 'r' and (g[2] - g[0]) * (g[3] - g[1]) > 1.0)):
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
        if any(np.hypot(x - OX - hx, y - OY - hy) < hr + 0.2 for hx, hy, hr in HOLES):
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
        items.append(('c', (x, y, rv), netname))
        if active[0] is not items:
            active[0].append(items[-1])

    n = 0
    y = OY + 1.0
    while fanout is None and grid and y < OY + H - 0.9:
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
        plane = [z for z in b.Zones() if not z.GetIsRuleArea() and z.GetLayer() == PLANES[netname]][0]
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
                if p.GetNetname() == netname and p.m_Uuid.AsString() not in pads]

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
            gv = [t.GetPosition() for t in b.GetTracks() if t.GetClass() == 'PCB_VIA' and t.GetNetname() == netname]
            for z in b.Zones():
                if z.GetIsRuleArea():
                    continue
                for L in (pcbnew.F_Cu, pcbnew.B_Cu):
                    if not z.IsOnLayer(L):
                        continue
                    ps = z.GetFilledPolysList(L)
                    for i in range(ps.OutlineCount()):
                        o = ps.Outline(i)
                        if any(o.PointInside(v) for v in gv):
                            continue
                        bb = o.BBox()
                        bb.Inflate(mm(0.5))
                        out += [p for f in b.GetFootprints() for p in f.Pads() if p.GetNetname() == netname
                                and p.IsOnLayer(L) and p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD
                                and bb.Intersects(p.GetBoundingBox())]
        for v in unc:
            for it in v.get('items', []):
                mm_ = re.match(r'(?:PTH )?[Pp]ad (\S+) \[%s\] of (\S+)' % re.escape(netname), it.get('description', ''))
                if mm_:
                    fp = b.FindFootprintByReference(mm_.group(2))
                    out += [p for p in fp.Pads() if p.GetNumber() == mm_.group(1) and p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD]
        return out

    m = 0
    todo = off_plane() if fanout is None else [p for r in fanout for p in b.FindFootprintByReference(r).Pads()
                                                 if p.GetNetname() == netname and p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD]
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

          def near(it, reach=8.0):
              g = it[1]
              if it[0] == 'c':
                  return abs(g[0] - px) < reach and abs(g[1] - py) < reach
              if it[0] == 's':
                  return min(g[0], g[2]) - reach < px < max(g[0], g[2]) + reach and min(g[1], g[3]) - reach < py < max(g[1], g[3]) + reach
              return g[0] - reach < px < g[2] + reach and g[1] - reach < py < g[3] + reach
          active[0] = [it for it in items if near(it)]
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
              cand = [(np.hypot(g[0] - px, g[1] - py), g[0], g[1], False) for kind, g, nn in items if kind == 'c' and nn == netname]
              stuck = {pad.m_Uuid.AsString()}
              for f2 in b.GetFootprints():
                  for q in f2.Pads():
                      if q.GetNetname() == netname and q.IsOnLayer(lay) and q.m_Uuid.AsString() not in stuck:
                          qp = q.GetPosition()
                          cand.append((np.hypot(qp.x / 1e6 - px, qp.y / 1e6 - py), qp.x / 1e6, qp.y / 1e6, False))
              for r in np.arange(0.8, 3.5, 0.2):
                  for ang in np.arange(0, 2 * np.pi, np.pi / 12):
                      x, y = float(px + r * np.cos(ang)), float(py + r * np.sin(ang))
                      if spot_ok(x, y) and all(seg_ok(lg, l, netname, x, y, x, y, rv, gap) for l in CU) and \
                              all(np.hypot(x - g[0], y - g[1]) > 0.9 for kind, g, nn in items if kind == 'c'):
                          cand.append((r + 0.5, x, y, True))
              for w in (tw, 0.15):       # a narrower track squeezes between fine-pitch pins
                  for dist, x, y, nv in sorted(cand):
                      if dist > 6:
                          break
                      for path in shapes((px, py), (x, y)):
                          if all(seg_ok(lg, lay, netname, *p0, *p1, w / 2) for p0, p1 in zip(path, path[1:])):
                              best, new_via, width = path, nv, w
                              break
                      if best:
                          break
                  if best:
                      break
          if not best and all(seg_ok(geo(b), l, netname, px, py, px, py, rv, gap) for l in CU):
              via(px, py)               # last resort: via in the pad (hand-solderable; plug it if paste-reflowing)
              print('  via in pad for GND pad %s-%s' % (fp.GetReference(), pad.GetNumber()))
              m += 1
              continue
          if not best:
              print('  no via spot for %s pad %s-%s' % (netname, fp.GetReference(), pad.GetNumber()))
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
              items.append(('s', (ax, ay, ex, ey, tw / 2), netname))
              active[0].append(items[-1])
          m += 1
    active[0] = items
    if fanout:
        print('%s fan-out: %d vias' % (netname, m))
        return
    # pour islands that still have no via of their own (a pad's thermal spokes can be their only tie): GND only,
    # the F / B pours are GND
    if netname != 'GND':
        left = off_plane()
        print('%s pads off the plane: %d' % (netname, len(left)))
        return
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    k = 0
    lg = geo(b)
    for z in b.Zones():
        if z.GetIsRuleArea():
            continue
        for L in (pcbnew.F_Cu, pcbnew.B_Cu):
            if not z.IsOnLayer(L):
                continue
            ps = z.GetFilledPolysList(L)
            for i in range(ps.OutlineCount()):
                o = ps.Outline(i)
                gv = [t for t in b.GetTracks() if t.GetClass() == 'PCB_VIA' and t.GetNetname() == netname]
                if any(o.PointInside(t.GetPosition()) for t in gv):
                    continue
                bb = o.BBox()
                done = False
                for yy in np.arange(bb.GetTop() / 1e6 + 0.3, bb.GetBottom() / 1e6 - 0.3, 0.1):
                    for xx in np.arange(bb.GetLeft() / 1e6 + 0.3, bb.GetRight() / 1e6 - 0.3, 0.1):
                        x, y = float(xx), float(yy)
                        if o.PointInside(VECTOR2I(mm(x), mm(y))) and spot_ok(x, y) and \
                                all(seg_ok(lg, l, netname, x, y, x, y, rv, gap) for l in CU) and \
                                all(np.hypot(x - hx, y - hy) > hr + 0.15 + 0.26 for kind, (hx, hy, hr), nn in
                                    [it for it in items if it[2] == '__hole']) and \
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
        for gi in fp.GraphicalItems():
            if gi.GetLayer() in (pcbnew.F_Cu, pcbnew.B_Cu):
                bb = gi.GetBoundingBox()
                out.append(({gi.GetLayer()}, 'r', (bb.GetLeft() / 1e6, bb.GetTop() / 1e6, bb.GetRight() / 1e6, bb.GetBottom() / 1e6), ''))
    return out

def net_clr(name):
    """The clearance a net's class asks for (0 for the default class): the post-route checks keep at least this much."""
    for _, (w, clr, members) in CLASSES.items():
        if name in members:
            return clr
    if re.match(r'^/S\d_12V$', name):
        return CLASSES['Power12'][1]
    if re.match(r'^/(S\d_5V|S\d_3V3|S\d_BATT|C\d_5V)$', name):
        return CLASSES['Power'][1]
    return 0.0


def seg_ok(items, lay, net, ax, ay, ex, ey, hw, gap=0.14):
    own = net_clr(net)
    reach = hw + gap + 3.0              # only the items near the segment matter
    x1, x2, y1, y2 = min(ax, ex) - reach, max(ax, ex) + reach, min(ay, ey) - reach, max(ay, ey) + reach

    def near(it):
        g = it[2]
        if it[1] == 'c':
            return x1 < g[0] < x2 and y1 < g[1] < y2
        if it[1] == 's':
            return not (max(g[0], g[2]) < x1 or min(g[0], g[2]) > x2 or max(g[1], g[3]) < y1 or min(g[1], g[3]) > y2)
        return not (g[2] < x1 or g[0] > x2 or g[3] < y1 or g[1] > y2)
    items = [it for it in items if it[3] != net and lay in it[0] and near(it)]
    steps = max(2, int(np.hypot(ex - ax, ey - ay) / 0.05))
    for k in range(steps + 1):
        x, y = ax + (ex - ax) * k / steps, ay + (ey - ay) * k / steps
        if not (OX + 0.4 < x < OX + W - 0.4 and OY + 0.4 < y < OY + H - 0.4):
            return False
        for lays, kind, g, n in items:
            if n == net or lay not in lays:
                continue
            gap_ = max(gap, own - 0.002, net_clr(n) - 0.002) if n else gap
            if kind == 'c':
                if np.hypot(x - g[0], y - g[1]) < hw + g[2] + gap_:
                    return False
            elif kind == 's':
                sx, sy, tx, ty, w = g
                dx, dy = tx - sx, ty - sy
                L = dx * dx + dy * dy
                u = 0 if L == 0 else max(0, min(1, ((x - sx) * dx + (y - sy) * dy) / L))
                if np.hypot(x - sx - u * dx, y - sy - u * dy) < hw + w + gap_:
                    return False
            elif g[0] - hw - gap_ < x < g[2] + hw + gap_ and g[1] - hw - gap_ < y < g[3] + hw + gap_:
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


def maze_join(b, ni, a, e, hw, gap=0.16, res=0.1, margin=8.0, dbg=False):
    """A* on a res-mm grid over F.Cu and B.Cu (vias between them) from end a to end e, avoiding every other net's
    copper by hw + gap. a / e: (x, y, {layers}). Adds the tracks and vias; returns True if it found a path."""
    import heapq
    net = ni.GetNetname()
    LAY = [pcbnew.F_Cu, pcbnew.B_Cu]
    # the grid is aligned to the start pin, so its escape corridor between fine-pitch neighbours is on the grid
    x0 = a[0] - np.ceil((a[0] - min(a[0], e[0]) + margin) / res) * res
    y0 = a[1] - np.ceil((a[1] - min(a[1], e[1]) + margin) / res) * res
    x1, y1 = max(a[0], e[0]) + margin, max(a[1], e[1]) + margin
    nx, ny = int((x1 - x0) / res) + 1, int((y1 - y0) / res) + 1
    gx = x0 + np.arange(nx) * res
    gy = y0 + np.arange(ny) * res
    X, Y = np.meshgrid(gx, gy)
    blocked = [np.zeros((ny, nx), bool), np.zeros((ny, nx), bool)]
    vblock = np.zeros((ny, nx), bool)
    # the board edge keep-out
    edge = (X < OX + 0.45) | (X > OX + W - 0.45) | (Y < OY + 0.45) | (Y > OY + H - 0.45)
    for k in (0, 1):
        blocked[k] |= edge
    vblock |= edge
    own = net_clr(net)
    for lays, kind, g, n in geo(b):
        if n == net:
            continue
        gap_ = max(gap, own + 0.02, net_clr(n) + 0.02) if n else gap
        for k, L in enumerate(LAY):
            on = L in lays
            for arr, inf, use in ((blocked[k], hw + gap_, on), (vblock, 0.3 + gap_, on or kind == 'c')):
                if not use:
                    continue
                if kind == 'c':
                    bx1, by1, bx2, by2 = g[0] - g[2], g[1] - g[2], g[0] + g[2], g[1] + g[2]
                elif kind == 's':
                    bx1, by1, bx2, by2 = min(g[0], g[2]) - g[4], min(g[1], g[3]) - g[4], max(g[0], g[2]) + g[4], max(g[1], g[3]) + g[4]
                else:
                    bx1, by1, bx2, by2 = g
                i1, i2 = int(np.floor((bx1 - inf - x0) / res)), int(np.ceil((bx2 + inf - x0) / res)) + 1
                j1, j2 = int(np.floor((by1 - inf - y0) / res)), int(np.ceil((by2 + inf - y0) / res)) + 1
                i1, j1, i2, j2 = max(i1, 0), max(j1, 0), min(i2, nx), min(j2, ny)
                if i1 >= i2 or j1 >= j2:
                    continue
                sx, sy = X[j1:j2, i1:i2], Y[j1:j2, i1:i2]
                if kind == 'c':
                    d = np.hypot(sx - g[0], sy - g[1]) - g[2]
                elif kind == 's':
                    ax_, ay_, ex_, ey_, w_ = g
                    dx, dy = ex_ - ax_, ey_ - ay_
                    L2 = dx * dx + dy * dy
                    u = np.clip(((sx - ax_) * dx + (sy - ay_) * dy) / L2, 0, 1) if L2 else 0
                    d = np.hypot(sx - ax_ - u * dx, sy - ay_ - u * dy) - w_
                else:
                    d = np.hypot(np.maximum(np.maximum(g[0] - sx, sx - g[2]), 0), np.maximum(np.maximum(g[1] - sy, sy - g[3]), 0))
                arr[j1:j2, i1:i2] |= d < inf

    def cell(p):
        return int(round((p[1] - y0) / res)), int(round((p[0] - x0) / res))
    sa, se = cell(a), cell(e)
    starts = [(k, sa[0], sa[1]) for k, L in enumerate(LAY) if L in a[2]]
    goals = {(k, se[0], se[1]) for k, L in enumerate(LAY) if L in e[2]}
    if not starts or not goals:
        return False

    def hcost(k, j, i):
        return np.hypot(j - se[0], i - se[1]) * res
    VIA = 2.0
    openq = [(hcost(*s_), 0.0, s_, None) for s_ in starts]
    heapq.heapify(openq)
    came, gbest = {}, {s_: 0.0 for s_ in starts}
    steps = [(dj, di, (res * 1.4142 if dj and di else res)) for dj in (-1, 0, 1) for di in (-1, 0, 1) if dj or di]
    found = None
    while openq:
        f_, gc, node, prev = heapq.heappop(openq)
        if node in came:
            continue
        came[node] = prev
        if node in goals:
            found = node
            break
        k, j, i = node
        nbrs = [((k, j + dj, i + di), c) for dj, di, c in steps]
        if not vblock[j, i]:
            nbrs.append(((1 - k, j, i), VIA))
        for (k2, j2, i2), c in nbrs:
            if not (0 <= j2 < ny and 0 <= i2 < nx) or blocked[k2][j2, i2] or (k2, j2, i2) in came:
                continue
            g2 = gc + c
            if g2 < gbest.get((k2, j2, i2), 1e18):
                gbest[(k2, j2, i2)] = g2
                heapq.heappush(openq, (g2 + hcost(k2, j2, i2), g2, (k2, j2, i2), node))
    if dbg:
        j0, i0 = starts[0][1], starts[0][2]
        for jj in range(j0 - 14, j0 + 15):
            print('MZ ' + ''.join('S' if (jj, ii) == (j0, i0) else ('#' if blocked[0][jj, ii] else '.') for ii in range(i0 - 30, i0 + 31)))
        print('MZ grid', nx, ny, 'blocked F %.2f B %.2f via %.2f' % (blocked[0].mean(), blocked[1].mean(), vblock.mean()),
              'start', [(s_, blocked[s_[0]][s_[1], s_[2]]) for s_ in starts], 'goals', [(g_, blocked[g_[0]][g_[1], g_[2]]) for g_ in goals],
              'expanded', len(came), 'found', found is not None)
    if not found:
        return False
    path = []
    n_ = found
    while n_ is not None:
        path.append(n_)
        n_ = came[n_]
    path.reverse()
    # tracks: runs of collinear moves on one layer; vias where the layer changes
    pts = lambda n_: (float(x0 + n_[2] * res), float(y0 + n_[1] * res))
    seg_start = path[0]
    prev_dir = None
    for p0, p1 in zip(path, path[1:]):
        if p0[0] != p1[0]:
            if seg_start != p0:
                _track(b, ni, LAY[p0[0]], pts(seg_start), pts(p0), hw)
            v = pcbnew.PCB_VIA(b)
            v.SetPosition(VECTOR2I(mm(pts(p0)[0]), mm(pts(p0)[1])))
            v.SetWidth(mm(0.6))
            v.SetDrill(mm(0.3))
            v.SetNet(ni)
            b.Add(v)
            seg_start, prev_dir = p1, None
            continue
        d_ = (p1[1] - p0[1], p1[2] - p0[2])
        if prev_dir is not None and d_ != prev_dir:
            _track(b, ni, LAY[p0[0]], pts(seg_start), pts(p0), hw)
            seg_start = p0
        prev_dir = d_
    if seg_start != path[-1]:
        _track(b, ni, LAY[path[-1][0]], pts(seg_start), pts(path[-1]), hw)
    # join the grid ends to the true end points
    _track(b, ni, LAY[path[0][0]], a[:2], pts(path[0]), hw)
    _track(b, ni, LAY[path[-1][0]], pts(path[-1]), e[:2], hw)
    return True


def _track(b, ni, lay, p0, p1, hw):
    if abs(p0[0] - p1[0]) < 1e-6 and abs(p0[1] - p1[1]) < 1e-6:
        return
    t = pcbnew.PCB_TRACK(b)
    t.SetStart(VECTOR2I(mm(float(p0[0])), mm(float(p0[1]))))
    t.SetEnd(VECTOR2I(mm(float(p1[0])), mm(float(p1[1]))))
    t.SetWidth(mm(hw * 2))
    t.SetLayer(lay)
    t.SetNet(ni)
    b.Add(t)


def finish(b, depth=0):
    """Join what Freerouting left of a net in separate pieces: the closest pair of pads / track ends between two
    pieces, by a straight or L-shaped track on a layer both ends are on, if it clears everything else."""
    b.BuildConnectivity()
    c = b.GetConnectivity()
    fixed = 0
    stuck = []
    for ni in list(b.GetNetInfo().NetsByName().values()):
        net = ni.GetNetname()
        if not net or net in PLANES or net.startswith('unconnected'):
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
                        for r in np.arange(0.7, 6.0, 0.15):
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
            if not done:                # through two vias: down beside each end, across on another layer
                def spots(pt, k=14):
                    out = []
                    for r in np.arange(0.7, 5.0, 0.2):
                        for ang in np.arange(0, 2 * np.pi, np.pi / 10):
                            vx, vy = float(pt[0] + r * np.cos(ang)), float(pt[1] + r * np.sin(ang))
                            if not all(seg_ok(items, l, net, vx, vy, vx, vy, 0.3) for l in CU):
                                continue
                            l0 = next((l for l in CU if l in pt[2] and seg_ok(items, l, net, pt[0], pt[1], vx, vy, hw)), None)
                            if l0 is not None:
                                out.append((vx, vy, l0))
                                break
                        if len(out) >= k:
                            break
                    return out

                def add(lay, p0, p1):
                    if p0 == p1:
                        return
                    t = pcbnew.PCB_TRACK(b)
                    t.SetStart(VECTOR2I(mm(float(p0[0])), mm(float(p0[1]))))
                    t.SetEnd(VECTOR2I(mm(float(p1[0])), mm(float(p1[1]))))
                    t.SetWidth(mm(hw * 2))
                    t.SetLayer(lay)
                    t.SetNet(ni)
                    b.Add(t)

                for d, a, e in pairs[:6]:
                    va, ve = spots(a), spots(e)
                    for (ax_, ay_, la) in va:
                        for (ex_, ey_, le) in ve:
                            if np.hypot(ax_ - ex_, ay_ - ey_) < 0.9:
                                continue
                            for lay in (pcbnew.F_Cu, pcbnew.B_Cu):
                                for path in shapes((ax_, ay_), (ex_, ey_)):
                                    if all(seg_ok(items, lay, net, *p0, *p1, hw) for p0, p1 in zip(path, path[1:])):
                                        add(la, a[:2], (ax_, ay_))
                                        add(le, e[:2], (ex_, ey_))
                                        for p0, p1 in zip(path, path[1:]):
                                            add(lay, p0, p1)
                                        for vx, vy in ((ax_, ay_), (ex_, ey_)):
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
            if not done:                # last: a grid maze route between the closest pair of ends
                for d, a, e in pairs[:3]:
                    if maze_join(b, ni, a, e, hw):
                        done = True
                        print('  maze-routed a piece of', net)
                        break
            if done:
                fixed += 1
                main_ += ends(cl)
            else:
                stuck.append((ni, pairs[0][1], pairs[0][2], hw))
    # rip-up and retry: clear other nets' track segments right around the stuck ends, route the stuck link through
    # the maze, then rejoin the nets that lost segments
    ripped = 0
    if stuck and depth < 2 and os.environ.get('DIGIDASH_RIPUP'):   # opt-in: at this density it tends to cascade
        for ni, a, e, hw in stuck:
            win = []
            for (px, py, _) in (a, e):
                win.append((px - 1.0, py - 1.0, px + 1.0, py + 1.0))
            for t in list(b.GetTracks()):
                if t.IsLocked() or t.GetNetname() in PLANES or t.GetNetname() == ni.GetNetname():
                    continue
                a_, e_ = t.GetStart(), t.GetEnd()     # a via's start and end are its position
                for (x1, y1, x2, y2) in win:
                    if not (max(a_.x, e_.x) / 1e6 < x1 or min(a_.x, e_.x) / 1e6 > x2 or
                            max(a_.y, e_.y) / 1e6 < y1 or min(a_.y, e_.y) / 1e6 > y2):
                        b.Delete(t)
                        ripped += 1
                        break
            ok = maze_join(b, ni, a, e, hw)
            print('  %s: ripped up nearby segments, maze %s' % (ni.GetNetname(), 'routed' if ok else 'failed'))
        print('finisher: %d links added, %d segments ripped up' % (fixed, ripped))
        finish(b, depth + 1)            # rejoin what the rip-up cut
        return
    for ni, a, e, hw in stuck:
        print('  could not join a piece of', ni.GetNetname())
    print('finisher: %d links added' % fixed)


def unrouted(b):
    b.BuildConnectivity()
    return b.GetConnectivity().GetUnconnectedCount(False)


def freeroute(jar):
    """Freerouting 1.9.0 (Java 17+; opens its window while it works): one run from an unrouted board, after locked
    fan-out vias for every SMD GND and +5 V pad, so the router never has to reach the planes itself."""
    dsn, ses = os.path.join(TMP, 'digidash2.dsn'), os.path.join(TMP, 'digidash2.ses')
    jar = os.path.abspath(jar)          # Freerouting runs in TMP
    b = pcbnew.LoadBoard(PCB)
    rules(b)
    smd = sorted({f.GetReference() for f in b.GetFootprints()
                  if any(p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD for p in f.Pads())})
    preroute(b)
    stitch(b, fanout=smd, netname='GND')
    stitch(b, fanout=smd, netname='+5V')
    pcbnew.SaveBoard(PCB, b)
    for z in list(b.Zones()):
        if not z.GetIsRuleArea() and z.GetLayer() not in PLANES.values():
            b.Remove(z)                 # the In1 / In2 planes stay: Freerouting reaches them with vias
    pcbnew.ExportSpecctraDSN(b, dsn)
    if os.path.exists(ses):
        os.remove(ses)
    java = os.environ.get('JAVA', 'java')
    subprocess.check_call([java, '-jar', jar, '-de', dsn, '-do', ses, '-mp', os.environ.get('FR_PASSES', '120'), '-mt', '1'],
                          cwd=TMP, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    after_route(ses)


def narrow_power_links(b):
    """Report tracks on the wide nets (class 0.8 mm and up) narrower than half their class: the finisher joins
    leftovers at 0.2-0.3 mm, which on a power net needs widening or a hand-drawn link (see README.md)."""
    width = {m: w for _, (w, _, ms) in CLASSES.items() for m in ms}
    found = {}
    for t in b.GetTracks():
        if t.GetClass() != 'PCB_TRACK':
            continue
        w = width.get(t.GetNetname(), 0.8 if re.match(r'^/S\d_12V$', t.GetNetname()) else 0)
        if w >= 0.8 and t.GetWidth() / 1e6 < w / 2:
            found[t.GetNetname()] = found.get(t.GetNetname(), 0) + t.GetLength() / 1e6
    for n, l in sorted(found.items()):
        print('  narrow link on %s: %.1f mm under half its class width: check it' % (n, l))
    print('narrow power links: %s' % (len(found) or 'none'))


def preroute(b):
    """Add PREROUTE as locked tracks (and a locked via at each layer change), so Freerouting routes around them."""
    prev = None
    for net, lay, w, pts in PREROUTE:
        ni = b.FindNet(net)
        assert ni, net
        layer = pcbnew.F_Cu if lay == 'F' else pcbnew.B_Cu
        if prev and prev[0] == net and prev[1] != lay:
            v = pcbnew.PCB_VIA(b)
            v.SetPosition(P(*pts[0]))
            v.SetWidth(mm(0.6))
            v.SetDrill(mm(0.3))
            v.SetNet(ni)
            v.SetLocked(True)
            b.Add(v)
        for p0, p1 in zip(pts, pts[1:]):
            t = pcbnew.PCB_TRACK(b)
            t.SetStart(P(*p0))
            t.SetEnd(P(*p1))
            t.SetWidth(mm(w))
            t.SetLayer(layer)
            t.SetNet(ni)
            t.SetLocked(True)
            b.Add(t)
        prev = (net, lay)


def after_route(ses):
    """Import the Freerouting session into the board (which has the fan-out vias), join leftovers, stitch, fill."""
    b = pcbnew.LoadBoard(PCB)
    rules(b)
    assert pcbnew.ImportSpecctraSES(b, ses)
    # the locked fan-out stays; Freerouting does not always route around it, so drop the pieces it ran through
    lg = geo(b)
    bad = []
    for t in b.GetTracks():
        if not t.IsLocked():
            continue
        if t.GetClass() == 'PCB_VIA':
            q = t.GetPosition()
            ok = all(seg_ok(lg, l, t.GetNetname(), q.x / 1e6, q.y / 1e6, q.x / 1e6, q.y / 1e6, 0.3, 0.1) for l in CU)
        else:
            a_, e_ = t.GetStart(), t.GetEnd()
            ok = seg_ok(lg, t.GetLayer(), t.GetNetname(), a_.x / 1e6, a_.y / 1e6, e_.x / 1e6, e_.y / 1e6, t.GetWidth() / 2e6, 0.1)
        if not ok:
            bad.append(t)
    for t in bad:
        b.Delete(t)
    print('fan-out pieces in the way of routed tracks, removed: %d' % len(bad))
    # vias Freerouting dropped against a same-net through-hole pin (its hole is too close; the pin reaches the plane)
    holes = [(p.GetPosition(), p.GetDrillSize().x) for f in b.GetFootprints() for p in f.Pads() if p.GetDrillSize().x > 0]
    close = [v for v in b.GetTracks() if v.GetClass() == 'PCB_VIA' and any(
        np.hypot((v.GetPosition().x - q.x) / 1e6, (v.GetPosition().y - q.y) / 1e6) < (v.GetDrill() + d) / 2e6 + 0.26
        for q, d in holes)]
    for v in close:
        b.Delete(v)
    print('vias too close to a through-hole pin, removed: %d' % len(close))
    finish(b)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    print('routed: %d tracks and vias, %d unrouted' % (len(b.GetTracks()), unrouted(b)))
    narrow_power_links(b)
    pcbnew.SaveBoard(PCB, b)
    b = pcbnew.LoadBoard(PCB)
    rules(b)
    stitch(b, pitch=4.0)
    stitch(b, netname='+5V', grid=False)
    add_labels(b)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(PCB, b)


# ---------------------------------------------------------------------------------------------- silkscreen labels
# ref: (title, {pin number: label}, legend). Pin labels sit beside each pin (one row of pins); a legend is one line of text for
# multi-row headers. Names come from the schematic's nets. Where the schematic has no better word the net name is kept.
LABELS = {
    'J1': ('CENTER SCREEN', None, None), 'J4': ('RIGHT SCREEN', None, None), 'J5': ('LEFT SCREEN', None, None),
    'J6': ('SERIAL', {1: '3V3', 2: 'TX', 3: 'RX', 4: 'GND'}, None),
    'J7': ('POWER IN', {1: '12V', 2: '12V', 3: 'GND', 4: 'GND', 5: 'IGN', 6: 'LAMP'}, None),
    'J9': ('CAMERA', {1: 'REV', 2: '12V', 3: '--', 4: 'GND'}, None),
    'J11': ('VIDEO IN', None, None),
    'J13': ('AUX 12V IN', {1: '12V', 2: 'GND'}, None),
    'J16': ('CAN TAP', {1: 'H', 2: 'L', 3: 'GND'}, None),
    'J17': ('CABLE 1', {1: '5V', 2: 'H', 3: 'L', 4: 'G'}, None), 'J22': ('CABLE 2', {1: '5V', 2: 'H', 3: 'L', 4: 'G'}, None),
    'J23': ('CABLE 3', {1: '5V', 2: 'H', 3: 'L', 4: 'G'}, None), 'J24': ('CABLE 4', {1: '5V', 2: 'H', 3: 'L', 4: 'G'}, None),
    'J25': ('TRUNK', {1: 'H', 2: 'L', 3: '12V', 4: 'G'}, None),
    'J27': ('SENSOR SERIAL', {1: 'TX IN', 2: 'GND'}, None),
    'SW1': ('1 TERM  2 --', None, 'ON = on; back side'),
}
# slots: the name; pin 1 is the square pad, at the bottom
LABELS.update({'J%d' % (31 + i): ('SLOT %d' % (i + 1), None, None) for i in range(6)})


def add_labels(b):
    """Silkscreen names and pin labels for every connector, in free space beside it. Reads the board as it is."""
    def tsize(s, size, rot):
        w, h = len(s) * size * 1.0 + 0.5, size * 1.3
        return (h, w) if rot else (w, h)

    known = {'BAY 1', 'BAY 2', 'BAY 1A', 'BAY 1B', 'BAY 2A', 'BAY 2B', 'BAY 1 POWER', 'BAY 2 POWER', 'GPS', 'AUX 12V',
             'DIGIDASH REV 2'}
    for title, pins, legend in LABELS.values():
        known.add(title)
        known.update((pins or {}).values())
        if legend:
            known.add(legend)
    for d in [d for d in b.GetDrawings() if d.GetClass() == 'PCB_TEXT' and d.GetLayer() in (pcbnew.F_SilkS, pcbnew.B_SilkS)
              and d.GetText() in known]:
        b.Delete(d)

    occs = {'F': Occ(), 'B': Occ()}

    def occupy(side, x, y, w, h, pad=0.15):
        occs[side].rect(x - w / 2, y - h / 2, x + w / 2, y + h / 2, pad=pad)

    def rect_free(side, x, y, w, h):
        a = occs[side]
        i1, i2 = int(np.floor((x - w / 2) / GRID)), int(np.ceil((x + w / 2) / GRID))
        j1, j2 = int(np.floor((y - h / 2) / GRID)), int(np.ceil((y + h / 2) / GRID))
        if i1 < 0 or j1 < 0 or i2 > NX or j2 > NY:
            return False
        return not a.a[j1:j2, i1:i2].any()

    for fp in b.GetFootprints():
        fp.BuildCourtyardCaches()
        tht = fp.GetAttributes() & pcbnew.FP_THROUGH_HOLE
        side = 'B' if fp.IsFlipped() else 'F'
        for s_, lay in (('F', pcbnew.F_CrtYd), ('B', pcbnew.B_CrtYd)):
            if s_ != side and not tht:
                continue
            c = fp.GetCourtyard(lay)
            for k in range(c.OutlineCount()):
                occs[s_].rect(*bb_mm(c.Outline(k).BBox()), pad=0.1)
        for pad in fp.Pads():
            for s_, lay in (('F', pcbnew.F_Cu), ('B', pcbnew.B_Cu)):
                if pad.IsOnLayer(lay) or pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
                    occs[s_].rect(*bb_mm(pad.GetBoundingBox()), pad=0.2)
        for fld in (fp.Reference(),):
            if fld.IsVisible() and fld.GetLayer() in (pcbnew.F_SilkS, pcbnew.B_SilkS):
                s_ = 'F' if fld.GetLayer() == pcbnew.F_SilkS else 'B'
                occs[s_].rect(*bb_mm(fld.GetBoundingBox()), pad=0.1)
    for d in b.GetDrawings():
        if d.GetClass() == 'PCB_TEXT' and d.GetLayer() in (pcbnew.F_SilkS, pcbnew.B_SilkS):
            s_ = 'F' if d.GetLayer() == pcbnew.F_SilkS else 'B'
            occs[s_].rect(*bb_mm(d.GetBoundingBox()), pad=0.1)
    for z in b.Zones():                 # keep-outs are not silk keep-outs
        pass

    def put_text(side, s, x, y, size, rot):
        t = pcbnew.PCB_TEXT(b)
        t.SetText(s)
        t.SetLayer(pcbnew.F_SilkS if side == 'F' else pcbnew.B_SilkS)
        t.SetTextSize(VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(0.12 if size < 1 else 0.15))
        t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_CENTER)
        t.SetVertJustify(pcbnew.GR_TEXT_V_ALIGN_CENTER)
        t.SetMirrored(side == 'B')
        # seen from the back the text is mirrored, and a quarter turn reads the other way round
        t.SetTextAngle(EDA_ANGLE((270 if side == 'B' else 90) if rot else 0, pcbnew.DEGREES_T))
        t.SetPosition(P(x, y))
        b.Add(t)
        w, h = tsize(s, size, rot)
        occupy(side, x, y, w, h)

    def place_near(side, s, size, anchors, rot, reach=9.0):
        """The first free spot for text s, trying each anchor (x, y) then growing rings around it."""
        w, h = tsize(s, size, rot)
        for ax, ay in anchors:
            for r in np.arange(0.0, reach, 0.4):
                for ang in (np.linspace(0, 2 * np.pi, 16, endpoint=False) if r else [0.0]):
                    x, y = ax + r * np.cos(ang), ay + r * np.sin(ang)
                    if rect_free(side, x, y, w, h):
                        put_text(side, s, float(x), float(y), size, rot)
                        return True
        return False

    missed = []
    for ref, (title, pins, legend) in LABELS.items():
        fp = b.FindFootprintByReference(ref)
        if fp is None:
            continue
        side = 'B' if fp.IsFlipped() else 'F'
        numbered = [(p.GetNumber(), p.GetPosition()) for p in fp.Pads() if p.GetNumber().isdigit()]
        if not numbered:
            continue
        px = [q.x / 1e6 - OX for _, q in numbered]
        py = [q.y / 1e6 - OY for _, q in numbered]
        cx, cy = (min(px) + max(px)) / 2, (min(py) + max(py)) / 2
        inx, iny = W / 2 - cx, H / 2 - cy               # toward the middle of the board
        along_x = (max(px) - min(px)) >= (max(py) - min(py))
        row = [(int(n), (q.x / 1e6 - OX, q.y / 1e6 - OY)) for n, q in numbered]
        all_placed = True
        if pins:
            plan = []
            for num, (x, y) in row:
                lab = pins.get(num)
                if not lab:
                    continue
                # perpendicular to the row: inboard first, then the other side; text runs along that direction
                dirs = [(0, 1 if iny > 0 else -1), (0, -1 if iny > 0 else 1)] if along_x else \
                       [(1 if inx > 0 else -1, 0), (-1 if inx > 0 else 1, 0)]
                rot = 1 if along_x else 0
                w, h = tsize(lab, 0.8, rot)
                length = h if along_x else w             # how far the text reaches away from the pin
                spot = None
                for dx, dy in dirs:
                    for off in np.arange(1.6, 7.0, 0.3):
                        x2, y2 = x + dx * (off + length / 2), y + dy * (off + length / 2)
                        if rect_free(side, x2, y2, w, h):
                            spot = (lab, x2, y2, rot)
                            break
                    if spot:
                        break
                if spot is None:
                    all_placed = False
                    break
                plan.append(spot)
            if all_placed:
                for lab, x2, y2, rot in plan:
                    put_text(side, lab, float(x2), float(y2), 0.8, rot)
            else:                                        # no room beside every pin: one legend line instead
                legend = ((legend + '  ') if legend else '') + '  '.join('%d %s' % (n, pins[n]) for n, _ in sorted(row) if pins.get(n))
        anchors = [(cx + inx / max(abs(inx), abs(iny), 1e-6) * d, cy + iny / max(abs(inx), abs(iny), 1e-6) * d) for d in (5, 8, 11)]
        anchors += [(cx, cy), (cx + 6, cy), (cx - 6, cy)]
        ok = place_near(side, title, 1.0, anchors, 0, reach=8.0)
        if legend:
            # wrapped at about 26 characters (on the double spaces), each line placed near the last
            lines, cur_line = [], ''
            for part in legend.split('  '):
                if cur_line and len(cur_line) + len(part) + 2 > 26:
                    lines.append(cur_line)
                    cur_line = part
                else:
                    cur_line = (cur_line + '  ' + part) if cur_line else part
            lines.append(cur_line)
            ax, ay = cx + (inx / max(abs(inx), abs(iny), 1e-6)) * 4.0, cy + (iny / max(abs(inx), abs(iny), 1e-6)) * 4.0
            for ln in lines:
                w, h = tsize(ln, 0.7, 0)
                spot = None
                for r in np.arange(0.0, 22.0, 0.5):
                    for ang in (np.linspace(0, 2 * np.pi, 24, endpoint=False) if r else [0.0]):
                        x, y = ax + r * np.cos(ang), ay + r * np.sin(ang)
                        if rect_free(side, x, y, w, h):
                            spot = (x, y)
                            break
                    if spot:
                        break
                if spot is None:
                    ok = False
                    break
                put_text(side, ln, float(spot[0]), float(spot[1]), 0.7, 0)
                ax, ay = spot[0], spot[1] + 1.3
        if not ok:
            missed.append(ref)
    if not place_near('B', 'DIGIDASH REV 2', 1.5, [(W - 60, 70), (W - 90, 20), (150, 60), (60, 60)], 0, reach=30.0):
        missed.append('title')
    print('labels: no room for some text near', sorted(set(missed)) or 'none')


if __name__ == '__main__':
    if '--fill' in sys.argv or '--stitch' in sys.argv:
        bb = pcbnew.LoadBoard(PCB)
        rules(bb)
        if '--stitch' in sys.argv:
            stitch(bb, pitch=4.0)
        pcbnew.ZONE_FILLER(bb).Fill(bb.Zones())
        pcbnew.SaveBoard(PCB, bb)
    elif '--from-ses' in sys.argv:        # rebuild the pre-route board (placement and fan-out are deterministic)
        main()
        bb = pcbnew.LoadBoard(PCB)
        rules(bb)
        smd = sorted({f.GetReference() for f in bb.GetFootprints()
                      if any(p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD for p in f.Pads())})
        stitch(bb, fanout=smd, netname='GND')
        stitch(bb, fanout=smd, netname='+5V')
        pcbnew.SaveBoard(PCB, bb)
        after_route(os.path.join(TMP, 'digidash2.ses'))
    elif '--labels' in sys.argv:          # add the silkscreen labels to the current board
        bb = pcbnew.LoadBoard(PCB)
        rules(bb)
        add_labels(bb)
        pcbnew.SaveBoard(PCB, bb)
    elif '--finish' in sys.argv:
        bb = pcbnew.LoadBoard(PCB)
        rules(bb)
        finish(bb)
        pcbnew.ZONE_FILLER(bb).Fill(bb.Zones())
        pcbnew.SaveBoard(PCB, bb)
    else:
        main()
