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

SYMDIR = os.environ.get('KICAD_SYMBOLS', '/usr/share/kicad/symbols')
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

    def walk(node):
        if not isinstance(node, list) or not node:
            return
        if node[0] == 'pin' and len(node) > 3:
            at = [x for x in node if isinstance(x, list) and x[0] == 'at'][0]
            nm = [x for x in node if isinstance(x, list) and x[0] == 'name'][0][1]
            nu = [x for x in node if isinstance(x, list) and x[0] == 'number'][0][1]
            pins[nu] = (float(at[1]), float(at[2]), int(float(at[3])), nm, node[1])
        elif node[0] == 'property' and len(node) > 2:
            at = [x for x in node if isinstance(x, list) and x[0] == 'at']
            if at:
                props[node[1]] = (float(at[0][1]), float(at[0][2]))
        else:
            for c in node[1:]:
                walk(c)

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


def place(lib_id, ref, value, footprint, x, y, nets, descr=None, show_value=True):
    """nets: {pin number: net name | None (unconnected -> no-connect flag)}; every pin must be listed or be omitted on purpose."""
    assert ref not in refs_seen, ref
    refs_seen.add(ref)
    pins = lib_pins[lib_id]
    props = lib_props[lib_id]

    def pos(k, dflt):
        px, py = props.get(k, dflt)
        return f(x + px), f(y - py)

    rp, vp = pos('Reference', (0, 3)), pos('Value', (0, -3))
    lines = ['  (symbol (lib_id "%s") (at %s %s 0) (unit 1) (in_bom yes) (on_board yes) (dnp no) (uuid "%s")' % (
        lib_id, f(x), f(y), uid('s' + ref)),
        '    (property "Reference" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))' % (ref, rp[0], rp[1]),
        '    (property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)%s))' % (
            value, vp[0], vp[1], '' if show_value else ' hide'),
        '    (property "Footprint" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (footprint, f(x), f(y)),
        '    (property "Datasheet" "~" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (f(x), f(y))]
    if descr:
        lines.append('    (property "Description" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (descr, f(x), f(y)))
    for num in pins:
        lines.append('    (pin "%s" (uuid "%s"))' % (num, uid('pin' + ref + num)))
    lines.append('    (instances (project "AudioCarrier" (path "/%s" (reference "%s") (unit 1))))' % (SHEET, ref))
    lines.append('  )')
    body.append('\n'.join(lines))
    for num, net in nets.items():
        px, py, ang, _, _ = pins[num]
        sx, sy = x + px, y - py
        if net is None:
            body.append('  (no_connect (at %s %s) (uuid "%s"))' % (f(sx), f(sy), uid('nc')))
        else:
            stub(sx, sy, ang, net)
    missing = set(pins) - set(nets)
    assert not missing, (ref, missing)


def text(x, y, s, size=1.6):
    s = s.replace('"', "'").replace('\n', '\\n')
    body.append('  (text "%s" (at %s %s 0) (effects (font (size %s %s)) (justify left top)) (uuid "%s"))' % (
        s, f(x), f(y), f(size), f(size), uid('t')))


# ----------------------------------------------------------------------------- parts
R = stock('Device', 'R')
C = stock('Device', 'C')
CP = stock('Device', 'C_Polarized')
D = stock('Device', 'D_Schottky')
COAX = stock('Connector', 'Conn_Coaxial')
HDR2x5 = stock('Connector_Generic', 'Conn_02x05_Odd_Even')
HDR1x2 = stock('Connector_Generic', 'Conn_01x02')
SJ = stock('Jumper', 'SolderJumper_2_Open')
CAN = stock('Interface_CAN_LIN', 'MCP2562-E-P')
for n in ('GND', '+5V', '+3V3', 'PWR_FLAG'):
    stock('power', n)

DEVKIT_LEFT = [('1', '3V3', 'power_out'), ('2', 'EN', 'input'), ('3', 'GPIO36_SVP', 'input'),
               ('4', 'GPIO39_SVN', 'input'), ('5', 'GPIO34', 'input'), ('6', 'GPIO35', 'input'),
               ('7', 'GPIO32', 'bidirectional'), ('8', 'GPIO33', 'bidirectional'),
               ('9', 'GPIO25', 'bidirectional'), ('10', 'GPIO26', 'bidirectional'),
               ('11', 'GPIO27', 'bidirectional'), ('12', 'GPIO14', 'bidirectional'),
               ('13', 'GPIO12', 'bidirectional'), ('14', 'GND', 'power_in'),
               ('15', 'GPIO13', 'bidirectional'), ('16', 'GPIO9_SD2', 'bidirectional'),
               ('17', 'GPIO10_SD3', 'bidirectional'), ('18', 'GPIO11_CMD', 'bidirectional'),
               ('19', '5V', 'power_in')]
DEVKIT_RIGHT = [('20', 'GND', 'power_in'), ('21', 'GPIO23', 'bidirectional'), ('22', 'GPIO22', 'bidirectional'),
                ('23', 'GPIO1_TXD0', 'bidirectional'), ('24', 'GPIO3_RXD0', 'bidirectional'),
                ('25', 'GPIO21', 'bidirectional'), ('26', 'GND', 'power_in'),
                ('27', 'GPIO19', 'bidirectional'), ('28', 'GPIO18', 'bidirectional'),
                ('29', 'GPIO5', 'bidirectional'), ('30', 'GPIO17', 'bidirectional'),
                ('31', 'GPIO16', 'bidirectional'), ('32', 'GPIO4', 'bidirectional'),
                ('33', 'GPIO0', 'bidirectional'), ('34', 'GPIO2', 'bidirectional'),
                ('35', 'GPIO15', 'bidirectional'), ('36', 'GPIO8_SD1', 'bidirectional'),
                ('37', 'GPIO7_SD0', 'bidirectional'), ('38', 'GPIO6_CLK', 'bidirectional')]
DEVKIT = box_symbol('AudioCarrier:ESP32-DevKitC-32', 'U', 'ESP32-DevKitC-32', DEVKIT_LEFT, DEVKIT_RIGHT, w=22.86,
                    footprint='AudioCarrier:ESP32-DevKitC-32_Sockets',
                    descr='ESP32-DevKitC-32 (classic ESP32, 2x19 header): pin numbers 1-19 left column, 20-38 right column, USB at the bottom')
AMP = box_symbol('AudioCarrier:MAX98357A_Breakout', 'U', 'MAX98357A breakout',
                 [('1', 'LRC', 'input'), ('2', 'BCLK', 'input'), ('3', 'DIN', 'input'), ('4', 'GAIN', 'input'),
                  ('5', 'SD', 'input'), ('6', 'GND', 'power_in'), ('7', 'Vin', 'power_in')], [], w=10.16,
                 footprint='Connector_PinSocket_2.54mm:PinSocket_1x07_P2.54mm_Vertical',
                 descr='MAX98357A I2S amplifier breakout, 7-pin header (LRC BCLK DIN GAIN SD GND Vin)')

FP_R = 'Resistor_THT:R_Axial_DIN0204_L3.6mm_D1.6mm_P5.08mm_Horizontal'
FP_C = 'Capacitor_THT:C_Disc_D3.0mm_W1.6mm_P2.50mm'
FP_CP = 'Capacitor_THT:CP_Radial_D5.0mm_P2.00mm'
FP_HDR2 = 'Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical'
FP_SJ = 'Jumper:SolderJumper-2_P1.3mm_Open_Pad1.0x1.5mm'

# ----------------------------------------------------------------------------- port + power
text(20, 14, 'AUDIO CARRIER  -  ESP32-D + 4 x MAX98357A -> 4 RCA line outputs (front L/R, rear L/R)', 3)
text(20, 22, 'A carrier for one DigiDash header position (docs/hardware/accessory_ports.md). Node 0x10 (both straps open). '
     'CAN only; everything else stays on this board.', 1.6)

place(HDR2x5, 'J1', 'DigiDash port', 'DigiDash_Accessory:Carrier_Header_2x05', 50, 60,
      {'1': '+5V', '2': '+5V', '3': 'GND', '4': 'GND', '5': 'CANH', '6': 'CANL', '7': 'GND', '8': None,
       '9': None, '10': None},
      descr='2x5 female header on the carrier underside. 1,2 +5V; 3,4,7 GND; 5 CANH; 6 CANL; 8 +12V (not used here); 9,10 reserved')
text(70, 48, 'Pin 8 (+12 V switched) is not used: four MAX98357A at line level draw ~10 mA,\nthe 5 V port has room. '
     '9 / 10 reserved.', 1.4)

# PWR_FLAGs: the header pins are passive, so tell ERC the rails are driven.
for k, (net, x, y) in enumerate([('+5V', 30, 100), ('GND', 42, 100)]):
    place('power:PWR_FLAG', '#FLG%02d' % (k + 1), 'PWR_FLAG', '', x, y, {'1': net}, show_value=False)

place(D, 'D1', '1N5819', 'Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal', 50, 130,
      {'1': '+5V_DEV', '2': '+5V'}, descr='Keeps the DevKit USB and the car 5 V rail from feeding each other')
place('power:PWR_FLAG', '#FLG03', 'PWR_FLAG', '', 30, 130, {'1': '+5V_DEV'}, show_value=False)
place(CP, 'C1', '220u 10V', 'Capacitor_THT:CP_Radial_D6.3mm_P2.50mm', 90, 130, {'1': '+5V', '2': 'GND'},
      descr='Bulk on the 5 V rail (amps and radio)')
text(30, 145, 'D1: +5 V -> DevKit 5V pin (about 4.7 V after the diode). The amps run from the un-diode\'d +5 V rail.\n'
     'Do not power the DevKit from USB and 5 V without D1.', 1.4)

# ----------------------------------------------------------------------------- MCU
place(DEVKIT, 'U1', 'ESP32-DevKitC-32', 'AudioCarrier:ESP32-DevKitC-32_Sockets', 170, 130, {
    '1': '+3V3', '19': '+5V_DEV', '14': 'GND', '20': 'GND', '26': 'GND',
    '7': 'CAN_TXD', '8': 'CAN_RX',
    '12': 'I2S_BCLK', '9': 'I2S_LRC', '11': 'I2S_DIN_F', '10': 'I2S_DIN_R',
    '15': 'NODE0', '21': 'NODE1',
    '2': None, '3': None, '4': None, '5': None, '6': None, '13': None, '16': None, '17': None, '18': None,
    '22': None, '23': None, '24': None, '25': None, '27': None, '28': None, '29': None, '30': None, '31': None,
    '32': None, '33': None, '34': None, '35': None, '36': None, '37': None, '38': None},
      descr='HiLetgo / Espressif ESP32-DevKitC-32 in two 1x19 sockets')
text(120, 63, 'GPIO14 BCLK, GPIO25 LRC, GPIO27 DIN front, GPIO26 DIN rear (I2S, same as the bridge firmware; rear is new)\n'
     'GPIO32 CAN TX, GPIO33 CAN RX, GPIO13 / GPIO23 node straps (jumper to GND = 1)', 1.4)

# ----------------------------------------------------------------------------- CAN
place(CAN, 'U2', 'MCP2562-E-P', 'Package_DIP:DIP-8_W7.62mm', 60, 215,
      {'1': 'CAN_TXD', '2': 'GND', '3': '+5V', '4': 'CAN_RXD', '5': '+3V3', '6': 'CANL', '7': 'CANH', '8': 'GND'})
place(R, 'R1', '1k', FP_R, 100, 208, {'1': 'CAN_RXD', '2': 'CAN_RX'}, descr='Series R on the receive line, as on the DigiDash board')
place(C, 'C2', '100n', FP_C, 100, 240, {'1': '+5V', '2': 'GND'})
place(C, 'C3', '100n', FP_C, 115, 240, {'1': '+3V3', '2': 'GND'})
text(20, 245, 'MCP2562: VDD 5 V, VIO from the DevKit 3V3 pin (a few uA), STBY low.\n'
     'No 120 R here: the carrier never terminates the bus (docs/hardware/accessory_ports.md).', 1.4)

# ----------------------------------------------------------------------------- straps and jumper
place(HDR1x2, 'JP1', 'Node bit 0', FP_HDR2, 50, 285, {'1': 'NODE0', '2': 'GND'},
      descr='Jumper = GPIO13 low: node 0x11 (with JP2 open)')
place(HDR1x2, 'JP2', 'Node bit 1', FP_HDR2, 100, 285, {'1': 'NODE1', '2': 'GND'},
      descr='Jumper = GPIO23 low: node 0x12 (with JP1 open)')
place(SJ, 'JP3', 'Rear = front data', FP_SJ, 150, 285, {'1': 'I2S_DIN_F', '2': 'I2S_DIN_R'},
      descr='Open: rear amps get their own data line (GPIO26). Close: rear plays the same stream as front (firmware with one I2S stream)')
text(20, 300, 'Node straps: both open = 0x10 (audio bridge default); JP1 = 0x11; JP2 = 0x12.  JP3 closed = rear amps copy the front stream '
     '(use it until the firmware drives GPIO26; leave GPIO26 an input then).', 1.4)

# ----------------------------------------------------------------------------- amplifier channels
CH = [('FL', 'front left', 'F', True, 'white', 'J6'),
      ('FR', 'front right', 'F', False, 'red', 'J7'),
      ('RL', 'rear left', 'R', True, 'white', 'J8'),
      ('RR', 'rear right', 'R', False, 'red', 'J9')]
text(290, 30, 'Each MAX98357A is one mono channel: GAIN is tied straight to +5 V (6 dB, the lowest fixed gain, less noise into a line input).\n'
     'SD_MODE picks the channel: to +5 V = left, through 680k = right (same as the first revision).  Front pair shares data line F, rear pair R.\n'
     'Line output: OUT+ only (the other half of the bridge is not used), DC block, 1.5k / 1k divider with 3.3n across the 1k (about 80 kHz low-pass), '
     'roughly 0.5 Vrms at full scale, ~600 R source.', 1.4)
for i, (ch, desc, din, left, colour, jack) in enumerate(CH):
    y = 75 + i * 75
    n = i + 3
    sd = '+5V' if left else 'AMP_%s_SD' % ch
    place(AMP, 'U%d' % n, 'Amp %s' % ch, 'Connector_PinSocket_2.54mm:PinSocket_1x07_P2.54mm_Vertical', 310, y, {
        '1': 'I2S_LRC', '2': 'I2S_BCLK', '3': 'I2S_DIN_' + din, '4': '+5V', '5': sd, '6': 'GND', '7': '+5V'},
        descr='MAX98357A breakout, %s' % desc)
    if not left:
        place(R, 'R%d' % (10 + i), '680k', FP_R, 285, y - 20, {'1': '+5V', '2': sd}, descr='SD_MODE to about 1 V: right channel')
    place(HDR1x2, 'J%d' % (2 + i), 'Amp %s out' % ch, FP_HDR2, 350, y - 3,
          {'1': '%s_OUTP' % ch, '2': None}, descr='Wire from the breakout output terminal: pin 1 = OUT+ (line tap), pin 2 = OUT- (unused)')
    place(CP, 'C%d' % (10 + i), '10u 16V', FP_CP, 385, y - 3, {'1': '%s_OUTP' % ch, '2': '%s_A' % ch},
          descr='DC block: + toward the amp (OUT+ idles near half the supply)')
    place(R, 'R%d' % (14 + i), '1.5k', FP_R, 415, y - 3, {'1': '%s_A' % ch, '2': '%s_LINE' % ch})
    place(R, 'R%d' % (18 + i), '1k', FP_R, 440, y - 3, {'1': '%s_LINE' % ch, '2': 'GND'})
    place(C, 'C%d' % (14 + i), '3.3n', FP_C, 465, y - 3, {'1': '%s_LINE' % ch, '2': 'GND'})
    place(COAX, jack, 'RCA %s (%s)' % (ch, colour), 'AudioCarrier:RCA_Jack_RCJ-022', 510, y - 3,
          {'1': '%s_LINE' % ch, '2': 'GND'}, descr='RCA jack, %s: %s' % (colour, desc))
    text(490, y + 8, '%s: %s' % (jack, colour.upper()), 1.6)

# ----------------------------------------------------------------------------- write
sym_lines = ['  (symbol ' + t[len('(symbol '):] if False else t for t in lib_text.values()]
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "A2")',
       '  (title_block (title "DigiDash audio carrier") (date "2026-09-29") (rev "A")',
       '    (comment 1 "ESP32-D + 4 x MAX98357A -> 4 RCA line outputs")',
       '    (comment 2 "Carrier for a DigiDash accessory header position"))',
       '  (lib_symbols']
for t in lib_text.values():
    out.append('\n'.join('    ' + ln for ln in t.split('\n')))
out.append('  )')
out += body
out += ['  (sheet_instances (path "/" (page "1")))', ')']
open(OUT, 'w').write('\n'.join(out) + '\n')
print('wrote', os.path.normpath(OUT), len(body), 'items')
