#!/usr/bin/env python3
"""Generate DigiDash2.kicad_sch: the DigiDash main board, rev 2 (docs/hardware/digidash_rev2_plan.md).

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
OUT = os.path.join(HERE, '..', 'DigiDash2.kicad_sch')
PROJECT = 'DigiDash2'
NS = uuid.UUID('b1f0c9a2-7e4d-4d1e-9c55-1d2d0a2e2002')
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
        lib_id = 'DigiDash2:' + as_name
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


def place(lib_id, ref, value, footprint, nets, descr=None, unit=1, first=True, at=None):
    """nets: {pin number: net | None (no-connect)}; every pin of the unit must be listed."""
    blk = cur[0]
    if first:
        assert ref not in used_refs, ref
        used_refs.add(ref)
        refs_info[ref] = {'group': blk.name, 'fp': footprint, 'value': value}
    x, y = at if at else blk.slot(lib_id, unit)
    pins = {k: v for k, v in lib_pins[lib_id].items() if v[5] in (0, unit)}
    props = lib_props[lib_id]

    def pos(k, dflt):
        px, py = props.get(k, dflt)
        return f(x + px), f(y - py)

    rp, vp = pos('Reference', (0, 3)), pos('Value', (0, -3))
    lines = ['  (symbol (lib_id "%s") (at %s %s 0) (unit %d) (in_bom yes) (on_board yes) (dnp no) (uuid "%s")' % (
        lib_id, f(x), f(y), unit, uid('s' + ref + str(unit))),
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
FB = stock('Device', 'FerriteBead_Small')
PTC = stock('Device', 'Polyfuse')
FUSE = stock('Device', 'Fuse')
LED = stock('Device', 'LED')
DZ = stock('Device', 'D_Zener')
DS = stock('Device', 'D_Schottky')
DD = stock('Device', 'D')
XTAL = stock('Device', 'Crystal_GND24')
# a plain two-pin symbol for the net tie: the stock NetTie_2 merges its two nets in the schematic
NETTIE = box_symbol('DigiDash2:NetTie_GND', 'NT', 'NetTie', [('1', 'A', 'passive')], [('2', 'B', 'passive')], w=5.08,
                    footprint='NetTie:NetTie-2_SMD_Pad2.0mm', descr='Net tie: joins two ground nets at one point')
BAT54S = stock('Diode', 'BAT54S')
NUP = stock('Power_Protection', 'NUP2105L')
NPN = stock('Transistor_BJT', 'Q_NPN_BEC', 'MMBT3904', 'MMBT3904')
PFET = stock('Transistor_FET', 'SUD19P06-60', 'SUD50P04-08', 'SUD50P04-08')
OPTO = stock('Isolator', 'PC817')
MCU = stock('MCU_Microchip_ATmega', 'ATmega640V-8A', 'ATmega2560-16A', 'ATmega2560-16AU')
CANC = stock('Interface_CAN_LIN', 'MCP2515-xSO')
XCVR = stock('Interface_CAN_LIN', 'MCP2562-E-SN')
FRAM = stock('Memory_EEPROM', '24LC16', 'MB85RC256V', 'MB85RC256V')
BUCK = stock('Regulator_Switching', 'TPS54560BDDA')
LDO33 = stock('Regulator_Linear', 'SPX2920M3-3.3_SOT223', 'AP7361C-33E', 'AP7361C-33E')
LDO18 = stock('Regulator_Linear', 'AP2204K-1.5', 'AP2112K-1.8', 'AP2112K-1.8')
DIPSW = stock('Switch', 'SW_DIP_x02')
COAX = stock('Connector', 'Conn_Coaxial')
MH = stock('Mechanical', 'MountingHole_Pad')
HDR = {n: stock('Connector_Generic', 'Conn_01x%02d' % n) for n in (2, 3, 4, 6, 8)}
HDR2 = {n: stock('Connector_Generic', 'Conn_02x%02d_Odd_Even' % n) for n in (2, 3, 5, 15, 20)}
for n in ('GND', '+5V', '+3V3'):
    stock('power', n)
stock('power', 'PWR_FLAG')

# TVP5150AM1, 32-pin TQFP (PBS), pin numbers from TI SLES209E table 2-1.
TVP = box_symbol('DigiDash2:TVP5150AM1', 'U', 'TVP5150AM1IPBS', [
    ('1', 'AIP1A', 'input'), ('2', 'AIP1B', 'input'), ('5', 'XTAL1/OSC', 'input'), ('6', 'XTAL2', 'output'),
    ('8', 'RESETB', 'input'), ('28', 'PDN', 'input'), ('29', 'REFP', 'passive'), ('30', 'REFM', 'passive'),
    ('4', 'PLL_AVDD', 'power_in'), ('32', 'CH_AVDD', 'power_in'), ('20', 'DVDD', 'power_in'), ('10', 'IO_DVDD', 'power_in'),
    ('3', 'PLL_AGND', 'power_in'), ('7', 'AGND', 'power_in'), ('31', 'CH_AGND', 'power_in'), ('19', 'DGND', 'power_in')],
    [('18', 'YOUT0', 'output'), ('17', 'YOUT1', 'output'), ('16', 'YOUT2', 'output'), ('15', 'YOUT3', 'output'),
     ('14', 'YOUT4', 'output'), ('13', 'YOUT5', 'output'), ('12', 'YOUT6', 'output'), ('11', 'YOUT7/I2CSEL', 'bidirectional'),
     ('9', 'SCLK', 'output'), ('25', 'HSYNC', 'output'), ('24', 'VSYNC/PALI', 'output'), ('23', 'FID/GLCO', 'output'),
     ('26', 'AVID', 'output'), ('27', 'INTREQ/GPCL/VBLK', 'output'), ('21', 'SCL', 'bidirectional'), ('22', 'SDA', 'bidirectional')],
    w=22.86, footprint='Package_QFP:TQFP-32_5x5mm_P0.5mm',
    descr='TI TVP5150AM1 NTSC/PAL video decoder, 8-bit BT.656 out, 1.8 V core, 3.3 V I/O')

FP_R, FP_C, FP_C10 = 'Resistor_SMD:R_0603_1608Metric', 'Capacitor_SMD:C_0603_1608Metric', 'Capacitor_SMD:C_0805_2012Metric'
FP_C1210 = 'Capacitor_SMD:C_1210_3225Metric'
FP_R1206 = 'Resistor_SMD:R_1206_3216Metric'
FP_PTC = 'Fuse:Fuse_1812_4532Metric'
FP_PTC_S = 'Fuse:Fuse_1206_3216Metric'
FP_SOT23 = 'Package_TO_SOT_SMD:SOT-23'
FP_SOD123 = 'Diode_SMD:D_SOD-123'
FP_SOIC8 = 'Package_SO:SOIC-8_3.9x4.9mm_P1.27mm'
FP_XTAL = 'Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm'
FP_FB = 'Inductor_SMD:L_0603_1608Metric'
FP_PH4 = 'Connector_JST:JST_PH_S4B-PH-K_1x04_P2.00mm_Horizontal'
FP_SLOT = 'DigiDash2:Slot_Socket_2x15_P2.54mm_Vertical'   # numbered to mate with the card's right-angle header
FP_DIPSW = 'Button_Switch_SMD:SW_DIP_SPSTx02_Slide_KingTek_DSHP02TS_W7.62mm_P1.27mm'


def r_(v, a, b, d=None, fp=FP_R):
    return place(R, nid('R'), v, fp, {'1': a, '2': b}, descr=d)


def c_(v, a, b, d=None, fp=None):
    if fp is None:
        fp = FP_C1210 if v in ('2.2u 100V', '47u 10V') else FP_C10 if v in ('10u', '4.7u') else FP_C
    return place(C, nid('C'), v, fp, {'1': a, '2': b}, descr=d)


def ptc(v, a, b, d=None, fp=FP_PTC):
    return place(PTC, nid('F'), v, fp, {'1': a, '2': b}, descr=d)


def npn(b, e, c, d=None, ref=None):
    return place(NPN, ref or nid('Q'), 'MMBT3904', FP_SOT23, {'1': b, '2': e, '3': c}, descr=d)


def zener(v, k, a, d=None):
    return place(DZ, nid('D'), v, FP_SOD123, {'1': k, '2': a}, descr=d)


# fixed reference numbers first, so the auto-numbered parts do not take them
for p, n in (('J', 36), ('U', 12), ('Q', 6), ('D', 3), ('F', 0), ('R', 0), ('C', 0)):
    cnt[p] = n

# ============================================================================= sheet
text(20, 15, 'DIGIDASH MAIN BOARD, REV 2  -  docs/hardware/digidash_rev2_plan.md', 5)
text(20, 26, 'All SMD except connectors. ATmega2560 sensor pack on CAN (MCP2515 + MCP2562, node 0x04) with the UART '
     'fan-out kept as a fallback. TVP5150AM1 and FRAM on the board. Six 2x15 card slots (cards stand on edge), '
     'four cable ports, a trunk port, and an AUX 12 V path for the slots.', 2)

# ----------------------------------------------------------------------------- power entry
cur[0] = Block('power', 1, 'POWER: J7 (battery, ignition, illumination), reverse protection, TVS, 5 A buck '
               '(TI TPS54560B typical application, 400 kHz), 3.3 V and 1.8 V LDOs, switched 12 V')
place(HDR[6], 'J7', 'Vehicle power', 'Connector_Molex:Molex_Micro-Fit_3.0_43650-0600_1x06_P3.00mm_Horizontal',
      {'1': 'BATT_IN', '2': 'BATT_IN', '3': 'GND', '4': 'GND', '5': 'IGN_IN', '6': 'ILLUM_IN'},
      descr='Micro-Fit 3.0, 6 pin: 1-2 battery 12 V (constant), 3-4 ground, 5 ignition 12 V (key on), 6 illumination (dash lights on)')
place(PFET, 'Q2', 'SUD50P04-08', 'Package_TO_SOT_SMD:TO-252-2', {'1': 'Q2_G', '2': 'BATT_IN', '3': '12V_BATT'},
      descr='Reverse-polarity protection: P-FET, drain to the input, gate to ground (on when the battery is the right way round)')
r_('10k', 'Q2_G', 'GND', 'Q2 gate to ground')
zener('BZT52C15', '12V_BATT', 'Q2_G', 'Q2 gate-source clamp (Vgs max 20 V)')
place(DZ, nid('D'), 'SMCJ33A', 'Diode_SMD:D_SMC', {'1': '12V_BATT', '2': 'GND'},
      descr='TVS, unidirectional, 33 V standoff, clamps load dump below the buck\'s 65 V')
place(CP, nid('C'), '100u 50V', 'Capacitor_SMD:CP_Elec_10x10.5', {'1': '12V_BATT', '2': 'GND'}, descr='Input bulk')
for _ in range(4):
    c_('2.2u 100V', '12V_BATT', 'GND', 'Buck input (C1-C3, C10 in the TI example)')
c_('100n', '12V_BATT', 'GND', 'Buck input, HF')
place(BUCK, 'U2', 'TPS54560BDDA', 'Package_SO:TI_SO-PowerPAD-8_ThermalVias',
      {'1': 'BUCK_BOOT', '2': '12V_BATT', '3': 'BUCK_EN', '4': 'BUCK_RT', '5': 'BUCK_FB', '6': 'BUCK_COMP',
       '7': 'GND', '8': 'BUCK_SW', '9': 'GND'}, descr='5 V, 5 A step-down, 4.5-60 V in')
c_('100n', 'BUCK_BOOT', 'BUCK_SW', 'Bootstrap (C4)')
r_('243k', 'BUCK_RT', 'GND', 'RT: 400 kHz (R3)')
place(L, nid('L'), '7.2u', 'Inductor_SMD:L_Wuerth_HCI-1050', {'1': 'BUCK_SW', '2': '+5V'},
      descr='7.2 uH, Isat > 7 A, shielded (Wurth WE-HCI 1050, 7443551720 or equal)')
place(DS, nid('D'), 'B560C', 'Diode_SMD:D_SMC', {'1': 'BUCK_SW', '2': 'GND'}, descr='Catch diode, 5 A 60 V Schottky')
for _ in range(3):
    c_('47u 10V', '+5V', 'GND', 'Buck output (C6, C7, C9)')
r_('53.6k', '+5V', 'BUCK_FB', 'Feedback top (R5): 5.0 V')
r_('10.2k', 'BUCK_FB', 'GND', 'Feedback bottom (R6)')
r_('16.9k', 'BUCK_COMP', 'BUCK_CZ', 'Compensation (R4)')
c_('4.7n', 'BUCK_CZ', 'GND', 'Compensation (C5)')
c_('47p', 'BUCK_COMP', 'GND', 'Compensation (C8)')
text(cur[0].x0, cur[0].y0 + 6, 'EN: the 5 V rail runs while the ignition is on, or while the sensor pack holds it (PWR_HOLD) to finish after key-off.\\n'
     'IGN 12 V -> 442k / 90.9k (starts above ~6.5 V), clamped by a 5.1 V zener (EN abs. max 8.4 V). PWR_HOLD (5 V) -> BAT54S -> 47k also lifts EN.', 1.6)
r_('442k', 'IGN_IN', 'BUCK_EN', 'EN divider top, from the ignition line (R1 in the TI example)')
r_('90.9k', 'BUCK_EN', 'GND', 'EN divider bottom (R2)')
zener('BZT52C5V1', 'BUCK_EN', 'GND', 'EN clamp')
place(BAT54S, nid('D'), 'BAT54S', FP_SOT23, {'1': 'PWR_HOLD', '2': None, '3': 'HOLD_K'},
      descr='Hold diode (pin 1 to pin 3): PWR_HOLD high keeps EN up after key-off; the second diode is unused')
r_('47k', 'HOLD_K', 'BUCK_EN', 'Hold resistor')
r_('100k', 'PWR_HOLD', 'GND', 'Hold pulled low while the 2560 is in reset')
# 3.3 V and 1.8 V
place(LDO33, 'U11', 'AP7361C-33E', 'Package_TO_SOT_SMD:SOT-223-3_TabPin2', {'1': '+5V', '2': 'GND', '3': '+3V3'},
      descr='3.3 V, 1 A LDO: CAN transceiver I/O, TVP5150 I/O, the serial port, the slots (0.1 A each), I2C pull-ups')
c_('10u', '+5V', 'GND', 'U11 in')
c_('10u', '+3V3', 'GND', 'U11 out')
place(LDO18, 'U12', 'AP2112K-1.8', 'Package_TO_SOT_SMD:SOT-23-5',
      {'1': '+3V3', '2': 'GND', '3': '+3V3', '4': None, '5': '+1V8'}, descr='1.8 V, 600 mA LDO: TVP5150 core')
c_('1u', '+3V3', 'GND', 'U12 in')
c_('4.7u', '+1V8', 'GND', 'U12 out')
place(FB, nid('FB'), '600R', FP_FB, {'1': '+1V8', '2': '+1V8A'}, descr='Ferrite: TVP5150 analog 1.8 V')
c_('10u', '+1V8A', 'GND', 'TVP5150 analog bulk')
# switched 12 V (slots pin 8, trunk port, camera)
text(cur[0].x0 + 300, cur[0].y0 + 6, 'SW12 (switched 12 V for the slots\' pin 8, the trunk port and the camera): a P-FET high-side switch from 12V_BATT, on while the ignition is on.', 1.6)
place(PFET, 'Q5', 'SUD50P04-08', 'Package_TO_SOT_SMD:TO-252-2', {'1': 'Q5_G', '2': 'SW12', '3': '12V_BATT'},
      descr='SW12 high-side switch')
r_('10k', '12V_BATT', 'Q5_G', 'Q5 gate pull-up (off)')
zener('BZT52C15', '12V_BATT', 'Q5_G', 'Q5 gate-source clamp')
r_('4.7k', 'Q5_G', 'Q4_C', 'Q5 gate drive')
npn('Q4_B', 'GND', 'Q4_C', 'Pulls Q5 on while the ignition is on', ref='Q4')
r_('22k', 'IGN_IN', 'Q4_B', 'Q4 base')
r_('10k', 'Q4_B', 'GND', 'Q4 base pull-down')
# single-point tie between the AUX return and the logic ground
place(NETTIE, 'NT1', 'NetTie', 'NetTie:NetTie-2_SMD_Pad2.0mm', {'1': 'GND', '2': 'AUX_GND'},
      descr='The only joint between the AUX 12 V return and the logic ground, at the power entry')
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', {'1': 'GND'})
place('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', {'1': 'BATT_IN'})
place('power:PWR_FLAG', '#FLG03', 'PWR_FLAG', '', {'1': 'IGN_IN'})
place('power:PWR_FLAG', '#FLG04', 'PWR_FLAG', '', {'1': 'AUX_IN_F'})
place('power:PWR_FLAG', '#FLG05', 'PWR_FLAG', '', {'1': 'AUX_GND'})
place('power:PWR_FLAG', '#FLG06', 'PWR_FLAG', '', {'1': '+1V8A'})
place('power:PWR_FLAG', '#FLG07', 'PWR_FLAG', '', {'1': 'ILLUM_IN'})
place('power:PWR_FLAG', '#FLG08', 'PWR_FLAG', '', {'1': '12V_BATT'})
place('power:PWR_FLAG', '#FLG09', 'PWR_FLAG', '', {'1': '+5V'})
place('power:PWR_FLAG', '#FLG10', 'PWR_FLAG', '', {'1': 'AVCC'})
place('power:PWR_FLAG', '#FLG11', 'PWR_FLAG', '', {'1': 'AUX12'})
place('power:PWR_FLAG', '#FLG12', 'PWR_FLAG', '', {'1': 'SW12'})
for i in range(4):
    place(MH, 'H%d' % (i + 1), 'MountingHole', 'MountingHole:MountingHole_3.2mm_M3_Pad', {'1': 'GND'},
          descr='Board mounting hole, M3 (provisional: moves with the bezel)')

# ----------------------------------------------------------------------------- AUX 12 V (bay power)
cur[0] = Block('aux', 1, 'AUX 12 V: slot power from its own fused feed (pins 25-30 of every slot). Its return (AUX_GND) meets GND only at NT1.')
place(HDR[2], 'J13', 'AUX 12V in', 'Connector_Molex:Molex_Mini-Fit_Jr_5566-02A_2x01_P4.20mm_Vertical',
      {'1': 'AUX_IN', '2': 'AUX_GND'}, descr='Mini-Fit Jr 2 pin: 1 AUX 12 V (own 10-15 A fuse at the fuse box, switched), 2 ground')
place(FUSE, nid('F'), '10A mini blade', 'Fuse:Fuseholder_Blade_Mini_Keystone_3568', {'1': 'AUX_IN', '2': 'AUX_IN_F'},
      descr='Board fuse for all six slots together (mini blade, 10 A)')
place(PFET, 'Q6', 'SUD50P04-08', 'Package_TO_SOT_SMD:TO-252-2', {'1': 'Q6_G', '2': 'AUX_IN_F', '3': 'AUX12'},
      descr='AUX reverse-polarity protection (8 mOhm)')
r_('10k', 'Q6_G', 'AUX_GND', 'Q6 gate to the AUX return')
zener('BZT52C15', 'AUX12', 'Q6_G', 'Q6 gate-source clamp')
place(DZ, nid('D'), 'SMCJ33A', 'Diode_SMD:D_SMC', {'1': 'AUX12', '2': 'AUX_GND'}, descr='AUX TVS')
place(CP, nid('C'), '100u 50V', 'Capacitor_SMD:CP_Elec_10x10.5', {'1': 'AUX12', '2': 'AUX_GND'}, descr='AUX bulk')

# ----------------------------------------------------------------------------- sensor pack core
cur[0] = Block('mcu', 2, 'SENSOR PACK: ATmega2560, 16 MHz, 5 V. Firmware: arduino/sketches/sensor_pack (ported to this pin map)')
m = {str(n): None for n in range(1, 101)}
m.update({'10': '+5V', '31': '+5V', '61': '+5V', '80': '+5V', '100': 'AVCC', '11': 'GND', '32': 'GND', '62': 'GND',
          '81': 'GND', '99': 'GND', '98': 'AREF', '30': 'RESET', '33': 'XTAL2', '34': 'XTAL1',
          '2': 'FTDI_RX', '3': 'FTDI_TX',                       # USART0: FTDI header
          '45': 'SENS_RX1', '46': 'SENS_TX1',                  # USART1: sensor frames to the screens (fallback)
          '19': 'CAN_CS', '20': 'SCK', '21': 'MOSI', '22': 'MISO', '6': 'CAN_INT',
          '43': 'I2C_SCL', '44': 'I2C_SDA',                    # TWI: FRAM
          '35': 'TACH_SIG', '36': 'SPEED_SIG',                 # ICP4 / ICP5
          '97': 'FUEL_ADC', '96': 'ENGINE_TEMP_ADC', '95': 'OIL_PRESSURE_ADC', '94': 'AMB_TEMP_ADC',
          '93': 'BATT_SENSE', '92': 'SPARE_A5', '91': 'SPARE_A6', '90': 'SPARE_A7', '89': 'SPARE_A8',
          '78': 'ALT_L', '77': 'IGN_SENSE', '76': 'FUEL_MON', '75': 'TEMP_MON', '74': 'PRESS_MON',
          '73': 'VOLT_PWR_MON', '72': 'RPM_SEL_A', '71': 'RPM_SEL_B',
          '53': 'DEMO', '54': 'PWR_HOLD', '26': 'STATUS_LED',
          '37': 'SPARE_D47', '38': 'SPARE_D46', '39': 'SPARE_D45', '40': 'SPARE_D44',
          '88': 'WAKE_SENSE'})                                  # ADC9 / PCINT17: a slot card's WAKE request
place(MCU, 'U1', 'ATmega2560-16AU', 'Package_QFP:TQFP-100_14x14mm_P0.5mm', m,
      descr='Sensor pack MCU. USART0 = FTDI header, USART1 = sensor frames (fallback), SPI = MCP2515 + ISP, ICP4 / ICP5 = tach / speed')
for _ in range(4):
    c_('100n', '+5V', 'GND', 'U1 VCC pins')
c_('10u', '+5V', 'GND', 'U1 bulk')
place(FB, nid('FB'), '600R', FP_FB, {'1': '+5V', '2': 'AVCC'}, descr='AVCC filter')
c_('100n', 'AVCC', 'GND', 'AVCC')
c_('100n', 'AREF', 'GND', 'AREF (AVCC reference selected in firmware)')
place(XTAL, 'Y1', '16MHz', FP_XTAL, {'1': 'XTAL1', '2': 'GND', '3': 'XTAL2', '4': 'GND'}, descr='16 MHz, 3225, CL about 10 pF')
c_('18p', 'XTAL1', 'GND', 'Y1 load')
c_('18p', 'XTAL2', 'GND', 'Y1 load')
r_('10k', '+5V', 'RESET', 'Reset pull-up')
c_('100n', 'FTDI_DTR', 'RESET', 'Auto-reset from the FTDI header DTR')
place(HDR2[3], 'J2', 'AVR-ISP-6', 'Connector_IDC:IDC-Header_2x03_P2.54mm_Vertical',
      {'1': 'MISO', '2': '+5V', '3': 'SCK', '4': 'MOSI', '5': 'RESET', '6': 'GND'}, descr='ISP (USBasp or AVRISP mkII; USBtiny is unreliable above 128 KB)')
place(HDR[6], 'J12', 'FTDI', 'Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical',
      {'1': 'GND', '2': 'GND', '3': None, '4': 'FTDI_RX', '5': 'FTDI_TX', '6': 'FTDI_DTR'},
      descr='FTDI 6-pin (5 V logic): 1 GND, 2 CTS (grounded), 3 VCC (not used: power from the board), 4 adapter TX, 5 adapter RX, 6 DTR')
place(LED, nid('D'), 'green', 'LED_SMD:LED_0603_1608Metric', {'1': 'LED_K', '2': 'STATUS_LED'}, descr='Status (D13)')
r_('1k', 'LED_K', 'GND', 'Status LED')
r_('10k', 'DEMO', 'GND', 'Demo mode pulled low (sensor frames are real data)')
place(HDR2[5], 'J26', 'Spare I/O', 'Connector_PinHeader_2.54mm:PinHeader_2x05_P2.54mm_Vertical',
      {'1': '+5V', '2': 'GND', '3': 'SPARE_A5', '4': 'SPARE_A6', '5': 'SPARE_A7', '6': 'SPARE_A8', '7': 'SPARE_D44',
       '8': 'SPARE_D45', '9': 'SPARE_D46', '10': 'SPARE_D47'}, descr='Unused 2560 pins: ADC5-8, D44-D47 (PWM capable)')
r_('10k', '+5V', 'SENS_RX1', 'USART1 RX idle high (keepalive now arrives on CAN; test point for a wired return)')
# CAN
place(CANC, 'U3', 'MCP2515-I/SO', 'Package_SO:SOIC-18W_7.5x11.6mm_P1.27mm',
      {'1': 'SNS_TXCAN', '2': 'SNS_RXCAN', '3': None, '4': None, '5': None, '6': None, '7': 'CAN_OSC2', '8': 'CAN_OSC1',
       '9': 'GND', '10': None, '11': None, '12': 'CAN_INT', '13': 'SCK', '14': 'MOSI', '15': 'MISO', '16': 'CAN_CS',
       '17': 'RESET', '18': '+5V'}, descr='Sensor pack CAN controller (node 0x04)')
c_('100n', '+5V', 'GND', 'U3')
r_('10k', '+5V', 'CAN_CS', 'Keeps U3 off the SPI bus while the 2560 is being programmed')
r_('10k', '+5V', 'CAN_INT', 'CAN_INT pull-up')
place(XTAL, 'Y2', '16MHz', FP_XTAL, {'1': 'CAN_OSC1', '2': 'GND', '3': 'CAN_OSC2', '4': 'GND'}, descr='MCP2515 clock, own crystal')
c_('18p', 'CAN_OSC1', 'GND', 'Y2 load')
c_('18p', 'CAN_OSC2', 'GND', 'Y2 load')
place(XCVR, 'U6', 'MCP2562-E/SN', FP_SOIC8,
      {'1': 'SNS_TXCAN', '2': 'GND', '3': '+5V', '4': 'SNS_RXCAN', '5': '+5V', '6': 'CANL', '7': 'CANH', '8': 'GND'},
      descr='Sensor pack CAN transceiver, VIO 5 V, no termination')
c_('100n', '+5V', 'GND', 'U6 VDD / VIO')
# FRAM
place(FRAM, 'U7', 'MB85RC256V', FP_SOIC8,
      {'1': 'GND', '2': 'GND', '3': 'GND', '4': 'GND', '5': 'I2C_SDA', '6': 'I2C_SCL', '7': 'GND', '8': '+5V'},
      descr='32 KB I2C FRAM, address 0x50: odometer (A / B slots with CRC-8)')
c_('100n', '+5V', 'GND', 'U7')
r_('4.7k', '+5V', 'I2C_SDA', 'TWI pull-up')
r_('4.7k', '+5V', 'I2C_SCL', 'TWI pull-up')
# sensor frame fan-out (fallback): one divider per screen, 5 V -> 3.3 V
text(cur[0].x0 + 300, cur[0].y0 + 6, 'Sensor UART fallback: USART1 TX -> 10k / 20k divider per screen (5 V -> 3.3 V) -> pin 15 of J5 / J1 / J4.', 1.6)
for side in ('L', 'C', 'R'):
    r_('10k', 'SENS_TX1', 'SENS_TX_' + side, 'Divider top, %s screen' % side)
    r_('20k', 'SENS_TX_' + side, 'GND', 'Divider bottom, %s screen' % side)
# battery voltage
r_('100k', '12V_BATT', 'BATT_SENSE', 'Battery sense top (16 V -> 2.9 V)')
r_('22k', 'BATT_SENSE', 'GND', 'Battery sense bottom')
c_('100n', 'BATT_SENSE', 'GND', 'Battery sense filter')

# ----------------------------------------------------------------------------- sensor inputs
cur[0] = Block('sensors', 2, 'SENSOR INPUTS (as rev 1, SMD): J3 senders, J10 speed / tach, ignition, alternator L, RPM select')
place(HDR[8], 'J3', 'Sensors', 'Connector_JST:JST_PH_S8B-PH-K_1x08_P2.00mm_Horizontal',
      {'1': 'FUEL', '2': 'ENGINE_TEMP', '3': 'ENGINE_TEMP_VIN', '4': 'ALT_L_IN', '5': 'AMB_TEMP',
       '6': 'OIL_PRESSURE_VIN', '7': 'OIL_PRESSURE', '8': 'GND'}, descr='Same pinout as rev 1')
ptc('PTC 0.1A', '+5V', 'FUEL_VIN', 'Fuel sender feed (TH1 on rev 1)', FP_PTC_S)
r_('470', 'FUEL_VIN', 'FUEL', 'Fuel sender pull-up')
r_('1k', 'FUEL_VIN', 'FUEL_MON', 'Fuel feed monitor')
r_('10k', 'FUEL_MON', 'GND', 'Monitor pull-down')
ptc('PTC 0.1A', '+5V', 'ENGINE_TEMP_VIN', 'Temperature sender feed (TH2)', FP_PTC_S)
r_('1k', 'ENGINE_TEMP_VIN', 'TEMP_MON', 'Temperature feed monitor')
r_('10k', 'TEMP_MON', 'GND', 'Monitor pull-down')
r_('100k', 'ENGINE_TEMP', 'GND', 'Temperature pull-down')
ptc('PTC 0.1A', '+5V', 'OIL_PRESSURE_VIN', 'Oil pressure sender feed (TH3)', FP_PTC_S)
r_('1k', 'OIL_PRESSURE_VIN', 'PRESS_MON', 'Oil feed monitor')
r_('10k', 'PRESS_MON', 'GND', 'Monitor pull-down')
r_('1k', 'OIL_PRESSURE', 'GND', 'Oil pressure pull-down')
r_('100k', 'AMB_TEMP', 'GND', 'Ambient (100k NTC) pull-down')
for net in ('FUEL', 'ENGINE_TEMP', 'OIL_PRESSURE', 'AMB_TEMP'):
    r_('1k', net, net + '_ADC', 'ADC series (protection)')
    c_('10n', net + '_ADC', 'GND', 'ADC filter')
r_('22k', 'ALT_L_IN', 'ALT_L', 'Alternator L divider top')
r_('10k', 'ALT_L', 'GND', 'Alternator L divider bottom')
c_('100n', 'ALT_L', 'GND', 'Alternator L filter')
r_('22k', 'IGN_IN', 'IGN_SENSE', 'Ignition sense divider top')
r_('10k', 'IGN_SENSE', 'GND', 'Ignition sense divider bottom')
c_('100n', 'IGN_SENSE', 'GND', 'Ignition sense filter')
r_('10k', '+5V', 'VOLT_PWR_MON', 'Charge channel has no supply of its own: monitor held high (as rev 1)')
place(HDR[4], 'J10', 'Speed Tach', FP_PH4, {'1': 'SPEED', '2': 'TACH', '3': 'SPEED_VIN', '4': 'GND'}, descr='Same pinout as rev 1')
ptc('PTC 0.1A', '+5V', 'SPEED_VIN', 'Hall sensor feed (TH4)', FP_PTC_S)
r_('1k', 'SPEED_VIN', 'GND', 'Hall feed load (as rev 1)')
r_('10k', '+5V', 'SPEED', 'Hall output pull-up (open collector)')
r_('1k', 'SPEED', 'SPEED_SIG', 'Speed input series (new in rev 2)')
place(BAT54S, nid('D'), 'BAT54S', FP_SOT23, {'1': 'GND', '2': '+5V', '3': 'SPEED_SIG'}, descr='Speed input clamp (new in rev 2)')
c_('1n', 'SPEED_SIG', 'GND', 'Speed input filter')
r_('4.7k', 'TACH', 'TACH_R', 'Coil input, 1206 (two in series for the voltage)', FP_R1206)
r_('4.7k', 'TACH_R', 'TACH_LED', 'Coil input, 1206', FP_R1206)
place(OPTO, 'U4', 'PC817', 'Package_DIP:SMDIP-4_W7.62mm', {'1': 'TACH_LED', '2': 'GND', '3': 'GND', '4': 'TACH_OC'},
      descr='Tach isolation')
place(DD, nid('D'), '1N4148W', FP_SOD123, {'1': 'TACH_LED', '2': 'GND'}, descr='Reverse clamp across the LED (coil kickback; new in rev 2)')
r_('10k', '+5V', 'TACH_OC', 'Optocoupler pull-up')
r_('1k', 'TACH_OC', 'TACH_SIG', 'Tach input series')
place(HDR2[2], 'J8', 'RPM_SEL', 'Connector_PinHeader_2.54mm:PinHeader_2x02_P2.54mm_Vertical',
      {'1': 'GND', '2': 'RPM_SEL_A', '3': 'GND', '4': 'RPM_SEL_B'}, descr='RPM select jumpers (as rev 1, no longer shared with ISP)')

# ----------------------------------------------------------------------------- screens and CAN
cur[0] = Block('screens', 1, 'SCREENS: J5 left, J1 center, J4 right (ESP32-P4, plugged in face down). One MCP2562 each '
               '(VIO from the board 3.3 V). 120 R at the left (R) and right (bridged jumper) ends of the bus.')
HDR_NETS = {
    'J1': {'1': '+5V', '3': '+5V', '5': 'GND', '8': 'V_D3', '10': 'GND', '11': 'V_D2', '12': 'V_D1', '13': 'GND',
           '14': 'V_D0', '15': 'SENS_TX_C', '16': 'V_D7', '20': 'SER_RX', '22': 'SER_TX', '23': 'CAN_RX_C',
           '24': 'V_VS', '25': 'V_PCLK', '26': 'GND', '27': 'REV_TRIGGER', '28': 'Illumination', '29': 'GND',
           '30': 'V_HS', '31': 'CAN_TX_C', '32': 'V_D6', '33': 'GND', '34': 'CL_SCL', '35': 'V_D5', '36': 'V_P17',
           '37': 'V_FID', '38': 'CL_SDA', '39': 'V_D4', '40': 'GND'},
    'J4': {'1': '+5V', '3': '+5V', '5': 'GND', '10': 'GND', '13': 'GND', '15': 'SENS_TX_R', '17': 'POS_R', '18': '3V3_R',
           '23': 'CAN_RX_R', '28': 'Illumination', '31': 'CAN_TX_R', '34': 'CL_SCL', '38': 'CL_SDA'},
    'J5': {'1': '+5V', '3': '+5V', '5': 'GND', '10': 'GND', '13': 'GND', '15': 'SENS_TX_L', '17': 'POS_L', '19': 'GND',
           '23': 'CAN_RX_L', '26': 'GND', '28': 'Illumination', '29': 'GND', '31': 'CAN_TX_L', '33': 'GND', '34': 'CL_SCL',
           '38': 'CL_SDA', '40': 'GND'},
}
for ref, name in (('J5', 'Left Screen'), ('J1', 'Center Screen - Master'), ('J4', 'Right Screen')):
    nets = {str(n): HDR_NETS[ref].get(str(n)) for n in range(1, 41)}
    place(HDR2[20], ref, name, 'Connector_PinHeader_2.54mm:PinHeader_2x20_P2.54mm_Vertical', nets,
          descr='Waveshare ESP32-P4 round display, face down; same pin use as rev 1. Pin 18 (the screen\'s 3V3) is not tied to the board 3.3 V')
r_('1k', 'POS_L', 'GND', 'Left position strap (R37 on rev 1)')
r_('1k', 'POS_R', '3V3_R', 'Right position strap, to the right screen\'s own 3V3 (R38 on rev 1)')
for side, ref in (('L', 'U5'), ('C', 'U9'), ('R', 'U10')):
    place(XCVR, ref, 'MCP2562-E/SN', FP_SOIC8,
          {'1': 'CAN_TX_' + side, '2': 'GND', '3': '+5V', '4': 'CAN_RX_' + side, '5': '+3V3', '6': 'CANL', '7': 'CANH', '8': 'GND'},
          descr='%s screen CAN transceiver' % {'L': 'Left', 'C': 'Center', 'R': 'Right'}[side])
    c_('100n', '+5V', 'GND', ref + ' VDD')
    c_('100n', '+3V3', 'GND', ref + ' VIO')
    r_('10k', '+3V3', 'CAN_TX_' + side, 'TXD pull-up (recessive while the screen boots)')
r_('120', 'CANH', 'CANL', 'Bus termination, left end (R35 on rev 1)')
r_('120', 'CANH', 'CAN_TERM_R', 'Bus termination, right end (R36 on rev 1), through switch SW1-1')
place(DIPSW, 'SW1', 'TERM / DEMO', FP_DIPSW, {'1': 'CAN_TERM_R', '4': 'CANL', '2': 'DEMO', '3': '+5V'},
      descr='Back-side slide switch. 1 TERM: right-end 120 R, ON at the factory, OFF when a trunk cable carries the bus on (fit 120 R at the far end). 2 DEMO: ON makes the sensor pack send a fake sweep (bench only)')
place(NUP, 'D2', 'NUP2105L', FP_SOT23, {'1': 'CANH', '2': 'CANL', '3': 'GND'}, descr='Bus ESD clamp')
place(HDR[3], 'J16', 'CAN debug', 'Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical',
      {'1': 'CANH', '2': 'CANL', '3': 'GND'}, descr='Tap for a USB-CAN adapter')
r_('4.7k', '+3V3', 'CL_SDA', 'Cluster I2C pull-up (R15)')
r_('4.7k', '+3V3', 'CL_SCL', 'Cluster I2C pull-up (R16)')
# vehicle inputs to the screens
text(cur[0].x0 + 300, cur[0].y0 + 6, 'Vehicle inputs to the screens (as rev 1): illumination (J7 pin 6), reverse (J9 pin 1). J6: serial port (center UART2).', 1.6)
r_('10k', 'ILLUM_IN', 'Q1_B', 'Illumination base')
r_('10k', 'Q1_B', 'GND', 'Illumination base pull-down')
npn('Q1_B', 'GND', 'Illumination', 'Illumination: low = dim (GPIO24 on each screen, internal pull-up)', ref='Q1')
c_('100n', 'Illumination', 'GND', 'Illumination filter')
place(HDR[4], 'J9', 'Backup Camera', FP_PH4, {'1': 'REV_IN', '2': 'CAM_12V', '3': None, '4': 'GND'},
      descr='1 reverse lamp 12 V, 2 camera power (switched, fused), 3 spare, 4 ground. Video: J11')
ptc('PTC 0.5A', 'SW12', 'CAM_12V', 'Camera 12 V')
r_('10k', 'REV_IN', 'Q3_B', 'Reverse trigger base')
r_('10k', 'Q3_B', 'GND', 'Reverse trigger base pull-down')
npn('Q3_B', 'GND', 'REV_TRIGGER', 'Reverse: low = in reverse (center GPIO25)', ref='Q3')
c_('100n', 'REV_TRIGGER', 'GND', 'Reverse filter')
place(HDR[4], 'J6', 'Serial', FP_PH4, {'1': '+3V3', '2': 'SER_TX', '3': 'SER_RX', '4': 'GND'},
      descr='Serial port: the center screen\'s UART2, 3.3 V logic. 1 3V3, 2 TX (out of the dash: to the device\'s RX), '
            '3 RX (into the dash: from the device\'s TX), 4 GND. Same pins as rev 1\'s GPS connector')

# ----------------------------------------------------------------------------- video
cur[0] = Block('video', 3, 'VIDEO: composite (J11) -> TVP5150AM1 -> 8-bit BT.656 to the center\'s camera pins, '
               'same J1 pins as rev 1\'s module. I2C on the cluster bus, address 0x5C (YOUT7 pulled down). TI SLES209E figure 6-1.')
place(COAX, 'J11', 'Video in', 'DigiDash2:RCA_RCJ-02x_FaceMount', {'1': 'VIDEO_IN', '2': 'GND'},
      descr='Composite video from the backup camera (RCA, Same Sky RCJ-02x)')
place(DZ, nid('D'), 'PESD5V0S1BL', 'Diode_SMD:D_SOD-323', {'1': 'VIDEO_IN', '2': 'GND'}, descr='Video input ESD')
r_('37.4', 'VIDEO_IN', 'VID_ATT', '75 R termination and 1/2 attenuation (R1)')
r_('37.4', 'VID_ATT', 'GND', '75 R termination and 1/2 attenuation (R2)')
c_('100n', 'VID_ATT', 'VID_AIP1A', 'Input coupling (C4)')
c_('100n', 'VID_AIP1B', 'GND', 'Unused input to ground (C5)')
place(TVP, 'U8', 'TVP5150AM1IPBS', 'Package_QFP:TQFP-32_5x5mm_P0.5mm', {
    '1': 'VID_AIP1A', '2': 'VID_AIP1B', '5': 'VID_XTAL1', '6': 'VID_XTAL2', '8': 'V_P17', '28': 'VID_PDN',
    '29': 'VID_REFP', '30': 'VID_REFM', '4': '+1V8A', '32': '+1V8A', '20': '+1V8', '10': '+3V3',
    '3': 'GND', '7': 'GND', '31': 'GND', '19': 'GND',
    '18': 'V_D0', '17': 'V_D1', '16': 'V_D2', '15': 'V_D3', '14': 'V_D4', '13': 'V_D5', '12': 'V_D6', '11': 'V_D7',
    '9': 'VID_SCLK', '25': 'V_HS', '24': 'V_VS', '23': 'V_FID', '26': None, '27': 'VID_INTREQ',
    '21': 'CL_SCL', '22': 'CL_SDA'}, descr='Video decoder')
r_('22', 'VID_SCLK', 'V_PCLK', 'Pixel clock series (edge damping)')
place(XTAL, 'Y3', '14.31818MHz', FP_XTAL, {'1': 'VID_XTAL1', '2': 'GND', '3': 'VID_XTAL2', '4': 'GND'},
      descr='14.31818 MHz, +/-50 ppm, fundamental')
c_('18p', 'VID_XTAL1', 'GND', 'Y3 load (2 CL - Cstray)')
c_('18p', 'VID_XTAL2', 'GND', 'Y3 load')
r_('100k', 'VID_XTAL1', 'VID_XTAL2', 'Across the crystal (TI note E)')
c_('1u', 'VID_REFP', 'GND', 'REFP (C1)')
c_('1u', 'VID_REFM', 'GND', 'REFM (C2)')
c_('1u', 'VID_REFP', 'VID_REFM', 'REFP-REFM (C3)')
r_('10k', '+3V3', 'VID_PDN', 'PDN high: always on')
r_('10k', 'VID_INTREQ', 'GND', 'INTREQ pull-down (required when disabled)')
r_('10k', 'V_D7', 'GND', 'YOUT7 / I2CSEL low: address 0xB8 (7-bit 0x5C)')
r_('100k', '+3V3', 'V_P17', 'RESETB pull-up; the center can pull it low on GPIO52 (J1 pin 36)')
c_('1u', 'V_P17', 'GND', 'RESETB delay (> 20 ms after the supplies)')
for net in ('+1V8A', '+1V8A', '+1V8', '+3V3'):
    c_('100n', net, 'GND', 'U8 decoupling')
c_('10u', '+1V8', 'GND', 'U8 digital bulk')

# ----------------------------------------------------------------------------- bays, cable ports, trunk
cur[0] = Block('ports', 3, 'CABLE PORTS J17, J22, J23, J24 (JST PH). Trunk port J25 (bus extension, 12 V).')
for i, ref in enumerate(('J17', 'J22', 'J23', 'J24')):
    p = 'C%d' % (i + 1)
    place(HDR[4], ref, 'Cable port %d' % (i + 1), FP_PH4, {'1': p + '_5V', '2': 'CANH', '3': 'CANL', '4': 'GND'},
          descr='Cable port: 1 +5 V, 2 CANH, 3 CANL, 4 GND (a short branch: under 30 cm)')
    ptc('PTC 0.5A', '+5V', p + '_5V', ref + ' 5 V')
place(HDR[4], 'J25', 'Trunk port', 'Connector_Molex:Molex_Micro-Fit_3.0_43650-0400_1x04_P3.00mm_Horizontal',
      {'1': 'CANH', '2': 'CANL', '3': 'TRUNK_12V', '4': 'GND'},
      descr='Bus extension to the rear of the car: 1 CANH, 2 CANL (twisted pair), 3 switched 12 V (1.1 A), 4 GND. Switch SW1-1 off and terminate at the far end')
ptc('PTC 1.1A', 'SW12', 'TRUNK_12V', 'Trunk 12 V')
place(NUP, 'D3', 'NUP2105L', FP_SOT23, {'1': 'CANH', '2': 'CANL', '3': 'GND'}, descr='ESD clamp at the trunk port')

# ----------------------------------------------------------------------------- card slots
cur[0] = Block('slots', 3, 'SLOTS: six 2x15 sockets J31-J36 (slot 1-6), cards stand on edge (docs/hardware/accessory_ports.md). '
               'Per slot: fused 5 V, switched 12 V, 3.3 V, constant 12 V, an ID resistor. Shared: CAN, WAKE, LAMP / REV levels, '
               'the analog audio bus, AUX 12 V, and the I2S rail (BCK, WS, MIC, RET, MCLK: bused to all six slots, the audio card is the clock master).')
SLOT_ID = ('1k', '3.3k', '6.8k', '12k', '22k', '47k')       # against the card's 10k pull-up to its 3.3 V
for i in range(6):
    n, s = i + 1, 'S%d' % (i + 1)
    place(HDR2[15], 'J%d' % (31 + i), 'Slot %d' % n, FP_SLOT,
          {'1': s + '_5V', '2': s + '_5V', '3': 'GND', '4': 'GND', '5': 'CANH', '6': 'CANL', '7': 'GND', '8': s + '_12V',
           '9': s + '_3V3', '10': s + '_BATT', '11': s + '_ID', '12': 'WAKE', '13': 'LAMP_LVL', '14': 'REV_LVL',
           '15': 'I2S_BCK', '16': 'I2S_WS', '17': 'I2S_MIC', '18': 'I2S_RET', '19': 'GND', '20': 'GND',
           '21': 'AUDIO_L', '22': 'AUDIO_R', '23': 'AUDIO_GND', '24': 'I2S_MCLK',
           '25': 'AUX12', '26': 'AUX12', '27': 'AUX12', '28': 'AUX_GND', '29': 'AUX_GND', '30': 'AUX_GND'},
          descr='Card slot %d: 2x15 female socket, 2.54 mm, vertical (Sullins PPTC152LFBN-RC). The card carries a right-angle '
                '2x15 male header on its bottom edge' % n)
    ptc('PTC 0.5A', '+5V', s + '_5V', 'Slot %d 5 V' % n)
    ptc('PTC 0.5A', 'SW12', s + '_12V', 'Slot %d switched 12 V' % n)
    ptc('PTC 0.1A', '+3V3', s + '_3V3', 'Slot %d 3.3 V' % n, FP_PTC_S)
    ptc('PTC 0.2A', '12V_BATT', s + '_BATT', 'Slot %d constant 12 V (key-off functions)' % n, FP_PTC_S)
    r_(SLOT_ID[i], s + '_ID', 'GND', 'Slot %d ID: the card reads it against its own 10k pull-up to 3.3 V' % n)
text(cur[0].x0 + 300, cur[0].y0 + 6, 'WAKE: a card drives it high (3.3 V, through its own diode) to start the 5 V rail with the key off; '
     'the 2560 reads it and can take over with PWR_HOLD. LAMP_LVL / REV_LVL: J7 pin 6 and J9 pin 1 scaled to 0-3.3 V.', 1.6)
r_('100k', 'WAKE', 'GND', 'WAKE idle low (cards only ever drive it high)')
place(BAT54S, nid('D'), 'BAT54S', FP_SOT23, {'1': 'WAKE', '2': None, '3': 'WAKE_K'},
      descr='Wake diode (pin 1 to pin 3): WAKE high lifts the buck EN, like PWR_HOLD; the second diode is unused')
r_('47k', 'WAKE_K', 'BUCK_EN', 'Wake resistor (3.3 V in gives EN about 2 V; it starts above 1.2 V)')
r_('47k', 'WAKE', 'WAKE_SENSE', 'WAKE into the 2560 (series: the 2560 may be unpowered)')
for src, dst, what in (('ILLUM_IN', 'LAMP_LVL', 'Dash-light level (rheostat voltage) for the cards'),
                       ('REV_IN', 'REV_LVL', 'Reverse level for the cards (high = in reverse)')):
    r_('100k', src, dst, what + ': divider top (14.5 V -> 3.1 V)')
    r_('27k', dst, 'GND', what + ': divider bottom')
    c_('100n', dst, 'GND', what + ': filter')
    place(BAT54S, nid('D'), 'BAT54S', FP_SOT23, {'1': 'GND', '2': '+3V3', '3': dst}, descr=what + ': clamp')
place(NETTIE, 'NT2', 'NetTie', 'NetTie:NetTie-2_SMD_Pad2.0mm', {'1': 'GND', '2': 'AUDIO_GND'},
      descr='The only joint between the slots\' audio-bus ground and the logic ground')

# ----------------------------------------------------------------------------- write
for b in blocks:
    col_y[b.col] = max(col_y[b.col], b.y + b.row_h + 25)
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "User" 1820 %d)' % (max(col_y.values()) + 60),
       '  (title_block (title "DigiDash main board") (date "2026-09-30") (rev "2")',
       '    (comment 1 "ATmega2560 sensor pack on CAN, TVP5150AM1, six card slots, AUX 12 V")',
       '    (comment 2 "docs/hardware/digidash_rev2_plan.md"))',
       '  (lib_symbols']
for t in lib_text.values():
    out.append('\n'.join('    ' + ln for ln in t.split('\n')))
out.append('  )')
out += body
out += ['  (sheet_instances (path "/" (page "1")))', ')']
open(OUT, 'w').write('\n'.join(out) + '\n')
json.dump(refs_info, open(os.path.join(HERE, 'refs.json'), 'w'), indent=1, sort_keys=True)
print('wrote', os.path.normpath(OUT), len(body), 'items', len(refs_info), 'parts')
