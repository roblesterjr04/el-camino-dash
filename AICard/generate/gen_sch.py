#!/usr/bin/env python3
"""Generate AICard.kicad_sch (KiCad 7 file format, opens in 7/8/9/10). Same pattern as LTECard/generate.

Needs the stock KiCad symbol libraries (KICAD_SYMBOLS, default /usr/share/kicad/symbols).
Every connection is a net label or power symbol on a short stub at the pin, so the file is
a netlist you can read; move things around in eeschema as you like.

The compute module's 40-pin header map (HEADER_NETS) follows the Raspberry Pi standard; the module's own pinout, outline
and mounting holes are PROVISIONAL until a module is in hand (docs/hardware/ai_card.md, AICard/README.md).
"""
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, grab_symbol  # noqa: E402

# ----------------------------------------------------------------------------- compute module header (Raspberry Pi 40-pin map)
# What each of the 40 header pins connects to on the card (None = not connected). Pins 2 / 4: the module's 5 V input from
# the card's buck. GND pins: AUX_GND (the module's return). The module's 3.3 V outputs (1, 17) are not used: the card's
# logic never depends on the module. UART: pin 8 is the module's TXD, pin 10 its RXD. 29 / 31 / 33: ALIVE, SHUTDOWN_REQ,
# ATTN (docs/protocols/ai_link.md, Control lines).
HEADER_NETS = {i: None for i in range(1, 41)}
for _p in (2, 4):
    HEADER_NETS[_p] = 'MOD_5V'
for _p in (6, 9, 14, 20, 25, 30, 34, 39):
    HEADER_NETS[_p] = 'AUX_GND'
HEADER_NETS[8] = 'MOD_TXD'
HEADER_NETS[10] = 'MOD_RXD'
HEADER_NETS[29] = 'MOD_ALIVE'
HEADER_NETS[31] = 'MOD_SHUT'
HEADER_NETS[33] = 'MOD_ATTN'

SYMDIR = os.environ.get('KICAD_SYMBOLS', next((d for d in ('/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols',
                                                               '/usr/share/kicad/symbols') if os.path.isdir(d)), ''))
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'AICard.kicad_sch')
NS = uuid.UUID('7b2e9f10-5c3d-4a68-9e41-d0a1c8b37f52')
SHEET = str(uuid.uuid5(NS, 'sheet'))
PROJECT = 'AICard'
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


def stock(lib, name, as_name=None, value=None):
    """A stock symbol; with as_name, a stock symbol (or the parent of one that extends another) under a new id."""
    txt = grab_symbol(os.path.join(SYMDIR, lib + '.kicad_sym'), name).strip()
    if as_name is None:
        lib_id = '%s:%s' % (lib, name)
        txt = re.sub(r'^\(symbol "%s"' % re.escape(name), '(symbol "%s"' % lib_id, txt, count=1)
    else:
        lib_id = 'AICard:' + as_name
        txt = txt.replace('(symbol "%s"' % name, '(symbol "%s"' % lib_id, 1)
        txt = txt.replace('"%s_' % name, '"%s_' % as_name)
        if value:
            txt = re.sub(r'(\(property "Value" )"[^"]*"', r'\1"%s"' % value, txt, count=1)
    assert txt.startswith('(symbol "%s"' % lib_id), lib_id
    register(lib_id, txt)
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


def stub(x, y, angle_toward_body, net):
    """Draw a 2.54 stub outward from a pin end and end it with a label / power symbol."""
    out = (angle_toward_body + 180) % 360
    dx, dy = {0: (2.54, 0), 180: (-2.54, 0), 90: (0, -2.54), 270: (0, 2.54)}[out]
    x2, y2 = x + dx, y + dy
    body.append('  (wire (pts (xy %s %s) (xy %s %s)) (stroke (width 0) (type default)) (uuid "%s"))' % (
        f(x), f(y), f(x2), f(y2), uid('w')))
    if net in POWER:
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
    (instances (project "%s" (path "/%s" (reference "%s") (unit 1))))
  )''' % (lib_id, f(x), f(y), rot, uid('p'), ref, f(x), f(y), net, f(x + 2.54), f(y), f(x), f(y), f(x), f(y), uid('pp'),
          PROJECT, SHEET, ref))


def place(lib_id, ref, value, footprint, x, y, nets, descr=None, show_value=True, unit=1, first=True):
    """nets: {pin number: net name | None (unconnected -> no-connect flag)}; every pin must be listed."""
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
    lines.append('    (instances (project "%s" (path "/%s" (reference "%s") (unit %d))))' % (PROJECT, SHEET, ref, unit))
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
L = stock('Device', 'L')
LED = stock('Device', 'LED')
DS = stock('Device', 'D_Schottky')
DZ = stock('Device', 'D_Zener')
NTC = stock('Device', 'Thermistor_NTC')
QP = stock('Device', 'Q_PMOS_GSD')
QN = stock('Device', 'Q_NMOS_GSD')
HDR2x15 = stock('Connector_Generic', 'Conn_02x15_Odd_Even')
HDR2x20 = stock('Connector_Generic', 'Conn_02x20_Odd_Even')
HDR3 = stock('Connector_Generic', 'Conn_01x03')
SW = stock('Switch', 'SW_Push')
CAN = stock('Interface_CAN_LIN', 'MCP2562-E-SN')
ESP = stock('RF_Module', 'ESP32-S3-WROOM-1')
USBC = stock('Connector', 'USB_C_Receptacle_USB2.0_16P')
ESD = stock('Power_Protection', 'USBLC6-2P6', 'USBLC6-2SC6', 'USBLC6-2SC6')
BUCK = stock('Regulator_Switching', 'TPS54560BDDA')
LDO = stock('Regulator_Linear', 'SPX2920M3-3.3_SOT223', 'AP7361C-33E', 'AP7361C-33E')
BUF = stock('74xx', '74LS125', '74LVC125', '74LVC125APW')
HOLE = stock('Mechanical', 'MountingHole')
for n in ('GND', '+5V', '+3V3', 'PWR_FLAG'):
    stock('power', n)

FP_R = 'Resistor_SMD:R_0603_1608Metric'
FP_C = 'Capacitor_SMD:C_0603_1608Metric'
FP_C0805 = 'Capacitor_SMD:C_0805_2012Metric'
FP_C1210 = 'Capacitor_SMD:C_1210_3225Metric'
FP_LED = 'LED_SMD:LED_0603_1608Metric'
FP_SOT23 = 'Package_TO_SOT_SMD:SOT-23'

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
    if fp is None:
        fp = FP_C1210 if v in ('2.2u 100V', '47u 10V') else FP_C0805 if v in ('10u', '22u') else FP_C
    place(C, ref, v, fp, x, y, {'1': a, '2': b}, descr=d)
    return ref


text(15, 12, 'AI VOICE CARD  -  ESP32-S3-WROOM-1U-N16R8 front end + compute module on a 2 x 20 header (Linux: speech, on-board model, online lookups) + MCP2562 CAN', 3)
text(15, 20, 'Card for a DigiDash slot, 80 x 60 mm, standing on edge (docs/hardware/accessory_ports.md, docs/hardware/ai_card.md). '
     'Any slot. CAN node 0x14 + slot - 1, product 0x0007. ESP32 and CAN from the slot 5 V; the compute module from AUX 12 V through its own '
     'buck, switched by the ESP32. The microphone and the speaker are the audio card\'s, through the slot\'s I2S rail.', 1.6)

# --- slot
G('port')
place(HDR2x15, 'J1', 'DigiDash slot', 'Connector_PinHeader_2.54mm:PinHeader_2x15_P2.54mm_Horizontal', 45, 60,
      {'1': '+5V', '2': '+5V', '3': 'GND', '4': 'GND', '5': 'CANH', '6': 'CANL', '7': 'GND', '8': None,
       '9': None, '10': None, '11': 'SLOT_ID', '12': None, '13': None, '14': None,
       '15': 'RAIL_BCK', '16': 'RAIL_WS', '17': 'RAIL_MIC', '18': 'RAIL_RET', '19': 'GND', '20': 'GND',
       '21': None, '22': None, '23': None, '24': None,
       '25': 'AUX12', '26': 'AUX12', '27': 'AUX12', '28': 'AUX_GND', '29': 'AUX_GND', '30': 'AUX_GND'},
      descr='Right-angle 2x15 header on the bottom edge (the slot pinout, docs/hardware/accessory_ports.md). Used: 5 V, GND, CAN, '
            'slot ID, I2S rail BCK / WS / MIC (inputs) and RET (a 3-state output), AUX 12 V / AUX GND (25-30). Not used: switched / '
            'constant 12 V, 3.3 V, WAKE, LAMP, REV, MCLK, audio bus')
text(75, 40, 'Slot ID (pin 11): 10k to 3.3 V here, the slot\'s own resistor to GND on the main board, read on GPIO4 (ADC1).\n'
     'AUX GND (28-30) is NOT joined to GND on this card (the LTE card\'s rule): on the main board the two meet only at NT1. The buck, its 470 uF\n'
     'and the compute module\'s return are on AUX_GND, the ESP32 / CAN / rail side on GND, and only logic crosses, through 1k resistors.', 1.4)
r_('10k', '+3V3', 'SLOT_ID', 25, 45, 'Slot ID pull-up (the main board has one resistor to GND per slot)')
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', 25, 95, {'1': '+5V'}, show_value=False)
place('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', 40, 95, {'1': 'GND'}, show_value=False)
place('power:PWR_FLAG', '#FLG03', 'PWR_FLAG', '', 55, 95, {'1': 'AUX12'}, show_value=False)
place('power:PWR_FLAG', '#FLG04', 'PWR_FLAG', '', 70, 95, {'1': 'AUX_GND'}, show_value=False)

# --- 3.3 V
G('power')
text(15, 105, '3.3 V: AP7361C-33 (1 A) from the slot 5 V: the ESP32-S3 (about 150 mA with the radio unused), the MCP2562 I/O side, '
     'the RET buffer.', 1.4)
c_('10u', '+5V', 'GND', 25, 125, d='U3 in')
place(LDO, 'U3', 'AP7361C-33E', 'Package_TO_SOT_SMD:SOT-223-3_TabPin2', 55, 120, {'1': '+5V', '2': 'GND', '3': '+3V3'},
      descr='3.3 V 1 A LDO, from the slot 5 V')
c_('22u', '+3V3', 'GND', 85, 125, d='U3 out')

# --- ESP32-S3
G('esp')
# GPIO map (firmware/ai_card/src/ai_pins.h). Avoided: strapping pins GPIO0 (BOOT only), 3, 45, 46; GPIO26-37 (flash and octal PSRAM
# on the N16R8); GPIO19 / 20 are the native USB; GPIO43 / 44 (TXD0 / RXD0) the debug serial.
ESP_NETS = {
    'IO41': 'LINK_TX',      # UART1 TX -> the module's RXD (pin 10), through 1k
    'IO42': 'LINK_RX',      # UART1 RX <- the module's TXD (pin 8), through 1k
    'IO11': 'BUCK_ON',      # high enables the module's 5 V buck (fail-safe: low or high-Z = off)
    'IO12': 'SHUT',         # SHUTDOWN_REQ out
    'IO13': 'ALIVE',        # ALIVE in
    'IO14': 'ATTN',         # spare
    'IO9': 'CAN_TXD', 'IO10': 'CAN_RX',
    'IO4': 'SLOT_ID',       # ADC1_CH3
    'IO5': 'AUX_SENSE',     # ADC1_CH4: AUX 12 V through 220k / 47k (2.1 V at 12 V)
    'IO8': 'NTC',           # ADC1_CH7: the card's thermistor
    'IO6': 'BCK', 'IO7': 'WS', 'IO15': 'MIC',   # the rail, I2S slave (inputs)
    'IO16': 'RET_D', 'IO17': 'RET_OE_N',        # RET data out, the buffer's enable (low = drive)
    'IO1': 'LED_MOD', 'IO2': 'LED_MIC',
    'USB_D-': 'USB_DN', 'USB_D+': 'USB_DP',
    'IO0': 'BOOT', 'EN': 'EN', 'TXD0': 'TXD0', 'RXD0': 'RXD0', '3V3': '+3V3', 'GND': 'GND'}
_names = {v[3] for v in lib_pins[ESP].values()}
if 'USB_D-' not in _names:      # KiCad 7's stock symbol calls the USB pins IO19 / IO20
    ESP_NETS['IO19'], ESP_NETS['IO20'] = ESP_NETS.pop('USB_D-'), ESP_NETS.pop('USB_D+')
assert set(ESP_NETS) <= _names, set(ESP_NETS) - _names
en = {num: ESP_NETS.get(v[3]) for num, v in lib_pins[ESP].items()}
place(ESP, 'U1', 'ESP32-S3-WROOM-1U-N16R8', 'RF_Module:ESP32-S3-WROOM-1U', 230, 90, en,
      descr='Espressif ESP32-S3-WROOM-1U-N16R8 (16 MB flash, 8 MB octal PSRAM), IPEX connector only (the radio is not used), '
            'so no antenna keep-out: it sits under the compute module')
c_('22u', '+3V3', 'GND', 195, 140, d='Module supply, at pin 2')
c_('100n', '+3V3', 'GND', 210, 140, d='Module supply, at pin 2')
text(190, 40, 'ESP32-S3: UART1 to the compute module TX GPIO41, RX GPIO42 (through 1k). Power: BUCK_ON GPIO11 (enable of the module\'s buck), '
     'SHUT GPIO12 (SHUTDOWN_REQ), ALIVE GPIO13, ATTN GPIO14.\n'
     'I2S rail (slave): BCK GPIO6, WS GPIO7, MIC data GPIO15 in; RET data GPIO16 out through the 74LVC125 (enable GPIO17, low = drive).\n'
     'CAN (TWAI) TX GPIO9, RX GPIO10. Slot ID GPIO4 (ADC1), AUX 12 V present GPIO5 (ADC1), thermistor GPIO8 (ADC1). LEDs GPIO1 (module), GPIO2 (microphone open).\n'
     'Native USB GPIO19 / 20 (USB-C), debug serial TXD0 / RXD0. Not used: straps GPIO3 / 45 / 46, flash and PSRAM GPIO26-37.', 1.4)

G('boot')
text(15, 160, 'PROGRAMMING: USB-C (native USB, GPIO19 / 20: USB-serial-JTAG, no driver chip) with BOOT and RESET, and a debug serial header.\n'
     'The card is powered from the slot while flashing: USB VBUS only feeds the ESD clamp.', 1.4)
r_('10k', '+3V3', 'EN', 30, 185)
c_('1u', 'EN', 'GND', 45, 185)
place(SW, 'SW1', 'RESET', 'Button_Switch_SMD:SW_SPST_PTS810', 70, 185, {'1': 'EN', '2': 'GND'})
r_('10k', '+3V3', 'BOOT', 30, 210)
place(SW, 'SW2', 'BOOT', 'Button_Switch_SMD:SW_SPST_PTS810', 70, 210, {'1': 'BOOT', '2': 'GND'})
place(HDR3, 'J3', 'Debug serial', 'Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Horizontal', 110, 200,
      {'1': 'GND', '2': 'TXD0', '3': 'RXD0'},
      descr='Right angle, left edge: GND, TX0 (to the adapter RX), RX0 (from the adapter TX). 3.3 V logic. Not a supply')

G('usb')
place(USBC, 'J4', 'USB-C', 'Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal', 40, 260,
      {'A1': 'GND', 'A12': 'GND', 'B1': 'GND', 'B12': 'GND', 'A4': 'USB_VBUS', 'A9': None, 'B4': None,
       'B9': 'USB_VBUS', 'A5': 'USB_CC1', 'B5': 'USB_CC2', 'A6': 'USB_DP', 'B6': 'USB_DP', 'A7': 'USB_DN',
       'B7': 'USB_DN', 'A8': None, 'B8': None, 'S1': 'GND'},
      descr='USB 2.0 Type-C receptacle, GCT USB4105-xx-A (16-pin, horizontal, top mount) on the left edge. Only the A4 / B9 VBUS '
            'contacts are wired (to the ESD clamp): VBUS powers nothing here, and A9 / B4 sit 4.8 mm away across the signal pads')
r_('5.1k', 'USB_CC1', 'GND', 80, 250, 'CC1 pull-down: the card is a USB device (sink)')
r_('5.1k', 'USB_CC2', 'GND', 95, 250, 'CC2 pull-down')
place(ESD, 'U4', 'USBLC6-2SC6', 'Package_TO_SOT_SMD:SOT-23-6', 120, 275,
      {'1': 'USB_DP', '6': 'USB_DP', '3': 'USB_DN', '4': 'USB_DN', '2': 'GND', '5': 'USB_VBUS'},
      descr='USB ESD clamp on D+ / D-, at the connector')

# --- CAN
G('can')
text(15, 300, 'CAN: MCP2562 (VDD 5 V, VIO 3.3 V, STBY low). No 120 R: a card never terminates the bus.', 1.4)
place(CAN, 'U2', 'MCP2562-E/SN', 'Package_SO:SOIC-8_3.9x4.9mm_P1.27mm', 55, 335,
      {'1': 'CAN_TXD', '2': 'GND', '3': '+5V', '4': 'CAN_RXD', '5': '+3V3', '6': 'CANL', '7': 'CANH', '8': 'GND'})
r_('1k', 'CAN_RXD', 'CAN_RX', 95, 325, 'Series R on the receive line, as on the DigiDash board')
c_('100n', '+5V', 'GND', 115, 325, d='U2 VDD')
c_('100n', '+3V3', 'GND', 130, 325, d='U2 VIO')

# --- LEDs
G('led')
text(15, 360, 'LEDs: D1 green: the compute module is running. D2 amber: the microphone is open (listening).', 1.4)
r_('1k', 'LED_MOD', 'LED_MOD_A', 30, 380)
place(LED, 'D1', 'Module green', FP_LED, 50, 380, {'2': 'LED_MOD_A', '1': 'GND'}, descr='Compute module running')
r_('1k', 'LED_MIC', 'LED_MIC_A', 70, 380)
place(LED, 'D2', 'Mic amber', FP_LED, 90, 380, {'2': 'LED_MIC_A', '1': 'GND'}, descr='Microphone open')

# --- I2S rail
G('rail')
text(15, 410, 'I2S RAIL (slot pins 15-18; the audio card is the clock master). BCK / WS / MIC are inputs to the ESP32-S3 through 100 R. RET is a 3-state\n'
     'output: the 74LVC125\'s enable (GPIO17) is pulled high, so the card never drives RET until the firmware holds the grant. 33 R at the pin.', 1.4)
for k, (a, b) in enumerate((('RAIL_BCK', 'BCK'), ('RAIL_WS', 'WS'), ('RAIL_MIC', 'MIC'))):
    r_('100', a, b, 30 + 25 * k, 430, 'Series on the rail input ' + a[5:])
place(BUF, 'U6', '74LVC125APW', 'Package_SO:TSSOP-14_4.4x5mm_P0.65mm', 130, 440,
      {'1': 'RET_OE_N', '2': 'RET_D', '3': 'RET_DRV'}, unit=1, first=True,
      descr='Quad buffer, 3-state (active-low enables): gate 1 drives RET. The other three gates are tied off')
for unit, (oe, a, y) in enumerate((('4', '5', '6'), ('10', '9', '8'), ('13', '12', '11')), start=2):
    place(BUF, 'U6', '74LVC125APW', 'Package_SO:TSSOP-14_4.4x5mm_P0.65mm', 130 + 40 * (unit - 1), 440,
          {oe: '+3V3', a: 'GND', y: None}, unit=unit, first=False)
place(BUF, 'U6', '74LVC125APW', 'Package_SO:TSSOP-14_4.4x5mm_P0.65mm', 130, 480, {'7': 'GND', '14': '+3V3'}, unit=5, first=False)
c_('100n', '+3V3', 'GND', 160, 480, d='U6 decoupling')
r_('100k', '+3V3', 'RET_OE_N', 190, 460, 'RET buffer off until the firmware pulls GPIO17 low (the rail rule: off at power-up)')
r_('33', 'RET_DRV', 'RAIL_RET', 190, 440, 'RET series damping')

# --- compute module interface
G('mod')
text(300, 20, 'COMPUTE MODULE on a 2 x 20 female header (J2, 2.54 mm, 8.5 mm insulator), Raspberry Pi 40-pin map (a Pi-compatible module: reference Radxa Zero 3W).\n'
     'Pins 2 / 4: 5 V in from the buck (MOD_5V). GND pins 6, 9, 14, 20, 25, 30, 34, 39: AUX_GND. UART: pin 8 module TXD, pin 10 module RXD, to the ESP32 through 1k.\n'
     'Pin 29 ALIVE (module out), 31 SHUTDOWN_REQ (module in), 33 ATTN (spare). Pins 1 / 17 (the module\'s 3.3 V) are not used. The ESP32 never drives\n'
     'a line into a module that is off: 1k in series, and the firmware holds its outputs low while the buck is off.', 1.4)
place(HDR2x20, 'J2', 'Compute module', 'Connector_PinSocket_2.54mm:PinSocket_2x20_P2.54mm_Vertical', 330, 80,
      {str(k): v for k, v in HEADER_NETS.items()},
      descr='Female socket 2 x 20, 2.54 mm, 8.5 mm insulator: the compute module plugs in here. Raspberry Pi 40-pin pinout')
r_('1k', 'LINK_TX', 'MOD_RXD', 400, 70, 'Link TX series (the module may be off)')
r_('1k', 'MOD_TXD', 'LINK_RX', 420, 70, 'Link RX series')
r_('1k', 'MOD_ALIVE', 'ALIVE', 440, 70, 'ALIVE series')
r_('100k', 'ALIVE', 'GND', 460, 70, 'ALIVE low when the module is off')
r_('1k', 'SHUT', 'MOD_SHUT', 400, 100, 'SHUTDOWN_REQ series')
r_('100k', 'SHUT', 'GND', 420, 100, 'SHUTDOWN_REQ low while the ESP32 resets')
r_('1k', 'MOD_ATTN', 'ATTN', 440, 100, 'ATTN series')
r_('100k', 'ATTN', 'GND', 460, 100, 'ATTN low when the module is off')
place(CP, nid('C'), '470u 10V', 'Capacitor_SMD:CP_Elec_8x10', 400, 135, {'1': 'MOD_5V', '2': 'AUX_GND'},
      descr='Bulk at the module\'s 5 V (CPU load steps): aluminium, 10 V or more, low ESR, 8 x 10 mm')
c_('100n', 'MOD_5V', 'AUX_GND', 420, 135, d='Module 5 V, HF')
text(400, 155, 'AUX PRESENT: AUX 12 V -> 220k / 47k -> GPIO5 (2.1 V at 12 V; the firmware starts the module above about 1 V = 6.4 V). It must read the buck\'s INPUT: the buck is off until the firmware sees AUX.', 1.4)
G('buck')
r_('220k', 'AUX12', 'AUX_SENSE', 400, 170, 'AUX present divider top (on the AUX 12 V side, in the buck area)')
G('mod')
r_('47k', 'AUX_SENSE', 'GND', 420, 170, 'AUX present divider bottom')
c_('100n', 'AUX_SENSE', 'GND', 440, 170, d='AUX present filter')

# --- thermistor
G('therm')
text(300, 190, 'CARD TEMPERATURE: 10k NTC (beta 3950) to GND, 10k to 3.3 V, GPIO8 (ADC1). The ESP32 stops the module at 80 C and restarts it under 65 C.', 1.4)
r_('10k', '+3V3', 'NTC', 330, 205, 'NTC divider top')
place(NTC, 'RT1', '10k NTC', FP_R, 350, 205, {'1': 'NTC', '2': 'GND'}, descr='Card thermistor, 10k at 25 C, beta 3950, 0603, beside the module\'s heat source')
c_('100n', 'NTC', 'GND', 370, 205, d='NTC filter')

# --- module 5 V buck (AUX domain: every ground here is AUX_GND), switched by the ESP32
G('buck')
text(300, 225, 'MODULE 5 V: TPS54560B from AUX 12 V (slot pins 25-27), the main board\'s 5 V circuit (TI datasheet typical application,\n'
     '400 kHz, 5.0 V) with a smaller inductor (6.8 uH, 7 x 7 mm). Load: the compute module, 1-1.5 A typical, 3 A peak.\n'
     'ENABLE: the EN divider (442k / 90.9k, starts above about 6.5 V, 5.1 V zener) is fed from AUX 12 V through a P-FET (Q1) that the ESP32 switches\n'
     'with GPIO11 through Q2. AUX_SENSE reads the buck\'s input (220k / 47k), never its output. Fail-safe: with the ESP32 off, in reset or unpowered the FET is off and the buck is disabled.', 1.4)
for k in range(3):
    c_('2.2u 100V', 'AUX12', 'AUX_GND', 300 + 15 * k, 255, d='Buck input, at VIN (C1-C3 in the TI example)')
c_('100n', 'AUX12', 'AUX_GND', 345, 255, fp=FP_C0805, d='Buck input, HF (100 V)')
place(BUCK, 'U5', 'TPS54560BDDA', 'Package_SO:TI_SO-PowerPAD-8_ThermalVias', 390, 270,
      {'1': 'BUCK_BOOT', '2': 'AUX12', '3': 'BUCK_EN', '4': 'BUCK_RT', '5': 'BUCK_FB', '6': 'BUCK_COMP',
       '7': 'AUX_GND', '8': 'BUCK_SW', '9': 'AUX_GND'}, descr='5 V step-down, 4.5-60 V in, 5 A switch')
c_('100n', 'BUCK_BOOT', 'BUCK_SW', 430, 250, d='Bootstrap (C4)')
r_('243k', 'BUCK_RT', 'AUX_GND', 360, 290, 'RT: 400 kHz (R3)')
r_('442k', 'EN_SRC', 'BUCK_EN', 300, 280, 'EN divider top (R1): starts above about 6.5 V')
r_('90.9k', 'BUCK_EN', 'AUX_GND', 315, 280, 'EN divider bottom (R2)')
place(DZ, 'D4', 'BZT52C5V1', 'Diode_SMD:D_SOD-123', 330, 290, {'1': 'BUCK_EN', '2': 'AUX_GND'}, descr='EN clamp, 5.1 V')
place(QP, 'Q1', 'DMP6350S', FP_SOT23, 300, 310, {'1': 'Q1_G', '2': 'AUX12', '3': 'EN_SRC'},
      descr='P-FET (60 V, SOT-23): connects AUX 12 V to the EN divider when its gate is pulled down by Q2. Vgs is limited by the 47k / 47k divider')
r_('47k', 'AUX12', 'Q1_G', 320, 310, 'Q1 gate pull-up (off by default)')
r_('47k', 'Q1_G', 'Q2_D', 340, 310, 'Q1 gate pull-down through Q2: Vgs = half of AUX 12 V (7 V at 14.4 V)')
place(QN, 'Q2', '2N7002', FP_SOT23, 360, 310, {'1': 'BUCK_ON_G', '2': 'AUX_GND', '3': 'Q2_D'}, descr='N-FET: GPIO11 high turns the module\'s buck on')
r_('1k', 'BUCK_ON', 'BUCK_ON_G', 380, 310, 'GPIO11 to Q2 gate')
r_('100k', 'BUCK_ON_G', 'AUX_GND', 400, 310, 'Q2 gate low while the ESP32 is off or resetting')
place(L, 'L1', '6.8u', 'Inductor_SMD:L_Bourns_SRP7028A_7.3x6.6mm', 450, 265, {'1': 'BUCK_SW', '2': 'MOD_5V'},
      descr='6.8 uH shielded, 7 x 7 mm, Isat >= 5 A, Irms >= 3 A (Bourns SRP7028A-6R8M or equal: check the ratings)')
place(DS, 'D3', 'B360B', 'Diode_SMD:D_SMB', 430, 290, {'1': 'BUCK_SW', '2': 'AUX_GND'},
      descr='Catch diode, 3 A 60 V Schottky, SMB')
for k in range(3):
    c_('47u 10V', 'MOD_5V', 'AUX_GND', 475 + 15 * k, 275, d='Buck output (C6, C7, C9)')
r_('53.6k', 'MOD_5V', 'BUCK_FB', 420, 325, 'Feedback top (R5): 5.0 V')
r_('10.2k', 'BUCK_FB', 'AUX_GND', 435, 325, 'Feedback bottom (R6)')
r_('16.9k', 'BUCK_COMP', 'BUCK_CZ', 360, 330, 'Compensation (R4)')
c_('4.7n', 'BUCK_CZ', 'AUX_GND', 375, 330, d='Compensation (C5)')
c_('47p', 'BUCK_COMP', 'AUX_GND', 390, 330, d='Compensation (C8)')
place('power:PWR_FLAG', '#FLG05', 'PWR_FLAG', '', 500, 300, {'1': 'MOD_5V'}, show_value=False)

# --- mounting
G('mech')
text(300, 350, 'MOUNTING: the compute module is held by two M2.5 standoffs (the 2 x 20 socket alone walks out of a vibrating dash). Hole positions are PROVISIONAL.', 1.4)
place(HOLE, 'H1', 'M2.5', 'MountingHole:MountingHole_2.7mm_M2.5', 300, 370, {})
place(HOLE, 'H2', 'M2.5', 'MountingHole:MountingHole_2.7mm_M2.5', 320, 370, {})

# ----------------------------------------------------------------------------- write
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "A2")',
       '  (title_block (title "DigiDash AI voice card") (date "2026-10-04") (rev "A")',
       '    (comment 1 "ESP32-S3-WROOM-1U-N16R8 front end + compute module on a 2x20 header + MCP2562, module 5 V buck from AUX 12 V")',
       '    (comment 2 "Card for a DigiDash slot (2x15, on edge), any slot. Compute module outline and holes PROVISIONAL"))',
       '  (lib_symbols']
for t in lib_text.values():
    out.append('\n'.join('    ' + ln for ln in t.split('\n')))
out.append('  )')
out += body
out += ['  (sheet_instances (path "/" (page "1")))', ')']
open(OUT, 'w').write('\n'.join(out) + '\n')
import json  # noqa: E402
json.dump(refs_info, open(os.path.join(os.path.dirname(OUT), 'generate', 'refs.json'), 'w'), indent=1, sort_keys=True)
print('wrote', os.path.normpath(OUT), len(body), 'items', len(refs_info), 'parts')
