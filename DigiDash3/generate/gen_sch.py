#!/usr/bin/env python3
"""Generate DigiDash3.kicad_sch: the DigiDash main board, rev 2 (docs/hardware/digidash_rev2_plan.md).

KiCad 7 file format (opens in 7-10). Needs the stock KiCad symbol libraries. Every connection is a net label or a
power symbol on a short stub at the pin, so the file reads as a netlist. Parts are laid out in blocks, one block per
function. Also writes generate/refs.json (each part's block, used by gen_pcb.py for placement).
"""
import json
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, grab_symbol  # noqa: E402

SYMDIR = os.environ.get('KICAD_SYMBOLS', next((d for d in ('/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols',
                                                               '/usr/share/kicad/symbols') if os.path.isdir(d)), ''))
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'DigiDash3.kicad_sch')
PROJECT = 'DigiDash3'
NS = uuid.UUID('c3a1d7e4-5b2f-4e8a-9d60-3e4f5a6b7c03')
SHEET = str(uuid.uuid5(NS, 'sheet'))
_n = 0


def uid(tag=''):
    global _n
    _n += 1
    return str(uuid.uuid5(NS, '%s#%d' % (tag, _n)))


def f(v):
    return ('%.4f' % v).rstrip('0').rstrip('.')


# ----------------------------------------------------------------------------- symbols
lib_text, lib_pins, lib_props = {}, {}, {}


def register(lib_id, text):
    lib_text[lib_id] = text
    t = parse(text)
    pins, props = {}, {}

    def walk(node, unit=0):
        if not isinstance(node, list) or not node:
            return
        if node[0] == 'symbol' and len(node) > 1 and isinstance(node[1], str):
            m = re.match(r'.*_(\d+)_\d+$', node[1])
            if m:
                unit = int(m.group(1))
        if node[0] == 'pin' and len(node) > 3:
            at = [x for x in node if isinstance(x, list) and x[0] == 'at'][0]
            nm = [x for x in node if isinstance(x, list) and x[0] == 'name'][0][1]
            nu = [x for x in node if isinstance(x, list) and x[0] == 'number'][0][1]
            pins[nu] = (float(at[1]), float(at[2]), int(float(at[3])), nm, node[1], unit)
        elif node[0] == 'property' and len(node) > 2:
            at = [x for x in node if isinstance(x, list) and x[0] == 'at']
            if at:
                props[node[1]] = (float(at[0][1]), float(at[0][2]))
        else:
            for c in node[1:]:
                walk(c, unit)

    walk(t)
    lib_pins[lib_id], lib_props[lib_id] = pins, props


def stock(lib, name, as_name=None, value=None):
    """A stock symbol; with as_name, a stock symbol (or the parent of one that extends another) under a new id."""
    txt = grab_symbol(os.path.join(SYMDIR, lib + '.kicad_sym'), name).strip()
    if as_name is None:
        lib_id = '%s:%s' % (lib, name)
        txt = re.sub(r'^\(symbol "%s"' % re.escape(name), '(symbol "%s"' % lib_id, txt, count=1)
    else:
        lib_id = 'DigiDash3:' + as_name
        txt = txt.replace('(symbol "%s"' % name, '(symbol "%s"' % lib_id, 1)
        txt = txt.replace('"%s_' % name, '"%s_' % as_name)
        if value:
            txt = re.sub(r'(\(property "Value" )"[^"]*"', r'\1"%s"' % value, txt, count=1)
    register(lib_id, txt)
    return lib_id


def box_symbol(lib_id, ref, value, left, right, w=15.24, footprint='', descr=''):
    """Boxed symbol. left/right: [(number, name, type)], pins every 2.54, 2.54 long."""
    rows = max(len(left), len(right))
    h = rows * 2.54 + 2.54
    top = h / 2
    name = lib_id.split(':')[1]
    o = ['(symbol "%s" (in_bom yes) (on_board yes)' % lib_id,
         '  (property "Reference" "%s" (at 0 %s 0) (effects (font (size 1.27 1.27))))' % (ref, f(top + 1.27)),
         '  (property "Value" "%s" (at 0 %s 0) (effects (font (size 1.27 1.27))))' % (value, f(-top - 1.27)),
         '  (property "Footprint" "%s" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))' % footprint,
         '  (property "Datasheet" "~" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))',
         '  (property "Description" "%s" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))' % descr,
         '  (symbol "%s_0_1"' % name,
         '    (rectangle (start %s %s) (end %s %s) (stroke (width 0.254) (type default)) (fill (type background)))' % (
             f(-w / 2), f(top), f(w / 2), f(-top)),
         '  )',
         '  (symbol "%s_1_1"' % name]
    for side, lst in ((0, left), (1, right)):
        for k, (num, nm, typ) in enumerate(lst):
            y = top - 2.54 - k * 2.54
            x, ang = (-w / 2 - 2.54, 0) if side == 0 else (w / 2 + 2.54, 180)
            o.append('    (pin %s line (at %s %s %d) (length 2.54) (name "%s" (effects (font (size 1.27 1.27)))) '
                     '(number "%s" (effects (font (size 1.27 1.27)))))' % (typ, f(x), f(y), ang, nm, num))
    o += ['  )', ')']
    register(lib_id, '\n'.join(o))
    return lib_id


# ----------------------------------------------------------------------------- placement on the sheet
body = []
pwr_n = [0]
refs_info = {}
ends = {}          # stub end position -> net, to catch two different nets meeting on the sheet
POWER = {'GND': ('power:GND', 'down'), '+5V': ('power:+5V', 'up'), '+3V3': ('power:+3V3', 'up')}


COLS = {1: 20, 2: 620, 3: 1220}
col_y = {1: 45, 2: 45, 3: 45}
blocks = []


class Block:
    """A band of one sheet column that parts of one function are packed into, left to right, then down. Each new
    block in a column starts below the last one."""

    def __init__(self, name, col, title, w=560):
        if blocks:
            b = blocks[-1]
            col_y[b.col] = max(col_y[b.col], b.y + b.row_h + 25)
        self.name, self.col, self.x0, self.y0, self.w = name, col, COLS[col], col_y[col], w
        self.x, self.y, self.row_h = self.x0, self.y0 + 12, 0
        blocks.append(self)
        text(self.x0, self.y0, title, 2.5)

    def slot(self, lib_id, unit=1):
        pins = [v for v in lib_pins[lib_id].values() if v[5] in (0, unit)]
        xs = [p[0] for p in pins] or [0]
        ys = [-p[1] for p in pins] or [0]
        pad_x, pad_y = 22, 8
        w = max(xs) - min(xs) + 2 * pad_x
        h = max(ys) - min(ys) + 2 * pad_y
        if self.x + w > self.x0 + self.w and self.x > self.x0:
            self.x, self.y, self.row_h = self.x0, self.y + self.row_h, 0
        cx = self.x + pad_x - min(xs)
        cy = self.y + pad_y - min(ys)
        self.x += w
        self.row_h = max(self.row_h, h)
        return round(cx / 2.54) * 2.54, round(cy / 2.54) * 2.54


def stub(x, y, angle_toward_body, net):
    out = (angle_toward_body + 180) % 360
    dx, dy = {0: (2.54, 0), 180: (-2.54, 0), 90: (0, -2.54), 270: (0, 2.54)}[out]
    x2, y2 = x + dx, y + dy
    key = (round(x2, 2), round(y2, 2))
    assert ends.get(key, net) == net, ('stub ends meet', key, ends.get(key), net)
    ends[key] = net
    body.append('  (wire (pts (xy %s %s) (xy %s %s)) (stroke (width 0) (type default)) (uuid "%s"))' % (
        f(x), f(y), f(x2), f(y2), uid('w')))
    if net in POWER:
        power(net, x2, y2, out)
    else:
        rot, just = (180, 'right') if out == 180 else (0, 'left')
        body.append('  (label "%s" (at %s %s %d) (effects (font (size 1.27 1.27)) (justify %s bottom)) (uuid "%s"))' % (
            net, f(x2), f(y2), rot, just, uid('l')))


def power(net, x, y, outward=None):
    lib_id, nat = POWER[net]
    rot = 180 if (outward == 90 and nat == 'down') or (outward == 270 and nat == 'up') else 0
    pwr_n[0] += 1
    ref = '#PWR%03d' % pwr_n[0]
    body.append('''  (symbol (lib_id "%s") (at %s %s %d) (unit 1) (in_bom no) (on_board no) (dnp no) (uuid "%s")
    (property "Reference" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))
    (property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))
    (property "Datasheet" "" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))
    (pin "1" (uuid "%s"))
    (instances (project "%s" (path "/%s" (reference "%s") (unit 1))))
  )''' % (lib_id, f(x), f(y), rot, uid('p'), ref, f(x), f(y), net, f(x + 2.54), f(y), f(x), f(y), f(x), f(y),
          uid('pp'), PROJECT, SHEET, ref))


cur = [None]
cnt = {}


def nid(prefix):
    cnt[prefix] = cnt.get(prefix, 0) + 1
    return '%s%d' % (prefix, cnt[prefix])


used_refs = set()


def place(lib_id, ref, value, footprint, nets, descr=None, unit=1, first=True, at=None, dnp=False):
    """nets: {pin number: net | None (no-connect)}; every pin of the unit must be listed. dnp: not fitted."""
    blk = cur[0]
    if first:
        assert ref not in used_refs, ref
        used_refs.add(ref)
        refs_info[ref] = {'group': blk.name, 'fp': footprint, 'value': value, 'dnp': dnp}
    x, y = at if at else blk.slot(lib_id, unit)
    pins = {k: v for k, v in lib_pins[lib_id].items() if v[5] in (0, unit)}
    props = lib_props[lib_id]

    def pos(k, dflt):
        px, py = props.get(k, dflt)
        return f(x + px), f(y - py)

    rp, vp = pos('Reference', (0, 3)), pos('Value', (0, -3))
    lines = ['  (symbol (lib_id "%s") (at %s %s 0) (unit %d) (in_bom yes) (on_board yes) (dnp %s) (uuid "%s")' % (
        lib_id, f(x), f(y), unit, 'yes' if dnp else 'no', uid('s' + ref + str(unit))),
        '    (property "Reference" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))' % (ref, rp[0], rp[1]),
        '    (property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))' % (value, vp[0], vp[1]),
        '    (property "Footprint" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (footprint, f(x), f(y)),
        '    (property "Datasheet" "~" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (f(x), f(y))]
    if descr:
        lines.append('    (property "Description" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (
            descr.replace('"', "'"), f(x), f(y)))
    for num in pins:
        lines.append('    (pin "%s" (uuid "%s"))' % (num, uid('pin' + ref + str(unit) + num)))
    lines.append('    (instances (project "%s" (path "/%s" (reference "%s") (unit %d))))' % (PROJECT, SHEET, ref, unit))
    lines.append('  )')
    body.append('\n'.join(lines))
    seen = set()
    for num, net in nets.items():
        px, py, ang, _, _, _ = pins[num]
        sx, sy = x + px, y - py
        k = (round(sx, 2), round(sy, 2))
        assert ends.get(k, net) == net, ('pin lands on another net', ref, num, net, ends.get(k))
        ends[k] = net
        if k in seen:           # stacked pins (same position): one stub
            continue
        seen.add(k)
        if net is None:
            body.append('  (no_connect (at %s %s) (uuid "%s"))' % (f(sx), f(sy), uid('nc')))
        else:
            stub(sx, sy, ang, net)
    missing = set(pins) - set(nets)
    assert not missing, (ref, unit, sorted(missing))
    return ref


def text(x, y, s, size=1.6):
    s = s.replace('"', "'").replace('\n', '\\n')
    body.append('  (text "%s" (at %s %s 0) (effects (font (size %s %s)) (justify left top)) (uuid "%s"))' % (
        s, f(x), f(y), f(size), f(size), uid('t')))


# ----------------------------------------------------------------------------- libraries and footprints

R = stock('Device', 'R')
C = stock('Device', 'C')
CP = stock('Device', 'C_Polarized')
L = stock('Device', 'L')
PTC = stock('Device', 'Polyfuse')
FUSE = stock('Device', 'Fuse')
DZ = stock('Device', 'D_Zener')
DS = stock('Device', 'D_Schottky')
DD = stock('Device', 'D')
TVS = stock('Device', 'D_TVS')
NETTIE = box_symbol('DigiDash3:NetTie_GND', 'NT', 'NetTie', [('1', 'A', 'passive')], [('2', 'B', 'passive')], w=5.08,
                    footprint='NetTie:NetTie-2_THT_Pad1.0mm', descr='Net tie: one copper joint between two grounds')
NPN = stock('Transistor_BJT', 'Q_NPN_EBC', '2N3904', '2N3904')                     # TO-92: pins 1 E, 2 B, 3 C
PFET = stock('Transistor_FET', 'IRF9540N', 'IRF4905', 'IRF4905')                   # TO-220: pins 1 G, 2 D, 3 S
XCVR = stock('Interface_CAN_LIN', 'MCP2562-E-P')            # DIP-8, same pins as the SOIC
LDO33 = stock('Regulator_Linear', 'AP1117-15', 'LD1117V33', 'LD1117V33')              # TO-220: pins 1 GND, 2 OUT, 3 IN
# LM2576HV in TO-220-5 (TI SNVS107): 1 VIN, 2 OUT, 3 GND, 4 FB, 5 ON/OFF (low = on)
BUCK = box_symbol('DigiDash3:LM2576HVT', 'U', 'LM2576HVT-5.0',
                  [('1', 'VIN', 'power_in'), ('5', '~{ON}/OFF', 'input'), ('3', 'GND', 'power_in')],
                  [('2', 'OUT', 'output'), ('4', 'FB', 'input')], w=15.24,
                  footprint='Package_TO_SOT_THT:TO-220-5_Horizontal_TabDown',
                  descr='TI LM2576HV-5.0 (or equal): 5 V, 3 A step-down, 4-60 V in, 52 kHz, TO-220-5')
DIPSW = stock('Switch', 'SW_DIP_x02')
MH = stock('Mechanical', 'MountingHole_Pad')
HDR = {n: stock('Connector_Generic', 'Conn_01x%02d' % n) for n in (2, 3, 4, 6)}
HDR2 = {n: stock('Connector_Generic', 'Conn_02x%02d_Odd_Even' % n) for n in (10, 15, 20)}
for n in ('GND', '+5V', '+3V3'):
    stock('power', n)
stock('power', 'PWR_FLAG')
# every part through-hole
FP_R = 'Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal'
FP_C = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm'                      # 100n ceramic, 5 mm pitch
FP_CP10 = 'Capacitor_THT:CP_Radial_D10.0mm_P5.00mm'                      # 100u 63V, 1000u 16V
FP_CP5 = 'Capacitor_THT:CP_Radial_D5.0mm_P2.00mm'                        # 10u 25V
FP_PTC = 'DigiDash3:PTC_Radial_P5.08mm'                                  # Bourns MF-R series
FP_DO35 = 'Diode_THT:D_DO-35_SOD27_P7.62mm_Horizontal'                   # 1N4148, BAT85
FP_DO41 = 'Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal'                  # 1N4744A
FP_DO15 = 'Diode_THT:D_DO-15_P10.16mm_Horizontal'                        # SA24CA
FP_DO201 = 'Diode_THT:D_DO-201AD_P15.24mm_Horizontal'                    # 1.5KE33A, SB560
FP_TO220_3 = 'Package_TO_SOT_THT:TO-220-3_Horizontal_TabDown'
FP_TO220_5 = 'Package_TO_SOT_THT:TO-220-5_Horizontal_TabDown'
FP_TO92 = 'Package_TO_SOT_THT:TO-92_Inline'
FP_DIP8 = 'Package_DIP:DIP-8_W7.62mm'
FP_L = 'Inductor_THT:L_Radial_D14.2mm_P10.00mm_Neosid_SD14'              # radial drum core on a 10 mm pitch (the Abracon's leads are bent out to it)
FP_DIPSW = 'Button_Switch_THT:SW_DIP_SPSTx02_Slide_6.7x6.64mm_W7.62mm_P2.54mm_LowProfile'
FP_PH4 = 'Connector_JST:JST_PH_B4B-PH-K_1x04_P2.00mm_Vertical'          # vertical: every cable plugs in from the rear
FP_SLOT = 'DigiDash3:Slot_Socket_2x15_P2.54mm_Vertical'                 # numbered to mate with the card's right-angle header
FP_H2 = 'Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical'
FP_H3 = 'Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical'
FP_H2X10 = 'Connector_PinHeader_2.54mm:PinHeader_2x10_P2.54mm_Vertical'


def r_(v, a, b, d=None, fp=FP_R, dnp=False, ref=None):
    return place(R, ref or nid('R'), v, fp, {'1': a, '2': b}, descr=d, dnp=dnp)


def c_(v, a, b, d=None, fp=FP_C, dnp=False, ref=None):
    return place(C, ref or nid('C'), v, fp, {'1': a, '2': b}, descr=d, dnp=dnp)


def cp_(v, a, b, d=None, fp=FP_CP10):
    return place(CP, nid('C'), v, fp, {'1': a, '2': b}, descr=d)


def ptc(v, a, b, d=None):
    return place(PTC, nid('F'), v, FP_PTC, {'1': a, '2': b}, descr=d)


def npn(b, e, c, d=None, ref=None):
    return place(NPN, ref or nid('Q'), '2N3904', FP_TO92, {'1': e, '2': b, '3': c}, descr=d)


def zener(k, a, d=None):
    return place(DZ, nid('D'), '1N4744A', FP_DO41, {'1': k, '2': a}, descr=d)


def tvs_pair(a, b, where):
    """Bidirectional TVS (SA24CA, DO-15) from CANH and CANL to GND: the through-hole stand-in for a NUP2105L."""
    place(TVS, nid('D'), 'SA24CA', FP_DO15, {'1': a, '2': 'GND'}, descr='CANH clamp, ' + where)
    place(TVS, nid('D'), 'SA24CA', FP_DO15, {'1': b, '2': 'GND'}, descr='CANL clamp, ' + where)


# fixed reference numbers first, so the auto-numbered parts do not take them
for p, n in (('J', 36), ('U', 12), ('Q', 7), ('D', 3), ('F', 0), ('R', 0), ('C', 0)):
    cnt[p] = n

# ============================================================================= sheet
text(20, 15, 'DIGIDASH MAIN BOARD, REV 3  -  all through-hole (no assembly service): rev 2 (DigiDash2/) with the SMD parts '
     'swapped for through-hole ones, two 3 A bucks in place of the 5 A one, and the video decoder on a plug-in module', 5)
text(20, 26, 'Same outline, slot row, screen headers, holes and rear connector group as rev 2 (the housing fits both). The '
     'video section is a 2x10 socket (J8) for a TVP5150 breakout module; CAN transceivers are the DIP MCP2562-E/P. '
     'Two layers.', 2)

# ----------------------------------------------------------------------------- power entry
cur[0] = Block('power', 1, 'POWER: J7 (battery, ignition, illumination), reverse protection, TVS, two LM2576HV 3 A bucks '
               '(U2: the three screens, U3: everything else), a 3.3 V LDO, switched 12 V')
place(HDR[6], 'J7', 'Vehicle power', 'Connector_Molex:Molex_Micro-Fit_3.0_43650-0615_1x06_P3.00mm_Vertical',
      {'1': 'BATT_IN', '2': 'BATT_IN', '3': 'GND', '4': 'GND', '5': 'IGN_IN', '6': 'ILLUM_IN'},
      descr='Micro-Fit 3.0, 6 pin: 1-2 battery 12 V (constant), 3-4 ground, 5 ignition 12 V (key on), 6 illumination (dash lights on)')
place(PFET, 'Q2', 'IRF4905', FP_TO220_3, {'1': 'Q2_G', '2': 'BATT_IN', '3': '12V_BATT'},
      descr='Reverse-polarity protection: P-FET (55 V, 20 mOhm, TO-220 lying flat), drain to the input, gate to ground')
r_('10k', 'Q2_G', 'GND', 'Q2 gate to ground')
zener('12V_BATT', 'Q2_G', 'Q2 gate-source clamp, 15 V (Vgs max 20 V)')
place(TVS, nid('D'), '1.5KE33A', FP_DO201, {'1': '12V_BATT', '2': 'GND'},
      descr='TVS, unidirectional, 33 V standoff (1.5KE33A, DO-201): clamps load dump below the bucks\' 60 V')
cp_('100u 63V', '12V_BATT', 'GND', 'Input bulk (63 V: above the TVS clamp)')
c_('100n', '12V_BATT', 'GND', 'Input HF')
text(cur[0].x0, cur[0].y0 + 6, 'Two LM2576HV-5.0 (TI SNVS107 figure 1 with 68 uH in place of 100 uH: about 45 % more ripple current, still continuous at this load; 1000 uF / 5 A Schottky each): U2 feeds the three screens '
     '(+5V_SCR), U3 feeds the board, the slots and the cable ports (+5V). Both ON/OFF pins hang on Q7: low (on) while the '
     'ignition is on or a card holds WAKE high. Off, the pins rest at 12V_BATT through 100k (ON/OFF max is VIN).', 1.6)
for ref, out, sw, what in (('U2', '+5V_SCR', 'SW_A', 'the three screens'), ('U3', '+5V', 'SW_B', 'the board, the slots and the cable ports')):
    place(BUCK, ref, 'LM2576HVT-5.0', FP_TO220_5, {'1': '12V_BATT', '2': sw, '3': 'GND', '4': out, '5': 'BUCK_ONOFF'},
          descr='5 V, 3 A step-down for %s (LM2576HV-5.0: 60 V in, fixed 5 V, FB tied to OUT), TO-220-5 lying flat' % what)
    cp_('100u 63V', '12V_BATT', 'GND', ref + ' input (100 uF, right at pin 1)')
    place(DS, nid('D'), 'SB560', FP_DO201, {'1': sw, '2': 'GND'}, descr=ref + ' catch diode: 5 A 60 V Schottky (1N5822 fits too)')
    place(L, nid('L'), '68u', FP_L, {'1': sw, '2': out},
          descr=ref + ' inductor: 68 uH, 3.3 A radial drum core (Abracon AIAP-03-680K, 11.4 x 23 mm; its leads bent out to the 10 mm pitch)')
    cp_('1000u 16V', out, 'GND', ref + ' output (1000 uF, low ESR)')
r_('100k', '12V_BATT', 'BUCK_ONOFF', 'ON/OFF pull-up: both bucks off until Q7 pulls the pins low')
npn('Q7_B', 'GND', 'BUCK_ONOFF', 'Pulls both bucks on (ON/OFF low) while the ignition is on or a card holds WAKE', ref='Q7')
r_('47k', 'IGN_IN', 'Q7_B', 'Ignition drives Q7 (14 V: 0.28 mA into the base)')
r_('22k', 'Q7_B', 'GND', 'Q7 base pull-down')
r_('10k', 'WAKE_K', 'Q7_B', 'WAKE (through the wake diode) drives Q7')
# 3.3 V
place(LDO33, 'U11', 'LD1117V33', FP_TO220_3, {'1': 'GND', '2': '+3V3', '3': '+5V'},
      descr='3.3 V, 800 mA LDO (TO-220 lying flat): CAN transceiver I/O, the serial port, the slots (0.1 A each), I2C pull-ups, the video module when strapped to 3.3 V')
cp_('10u 25V', '+5V', 'GND', 'U11 in', FP_CP5)
cp_('10u 25V', '+3V3', 'GND', 'U11 out (the LD1117 wants 10 uF or more here)', FP_CP5)
c_('100n', '+3V3', 'GND', 'U11 out, HF')
# switched 12 V (slots pin 8, trunk port, camera)
text(cur[0].x0 + 300, cur[0].y0 + 6, 'SW12 (switched 12 V for the slots\' pin 8, the trunk port and the camera): a P-FET high-side switch from 12V_BATT, on while the ignition is on.', 1.6)
place(PFET, 'Q5', 'IRF4905', FP_TO220_3, {'1': 'Q5_G', '2': 'SW12', '3': '12V_BATT'}, descr='SW12 high-side switch (TO-220 lying flat)')
r_('10k', '12V_BATT', 'Q5_G', 'Q5 gate pull-up (off)')
zener('12V_BATT', 'Q5_G', 'Q5 gate-source clamp')
r_('4.7k', 'Q5_G', 'Q4_C', 'Q5 gate drive')
npn('Q4_B', 'GND', 'Q4_C', 'Pulls Q5 on while the ignition is on', ref='Q4')
r_('22k', 'IGN_IN', 'Q4_B', 'Q4 base')
r_('10k', 'Q4_B', 'GND', 'Q4 base pull-down')
# single-point tie between the AUX return and the logic ground
place(NETTIE, 'NT1', 'NetTie', 'NetTie:NetTie-2_THT_Pad1.0mm', {'1': 'GND', '2': 'AUX_GND'},
      descr='The only joint between the AUX 12 V return and the logic ground, at the power entry')
for k, n in enumerate(('GND', 'BATT_IN', 'IGN_IN', 'AUX_IN_F', 'AUX_GND', 'ILLUM_IN', '12V_BATT', '+5V', '+5V_SCR', 'AUX12', 'SW12', 'VID_VCC'), 1):
    place('power:PWR_FLAG', '#FLG%02d' % k, 'PWR_FLAG', '', {'1': n})
for i in range(4):
    place(MH, 'H%d' % (i + 1), 'MountingHole', 'MountingHole:MountingHole_3.2mm_M3_Pad', {'1': 'GND'},
          descr='Board mounting hole, M3 (the housing\'s four posts)')

# ----------------------------------------------------------------------------- AUX 12 V (bay power)
cur[0] = Block('aux', 1, 'AUX 12 V: slot power from its own fused feed (pins 25-30 of every slot). Its return (AUX_GND) meets GND only at NT1.')
place(HDR[2], 'J13', 'AUX 12V in', 'Connector_Molex:Molex_Mini-Fit_Jr_5566-02A_2x01_P4.20mm_Vertical',
      {'1': 'AUX_IN', '2': 'AUX_GND'}, descr='Mini-Fit Jr 2 pin: 1 AUX 12 V (own 7.5 A fuse at the fuse box, switched), 2 ground')
place(FUSE, nid('F'), '5A mini blade', 'Fuse:Fuseholder_Blade_Mini_Keystone_3568', {'1': 'AUX_IN', '2': 'AUX_IN_F'},
      descr='Board fuse for all six slots together (mini blade, 5 A: AUX is sized for about 3 A, the LTE card)')
place(PFET, 'Q6', 'IRF4905', FP_TO220_3, {'1': 'Q6_G', '2': 'AUX_IN_F', '3': 'AUX12'}, descr='AUX reverse-polarity protection (TO-220 lying flat)')
r_('10k', 'Q6_G', 'AUX_GND', 'Q6 gate to the AUX return')
zener('AUX12', 'Q6_G', 'Q6 gate-source clamp')
place(TVS, nid('D'), '1.5KE33A', FP_DO201, {'1': 'AUX12', '2': 'AUX_GND'}, descr='AUX TVS')
cp_('100u 63V', 'AUX12', 'AUX_GND', 'AUX bulk (63 V: above the TVS clamp)')

# ----------------------------------------------------------------------------- screens and CAN
cur[0] = Block('screens', 1, 'SCREENS: J5 left, J1 center, J4 right (ESP32-P4, plugged in face down), 5 V from U2. One MCP2562 '
               '(DIP) each, VIO from the board 3.3 V. 120 R at the left (R) and right (switched) ends of the bus.')
HDR_NETS = {
    'J1': {'1': '+5V_SCR', '3': '+5V_SCR', '5': 'GND', '8': 'V_D3', '10': 'GND', '11': 'V_D2', '12': 'V_D1', '13': 'GND',
           '14': 'V_D0', '15': 'SENS_TX_C', '16': 'V_D7', '20': 'SER_RX', '22': 'SER_TX', '23': 'CAN_RX_C',
           '24': 'V_VS', '25': 'V_PCLK', '26': 'GND', '27': 'REV_TRIGGER', '28': 'Illumination', '29': 'GND',
           '30': 'V_HS', '31': 'CAN_TX_C', '32': 'V_D6', '33': 'GND', '34': 'CL_SCL', '35': 'V_D5', '36': 'V_P17',
           '37': 'V_FID', '38': 'CL_SDA', '39': 'V_D4', '40': 'GND'},
    'J4': {'1': '+5V_SCR', '3': '+5V_SCR', '5': 'GND', '10': 'GND', '13': 'GND', '15': 'SENS_TX_R', '17': 'POS_R', '18': '3V3_R',
           '23': 'CAN_RX_R', '31': 'CAN_TX_R', '34': 'CL_SCL', '38': 'CL_SDA'},
    'J5': {'1': '+5V_SCR', '3': '+5V_SCR', '5': 'GND', '10': 'GND', '13': 'GND', '15': 'SENS_TX_L', '17': 'POS_L', '19': 'GND',
           '23': 'CAN_RX_L', '26': 'GND', '29': 'GND', '31': 'CAN_TX_L', '33': 'GND', '34': 'CL_SCL',
           '38': 'CL_SDA', '40': 'GND'},
}
# Pin 28 (GPIO24) is wired on the center screen only: on the side screens (4C) GPIO24 is the P4's USB D-
for ref, name in (('J5', 'Left Screen'), ('J1', 'Center Screen - Master'), ('J4', 'Right Screen')):
    nets = {str(n): HDR_NETS[ref].get(str(n)) for n in range(1, 41)}
    place(HDR2[20], ref, name, 'Connector_PinHeader_2.54mm:PinHeader_2x20_P2.54mm_Vertical', nets,
          descr='Waveshare ESP32-P4 round display, face down; same pin use as rev 1 and rev 2. Pin 18 (the screen\'s 3V3) is not tied to the board 3.3 V')
r_('1k', 'POS_L', 'GND', 'Left position strap')
r_('1k', 'POS_R', '3V3_R', 'Right position strap, to the right screen\'s own 3V3')
for side, ref in (('L', 'U5'), ('C', 'U9'), ('R', 'U10')):
    place(XCVR, ref, 'MCP2562-E/P', FP_DIP8,
          {'1': 'CAN_TX_' + side, '2': 'GND', '3': '+5V', '4': 'CAN_RX_' + side, '5': '+3V3', '6': 'CANL', '7': 'CANH', '8': 'GND'},
          descr='%s screen CAN transceiver (DIP-8)' % {'L': 'Left', 'C': 'Center', 'R': 'Right'}[side])
    c_('100n', '+5V', 'GND', ref + ' VDD')
    c_('100n', '+3V3', 'GND', ref + ' VIO')
    r_('10k', '+3V3', 'CAN_TX_' + side, 'TXD pull-up (recessive while the screen boots)')
r_('120', 'CANH', 'CANL', 'Bus termination, left end')
r_('120', 'CANH', 'CAN_TERM_R', 'Bus termination, right end, through switch SW1-1')
place(DIPSW, 'SW1', 'TERM', FP_DIPSW, {'1': 'CAN_TERM_R', '4': 'CANL', '2': None, '3': None},
      descr='Back-side slide switch. 1 TERM: right-end 120 R, ON at the factory, OFF when a trunk cable carries the bus on (fit 120 R at the far end). 2: spare')
tvs_pair('CANH', 'CANL', 'at the transceivers')
place(HDR[3], 'J16', 'CAN debug', FP_H3, {'1': 'CANH', '2': 'CANL', '3': 'GND'}, descr='Tap for a USB-CAN adapter')
r_('4.7k', '+3V3', 'CL_SDA', 'Cluster I2C pull-up')
r_('4.7k', '+3V3', 'CL_SCL', 'Cluster I2C pull-up')
# vehicle inputs to the screens
text(cur[0].x0 + 300, cur[0].y0 + 6, 'Vehicle inputs to the screens (as rev 1): illumination (J7 pin 6), reverse (J9 pin 1). J6: serial port (center UART2).', 1.6)
r_('10k', 'ILLUM_IN', 'Q1_B', 'Illumination base')
r_('10k', 'Q1_B', 'GND', 'Illumination base pull-down')
npn('Q1_B', 'GND', 'Illumination', 'Illumination: low = dim (center screen GPIO24, internal pull-up)', ref='Q1')
c_('100n', 'Illumination', 'GND', 'Illumination filter')
place(HDR[4], 'J9', 'Backup Camera', FP_PH4, {'1': 'REV_IN', '2': 'CAM_12V', '3': None, '4': 'GND'},
      descr='1 reverse lamp 12 V, 2 camera power (switched, fused), 3 spare, 4 ground. Video goes to the module on J8')
ptc('PTC 0.5A', 'SW12', 'CAM_12V', 'Camera 12 V')
r_('10k', 'REV_IN', 'Q3_B', 'Reverse trigger base')
r_('10k', 'Q3_B', 'GND', 'Reverse trigger base pull-down')
npn('Q3_B', 'GND', 'REV_TRIGGER', 'Reverse: low = in reverse (center GPIO25)', ref='Q3')
c_('100n', 'REV_TRIGGER', 'GND', 'Reverse filter')
text(cur[0].x0 + 300, cur[0].y0 + 12, 'Sensor serial (optional, a jumper wire from the sensor card): J27 pin 1 (5 V TX) -> 10k / 20k divider per screen '
     '(5 V -> 3.3 V) -> pin 15 of J5 / J1 / J4. Sensor data normally arrives on CAN; this is the fallback path.', 1.6)
place(HDR[2], 'J27', 'Sensor serial in', FP_H2, {'1': 'SENS_TX', '2': 'GND'},
      descr='Sensor serial in (optional jumper wire from the sensor card J8): 1 TX from the card (5 V logic, 115200 8N1), 2 GND')
for side in ('L', 'C', 'R'):
    r_('10k', 'SENS_TX', 'SENS_TX_' + side, 'Divider top, %s screen' % side)
    r_('20k', 'SENS_TX_' + side, 'GND', 'Divider bottom, %s screen' % side)
place(HDR[4], 'J6', 'Serial', FP_PH4, {'1': 'SER_3V3', '2': 'SER_TX', '3': 'SER_RX', '4': 'GND'},
      descr='Serial port: the center screen\'s UART2, 3.3 V logic. 1 3V3, 2 TX (out of the dash), 3 RX (into the dash), 4 GND')
ptc('PTC 0.1A', '+3V3', 'SER_3V3', 'Serial port 3.3 V (J6 pin 1): a short on the cable cannot take the rail down')

# ----------------------------------------------------------------------------- video module
cur[0] = Block('video', 3, 'VIDEO: a TVP5150 breakout module on J8 (2x10 socket; the module carries its own video input, '
               'crystal and regulators) -> 8-bit BT.656 to the center screen\'s camera pins (same J1 pins as rev 1 and 2). '
               'I2C on the cluster bus. JP1 straps the module\'s VCC to 5 V or 3.3 V. J10 brings PCLK / RESET on wires.')
text(cur[0].x0, cur[0].y0 + 6, 'J8 pin order is the module\'s: 1 VCC, 2 GND, 3 SCL, 4 SDA, 5 VSYNC, 6 HSYNC, 7-8 n.c., 9-16 D7..D0, 17 n.c., 18 FID, 19 INT, '
     '20 ID. The module has no pixel-clock pin on its header: J10 pin 1 is the center screen\'s PCLK (J1 pin 25), for a wire '
     'from the module\'s PCLK point; J10 pin 2 is the center\'s reset line (GPIO52) for the module\'s RESET, if it has one.', 1.6)
place(HDR2[10], 'J8', 'Video module', FP_H2X10,
      {'1': 'VID_VCC', '2': 'GND', '3': 'CL_SCL', '4': 'CL_SDA', '5': 'V_VS', '6': 'V_HS', '7': None, '8': None,
       '9': 'V_D7', '10': 'V_D6', '11': 'V_D5', '12': 'V_D4', '13': 'V_D3', '14': 'V_D2', '15': 'V_D1', '16': 'V_D0',
       '17': None, '18': 'V_FID', '19': 'VID_INT', '20': 'VID_ID'},
      descr='TVP5150 breakout module, 2x10 pin socket (fit a 2x10 female header; the module plugs in face up)')
place(HDR[3], 'JP1', 'Video VCC', FP_H3, {'1': '+5V', '2': 'VID_VCC', '3': '+3V3'},
      descr='Module supply strap: shunt 1-2 = 5 V, shunt 2-3 = 3.3 V (check the module: most TVP5150 breakouts take 3.3 V)')
place(HDR[3], 'J10', 'Video aux', FP_H3, {'1': 'V_PCLK', '2': 'V_P17', '3': 'GND'},
      descr='1 PCLK to the center screen (J1 pin 25): wire from the module\'s pixel clock. 2 the center\'s reset line (GPIO52, J1 pin 36): wire to the module\'s RESET. 3 GND')
r_('10k', 'VID_INT', 'GND', 'INT idle low (not used by the screens)')
r_('10k', 'VID_ID', 'GND', 'ID idle low (fit only if the module wants its I2C-address strap here)', dnp=True)
c_('100n', 'VID_VCC', 'GND', 'Module supply, at the socket')

# ----------------------------------------------------------------------------- cable ports, trunk
cur[0] = Block('ports', 3, 'CABLE PORTS J17, J22, J23, J24 (JST PH). Trunk port J25 (bus extension, 12 V).')
for i, ref in enumerate(('J17', 'J22', 'J23', 'J24')):
    p = 'C%d' % (i + 1)
    place(HDR[4], ref, 'Cable port %d' % (i + 1), FP_PH4, {'1': p + '_5V', '2': 'CANH', '3': 'CANL', '4': 'GND'},
          descr='Cable port: 1 +5 V, 2 CANH, 3 CANL, 4 GND (a short branch: under 30 cm)')
    ptc('PTC 0.5A', '+5V', p + '_5V', ref + ' 5 V')
place(HDR[4], 'J25', 'Trunk port', 'Connector_Molex:Molex_Micro-Fit_3.0_43650-0415_1x04_P3.00mm_Vertical',
      {'1': 'CANH', '2': 'CANL', '3': 'TRUNK_12V', '4': 'GND'},
      descr='Bus extension to the rear of the car: 1 CANH, 2 CANL (twisted pair), 3 switched 12 V (1.1 A), 4 GND. Switch SW1-1 off and terminate at the far end')
ptc('PTC 1.1A', 'SW12', 'TRUNK_12V', 'Trunk 12 V')
tvs_pair('CANH', 'CANL', 'at the trunk port')

# ----------------------------------------------------------------------------- card slots
cur[0] = Block('slots', 3, 'SLOTS: six 2x15 sockets J31-J36 (slot 1-6), cards stand on edge (docs/hardware/accessory_ports.md). '
               'Per slot: fused 5 V, switched 12 V, 3.3 V, constant 12 V, an ID resistor. Shared: CAN, WAKE, LAMP / REV levels, '
               'the analog audio bus, AUX 12 V, and the I2S rail (BCK, WS, MIC, RET, MCLK: bused to all six slots).')
SLOT_ID = ('1k', '3.3k', '6.8k', '12k', '22k', '47k')       # against the card's 10k pull-up to its 3.3 V
for i in range(6):
    n, s = i + 1, 'S%d' % (i + 1)
    place(HDR2[15], 'J%d' % (31 + i), 'Slot %d' % n, FP_SLOT,
          {'1': s + '_5V', '2': s + '_5V', '3': 'GND', '4': 'GND', '5': 'CANH', '6': 'CANL', '7': 'GND', '8': s + '_12V',
           '9': s + '_3V3', '10': s + '_BATT', '11': s + '_ID', '12': 'WAKE', '13': 'LAMP_LVL', '14': 'REV_LVL',
           '15': 'I2S_BCK', '16': 'I2S_WS', '17': 'I2S_MIC', '18': 'I2S_RET', '19': 'GND', '20': 'GND',
           '21': 'AUDIO_L', '22': 'AUDIO_R', '23': 'AUDIO_GND', '24': 'I2S_MCLK',
           '25': 'AUX12', '26': 'AUX12', '27': 'AUX12', '28': 'AUX_GND', '29': 'AUX_GND', '30': 'AUX_GND'},
          descr='Card slot %d: 2x15 female socket, 2.54 mm, vertical. The card carries a right-angle 2x15 male header on its bottom edge' % n)
    ptc('PTC 0.5A', '+5V', s + '_5V', 'Slot %d 5 V' % n)
    ptc('PTC 0.5A', 'SW12', s + '_12V', 'Slot %d switched 12 V' % n)
    ptc('PTC 0.1A', '+3V3', s + '_3V3', 'Slot %d 3.3 V' % n)
    ptc('PTC 0.2A', '12V_BATT', s + '_BATT', 'Slot %d constant 12 V (key-off functions)' % n)
    r_(SLOT_ID[i], s + '_ID', 'GND', 'Slot %d ID: the card reads it against its own 10k pull-up to 3.3 V' % n)
text(cur[0].x0 + 300, cur[0].y0 + 6, 'WAKE: a card drives it high (about 3.3 V, through its own diode) to start or hold the 5 V rails with the key off '
     '(the sensor card holds it to finish after key-off). LAMP_LVL / REV_LVL: J7 pin 6 and J9 pin 1 scaled to 0-3.3 V.', 1.6)
r_('100k', 'WAKE', 'GND', 'WAKE idle low (cards only ever drive it high)')
place(DD, nid('D'), '1N4148', FP_DO35, {'1': 'WAKE_K', '2': 'WAKE'}, descr='Wake diode: WAKE high drives Q7 (the bucks\' ON/OFF) through 10k')
for src, dst, what in (('ILLUM_IN', 'LAMP_LVL', 'Dash-light level (rheostat voltage) for the cards'),
                       ('REV_IN', 'REV_LVL', 'Reverse level for the cards (high = in reverse)')):
    r_('100k', src, dst, what + ': divider top (14.5 V -> 3.1 V)')
    r_('27k', dst, 'GND', what + ': divider bottom')
    c_('100n', dst, 'GND', what + ': filter')
    place(DS, nid('D'), 'BAT85', FP_DO35, {'1': '+3V3', '2': dst}, descr=what + ': clamp to 3.3 V')
    place(DS, nid('D'), 'BAT85', FP_DO35, {'1': dst, '2': 'GND'}, descr=what + ': clamp to ground')
place(NETTIE, 'NT2', 'NetTie', 'NetTie:NetTie-2_THT_Pad1.0mm', {'1': 'GND', '2': 'AUDIO_GND'},
      descr='The only joint between the slots\' audio-bus ground and the logic ground')

# ----------------------------------------------------------------------------- write
for b in blocks:
    col_y[b.col] = max(col_y[b.col], b.y + b.row_h + 25)
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "User" 1820 %d)' % (max(col_y.values()) + 60),
       '  (title_block (title "DigiDash main board") (date "2026-10-26") (rev "3")',
       '    (comment 1 "All through-hole: two LM2576HV bucks, DIP CAN transceivers, TVP5150 module socket, six card slots, AUX 12 V")',
       '    (comment 2 "docs/hardware/digidash_rev2_plan.md (rev 2 plan); DigiDash3/README.md"))',
       '  (lib_symbols']
for t in lib_text.values():
    out.append('\n'.join('    ' + ln for ln in t.split('\n')))
out.append('  )')
out += body
out += ['  (sheet_instances (path "/" (page "1")))', ')']
open(OUT, 'w').write('\n'.join(out) + '\n')
json.dump(refs_info, open(os.path.join(HERE, 'refs.json'), 'w'), indent=1, sort_keys=True)
print('wrote', os.path.normpath(OUT), len(body), 'items', len(refs_info), 'parts')
