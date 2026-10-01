#!/usr/bin/env python3
"""Generate AudioCarrier.kicad_sch (KiCad 7 file format, opens in 7/8/9/10).

Needs the stock KiCad symbol libraries (KICAD_SYMBOLS, default /usr/share/kicad/symbols).
Every connection is a net label or power symbol on a short stub at the pin, so the file is
a netlist you can read; move things around in eeschema as you like.
"""
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, grab_symbol  # noqa: E402

SYMDIR = os.environ.get('KICAD_SYMBOLS', next((d for d in ('/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols',
                                                               '/usr/share/kicad/symbols') if os.path.isdir(d)), ''))
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'AudioCarrier.kicad_sch')
NS = uuid.UUID('7d0c6a52-3b0e-4d0e-9b5e-a0d10c4a2c01')
SHEET = str(uuid.uuid5(NS, 'sheet'))
_n = 0


def uid(tag=''):
    global _n
    _n += 1
    return str(uuid.uuid5(NS, '%s#%d' % (tag, _n)))


def f(v):
    return ('%.4f' % v).rstrip('0').rstrip('.')


# ----------------------------------------------------------------------------- symbols
lib_text = {}   # lib_id -> text (top-level name prefixed)
lib_pins = {}   # lib_id -> {number: (x, y, angle, name, type)}
lib_props = {}  # lib_id -> {prop: (x, y)}


def register(lib_id, text):
    lib_text[lib_id] = text
    t = parse(text)
    pins = {}
    props = {}

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
    lib_pins[lib_id] = pins
    lib_props[lib_id] = props


def stock(lib, name):
    txt = grab_symbol(os.path.join(SYMDIR, lib + '.kicad_sym'), name)
    lib_id = '%s:%s' % (lib, name)
    txt = txt.strip()
    txt = re.sub(r'^\(symbol "%s"' % re.escape(name), '(symbol "%s"' % lib_id, txt, count=1)
    assert txt.startswith('(symbol "%s"' % lib_id), lib_id
    register(lib_id, txt)
    return lib_id


def stock_as(lib, name, new_id, value):
    """Use a stock symbol under another lib id / value (also for symbols that extend another one)."""
    txt = grab_symbol(os.path.join(SYMDIR, lib + '.kicad_sym'), name).strip()
    short = new_id.split(':')[1]
    txt = txt.replace('(symbol "%s"' % name, '(symbol "%s"' % new_id, 1)
    txt = txt.replace('"%s_' % name, '"%s_' % short)
    txt = re.sub(r'(\(property "Value" )"[^"]*"', r'\1"%s"' % value, txt, count=1)
    register(new_id, txt)
    return new_id


def box_symbol(lib_id, ref, value, left, right, w=10.16, footprint='', descr=''):
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
    for k, (num, nm, typ) in enumerate(left):
        y = top - 2.54 - k * 2.54
        o.append('    (pin %s line (at %s %s 0) (length 2.54) (name "%s" (effects (font (size 1.27 1.27)))) '
                 '(number "%s" (effects (font (size 1.27 1.27)))))' % (typ, f(-w / 2 - 2.54), f(y), nm, num))
    for k, (num, nm, typ) in enumerate(right):
        y = top - 2.54 - k * 2.54
        o.append('    (pin %s line (at %s %s 180) (length 2.54) (name "%s" (effects (font (size 1.27 1.27)))) '
                 '(number "%s" (effects (font (size 1.27 1.27)))))' % (typ, f(w / 2 + 2.54), f(y), nm, num))
    o += ['  )', ')']
    register(lib_id, '\n'.join(o))
    return lib_id


# ----------------------------------------------------------------------------- placement
body = []
pwr_n = [0]
refs_seen = set()
refs_info = {}
cur_group = ['misc']


def G(name):
    cur_group[0] = name
drawn = set()


def stub(x, y, angle_toward_body, net, sheet_pins=None):
    """Draw a 2.54 stub outward from a pin end and end it with a label / power symbol."""
    out = (angle_toward_body + 180) % 360
    dx, dy = {0: (2.54, 0), 180: (-2.54, 0), 90: (0, -2.54), 270: (0, 2.54)}[out]
    # KiCad symbol angle: 0 = pin points right (from connection point to body), 90 = up (y up in lib).
    # Outward on the sheet (y down): out 0 -> +x, 180 -> -x, 90 (up in lib) -> -y, 270 -> +y.
    x2, y2 = x + dx, y + dy
    body.append('  (wire (pts (xy %s %s) (xy %s %s)) (stroke (width 0) (type default)) (uuid "%s"))' % (
        f(x), f(y), f(x2), f(y2), uid('w')))
    if net in ('GND', '+5V', '+3V3'):
        power(net, x2, y2, out)
    else:
        rot, just = (180, 'right') if out == 180 else (0, 'left')
        body.append('  (label "%s" (at %s %s %d) (effects (font (size 1.27 1.27)) (justify %s bottom)) (uuid "%s"))' % (
            net, f(x2), f(y2), rot, just, uid('l')))


POWER = {'GND': ('power:GND', 'down'), '+5V': ('power:+5V', 'up'), '+3V3': ('power:+3V3', 'up')}


def power(net, x, y, outward=None):
    lib_id, nat = POWER[net]
    rot = 0
    if outward == 90 and nat == 'down':      # stub leaves upward, symbol would hang over it
        rot = 180
    if outward == 270 and nat == 'up':
        rot = 180
    pwr_n[0] += 1
    ref = '#PWR%02d' % pwr_n[0]
    body.append('''  (symbol (lib_id "%s") (at %s %s %d) (unit 1) (in_bom no) (on_board no) (dnp no) (uuid "%s")
    (property "Reference" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))
    (property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))
    (property "Datasheet" "" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))
    (pin "1" (uuid "%s"))
    (instances (project "AudioCarrier" (path "/%s" (reference "%s") (unit 1))))
  )''' % (lib_id, f(x), f(y), rot, uid('p'), ref, f(x), f(y), net, f(x + 2.54), f(y), f(x), f(y), f(x), f(y), uid('pp'), SHEET, ref))


def place(lib_id, ref, value, footprint, x, y, nets, descr=None, show_value=True, unit=1, first=True):
    """nets: {pin number: net name | None (unconnected -> no-connect flag)}; every pin must be listed or be omitted on purpose."""
    if first:
        assert ref not in refs_seen, ref
        refs_seen.add(ref)
        refs_info[ref] = {'group': cur_group[0], 'fp': footprint, 'value': value}
    pins = {k: v for k, v in lib_pins[lib_id].items() if v[5] in (0, unit)}
    props = lib_props[lib_id]

    def pos(k, dflt):
        px, py = props.get(k, dflt)
        return f(x + px), f(y - py)

    rp, vp = pos('Reference', (0, 3)), pos('Value', (0, -3))
    lines = ['  (symbol (lib_id "%s") (at %s %s 0) (unit %d) (in_bom yes) (on_board yes) (dnp no) (uuid "%s")' % (
        lib_id, f(x), f(y), unit, uid('s' + ref + str(unit))),
        '    (property "Reference" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))' % (ref, rp[0], rp[1]),
        '    (property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)%s))' % (
            value, vp[0], vp[1], '' if show_value else ' hide'),
        '    (property "Footprint" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (footprint, f(x), f(y)),
        '    (property "Datasheet" "~" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (f(x), f(y))]
    if descr:
        lines.append('    (property "Description" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (descr, f(x), f(y)))
    for num in pins:
        lines.append('    (pin "%s" (uuid "%s"))' % (num, uid('pin' + ref + str(unit) + num)))
    lines.append('    (instances (project "AudioCarrier" (path "/%s" (reference "%s") (unit %d))))' % (SHEET, ref, unit))
    lines.append('  )')
    body.append('\n'.join(lines))
    for num, net in nets.items():
        px, py, ang, _, _, _ = pins[num]
        sx, sy = x + px, y - py
        if (round(sx, 2), round(sy, 2)) in drawn:
            continue
        drawn.add((round(sx, 2), round(sy, 2)))
        if net is None:
            body.append('  (no_connect (at %s %s) (uuid "%s"))' % (f(sx), f(sy), uid('nc')))
        else:
            stub(sx, sy, ang, net)
    missing = set(pins) - set(nets)
    assert not missing, (ref, unit, missing)


def text(x, y, s, size=1.6):
    s = s.replace('"', "'").replace('\n', '\\n')
    body.append('  (text "%s" (at %s %s 0) (effects (font (size %s %s)) (justify left top)) (uuid "%s"))' % (
        s, f(x), f(y), f(size), f(size), uid('t')))


# ----------------------------------------------------------------------------- parts
R = stock('Device', 'R')
C = stock('Device', 'C')
CP = stock('Device', 'C_Polarized')
FB = stock('Device', 'FerriteBead')
JACK = box_symbol('AudioCarrier:TS3328', 'J', 'TS3328 3.5mm jack',
                  [('2', 'PIN_2', 'passive'), ('3', 'PIN_3', 'passive'), ('4', 'PIN_4', 'passive'), ('5', 'PIN_5', 'passive')], [],
                  w=12.7, footprint='AudioCarrier:Jack_3.5mm_TS3328_Horizontal',
                  descr='Tinkersphere TS3328 3.5 mm jack (right angle, 5 flat pins). Pin 4 has two tabs. The pin functions are from the vendor drawing and NOT verified: see JACK_MAP in gen_sch.py')
RCA4 = box_symbol('AudioCarrier:PJRAS2X2', 'J', 'PJRAS2X2S01X',
                  [('1', 'TIP_FL_white', 'passive'), ('2', 'TIP_FR_red', 'passive'), ('3', 'TIP_RL_white', 'passive'),
                   ('4', 'TIP_RR_red', 'passive')],
                  [('5', 'SLEEVE_L', 'passive'), ('6', 'SLEEVE_R', 'passive'), ('7', 'PLATE_L', 'passive'), ('8', 'PLATE_R', 'passive')],
                  w=15.24, footprint='AudioCarrier:RCA_Switchcraft_PJRAS2X2_RightAngle',
                  descr='Switchcraft PJRAS2X2S01X: 2 x 2 right-angle RCA jacks, white on top, red below. Left column FL / FR, right column RL / RR (seen from the plug side)')
HDR2x15 = stock('Connector_Generic', 'Conn_02x15_Odd_Even')
BUF = stock_as('74xx', '74LS125', 'AudioCarrier:74LVC125', '74LVC125APW')
HDR1x5 = stock('Connector_Generic', 'Conn_01x05')
SJ = stock('Jumper', 'SolderJumper_2_Open')
SW = stock('Switch', 'SW_Push')
CAN = stock('Interface_CAN_LIN', 'MCP2562-E-P')
OPA = stock_as('Amplifier_Operational', 'LM2904', 'AudioCarrier:OPA2376', 'OPA2376AIDGK')
LDO = stock_as('Regulator_Linear', 'AP1117-15', 'AudioCarrier:AMS1117-3.3', 'AMS1117-3.3')
for n in ('GND', '+5V', '+3V3', 'PWR_FLAG'):
    stock('power', n)

# ESP32-PICO-MINI-02 pin numbers from the Espressif datasheet v1.7, table 3 (IO23 and IO18 are not brought out).
GND_PINS = ['1', '2', '11', '14'] + [str(n) for n in range(36, 54)]
ESP = box_symbol('AudioCarrier:ESP32-PICO-MINI-02', 'U', 'ESP32-PICO-MINI-02',
                 [('3', '3V3', 'power_in'), ('8', 'EN', 'input'), ('4', 'I36', 'input'), ('5', 'I37', 'input'),
                  ('6', 'I38', 'input'), ('7', 'I39', 'input'), ('9', 'I34', 'input'), ('10', 'I35', 'input'),
                  ('30', 'RXD0', 'bidirectional'), ('31', 'TXD0', 'bidirectional'), ('25', 'NC', 'no_connect'),
                  ('32', 'NC', 'no_connect')] + [(n, 'GND', 'power_in') for n in GND_PINS],
                 [('23', 'IO0', 'bidirectional'), ('22', 'IO2', 'bidirectional'), ('24', 'IO4', 'bidirectional'),
                  ('29', 'IO5', 'bidirectional'), ('27', 'IO7', 'bidirectional'), ('28', 'IO8', 'bidirectional'),
                  ('19', 'IO12', 'bidirectional'), ('20', 'IO13', 'bidirectional'), ('18', 'IO14', 'bidirectional'),
                  ('21', 'IO15', 'bidirectional'), ('33', 'IO19', 'bidirectional'), ('26', 'IO20', 'bidirectional'),
                  ('35', 'IO21', 'bidirectional'), ('34', 'IO22', 'bidirectional'), ('15', 'IO25', 'bidirectional'),
                  ('16', 'IO26', 'bidirectional'), ('17', 'IO27', 'bidirectional'), ('12', 'IO32', 'bidirectional'),
                  ('13', 'IO33', 'bidirectional')],
                 w=17.78, footprint='AudioCarrier:ESP32-PICO-MINI-02',
                 descr='Espressif ESP32-PICO-MINI-02 module: classic ESP32 (Bluetooth Classic for A2DP / HFP), 8 MB flash, 13.2 x 16.6 mm')

# Pin tables below are from the TI datasheets (PCM5100A/5101A/5102A SLAS859C, PCM1808 SLES177B), PW packages.
DAC = box_symbol('AudioCarrier:PCM5102A_TSSOP20', 'U', 'PCM5102APW',
                 [('1', 'CPVDD', 'power_in'), ('2', 'CAPP', 'passive'), ('3', 'CPGND', 'power_in'), ('4', 'CAPM', 'passive'),
                  ('5', 'VNEG', 'passive'), ('6', 'OUTL', 'output'), ('7', 'OUTR', 'output'), ('8', 'AVDD', 'power_in'),
                  ('9', 'AGND', 'power_in'), ('10', 'DEMP', 'input')],
                 [('20', 'DVDD', 'power_in'), ('19', 'DGND', 'power_in'), ('18', 'LDOO', 'passive'), ('17', 'XSMT', 'input'),
                  ('16', 'FMT', 'input'), ('15', 'LRCK', 'input'), ('14', 'DIN', 'input'), ('13', 'BCK', 'input'),
                  ('12', 'SCK', 'input'), ('11', 'FLT', 'input')],
                 w=15.24, footprint='Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm',
                 descr='TI PCM5102A stereo DAC, 2.1 Vrms ground-centred line out, internal PLL (SCK to ground), 20-pin TSSOP')
ADC = box_symbol('AudioCarrier:PCM1808_TSSOP14', 'U', 'PCM1808PW',
                 [('1', 'VREF', 'passive'), ('2', 'AGND', 'power_in'), ('3', 'VCC', 'power_in'), ('4', 'VDD', 'power_in'),
                  ('5', 'DGND', 'power_in'), ('6', 'SCKI', 'input'), ('7', 'LRCK', 'input')],
                 [('14', 'VINR', 'input'), ('13', 'VINL', 'input'), ('12', 'FMT', 'input'), ('11', 'MD1', 'input'),
                  ('10', 'MD0', 'input'), ('9', 'DOUT', 'output'), ('8', 'BCK', 'input')],
                 w=15.24, footprint='Package_SO:TSSOP-14_4.4x5mm_P0.65mm',
                 descr='TI PCM1808 stereo 24-bit ADC, 5 V analog / 3.3 V digital, slave mode (MD0 = MD1 = low), 14-pin TSSOP')

FP_R = 'Resistor_SMD:R_0603_1608Metric'
FP_C = 'Capacitor_SMD:C_0603_1608Metric'
FP_C22 = 'Capacitor_SMD:C_0805_2012Metric'
FP_SJ = 'Jumper:SolderJumper-2_P1.3mm_Open_Pad1.0x1.5mm'
FP_SOIC = 'Package_SO:SOIC-8_3.9x4.9mm_P1.27mm'
FP_VSSOP = 'Package_SO:VSSOP-8_3x3mm_P0.65mm'

cnt = {}


def nid(prefix):
    cnt[prefix] = cnt.get(prefix, 0) + 1
    return '%s%d' % (prefix, cnt[prefix])


X = [30]  # running x for auto layout of the schematic rows


def r_(v, a, b, x, y, d=None):
    ref = nid('R')
    place(R, ref, v, FP_R, x, y, {'1': a, '2': b}, descr=d)
    return ref


def c_(v, a, b, x, y, fp=None, d=None):
    ref = nid('C')
    place(C, ref, v, fp or (FP_C22 if v == '22u' else FP_C), x, y,
          {'1': a, '2': b}, descr=d)
    return ref


text(15, 12, 'AUDIO CARD  -  ESP32-PICO-MINI-02 + 2 x PCM5102A (4 line outputs) + PCM1808 (microphone input)', 3)
text(15, 20, 'Card for a DigiDash slot, 80 x 60 mm, standing on edge (docs/hardware/accessory_ports.md); its place is slot 1 '
     '(the RCA block is 28.5 mm tall). CAN node 0x10 (both straps open). 5 V from the slot only; 3.3 V on board.', 1.6)

# --- port
G('port')
place(HDR2x15, 'J1', 'DigiDash slot', 'Connector_PinHeader_2.54mm:PinHeader_2x15_P2.54mm_Horizontal', 45, 55,
      {'1': '+5V', '2': '+5V', '3': 'GND', '4': 'GND', '5': 'CANH', '6': 'CANL', '7': 'GND', '8': None,
       '9': None, '10': None, '11': 'SLOT_ID', '12': None, '13': None, '14': None,
       '15': 'LINK0', '16': 'LINK1', '17': 'LINK2', '18': 'LINK3', '19': 'GND', '20': 'GND',
       '21': 'AUDIO_L', '22': 'AUDIO_R', '23': 'AUDIO_GND', '24': 'LINK4',
       '25': None, '26': None, '27': None, '28': None, '29': None, '30': None},
      descr='Right-angle 2x15 header on the bottom edge (the slot pinout, docs/hardware/accessory_ports.md). Used: 5 V, GND, CAN, '
            'slot ID, LINK0-4 (I2S to the card in the partner slot), the analog audio bus. Not used: switched / constant 12 V, '
            '3.3 V, WAKE, LAMP, REV, AUX 12 V')
text(70, 40, 'Only 5 V is used: the card draws well under the slot\'s 0.5 A fuse. Slot ID (pin 11): 10k to 3.3 V here, '
     'the slot\'s own resistor to GND on the main board, read on GPIO36.', 1.4)
r_('10k', '+3V3', 'SLOT_ID', 25, 45, 'Slot ID pull-up (the main board has one resistor to GND per slot)')
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', 25, 85, {'1': '+5V'}, show_value=False)
place('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', 40, 85, {'1': 'GND'}, show_value=False)
place('power:PWR_FLAG', '#FLG03', 'PWR_FLAG', '', 55, 85, {'1': '+5VA'}, show_value=False)
place('power:PWR_FLAG', '#FLG04', 'PWR_FLAG', '', 70, 85, {'1': '+3V3A'}, show_value=False)

# --- power
G('power')
text(15, 100, 'POWER: +5 V -> AMS1117-3.3 (ESP32, digital sides). Ferrites make +5VA (PCM1808 analog, mic op-amp) and +3V3A (PCM5102A analog + charge pump).', 1.4)
c_('22u', '+5V', 'GND', 25, 125)
c_('10u', '+5V', 'GND', 40, 125)
place(LDO, 'U7', 'AMS1117-3.3', 'Package_TO_SOT_SMD:SOT-223-3_TabPin2', 75, 120, {'1': 'GND', '2': '+3V3', '3': '+5V'})
place(CP, nid('C'), '22u tant', 'Capacitor_Tantalum_SMD:CP_EIA-3216-18_Kemet-A', 105, 125, {'1': '+3V3', '2': 'GND'},
      descr='AMS1117 wants >= 22 uF with some ESR on the output')
c_('100n', '+3V3', 'GND', 120, 125)
place(FB, 'FB1', '600R @100MHz', 'Inductor_SMD:L_0603_1608Metric', 145, 125, {'1': '+5V', '2': '+5VA'})
c_('10u', '+5VA', 'GND', 165, 125)
c_('100n', '+5VA', 'GND', 180, 125)
place(FB, 'FB2', '600R @100MHz', 'Inductor_SMD:L_0603_1608Metric', 200, 125, {'1': '+3V3', '2': '+3V3A'})
c_('10u', '+3V3A', 'GND', 220, 125)

# --- ESP32
G('esp')
en = {}
sig = {'IO32': 'CAN_TXD', 'IO33': 'CAN_RX', 'IO14': 'I2S_BCLK', 'IO25': 'I2S_LRC', 'IO27': 'I2S_DOUT_F',
       'IO26': 'I2S_DOUT_R', 'I35': 'ADC_DOUT', 'IO4': 'DAC_MUTE', 'IO13': 'NODE0', 'IO22': 'NODE1',
       'I34': 'LINK_SDIN', 'I36': 'SLOT_ID', 'IO21': 'LINK_EN_N',
       'IO0': 'BOOT', 'EN': 'EN', 'TXD0': 'UART_TX', 'RXD0': 'UART_RX', '3V3': '+3V3', 'GND': 'GND'}
for num, (px, py, ang, nm, typ, u) in lib_pins[ESP].items():
    en[num] = sig.get(nm)
place(ESP, 'U1', 'ESP32-PICO-MINI-02', 'AudioCarrier:ESP32-PICO-MINI-02', 260, 215, en,
      descr='Espressif ESP32-PICO-MINI-02-N8R2 (or -N8): classic ESP32 module, antenna on the board edge')
c_('10u', '+3V3', 'GND', 230, 290, d='Module supply, at pin 3')
c_('100n', '+3V3', 'GND', 245, 290, d='Module supply, at pin 3')
text(215, 150, 'I2S0 master: BCK GPIO14, WS GPIO25, DOUT GPIO27 (front DAC), DIN GPIO35 (PCM1808), MCLK GPIO0 (12.288 MHz for the ADC).\n'
     'I2S1 slave on the same BCK / WS pins: DOUT GPIO26 (rear DAC). GPIO4 = DAC mute (XSMT), pulled low. '
     'GPIO32 / 33 CAN, GPIO13 / 22 node straps (the module has no GPIO23). GPIO12 / 2 / 15 / 5 left open (boot straps).\n'
     'Slot: GPIO36 slot ID (ADC1), GPIO34 I2S1 data in from the partner card (LINK3), GPIO21 low enables the link buffer.', 1.4)

G('boot')
text(15, 150, 'BOOT / PROGRAMMING', 1.6)
r_('10k', '+3V3', 'EN', 30, 180)
c_('1u', 'EN', 'GND', 50, 180)
place(SW, 'SW1', 'RESET', 'Button_Switch_SMD:SW_SPST_PTS810', 75, 180, {'1': 'EN', '2': 'GND'})
r_('10k', '+3V3', 'BOOT', 30, 215)
place(SW, 'SW2', 'BOOT', 'Button_Switch_SMD:SW_SPST_PTS810', 75, 215, {'1': 'BOOT', '2': 'GND'})
place(HDR1x5, 'J10', 'Program', 'Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical', 115, 200,
      {'1': 'GND', '2': 'UART_TX', '3': 'UART_RX', '4': 'BOOT', '5': 'EN'},
      descr='USB-serial adapter: GND, adapter RX <- TX0, adapter TX -> RX0, IO0, EN. 3.3 V logic. Do not power from the adapter')
text(15, 235, 'Flash: hold BOOT, tap RESET, then upload. Power the board from the port. BOOT (GPIO0) is also the ADC master clock: normal boot needs it high.', 1.4)

# --- CAN
G('can')
text(15, 250, 'CAN: MCP2562 (VDD 5 V, VIO 3.3 V, STBY low). No 120 R: the carrier never terminates the bus.', 1.4)
place(CAN, 'U2', 'MCP2562-E/SN', FP_SOIC, 55, 285, {'1': 'CAN_TXD', '2': 'GND', '3': '+5V', '4': 'CAN_RXD', '5': '+3V3',
                                                      '6': 'CANL', '7': 'CANH', '8': 'GND'})
r_('1k', 'CAN_RXD', 'CAN_RX', 95, 275, 'Series R on the receive line, as on the DigiDash board')
c_('100n', '+5V', 'GND', 115, 275)
c_('100n', '+3V3', 'GND', 130, 275)
place(SJ, 'JP1', 'Node bit 0', FP_SJ, 100, 310, {'1': 'NODE0', '2': 'GND'}, descr='Close = GPIO13 low: node 0x11 (JP2 open)')
place(SJ, 'JP2', 'Node bit 1', FP_SJ, 140, 310, {'1': 'NODE1', '2': 'GND'}, descr='Close = GPIO22 low: node 0x12 (JP1 open)')
text(15, 322, 'Node: both open = 0x10 (audio bridge), JP1 = 0x11, JP2 = 0x12.', 1.4)

# --- DACs
text(330, 30, 'PCM5102A: 3-wire I2S, SCK tied to ground (internal PLL from BCK, BCK must be 64 fS), FMT low = I2S, FLT low, DEMP low.\n'
     'XSMT is driven by GPIO4 and pulled low, so the outputs stay muted until the firmware un-mutes them (no boot pop).\n'
     'Line outputs are ground-centred 2.1 Vrms: no DC-block caps. 470 R + 2.2n at each output as in the datasheet. Both DACs share BCK / LRCK; '
     'each has its own data line (front GPIO27, rear GPIO26).', 1.4)
for n, (grp, ref, din, lo, ro, y, name) in enumerate([('dac_f', 'U5', 'I2S_DOUT_F', 'FL', 'FR', 90, 'front'),
                                                       ('dac_r', 'U6', 'I2S_DOUT_R', 'RL', 'RR', 190, 'rear')]):
    G(grp)
    place(DAC, ref, 'PCM5102APW', 'Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm', 340, y, {
        '1': '+3V3A', '2': '%s_CAPP' % ref, '3': 'GND', '4': '%s_CAPM' % ref, '5': '%s_VNEG' % ref,
        '6': '%s_OUT' % lo, '7': '%s_OUT' % ro, '8': '+3V3A', '9': 'GND', '10': 'GND',
        '20': '+3V3', '19': 'GND', '18': '%s_LDOO' % ref, '17': 'DAC_MUTE', '16': 'GND', '15': 'I2S_LRC', '14': din,
        '13': 'I2S_BCLK', '12': 'GND', '11': 'GND'}, descr='%s pair' % name)
    x = 385
    c_('2.2u', '%s_CAPP' % ref, '%s_CAPM' % ref, x, y - 20, d='Charge-pump flying cap')
    c_('2.2u', '%s_VNEG' % ref, 'GND', x + 15, y - 20, d='VNEG decoupling')
    c_('100n', '+3V3A', 'GND', x, y - 5, d='CPVDD')
    c_('10u', '+3V3A', 'GND', x + 15, y - 5, d='CPVDD / AVDD')
    c_('100n', '+3V3A', 'GND', x + 30, y - 5, d='AVDD')
    c_('100n', '+3V3', 'GND', x, y + 10, d='DVDD')
    c_('100n', '%s_LDOO' % ref, 'GND', x + 30, y + 10, d='LDOO (0.1 uF per the datasheet)')
    r_('470', '%s_OUT' % lo, '%s_LINE' % lo, x + 70, y - 12)
    c_('2.2n', '%s_LINE' % lo, 'GND', x + 90, y - 12)
    r_('470', '%s_OUT' % ro, '%s_LINE' % ro, x + 70, y + 8)
    c_('2.2n', '%s_LINE' % ro, 'GND', x + 90, y + 8)
r_('100k', 'DAC_MUTE', 'GND', 445, 60, 'Keeps both DACs muted until GPIO4 goes high')

G('rca')
place(RCA4, 'J2', 'RCA block', 'AudioCarrier:RCA_Switchcraft_PJRAS2X2_RightAngle', 500, 90,
      {'1': 'FL_LINE', '2': 'FR_LINE', '3': 'RL_LINE', '4': 'RR_LINE', '5': 'GND', '6': 'GND', '7': 'GND', '8': 'GND'},
      descr='Switchcraft PJRAS2X2S01X on the right edge: FL white / FR red (left column), RL white / RR red (right column), seen from the plug side. Front-plate tabs and sleeves to ground')
text(520, 80, 'J2: 2 x 2 RCA block (Switchcraft PJRAS2X2S01X, right angle)\nleft column FL white / FR red, right column RL white / RR red', 1.6)
G('port')

# --- microphone
G('mic')
text(30, 340, 'MICROPHONE: 3.5 mm TRS jack J6. Tip = electret mic (2.2k bias from filtered 3.3 V, DC-blocked), ring = second input (line / aux, no bias).\n'
     'Mic preamp: OPA2376 non-inverting, gain 1 + 47k / 2.2k = 22 (27 dB), DC gain 1 at mid-supply (2.5 V). Ring input is a unity-gain buffer. '
     'PCM1808 full scale is 3 Vp-p (about 1 Vrms).', 1.4)
r_('1k', '+3V3A', 'MICV', 90, 365)
c_('10u', 'MICV', 'GND', 105, 365)
r_('2.2k', 'MICV', 'MIC_TIP', 120, 365, 'Electret bias')
c_('100p', 'MIC_TIP', 'GND', 135, 365, d='RF bypass, at the jack')
# PROVISIONAL pin functions of the TS3328 (the vendor's drawing numbers the pins 2, 3, 4, 4, 5 but does not name them).
# Read from its wiring diagram, which looks 4-pole: sleeve = 5, then the springs from the deepest: tip = 2, ring 1 = 4,
# ring 2 = 3. A TRS plug's sleeve touches ring 2 and the sleeve spring, so 3 and 5 are both ground. Verify with a plug and
# a meter (continuity from the plug's tip / ring / sleeve to each pin), then edit this table and re-run.
JACK_MAP = {'2': 'MIC_TIP', '4': 'RING', '3': 'GND', '5': 'GND'}
place(JACK, 'J6', '3.5mm mic / aux in', 'AudioCarrier:Jack_3.5mm_TS3328_Horizontal', 60, 380, JACK_MAP,
      descr='Tinkersphere TS3328. Tip = mic, ring = aux in, sleeve = ground. Pin functions provisional (see JACK_MAP in gen_sch.py)')
c_('1u', 'MIC_TIP', 'MIC_IN', 150, 365)
r_('100k', 'MIC_IN', 'VMID', 165, 365)
r_('10k', '+5VA', 'VMID', 180, 380)
r_('10k', 'VMID', 'GND', 195, 380)
c_('10u', 'VMID', 'GND', 210, 380)
place(OPA, 'U3', 'OPA2376AIDGK', FP_VSSOP, 250, 380, {'3': 'MIC_IN', '2': 'MIC_FB', '1': 'MIC_OP'}, unit=1,
      descr='Dual RRIO low-noise op-amp on 5 V, VSSOP-8 (any RRIO dual with the standard pinout will do)')
r_('47k', 'MIC_OP', 'MIC_FB', 280, 365)
c_('100p', 'MIC_OP', 'MIC_FB', 295, 365, d='Roll-off, about 34 kHz')
r_('2.2k', 'MIC_FB', 'MIC_G', 310, 365)
c_('10u', 'MIC_G', 'GND', 325, 365)
c_('1u', 'MIC_OP', 'ADC_INL', 340, 380)
c_('1u', 'RING', 'RING_IN', 150, 400)
r_('100k', 'RING_IN', 'VMID', 165, 400)
place(OPA, 'U3', 'OPA2376AIDGK', FP_VSSOP, 250, 410, {'5': 'RING_IN', '6': 'RING_OP', '7': 'RING_OP'}, unit=2, first=False)
c_('1u', 'RING_OP', 'ADC_INR', 340, 410)
place(OPA, 'U3', 'OPA2376AIDGK', FP_VSSOP, 290, 410, {'4': 'GND', '8': '+5VA'}, unit=3, first=False)
c_('100n', '+5VA', 'GND', 310, 410, d='U3 decoupling, next to the pins')

G('adc')
place(ADC, 'U4', 'PCM1808PW', 'Package_SO:TSSOP-14_4.4x5mm_P0.65mm', 400, 380, {
    '1': 'ADC_VREF', '2': 'GND', '3': '+5VA', '4': '+3V3', '5': 'GND', '6': 'ADC_SCKI', '7': 'I2S_LRC',
    '8': 'I2S_BCLK', '9': 'ADC_DOUT', '10': 'GND', '11': 'GND', '12': 'GND', '13': 'ADC_INL', '14': 'ADC_INR'},
      descr='Slave mode: MD0 = MD1 = low, I2S 24-bit (FMT low). SCKI = 256 fS master clock from GPIO0')
c_('100n', 'ADC_VREF', 'GND', 440, 365, d='VREF decoupling')
c_('10u', 'ADC_VREF', 'GND', 455, 365, d='VREF decoupling')
c_('100n', '+5VA', 'GND', 470, 365, d='VCC')
c_('10u', '+5VA', 'GND', 485, 365, d='VCC')
c_('100n', '+3V3', 'GND', 500, 365, d='VDD')
r_('33', 'BOOT', 'ADC_SCKI', 440, 395, 'Series damping on the master clock (GPIO0 doubles as the BOOT strap net)')
text(430, 415, 'GPIO0 outputs the 12.288 MHz clock (256 x 48 kHz) after boot. The BOOT switch / J10 IO0 still pull it low to enter the ROM loader.', 1.4)

# --- links and the audio bus
G('link')
text(530, 300, 'LINKS (to the card in the partner slot, slot 2): a 74LVC125 drives LINK0 BCK, LINK1 WS, LINK2 the mic / aux data '
     '(PCM1808 DOUT) and LINK4 MCLK, only while GPIO21 holds its enables low (pulled high: off at reset, off until the partner '
     'is found on CAN). LINK3 (the partner\'s I2S data) comes in on GPIO34 (I2S1 data in).\n'
     'AUDIO BUS: the front pair drives AUDIO_L / R through 100 R; AUDIO_GND joins this card\'s ground through 10 R.', 1.4)
for unit, (oe, a, y, src, dst) in enumerate([('1', '2', '3', 'I2S_BCLK', 'LINK0'), ('4', '5', '6', 'I2S_LRC', 'LINK1'),
                                             ('10', '9', '8', 'ADC_DOUT', 'LINK2'), ('13', '12', '11', 'ADC_SCKI', 'LINK4')]):
    place(BUF, 'U8', '74LVC125APW', 'Package_SO:TSSOP-14_4.4x5mm_P0.65mm', 560, 330 + 18 * unit,
          {oe: 'LINK_EN_N', a: src, y: dst + '_DRV'}, unit=unit + 1, first=(unit == 0),
          descr='Quad buffer, 3-state (active-low enables): the link outputs')
    r_('33', dst + '_DRV', dst, 600, 330 + 18 * unit, 'Series damping on ' + dst)
place(BUF, 'U8', '74LVC125APW', 'Package_SO:TSSOP-14_4.4x5mm_P0.65mm', 640, 345, {'7': 'GND', '14': '+3V3'}, unit=5, first=False)
c_('100n', '+3V3', 'GND', 660, 345, d='U8 decoupling')
r_('10k', '+3V3', 'LINK_EN_N', 560, 410, 'Link buffer off until the firmware pulls GPIO21 low')
r_('33', 'LINK3', 'LINK_SDIN', 600, 410, 'LINK3 in: series')
r_('100k', 'LINK_SDIN', 'GND', 620, 410, 'GPIO34 has no pull: low when no partner card drives LINK3')
r_('100', 'FL_LINE', 'AUDIO_L', 560, 440, 'Audio bus left (front pair)')
r_('100', 'FR_LINE', 'AUDIO_R', 580, 440, 'Audio bus right (front pair)')
r_('10', 'AUDIO_GND', 'GND', 600, 440, 'Audio bus ground: tied here through 10 R (ground lift), as the bus driver')

# ----------------------------------------------------------------------------- write
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "A2")',
       '  (title_block (title "DigiDash audio card") (date "2026-10-01") (rev "F")',
       '    (comment 1 "ESP32-PICO-MINI-02 + 2 x PCM5102A + PCM1808 -> 4 RCA line outputs + 3.5 mm mic / aux input")',
       '    (comment 2 "Card for a DigiDash slot (2x15, on edge), slot 1"))',
       '  (lib_symbols']
for t in lib_text.values():
    out.append('\n'.join('    ' + ln for ln in t.split('\n')))
out.append('  )')
out += body
out += ['  (sheet_instances (path "/" (page "1")))', ')']
open(OUT, 'w').write('\n'.join(out) + '\n')
import json
json.dump(refs_info, open(os.path.join(os.path.dirname(OUT), 'generate', 'refs.json'), 'w'), indent=1, sort_keys=True)
print('wrote', os.path.normpath(OUT), len(body), 'items', len(refs_info), 'parts')
