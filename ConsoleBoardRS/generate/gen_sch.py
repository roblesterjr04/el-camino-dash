#!/usr/bin/env python3
"""Generate ConsoleBoardRS.kicad_sch (KiCad 7 file format, opens in 7/8/9/10).

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
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ConsoleBoardRS.kicad_sch')
NS = uuid.UUID('7d2e9c41-5b8a-4f3e-a1c6-24b0e8d3f914')
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
    (instances (project "ConsoleBoardRS" (path "/%s" (reference "%s") (unit 1))))
  )''' % (lib_id, f(x), f(y), rot, uid('p'), ref, f(x), f(y), net, f(x + 2.54), f(y), f(x), f(y), f(x), f(y), uid('pp'), SHEET, ref))


def place(lib_id, ref, value, footprint, x, y, nets, descr=None, show_value=True, unit=1, first=True, fitted=True):
    """nets: {pin number: net name | None (unconnected -> no-connect flag)}; every pin must be listed or be omitted on purpose."""
    if first:
        assert ref not in refs_seen, ref
        refs_seen.add(ref)
        refs_info[ref] = {'group': cur_group[0], 'fp': footprint, 'value': value}
        if not fitted:
            refs_info[ref]['dnp'] = True
    pins = {k: v for k, v in lib_pins[lib_id].items() if v[5] in (0, unit)}
    props = lib_props[lib_id]

    def pos(k, dflt):
        px, py = props.get(k, dflt)
        return f(x + px), f(y - py)

    rp, vp = pos('Reference', (0, 3)), pos('Value', (0, -3))
    lines = ['  (symbol (lib_id "%s") (at %s %s 0) (unit %d) (in_bom %s) (on_board yes) (dnp %s) (uuid "%s")' % (
        lib_id, f(x), f(y), unit, 'yes' if fitted else 'no', 'no' if fitted else 'yes', uid('s' + ref + str(unit))),
        '    (property "Reference" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))' % (ref, rp[0], rp[1]),
        '    (property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)%s))' % (
            value, vp[0], vp[1], '' if show_value else ' hide'),
        '    (property "Footprint" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (footprint, f(x), f(y)),
        '    (property "Datasheet" "~" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (f(x), f(y))]
    if descr:
        lines.append('    (property "Description" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (descr, f(x), f(y)))
    for num in pins:
        lines.append('    (pin "%s" (uuid "%s"))' % (num, uid('pin' + ref + str(unit) + num)))
    lines.append('    (instances (project "ConsoleBoardRS" (path "/%s" (reference "%s") (unit %d))))' % (SHEET, ref, unit))
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
DS = stock('Device', 'D_Schottky')
MH = stock('Mechanical', 'MountingHole_Pad')
HDR16 = stock('Connector_Generic', 'Conn_02x08_Odd_Even')
HDR4 = stock('Connector_Generic', 'Conn_01x04')
HDR3 = stock('Connector_Generic', 'Conn_01x03')
SW = stock('Switch', 'SW_Push')
CAN = stock('Interface_CAN_LIN', 'MCP2562-E-SN')
NUP = stock('Power_Protection', 'NUP2105L')
USBC = stock('Connector', 'USB_C_Receptacle_USB2.0_16P')
ESP = stock('RF_Module', 'ESP32-S3-WROOM-1')
LDO = stock_as('Regulator_Linear', 'SPX2920M3-3.3_SOT223', 'ConsoleBoardRS:AP7361C-33E', 'AP7361C-33E')  # SOT-223: 1 IN, 2 GND (tab), 3 OUT
USBESD = stock_as('Power_Protection', 'USBLC6-2P6', 'ConsoleBoardRS:USBLC6-2SC6', 'USBLC6-2SC6')
for n in ('GND', '+5V', '+3V3', 'PWR_FLAG'):
    stock('power', n)
for k in (LDO, USBESD, ESP, USBC):
    print(k, sorted((v[3], p) for p, v in lib_pins[k].items())[:0] or {p: v[3] for p, v in lib_pins[k].items()})

FP_R = 'Resistor_SMD:R_0603_1608Metric'
FP_C = 'Capacitor_SMD:C_0603_1608Metric'
FP_C10 = 'Capacitor_SMD:C_0805_2012Metric'
FP_SOIC = 'Package_SO:SOIC-8_3.9x4.9mm_P1.27mm'
FP_SOT23 = 'Package_TO_SOT_SMD:SOT-23'
FP_SOD123 = 'Diode_SMD:D_SOD-123'
FP_PH = 'Connector_JST:JST_PH_B4B-PH-K_1x04_P2.00mm_Vertical'   # vertical (top entry): the plug goes in from behind the board
FP_RJ = 'ConsoleBoardRS:RJ9_Assmann_A-2014-3-4-N-T-R_Vertical'
FP_SW = 'Button_Switch_SMD:SW_SPST_PTS810'

cnt = {}


def nid(prefix):
    cnt[prefix] = cnt.get(prefix, 0) + 1
    return '%s%d' % (prefix, cnt[prefix])


def r_(v, a, b, x, y, d=None):
    ref = nid('R')
    place(R, ref, v, FP_R, x, y, {'1': a, '2': b}, descr=d)
    return ref


def c_(v, a, b, x, y, fp=None, d=None):
    ref = nid('C')
    place(C, ref, v, fp or (FP_C10 if v in ('10u', '22u') else FP_C), x, y, {'1': a, '2': b}, descr=d)
    return ref


text(15, 12, 'CONSOLE BOARD, RETROSOUND KNOBS  -  ESP32-S3-WROOM-1 + SSD1322 256x64 OLED (16-pin, SPI) + two RetroSound knobs on RJ9 + CAN node 0x13', 3)
text(15, 20, 'Screen sits on standoffs above the board, soldered to J1. CAN cable J3 (5 V, CANH, CANL, GND) to a DigiDash cable port. '
     'The two knobs (RetroSound 258A style, resistor ladders) plug into RJ9 jacks J4 / J5. USB-C is for programming (and bench power); in the car the board is powered from J3.', 1.6)

# --- OLED header
G('oled')
OLED_PINS = {   # the 100 x 33.5 mm 3.12" SSD1322 module's 2 x 8 header (odd pins in the row marked 1, even in the row marked 2),
    # as measured on the module in hand (and working on the bench): only the seven SPI pins are used
    '1': 'GND',
    '2': '+3V3',       # VCC
    '3': None,         # NC
    '4': 'OLED_SCK',   # D0 = CLK
    '5': 'OLED_MOSI',  # D1 = DIN
    '6': None, '7': None, '8': None, '9': None, '10': None, '11': None,   # D2..D7: open in SPI mode (as on the bench)
    '12': None, '13': None,   # E/RD, R/W: open (the module holds them for SPI)
    '14': 'OLED_DC',
    '15': 'OLED_RST',  # RES
    '16': 'OLED_CS',
}
# Checked against the module in hand: its even row (2, 4, ... 16) faces the board's outer (right) edge, with pin 2 at the
# bottom. J1's pads run top to bottom, odd ones in the outer column, so module pin n sits on pad 17 - n
# (pin 2 VCC on pad 15, pin 1 GND on pad 16, ..., pin 16 CS on pad 1).
SOCKET_NETS = {str(17 - int(k)): v for k, v in OLED_PINS.items()}
place(HDR16, 'J1', 'OLED 2x8 socket', 'Connector_PinSocket_2.54mm:PinSocket_2x08_P2.54mm_Vertical', 60, 60,
      SOCKET_NETS, descr='SSD1322 256x64 OLED module (100 x 33.5 mm), 4-wire SPI: 1 GND, 2 VCC, 4 D0 CLK, 5 D1 DIN, 14 D/C, 15 RES, '
      '16 CS; 3 and 6-13 open. Pinout measured on the module. Module pin n sits on socket pad 17 - n (even row on the outer edge, pin 2 at the bottom, checked on the module)')
text(80, 40, 'J1: female 2 x 8 socket on the front; the module\'s own pins plug into it. Pinout measured on the module: 1 GND, 2 VCC, 4 CLK, '
     '5 DIN, 14 D/C, 15 RES, 16 CS; the parallel-bus pins stay open, as on the working bench setup.', 1.4)
r_('100k', 'OLED_CS', '+3V3', 20, 100, 'Keeps the panel deselected while the ESP32 boots')

# --- ESP32
G('esp')
sig = {'IO12': 'OLED_SCK', 'IO11': 'OLED_MOSI', 'IO10': 'OLED_CS', 'IO9': 'OLED_DC', 'IO8': 'OLED_RST',
       'IO5': 'CAN_TXD', 'IO6': 'CAN_RX', 'IO1': 'SEL_2', 'IO2': 'SEL_3', 'IO4': 'VOL_2', 'IO7': 'VOL_3',
       'IO39': 'IR_IN', 'USB_D-': 'USB_DM', 'USB_D+': 'USB_DP', 'IO0': 'BOOT', 'EN': 'EN',
       '3V3': '+3V3', 'GND': 'GND'}
en = {}
for num, (px, py, ang, nm, typ, u) in lib_pins[ESP].items():
    en[num] = sig.get(nm)
place(ESP, 'U1', 'ESP32-S3-WROOM-1-N8', 'RF_Module:ESP32-S3-WROOM-1', 180, 100, en,
      descr='Espressif ESP32-S3-WROOM-1-N8 (8 MB flash, no PSRAM, so GPIO35-37 stay free). Antenna on the board edge')
c_('10u', '+3V3', 'GND', 150, 150, d='Module supply, at pin 2')
c_('100n', '+3V3', 'GND', 165, 150, d='Module supply, at pin 2')
c_('22u', '+3V3', 'GND', 135, 150, d='Bulk, for the module current peaks')
text(120, 25, 'GPIO map: OLED SPI (FSPI) SCK 12, MOSI 11, CS 10, DC 9, RST 8. CAN TX 5, RX 6. Select knob (J4) contacts 2 / 3 on ADC1 1 / 2, '
     'Volume knob (J5) contacts 2 / 3 on ADC1 4 / 7. IR in 39. USB 19 / 20. Strapping pins 0, 3, 45, 46 not used '
     'except BOOT (0).', 1.4)

G('boot')
r_('10k', '+3V3', 'EN', 100, 180)
c_('1u', 'EN', 'GND', 115, 180)
place(SW, 'SW1', 'RESET', FP_SW, 130, 180, {'1': 'EN', '2': 'GND'})
place(SW, 'SW2', 'BOOT', FP_SW, 160, 180, {'1': 'BOOT', '2': 'GND'})
text(100, 195, 'Native USB: hold BOOT, tap RESET, then upload over USB-C (or use the ROM loader on the USB CDC port).', 1.4)

# --- power
G('power')
text(15, 130, 'POWER: J3 +5 V (through the cable port PTC) or USB VBUS, ORed by two Schottkys into +5V. AP7361C-33E (1 A, 0.36 V dropout, SOT-223 for the heat) makes +3V3 (ESP32, OLED, CAN VIO). The CAN transceiver takes its 5 V straight from the cable, ahead of the diode.', 1.4)
place(DS, 'D1', 'B5819W', FP_SOD123, 30, 150, {'1': '+5V', '2': 'VBUS'}, descr='USB VBUS to +5V (cathode at +5V)')
place(DS, 'D2', 'B5819W', FP_SOD123, 55, 150, {'1': '+5V', '2': 'CAB_5V'}, descr='Cable 5 V to +5V (cathode at +5V)')
c_('10u', '+5V', 'GND', 80, 150)
c_('100n', '+5V', 'GND', 95, 150)
place(LDO, 'U3', 'AP7361C-33E', 'Package_TO_SOT_SMD:SOT-223-3_TabPin2', 40, 190, {'1': '+5V', '2': 'GND', '3': '+3V3'},
      descr='Diodes AP7361C-33E-13, 3.3 V 1 A LDO. Give the tab (GND) copper: it carries the heat')
c_('10u', '+5V', 'GND', 60, 200, d='LDO input')
c_('10u', '+3V3', 'GND', 75, 200, d='LDO output')
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', 25, 85, {'1': '+5V'}, show_value=False)
place('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', 40, 85, {'1': 'GND'}, show_value=False)
place('power:PWR_FLAG', '#FLG03', 'PWR_FLAG', '', 55, 85, {'1': '+3V3'}, show_value=False)

# --- USB
G('usb')
text(15, 215, 'USB-C (USB 2.0, device only): 5.1k on CC1 / CC2, USBLC6 on D+ / D-, shield to GND.', 1.4)
place(USBC, 'J2', 'USB-C', 'Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal', 40, 240,
      {'A1': 'GND', 'A4': 'VBUS', 'A5': 'CC1', 'A6': 'USB_DP', 'A7': 'USB_DM', 'A8': None, 'A9': 'VBUS', 'A12': 'GND',
       'B1': 'GND', 'B4': 'VBUS', 'B5': 'CC2', 'B6': 'USB_DP', 'B7': 'USB_DM', 'B8': None, 'B9': 'VBUS', 'B12': 'GND',
       'SH': 'GND'})
r_('5.1k', 'CC1', 'GND', 80, 230)
r_('5.1k', 'CC2', 'GND', 95, 230)
place(USBESD, 'U4', 'USBLC6-2SC6', 'Package_TO_SOT_SMD:SOT-23-6', 130, 240,
      {'1': 'USB_DP', '2': 'GND', '3': 'USB_DM', '4': 'USB_DM', '5': 'VBUS', '6': 'USB_DP'})
c_('100n', 'VBUS', 'GND', 100, 250)

# --- CAN
G('can')
text(15, 280, 'CAN (node 0x13): MCP2562, VDD 5 V straight from the cable (CAB_5V, ahead of the OR diode, so it stays above its 4.5 V minimum), VIO 3.3 V, STBY low. No 120 R: the console never terminates the bus. J3 pin order is the DigiDash cable-port one.', 1.4)
place(CAN, 'U2', 'TJA1051T/3', FP_SOIC, 55, 315, {'1': 'CAN_TXD', '2': 'GND', '3': 'CAB_5V', '4': 'CAN_RXD', '5': '+3V3',
                                                      '6': 'CANL', '7': 'CANH', '8': 'GND'})
r_('1k', 'CAN_RXD', 'CAN_RX', 95, 305, 'Series R on the receive line, as on the DigiDash board')
c_('100n', 'CAB_5V', 'GND', 115, 305, d='MCP2562 VDD')
c_('100n', '+3V3', 'GND', 130, 305)
place(NUP, 'D5', 'NUP2105L', FP_SOT23, 100, 335, {'1': 'CANH', '2': 'CANL', '3': 'GND'}, descr='CAN ESD clamp at the cable')
place(HDR4, 'J3', 'CAN to DigiDash', FP_PH, 20, 335, {'1': 'CAB_5V', '2': 'CANH', '3': 'CANL', '4': 'GND'},
      descr='JST PH 4-pin side entry: 1 +5 V, 2 CANH, 3 CANL, 4 GND (cable port order, docs/hardware/accessory_ports.md)')

# --- knobs
G('ui')
text(220, 222, 'KNOBS: RetroSound 258A style controls on RJ9 (4P4C). Contacts 1 black and 4 yellow are ground (shorted inside the knob); 2 red is the encoder '
     '(to ground through open / 82k / 30k / 47k, a 2-bit Gray code), 3 green the switches (open; 6.8k outer left; 22k outer right; 0 R push). '
     'A reversed phone cord swaps 2 and 3, so both get the same 33k pull-up and 10 nF and go to ADC1: the levels tell the firmware which is which '
     '(green 0 / 0.56 / 1.32 / 3.3 V, red 1.57 / 1.94 / 2.35 / 3.3 V).', 1.4)
place(HDR4, 'J4', 'Select knob (RJ9)', FP_RJ, 240, 300, {'1': 'GND', '2': 'SEL_2', '3': 'SEL_3', '4': 'GND'},
      descr='RJ9 4P4C top-entry jack, Assmann WSW A-2014-3-4-N-T-R (footprint pins 1-4 = jack contacts 1-4): 1 black GND, 2 red encoder, 3 green switches, 4 yellow GND')
place(HDR4, 'J5', 'Volume knob (RJ9)', FP_RJ, 290, 300, {'1': 'GND', '2': 'VOL_2', '3': 'VOL_3', '4': 'GND'},
      descr='RJ9 4P4C top-entry jack, Assmann WSW A-2014-3-4-N-T-R (footprint pins 1-4 = jack contacts 1-4): 1 black GND, 2 red encoder, 3 green switches, 4 yellow GND')
place(HDR3, 'J6', 'TSOP4838', 'OptoDevice:Vishay_MOLD-3Pin', 340, 300,
      {'1': 'IR_IN', '2': 'GND', '3': 'IR_VS'},
      descr='IR receiver, Vishay TSOP4838 (38 kHz, MOLD package) fitted straight in: 1 OUT (signal), 2 GND, 3 VS on 3.3 V through its filter')
for i, n in enumerate(['SEL_2', 'SEL_3', 'VOL_2', 'VOL_3', 'IR_IN']):
    r_('33k' if n[:3] in ('SEL', 'VOL') else '10k', '+3V3', n, 230 + 15 * i, 340,
       'Ladder pull-up: the same on both knob lines, so a reversed cord still reads' if n[:3] in ('SEL', 'VOL') else None)
    if n != 'IR_IN':
        c_('10n', n, 'GND', 230 + 15 * i, 355)
# the TSOP's supply filter (Vishay's application circuit): 100 R in series with VS and 100 nF across it, so the car's
# supply noise stays out of the receiver
r_('100', '+3V3', 'IR_VS', 355, 330, 'IR receiver supply filter: in series with VS')
c_('100n', 'IR_VS', 'GND', 370, 330, d='IR receiver supply filter: VS to GND, next to J6')

# --- mounting
G('mount')
for i, ref in enumerate(('H1', 'H2', 'H4')):  # no H3: the IR receiver has that corner
    place(MH, ref, 'MountingHole', 'MountingHole:MountingHole_2.2mm_M2_Pad', 20 + 20 * i, 380, {'1': 'GND'},
          descr='M2 hole for a screen standoff, tied to GND')

# ----------------------------------------------------------------------------- write
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "A2")',
       '  (title_block (title "DigiDash console board, RetroSound knobs") (date "2026-10-03") (rev "A")',
       '    (comment 1 "ESP32-S3 + SSD1322 256x64 OLED + 2 RetroSound knobs on RJ9 + CAN (node 0x13)")',
       '    (comment 2 "Cable to a DigiDash cable port"))',
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
