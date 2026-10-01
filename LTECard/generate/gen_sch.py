#!/usr/bin/env python3
"""Generate LTECard.kicad_sch (KiCad 7 file format, opens in 7/8/9/10). Same pattern as AudioCarrier/generate.

Needs the stock KiCad symbol libraries (KICAD_SYMBOLS, default /usr/share/kicad/symbols).
Every connection is a net label or power symbol on a short stub at the pin, so the file is
a netlist you can read; move things around in eeschema as you like.

The LTE core board's pin order is NOT measured yet: CORE_ROW_A / CORE_ROW_B below are placeholders from the vendor
listings (docs/hardware/lte_card.md). Check them against the board in hand, edit, and re-run gen_sch.py and gen_pcb.py.
"""
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, grab_symbol  # noqa: E402

# ----------------------------------------------------------------------------- LTE core board (PROVISIONAL)
# SIMCom A7670 core board: 23 pins at 2.54 mm on two rows down its short edges (the vendor listings; not measured).
# Pin 1 of each row is at the top (the card's top edge). Row A (12 pins) is on the left, toward the ESP32 and the
# buck; row B (11 pins) on the right. The ORDER IS A GUESS: measure the board, then edit these two lists (the
# geometry is in gen_pcb.py: CORE_* constants). Names are the board's silkscreen names.
CORE_ROW_A = ['VIN', 'VIN', 'GND', 'VBTA', '5V', 'VEXT', 'PEN', 'RST', 'STA', 'NET', 'IO0', 'IO1']
CORE_ROW_B = ['GND', 'VTTL', 'TXD', 'RXD', 'RTS', 'CTS', 'DTR', 'RI', 'SR', 'SD', 'SC']
# What each core-board pin connects to on the card (None = not connected). TXD / RXD / RTS / CTS / DTR / RI are named
# from the module (TXD is its output); STA / NET / PEN / RST go through 1k series resistors (their levels are not
# documented). No pull-ups on the UART lines: the board's TXS0108E level shifter does not like them.
CORE_NETS = {'VIN': 'LTE_5V', 'GND': 'AUX_GND', 'VTTL': '+3V3', 'TXD': 'LTE_TXD', 'RXD': 'LTE_RXD', 'RTS': 'LTE_RTS',
             'CTS': 'LTE_CTS', 'DTR': 'LTE_DTR', 'RI': 'LTE_RI', 'PEN': 'LTE_PEN', 'RST': 'LTE_RST', 'STA': 'LTE_STA',
             'NET': 'LTE_NET', 'VBTA': None, 'VEXT': None, '5V': None, 'IO0': None, 'IO1': None, 'SR': None, 'SD': None,
             'SC': None}
assert len(CORE_ROW_A) + len(CORE_ROW_B) == 23 and set(CORE_ROW_A + CORE_ROW_B) == set(CORE_NETS)

SYMDIR = os.environ.get('KICAD_SYMBOLS', next((d for d in ('/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols',
                                                               '/usr/share/kicad/symbols') if os.path.isdir(d)), ''))
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'LTECard.kicad_sch')
NS = uuid.UUID('3f1c2d64-8a7e-4c55-b0e2-6c1a7d2e4b10')
SHEET = str(uuid.uuid5(NS, 'sheet'))
PROJECT = 'LTECard'
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
        lib_id = 'LTECard:' + as_name
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
HDR2x15 = stock('Connector_Generic', 'Conn_02x15_Odd_Even')
HDR = {n: stock('Connector_Generic', 'Conn_01x%02d' % n) for n in (3, 11, 12)}
SW = stock('Switch', 'SW_Push')
CAN = stock('Interface_CAN_LIN', 'MCP2562-E-SN')
ESP = stock('RF_Module', 'ESP32-S3-WROOM-1')
USBC = stock('Connector', 'USB_C_Receptacle_USB2.0_16P')
ESD = stock('Power_Protection', 'USBLC6-2P6', 'USBLC6-2SC6', 'USBLC6-2SC6')
BUCK = stock('Regulator_Switching', 'TPS54560BDDA')
LDO = stock('Regulator_Linear', 'SPX2920M3-3.3_SOT223', 'AP7361C-33E', 'AP7361C-33E')
for n in ('GND', '+5V', '+3V3', 'PWR_FLAG'):
    stock('power', n)

FP_R = 'Resistor_SMD:R_0603_1608Metric'
FP_C = 'Capacitor_SMD:C_0603_1608Metric'
FP_C0805 = 'Capacitor_SMD:C_0805_2012Metric'
FP_C1210 = 'Capacitor_SMD:C_1210_3225Metric'
FP_LED = 'LED_SMD:LED_0603_1608Metric'

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


text(15, 12, 'LTE GATEWAY CARD  -  ESP32-S3-WROOM-1-N16R8 + SIMCom A7670 core board (Cat-1) + MCP2562 CAN', 3)
text(15, 20, 'Card for a DigiDash slot, 80 x 60 mm, standing on edge (docs/hardware/accessory_ports.md, docs/hardware/lte_card.md). '
     'Any slot. CAN node 0x06. ESP32 and CAN from the slot 5 V; the LTE board from AUX 12 V through its own buck.', 1.6)

# --- slot
G('port')
place(HDR2x15, 'J1', 'DigiDash slot', 'Connector_PinHeader_2.54mm:PinHeader_2x15_P2.54mm_Horizontal', 45, 60,
      {'1': '+5V', '2': '+5V', '3': 'GND', '4': 'GND', '5': 'CANH', '6': 'CANL', '7': 'GND', '8': None,
       '9': None, '10': None, '11': 'SLOT_ID', '12': None, '13': None, '14': None,
       '15': None, '16': None, '17': None, '18': None, '19': 'GND', '20': 'GND',
       '21': None, '22': None, '23': None, '24': None,
       '25': 'AUX12', '26': 'AUX12', '27': 'AUX12', '28': 'AUX_GND', '29': 'AUX_GND', '30': 'AUX_GND'},
      descr='Right-angle 2x15 header on the bottom edge (the slot pinout, docs/hardware/accessory_ports.md). Used: 5 V, GND, CAN, '
            'slot ID, AUX 12 V / AUX GND (25-30). Not used: switched / constant 12 V, 3.3 V, WAKE, LAMP, REV, links, audio bus')
text(75, 40, 'Slot ID (pin 11): 10k to 3.3 V here, the slot\'s own resistor to GND on the main board, read on GPIO4 (ADC1).\n'
     'AUX GND (28-30) is NOT joined to GND on this card: on the main board the two meet only at NT1, at the power entry.\n'
     'Joining them here too would put a loop through NT1 and send AUX return current (this card\'s and other cards\') through the logic ground.\n'
     'So the LTE side (buck, 470 uF, core board) is all on AUX_GND, the ESP32 / CAN side on GND, and only logic signals cross between them.', 1.4)
r_('10k', '+3V3', 'SLOT_ID', 25, 45, 'Slot ID pull-up (the main board has one resistor to GND per slot)')
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', 25, 95, {'1': '+5V'}, show_value=False)
place('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', 40, 95, {'1': 'GND'}, show_value=False)
place('power:PWR_FLAG', '#FLG03', 'PWR_FLAG', '', 55, 95, {'1': 'AUX12'}, show_value=False)
place('power:PWR_FLAG', '#FLG04', 'PWR_FLAG', '', 70, 95, {'1': 'AUX_GND'}, show_value=False)

# --- 3.3 V
G('power')
text(15, 105, '3.3 V: AP7361C-33 (1 A) from the slot 5 V: the ESP32-S3 (about 500 mA peak with Wi-Fi), the MCP2562 I/O side, '
     'the core board\'s level-shifter reference (VTTL).', 1.4)
c_('10u', '+5V', 'GND', 25, 125, d='U3 in')
place(LDO, 'U3', 'AP7361C-33E', 'Package_TO_SOT_SMD:SOT-223-3_TabPin2', 55, 120, {'1': '+5V', '2': 'GND', '3': '+3V3'},
      descr='3.3 V 1 A LDO, from the slot 5 V')
c_('22u', '+3V3', 'GND', 85, 125, d='U3 out')

# --- ESP32-S3
G('esp')
# GPIO map. Avoided: strapping pins GPIO0 (BOOT only), 3, 45, 46; GPIO35-37 (octal PSRAM on the N16R8);
# GPIO19 / 20 are the native USB; GPIO43 / 44 (TXD0 / RXD0) the debug serial.
ESP_NETS = {
    'IO41': 'LTE_RXD',      # UART1 TX  -> core board RXD
    'IO42': 'LTE_TXD',      # UART1 RX  <- core board TXD
    'IO40': 'LTE_RTS',      # UART1 RTS -> core board RTS (module input)
    'IO39': 'LTE_CTS',      # UART1 CTS <- core board CTS (module output)
    'IO38': 'LTE_DTR',      # DTR, held low (awake)
    'IO21': 'LTE_RI',       # RI in (can wake the ESP32: RTC-capable is not needed, GPIO wake works from light sleep)
    'IO11': 'PEN_IO',       # PEN out (through 1k), 100k pull-down: LTE off until the firmware raises it
    'IO12': 'RST_IO',       # RST out (through 1k)
    'IO13': 'STA_IO',       # STA in (through 1k)
    'IO14': 'NET_IO',       # NET in (through 1k)
    'IO9': 'CAN_TXD', 'IO10': 'CAN_RX',
    'IO4': 'SLOT_ID',       # ADC1_CH3
    'IO5': 'AUX_SENSE',     # ADC1_CH4: LTE 5 V through 22k / 33k (3.0 V when the buck runs)
    'IO1': 'LED_LTE', 'IO2': 'LED_WIFI',
    'USB_D-': 'USB_DN', 'USB_D+': 'USB_DP',
    'IO0': 'BOOT', 'EN': 'EN', 'TXD0': 'TXD0', 'RXD0': 'RXD0', '3V3': '+3V3', 'GND': 'GND'}
en = {}
for num, (px, py, ang, nm, typ, u) in lib_pins[ESP].items():
    en[num] = ESP_NETS.get(nm)
assert set(ESP_NETS) <= {v[3] for v in lib_pins[ESP].values()}
place(ESP, 'U1', 'ESP32-S3-WROOM-1-N16R8', 'RF_Module:ESP32-S3-WROOM-1', 230, 90, en,
      descr='Espressif ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM), PCB antenna on the top edge at the left corner')
c_('22u', '+3V3', 'GND', 195, 140, d='Module supply, at pin 2')
c_('100n', '+3V3', 'GND', 210, 140, d='Module supply, at pin 2')
text(190, 40, 'ESP32-S3: UART1 to the LTE board TX GPIO41, RX GPIO42, RTS GPIO40, CTS GPIO39 (GPIO matrix); DTR GPIO38 (out), RI GPIO21 (in).\n'
     'LTE control through 1k: PEN GPIO11 (out, 100k pull-down), RST GPIO12 (out), STA GPIO13 (in), NET GPIO14 (in).\n'
     'CAN (TWAI) TX GPIO9, RX GPIO10. Slot ID GPIO4 (ADC1), AUX present GPIO5 (ADC1). LEDs GPIO1 (LTE), GPIO2 (Wi-Fi).\n'
     'Native USB GPIO19 / 20 (USB-C), debug serial TXD0 / RXD0. Not used: straps GPIO3 / 45 / 46, PSRAM GPIO35-37.', 1.4)

G('boot')
text(15, 160, 'PROGRAMMING: USB-C (native USB, GPIO19 / 20: USB-serial-JTAG, no driver chip) with BOOT and RESET, and a debug serial header.\n'
     'The card is powered from the slot while flashing: USB VBUS only feeds the ESD clamp.', 1.4)
r_('10k', '+3V3', 'EN', 30, 185)
c_('1u', 'EN', 'GND', 45, 185)
place(SW, 'SW1', 'RESET', 'Button_Switch_SMD:SW_SPST_PTS810', 70, 185, {'1': 'EN', '2': 'GND'})
r_('10k', '+3V3', 'BOOT', 30, 210)
place(SW, 'SW2', 'BOOT', 'Button_Switch_SMD:SW_SPST_PTS810', 70, 210, {'1': 'BOOT', '2': 'GND'})
place(HDR[3], 'J5', 'Debug serial', 'Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Horizontal', 110, 200,
      {'1': 'GND', '2': 'TXD0', '3': 'RXD0'},
      descr='Right angle, left edge: GND, TX0 (to the adapter RX), RX0 (from the adapter TX). 3.3 V logic. Not a supply')

G('usb')
place(USBC, 'J4', 'USB-C', 'Connector_USB:USB_C_Receptacle_Amphenol_12401948E412A', 40, 260,
      {'A1': 'GND', 'A12': 'GND', 'B1': 'GND', 'B12': 'GND', 'A4': 'USB_VBUS', 'A9': 'USB_VBUS', 'B4': 'USB_VBUS',
       'B9': 'USB_VBUS', 'A5': 'USB_CC1', 'B5': 'USB_CC2', 'A6': 'USB_DP', 'B6': 'USB_DP', 'A7': 'USB_DN',
       'B7': 'USB_DN', 'A8': None, 'B8': None, 'SH': 'GND'},
      descr='USB 2.0 Type-C receptacle, mid-mount (sits in a notch in the left edge), Amphenol 12401948E412A')
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
text(15, 360, 'LEDs: D1 LTE (off / searching / data up), D2 Wi-Fi (access point on, blinks with clients).', 1.4)
r_('1k', 'LED_LTE', 'LED_LTE_A', 30, 380)
place(LED, 'D1', 'LTE green', FP_LED, 50, 380, {'2': 'LED_LTE_A', '1': 'GND'}, descr='LTE status LED')
r_('1k', 'LED_WIFI', 'LED_WIFI_A', 70, 380)
place(LED, 'D2', 'Wi-Fi blue', FP_LED, 90, 380, {'2': 'LED_WIFI_A', '1': 'GND'}, descr='Wi-Fi access point LED')

# --- LTE 5 V buck (AUX domain: every ground here is AUX_GND)
G('buck')
text(300, 220, 'LTE 5 V: TPS54560B from AUX 12 V (slot pins 25-27), the main board\'s 5 V circuit (TI datasheet typical application,\n'
     '400 kHz, 5.0 V) with a smaller inductor (6.8 uH, 7 x 7 mm). Load: the core board, about 2 A in LTE transmit bursts.\n'
     'All of its grounds are AUX_GND (slot pins 28-30), the AUX return: the LTE current never flows in the logic ground.\n'
     'EN from AUX through 442k / 90.9k: starts above about 6.5 V, clamped by a 5.1 V zener (EN abs. max 8.4 V).', 1.4)
for k in range(3):
    c_('2.2u 100V', 'AUX12', 'AUX_GND', 300 + 15 * k, 255, d='Buck input, at VIN (C1-C3 in the TI example)')
c_('100n', 'AUX12', 'AUX_GND', 345, 255, fp=FP_C0805, d='Buck input, HF (100 V)')
place(BUCK, 'U5', 'TPS54560BDDA', 'Package_SO:TI_SO-PowerPAD-8_ThermalVias', 390, 270,
      {'1': 'BUCK_BOOT', '2': 'AUX12', '3': 'BUCK_EN', '4': 'BUCK_RT', '5': 'BUCK_FB', '6': 'BUCK_COMP',
       '7': 'AUX_GND', '8': 'BUCK_SW', '9': 'AUX_GND'}, descr='5 V step-down, 4.5-60 V in, 5 A switch')
c_('100n', 'BUCK_BOOT', 'BUCK_SW', 430, 250, d='Bootstrap (C4)')
r_('243k', 'BUCK_RT', 'AUX_GND', 360, 290, 'RT: 400 kHz (R3)')
r_('442k', 'AUX12', 'BUCK_EN', 300, 280, 'EN divider top (R1): starts above about 6.5 V')
r_('90.9k', 'BUCK_EN', 'AUX_GND', 315, 280, 'EN divider bottom (R2)')
place(DZ, 'D4', 'BZT52C5V1', 'Diode_SMD:D_SOD-123', 330, 290, {'1': 'BUCK_EN', '2': 'AUX_GND'}, descr='EN clamp, 5.1 V')
place(L, 'L1', '6.8u', 'Inductor_SMD:L_Bourns_SRP7028A_7.3x6.6mm', 450, 265, {'1': 'BUCK_SW', '2': 'LTE_5V'},
      descr='6.8 uH shielded, 7 x 7 mm, Isat >= 5 A, Irms >= 3 A (Bourns SRP7028A-6R8M or equal: check the ratings)')
place(DS, 'D3', 'B360B', 'Diode_SMD:D_SMB', 430, 290, {'1': 'BUCK_SW', '2': 'AUX_GND'},
      descr='Catch diode, 3 A 60 V Schottky, SMB')
for k in range(3):
    c_('47u 10V', 'LTE_5V', 'AUX_GND', 475 + 15 * k, 275, d='Buck output (C6, C7, C9)')
r_('53.6k', 'LTE_5V', 'BUCK_FB', 420, 310, 'Feedback top (R5): 5.0 V')
r_('10.2k', 'BUCK_FB', 'AUX_GND', 435, 310, 'Feedback bottom (R6)')
r_('16.9k', 'BUCK_COMP', 'BUCK_CZ', 360, 310, 'Compensation (R4)')
c_('4.7n', 'BUCK_CZ', 'AUX_GND', 375, 310, d='Compensation (C5)')
c_('47p', 'BUCK_COMP', 'AUX_GND', 390, 310, d='Compensation (C8)')
place('power:PWR_FLAG', '#FLG05', 'PWR_FLAG', '', 500, 300, {'1': 'LTE_5V'}, show_value=False)

# --- LTE core board
G('lte')
text(300, 40, 'LTE CORE BOARD (SIMCom A7670, Cat-1) on two female sockets, 2.54 mm, 8.5 mm tall: J2 1 x 12 (row A, left), J3 1 x 11 (row B, right).\n'
     'PIN ORDER AND ROW SPACING ARE PROVISIONAL (from the vendor listings): measure the board, then edit CORE_ROW_A / CORE_ROW_B in gen_sch.py\n'
     'and the CORE_* constants in gen_pcb.py. VIN (5-16 V) <- LTE_5V from the card\'s buck, + 470 uF bulk at the socket. VTTL = 3.3 V\n'
     '(the board\'s TXS0108E reference). GND = AUX_GND. VBTA, VEXT, 5V, IO0, IO1, SR, SD, SC not connected.', 1.4)
place(HDR[12], 'J2', 'LTE core row A', 'Connector_PinSocket_2.54mm:PinSocket_1x12_P2.54mm_Vertical', 320, 110,
      {str(k + 1): CORE_NETS[n] for k, n in enumerate(CORE_ROW_A)},
      descr='Female socket 1 x 12, 2.54 mm, 8.5 mm insulator. Row A of the LTE core board: ' + ' '.join(CORE_ROW_A) + ' (provisional order)')
place(HDR[11], 'J3', 'LTE core row B', 'Connector_PinSocket_2.54mm:PinSocket_1x11_P2.54mm_Vertical', 380, 110,
      {str(k + 1): CORE_NETS[n] for k, n in enumerate(CORE_ROW_B)},
      descr='Female socket 1 x 11, 2.54 mm, 8.5 mm insulator. Row B of the LTE core board: ' + ' '.join(CORE_ROW_B) + ' (provisional order)')
text(310, 75, 'Row A: ' + ', '.join('%d %s' % (k + 1, n) for k, n in enumerate(CORE_ROW_A)), 1.27)
text(310, 79, 'Row B: ' + ', '.join('%d %s' % (k + 1, n) for k, n in enumerate(CORE_ROW_B)), 1.27)
place(CP, nid('C'), '470u 10V', 'Capacitor_SMD:CP_Elec_8x10', 430, 120, {'1': 'LTE_5V', '2': 'AUX_GND'},
      descr='Bulk at the core board VIN (LTE transmit bursts about 2 A): aluminium, 10 V or more, low ESR, 8 x 10 mm')
c_('100n', '+3V3', 'GND', 445, 120, d='VTTL, at the socket')
r_('1k', 'PEN_IO', 'LTE_PEN', 420, 150, 'PEN series (level and pull-up on the core board not documented)')
r_('100k', 'PEN_IO', 'GND', 420, 170, 'PEN held low while the ESP32 is in reset: the LTE board stays off')
r_('1k', 'RST_IO', 'LTE_RST', 440, 150, 'RST series')
r_('1k', 'LTE_STA', 'STA_IO', 460, 150, 'STA series (input)')
r_('1k', 'LTE_NET', 'NET_IO', 480, 150, 'NET series (input)')
text(410, 185, 'AUX PRESENT: LTE_5V -> 22k / 33k -> GPIO5 (3.0 V while the buck runs). Tells "no AUX power" from "no LTE signal".', 1.4)
r_('22k', 'LTE_5V', 'AUX_SENSE', 420, 200, 'AUX present divider top')
r_('33k', 'AUX_SENSE', 'GND', 440, 200, 'AUX present divider bottom')
c_('100n', 'AUX_SENSE', 'GND', 460, 200, d='AUX present filter')

# ----------------------------------------------------------------------------- write
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "A2")',
       '  (title_block (title "DigiDash LTE gateway card") (date "2026-10-01") (rev "A")',
       '    (comment 1 "ESP32-S3-WROOM-1-N16R8 + SIMCom A7670 core board on sockets + MCP2562, LTE 5 V buck from AUX 12 V")',
       '    (comment 2 "Card for a DigiDash slot (2x15, on edge), any slot. Core board pin order PROVISIONAL"))',
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
