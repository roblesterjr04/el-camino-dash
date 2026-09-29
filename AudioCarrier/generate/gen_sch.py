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
COAX = stock('Connector', 'Conn_Coaxial')
HDR2x5 = stock('Connector_Generic', 'Conn_02x05_Odd_Even')
HDR1x5 = stock('Connector_Generic', 'Conn_01x05')
SJ = stock('Jumper', 'SolderJumper_2_Open')
SW = stock('Switch', 'SW_Push')
CAN = stock('Interface_CAN_LIN', 'MCP2562-E-P')
ESP = stock('RF_Module', 'ESP32-WROOM-32')
OPA = stock_as('Amplifier_Operational', 'LM2904', 'AudioCarrier:OPA2376', 'OPA2376AID')
LDO = stock_as('Regulator_Linear', 'AP1117-15', 'AudioCarrier:AMS1117-3.3', 'AMS1117-3.3')
for n in ('GND', '+5V', '+3V3', 'PWR_FLAG'):
    stock('power', n)

DAC = box_symbol('AudioCarrier:MAX5134_TSSOP16', 'U', 'MAX5134AGUE+',
                 [('1', 'REFI', 'input'), ('2', 'AVDD', 'power_in'), ('3', 'OUT0', 'output'), ('4', 'DVDD', 'power_in'),
                  ('5', 'READY', 'output'), ('6', 'OUT3', 'output'), ('7', 'GND', 'power_in'), ('8', 'DIN', 'input')],
                 [('9', 'CS', 'input'), ('10', 'SCLK', 'input'), ('11', 'OUT2', 'output'), ('12', 'LDAC', 'input'),
                  ('13', 'M/Z', 'input'), ('14', 'OUT1', 'output'), ('15', 'GND', 'power_in'), ('16', 'REFO', 'output')],
                 w=15.24, footprint='Package_SO:TSSOP-16_4.4x5mm_P0.65mm',
                 descr='Maxim MAX5134 quad 16-bit SPI DAC, 16-pin TSSOP (pin numbers from the TSSOP column of the datasheet pin table)')

FP_R = 'Resistor_SMD:R_0603_1608Metric'
FP_C = 'Capacitor_SMD:C_0603_1608Metric'
FP_C10 = 'Capacitor_SMD:C_1206_3216Metric'
FP_SJ = 'Jumper:SolderJumper-2_P1.3mm_Open_Pad1.0x1.5mm'
FP_SOIC = 'Package_SO:SOIC-8_3.9x4.9mm_P1.27mm'

text(15, 12, 'AUDIO CARRIER  -  ESP32 module + MAX5134 quad 16-bit DAC -> 4 face-mount RCA line outputs (front L/R, rear L/R)', 3)
text(15, 20, 'Carrier for one DigiDash header position (docs/hardware/accessory_ports.md). CAN node 0x10 (both straps open). '
     '5 V from the port only; 3.3 V on board.', 1.6)

# --- port
place(HDR2x5, 'J1', 'DigiDash port', 'DigiDash_Accessory:Carrier_Header_2x05', 45, 55,
      {'1': '+5V', '2': '+5V', '3': 'GND', '4': 'GND', '5': 'CANH', '6': 'CANL', '7': 'GND', '8': None,
       '9': None, '10': None}, descr='1,2 +5V; 3,4,7 GND; 5 CANH; 6 CANL; 8 +12V (not used); 9,10 reserved')
text(70, 45, 'Pin 8 (+12 V) is not used: the whole board draws well under the 0.5 A port fuse.', 1.4)
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', 25, 85, {'1': '+5V'}, show_value=False)
place('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', 40, 85, {'1': 'GND'}, show_value=False)

# --- power
text(15, 100, 'POWER: +5 V -> AMS1117-3.3 for the ESP32 and the DAC digital side; analog 5 V (+5VA) through a ferrite for the DAC and op-amps.', 1.4)
place(C, 'C4', '10u', FP_C10, 40, 125, {'1': '+5V', '2': 'GND'})
place(LDO, 'U6', 'AMS1117-3.3', 'Package_TO_SOT_SMD:SOT-223-3_TabPin2', 75, 120, {'1': 'GND', '2': '+3V3', '3': '+5V'})
place(CP, 'C5', '22u tant', 'Capacitor_Tantalum_SMD:CP_EIA-3528-21_Kemet-B', 110, 125, {'1': '+3V3', '2': 'GND'},
      descr='AMS1117 wants >= 22 uF with some ESR on the output; a tantalum is the safe choice')
place(C, 'C6', '100n', FP_C, 125, 125, {'1': '+3V3', '2': 'GND'})
place(CP, 'C1', '47u 10V', 'Capacitor_SMD:CP_Elec_5x5.4', 25, 125, {'1': '+5V', '2': 'GND'})
place(FB, 'FB1', '600R @100MHz', 'Inductor_SMD:L_0805_2012Metric', 150, 125, {'1': '+5V', '2': '+5VA'})
place(C, 'C2', '10u', FP_C10, 175, 125, {'1': '+5VA', '2': 'GND'})
place(C, 'C3', '100n', FP_C, 190, 125, {'1': '+5VA', '2': 'GND'})

# --- ESP32
espn = {}
for num, (px, py, ang, nm, typ, u) in lib_pins[ESP].items():
    espn[nm] = num
sig = {'IO32': 'CAN_TXD', 'IO33': 'CAN_RX', 'IO18': 'DAC_SCLK', 'IO19': 'DAC_DIN', 'IO5': 'DAC_CS', 'IO21': 'DAC_LDAC',
       'IO22': 'DAC_READY', 'IO13': 'NODE0', 'IO23': 'NODE1', 'IO0': 'BOOT', 'EN': 'EN', 'TXD0/IO1': 'UART_TX',
       'RXD0/IO3': 'UART_RX', 'VDD': '+3V3', 'GND': 'GND'}
en = {}
for num, (px, py, ang, nm, typ, u) in lib_pins[ESP].items():
    en[num] = sig.get(nm)
place(ESP, 'U1', 'ESP32-WROOM-32E', 'RF_Module:ESP32-WROOM-32', 260, 215, en,
      descr='Espressif ESP32-WROOM-32E module (the module on the DevKitC). 4 MB flash version is enough')
text(215, 150, 'GPIO32 CAN TX, GPIO33 CAN RX (1k), GPIO13 / GPIO23 node straps (solder jumper to GND = 1),\n'
     'GPIO18 SCLK, GPIO19 DIN (SPI MOSI via the GPIO matrix), GPIO5 CS (idles high: strapping pin), GPIO21 LDAC, GPIO22 READY.\n'
     'GPIO12 / GPIO2 / GPIO15 left open (boot straps).', 1.4)

# boot / programming
text(15, 150, 'BOOT / PROGRAMMING', 1.6)
place(R, 'R2', '10k', FP_R, 30, 180, {'1': '+3V3', '2': 'EN'})
place(C, 'C7', '1u', FP_C, 50, 180, {'1': 'EN', '2': 'GND'})
place(SW, 'SW1', 'RESET', 'Button_Switch_SMD:SW_SPST_PTS810', 75, 180, {'1': 'EN', '2': 'GND'})
place(R, 'R3', '10k', FP_R, 30, 215, {'1': '+3V3', '2': 'BOOT'})
place(SW, 'SW2', 'BOOT', 'Button_Switch_SMD:SW_SPST_PTS810', 75, 215, {'1': 'BOOT', '2': 'GND'})
place(HDR1x5, 'J10', 'Program', 'Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical', 115, 200,
      {'1': 'GND', '2': 'UART_TX', '3': 'UART_RX', '4': 'BOOT', '5': 'EN'},
      descr='USB-serial adapter: GND, adapter RX <- TX0, adapter TX -> RX0, IO0, EN. 3.3 V logic. Do not power from the adapter')
text(15, 235, 'Flash: hold BOOT, tap RESET (or pull IO0 low from a DTR/RTS adapter), then upload. Power the board from the port.', 1.4)

# --- CAN
text(15, 250, 'CAN: MCP2562 (VDD 5 V, VIO 3.3 V, STBY low). No 120 R: the carrier never terminates the bus.', 1.4)
place(CAN, 'U5', 'MCP2562-E/SN', FP_SOIC, 55, 285, {'1': 'CAN_TXD', '2': 'GND', '3': '+5V', '4': 'CAN_RXD', '5': '+3V3',
                                                      '6': 'CANL', '7': 'CANH', '8': 'GND'})
place(R, 'R1', '1k', FP_R, 95, 275, {'1': 'CAN_RXD', '2': 'CAN_RX'})
place(C, 'C8', '100n', FP_C, 115, 275, {'1': '+5V', '2': 'GND'})
place(C, 'C9', '100n', FP_C, 130, 275, {'1': '+3V3', '2': 'GND'})
place(SJ, 'JP1', 'Node bit 0', FP_SJ, 100, 310, {'1': 'NODE0', '2': 'GND'}, descr='Close = GPIO13 low: node 0x11 (JP2 open)')
place(SJ, 'JP2', 'Node bit 1', FP_SJ, 140, 310, {'1': 'NODE1', '2': 'GND'}, descr='Close = GPIO23 low: node 0x12 (JP1 open)')
text(15, 322, 'Node: both open = 0x10 (audio bridge), JP1 = 0x11, JP2 = 0x12.', 1.4)

# --- DAC
text(330, 30, 'MAX5134 (16-bit, SPI up to 30 MHz): 4 buffered outputs, internal 2.44 V reference (REFO -> REFI), full scale 2.44 V, midscale 1.22 V.\n'
     'M/Z high = outputs power up at midscale (no step into the DC-block caps). LDAC pulled high: use the write-through command per channel,\n'
     'or pulse LDAC from GPIO21 to update all four together. Every frame: 4 x 24 bits, so 96 kHz per channel needs a 9.2 MHz clock.', 1.4)
place(DAC, 'U2', 'MAX5134AGUE+', 'Package_SO:TSSOP-16_4.4x5mm_P0.65mm', 375, 90, {
    '1': 'VREF', '16': 'VREF', '2': '+5VA', '4': '+3V3', '7': 'GND', '15': 'GND',
    '3': 'DAC_FL', '14': 'DAC_FR', '11': 'DAC_RL', '6': 'DAC_RR',
    '5': 'DAC_READY', '8': 'DAC_DIN', '9': 'DAC_CS', '10': 'DAC_SCLK', '12': 'DAC_LDAC', '13': '+5VA'})
place(C, 'C10', '47p', 'Capacitor_SMD:C_0603_1608Metric', 440, 75, {'1': 'VREF', '2': 'GND'}, descr='REFO bypass: 47 pF (max 100 pF), datasheet')
place(C, 'C11', '100n', FP_C, 460, 75, {'1': '+5VA', '2': 'GND'})
place(C, 'C12', '100n', FP_C, 480, 75, {'1': '+3V3', '2': 'GND'})
place(R, 'R5', '100k', FP_R, 500, 75, {'1': '+3V3', '2': 'DAC_LDAC'})
place(R, 'R6', '100k', FP_R, 520, 75, {'1': '+3V3', '2': 'DAC_CS'})

# --- channels
CH = [('FL', 'front left', 'white', 'RCJ-023', 'J2', 'U3', 1), ('FR', 'front right', 'red', 'RCJ-022', 'J3', 'U3', 2),
      ('RL', 'rear left', 'white', 'RCJ-023', 'J4', 'U4', 1), ('RR', 'rear right', 'red', 'RCJ-022', 'J5', 'U4', 2)]
text(300, 125, 'Per channel: 2nd-order Sallen-Key low-pass (3.9k / 3.9k, 2.2n feedback / 1n to ground, about 27 kHz, Q 0.74) to strip the sample images,\n'
     'then a 10u DC block (the DAC idles at 1.22 V), 47k to ground, 100 R in series with the jack. Full scale about 0.86 Vrms. '
     'Use C0G/NP0 for the 1n / 2.2n caps.', 1.4)
for i, (ch, desc, colour, part, jack, opa, unit) in enumerate(CH):
    y = 165 + i * 45
    r1, r2, r3, r4 = 20 + i * 4, 21 + i * 4, 22 + i * 4, 23 + i * 4
    c1, c2, c3 = 20 + i * 3, 21 + i * 3, 22 + i * 3
    inp, outp = ('3', '1') if unit == 1 else ('5', '7')
    inn = '2' if unit == 1 else '6'
    place(OPA, opa, 'OPA2376AID', FP_SOIC, 335, y, {inp: '%s_N2' % ch, inn: '%s_OP' % ch, outp: '%s_OP' % ch},
          unit=unit, first=(unit == 1), descr='Dual RRIO low-noise op-amp on 5 V (any RRIO dual in SOIC-8 with the standard pinout will do)')
    place(R, 'R%d' % r1, '3.9k', FP_R, 365, y - 12, {'1': 'DAC_%s' % ch, '2': '%s_N1' % ch})
    place(R, 'R%d' % r2, '3.9k', FP_R, 385, y - 12, {'1': '%s_N1' % ch, '2': '%s_N2' % ch})
    place(C, 'C%d' % (20 + i * 3 + 20), '2.2n C0G', FP_C, 405, y - 12, {'1': '%s_N1' % ch, '2': '%s_OP' % ch})
    place(C, 'C%d' % (21 + i * 3 + 20), '1n C0G', FP_C, 425, y - 12, {'1': '%s_N2' % ch, '2': 'GND'})
    place(C, 'C%d' % (22 + i * 3 + 20), '10u', FP_C10, 445, y - 12, {'1': '%s_OP' % ch, '2': '%s_AC' % ch}, descr='DC block, + toward the op-amp')
    place(R, 'R%d' % r3, '47k', FP_R, 470, y - 12, {'1': '%s_AC' % ch, '2': 'GND'})
    place(R, 'R%d' % r4, '100', FP_R, 490, y - 12, {'1': '%s_AC' % ch, '2': '%s_LINE' % ch})
    place(COAX, jack, 'RCA %s (%s)' % (ch, colour), 'AudioCarrier:RCA_Jack_SameSky_RCJ-02_Vertical', 530, y - 12,
          {'1': '%s_LINE' % ch, '2': 'GND'}, descr='%s, face-mount RCA jack, %s: %s' % (part, colour, desc))
    text(510, y - 2, '%s  %s' % (jack, colour.upper()), 1.6)
place(OPA, 'U3', 'OPA2376AID', FP_SOIC, 335, 335, {'4': 'GND', '8': '+5VA'}, unit=3, first=False)
place(OPA, 'U4', 'OPA2376AID', FP_SOIC, 385, 335, {'4': 'GND', '8': '+5VA'}, unit=3, first=False)
place(C, 'C60', '100n', FP_C, 430, 335, {'1': '+5VA', '2': 'GND'}, descr='U3 decoupling, next to the pins')
place(C, 'C61', '100n', FP_C, 450, 335, {'1': '+5VA', '2': 'GND'}, descr='U4 decoupling, next to the pins')
place('power:PWR_FLAG', '#FLG03', 'PWR_FLAG', '', 25, 100, {'1': '+5VA'}, show_value=False)

# ----------------------------------------------------------------------------- write
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "A2")',
       '  (title_block (title "DigiDash audio carrier") (date "2026-09-29") (rev "B")',
       '    (comment 1 "ESP32-WROOM-32E + MAX5134 quad DAC -> 4 RCA line outputs")',
       '    (comment 2 "Carrier for a DigiDash accessory header position"))',
       '  (lib_symbols']
for t in lib_text.values():
    out.append('\n'.join('    ' + ln for ln in t.split('\n')))
out.append('  )')
out += body
out += ['  (sheet_instances (path "/" (page "1")))', ')']
open(OUT, 'w').write('\n'.join(out) + '\n')
print('wrote', os.path.normpath(OUT), len(body), 'items')
