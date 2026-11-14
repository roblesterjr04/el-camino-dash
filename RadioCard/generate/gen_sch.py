#!/usr/bin/env python3
"""Generate RadioCard.kicad_sch (KiCad 7 file format, opens in 7/8/9/10).

Same pattern as GPSCard/generate/gen_sch.py. Needs the stock KiCad symbol libraries (KICAD_SYMBOLS, default
/Applications/KiCad/... or /usr/share/kicad/symbols). Every connection is a net label or power symbol on a short stub
at the pin, so the file is a netlist you can read; move things around in eeschema as you like.

Spec: docs/hardware/radio_card.md. Slot pinout: docs/hardware/accessory_ports.md (rev 2).
"""
import json
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import grab_symbol  # noqa: E402
from sexp import parse  # noqa: E402

SYMDIR = os.environ.get('KICAD_SYMBOLS', next((d for d in ('/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols',
                                                               '/usr/share/kicad/symbols') if os.path.isdir(d)), ''))
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'RadioCard.kicad_sch')
PROJECT = 'RadioCard'
NS = uuid.UUID('7a1c4e92-5d3b-4c8a-b6f0-2e9d1a3c5b77')
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
lib_pins = {}   # lib_id -> {number: (x, y, angle, name, type, unit)}
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
    assert '(extends' not in txt, (lib, name, 'use stock_as for a derived symbol')
    lib_id = '%s:%s' % (lib, name)
    txt = txt.strip()
    txt = re.sub(r'^\(symbol "%s"' % re.escape(name), '(symbol "%s"' % lib_id, txt, count=1)
    assert txt.startswith('(symbol "%s"' % lib_id), lib_id
    register(lib_id, txt)
    return lib_id


def stock_as(lib, name, new_id, value):
    """Use a stock symbol under another lib id / value. A derived symbol ('extends') is flattened: its parent's body
    is used, renamed (the schematic's lib_symbols cannot hold a derived symbol without its parent)."""
    path = os.path.join(SYMDIR, lib + '.kicad_sym')
    txt = grab_symbol(path, name).strip()
    m = re.search(r'\(extends "([^"]+)"\)', txt)
    if m:
        name = m.group(1)
        txt = grab_symbol(path, name).strip()
    short = new_id.split(':')[1]
    txt = txt.replace('(symbol "%s"' % name, '(symbol "%s"' % new_id, 1)
    txt = txt.replace('"%s_' % name, '"%s_' % short)
    txt = re.sub(r'(\(property "Value"\s+)"[^"]*"', r'\1"%s"' % value, txt, count=1)
    register(new_id, txt)
    return new_id


# ----------------------------------------------------------------------------- placement
body = []
pwr_n = [0]
refs_seen = set()
refs_info = {}
netmap = {}
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


def place(lib_id, ref, value, footprint, x, y, nets, descr=None, show_value=True, unit=1, first=True, dnp=False):
    """nets: {pin number: net name | None (unconnected -> no-connect flag)}; every pin must be listed."""
    if first:
        assert ref not in refs_seen, ref
        refs_seen.add(ref)
        refs_info[ref] = {'group': cur_group[0], 'fp': footprint, 'value': value, 'dnp': dnp}
    pins = {k: v for k, v in lib_pins[lib_id].items() if v[5] in (0, unit)}
    props = lib_props[lib_id]

    def pos(k, dflt):
        px, py = props.get(k, dflt)
        return f(x + px), f(y - py)

    rp, vp = pos('Reference', (0, 3)), pos('Value', (0, -3))
    lines = ['  (symbol (lib_id "%s") (at %s %s 0) (unit %d) (in_bom yes) (on_board yes) (dnp %s) (uuid "%s")' % (
        lib_id, f(x), f(y), unit, 'yes' if dnp else 'no', uid('s' + ref + str(unit))),
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
    for num, net in nets.items():
        if net is not None:
            netmap.setdefault(net, []).append('%s.%s' % (ref, num))
    missing = set(pins) - set(nets)
    assert not missing, (ref, unit, missing)


def text(x, y, s, size=1.6):
    s = s.replace('"', "'").replace('\n', '\\n')
    body.append('  (text "%s" (at %s %s 0) (effects (font (size %s %s)) (justify left top)) (uuid "%s"))' % (
        s, f(x), f(y), f(size), f(size), uid('t')))


# ----------------------------------------------------------------------------- parts
R = stock('Device', 'R')
C = stock('Device', 'C')
LED = stock('Device', 'LED')
FB = stock('Device', 'FerriteBead_Small')
TVS = stock('Device', 'D_TVS')
IND = stock('Device', 'L')
OSC = stock('Oscillator', 'ASE-xxxMHz')
HDR2x15 = stock('Connector_Generic', 'Conn_02x15_Odd_Even')
HDR1x5 = stock('Connector_Generic', 'Conn_01x05')
HDR1x3 = stock('Connector_Generic', 'Conn_01x03')
HDR1x2 = stock('Connector_Generic', 'Conn_01x02')
COAX = stock('Connector', 'Conn_Coaxial')
SJ3 = stock_as('Jumper', 'SolderJumper_3_Bridged12', 'RadioCard:SolderJumper_3_Bridged12', 'Tuner address')
SJ2 = stock_as('Jumper', 'SolderJumper_2_Open', 'RadioCard:SolderJumper_2_Open', 'AM from whip')
CAN = stock('Interface_CAN_LIN', 'MCP2562-E-SN')
LDO = stock_as('Regulator_Linear', 'AP2112K-3.3', 'RadioCard:AP2112K-3.3', 'AP2112K-3.3')
MCU = stock_as('MCU_ST_STM32G0', 'STM32G0B1KBUx', 'RadioCard:STM32G0B1KBU6', 'STM32G0B1KBU6')
TUNER = stock_as('RF_AM_FM', 'Si4735-D60-GU', 'RadioCard:Si4735-D60-GU', 'Si4735-D60-GU')
# pins 10 / 11 are NC pins that the datasheet says to tie to GND: make them passive so KiCad keeps the connection
_t = lib_text[TUNER]
for _pn in ('10', '11'):
    _t, _k = re.subn(r'\(pin no_connect (\w+)((?:(?!\(pin ).)*?\(number "%s")' % _pn, r'(pin passive \1\2', _t, count=1, flags=re.S)
    assert _k == 1, _pn
register(TUNER, _t)
BUF = stock('74xGxx', '74LVC1G125')
for n in ('GND', '+5V', '+3V3', 'PWR_FLAG'):
    stock('power', n)

FP_R = 'Resistor_SMD:R_0603_1608Metric'
FP_C = 'Capacitor_SMD:C_0603_1608Metric'
FP_C10 = 'Capacitor_SMD:C_0805_2012Metric'
FP_PINS = 'Connector_PinHeader_2.54mm:PinHeader_%s_P2.54mm_Horizontal'

cnt = {}


def nid(prefix):
    cnt[prefix] = cnt.get(prefix, 0) + 1
    return '%s%d' % (prefix, cnt[prefix])


def r_(v, a, b, x, y, d=None, dnp=False):
    ref = nid('R')
    place(R, ref, v, FP_R, x, y, {'1': a, '2': b}, descr=d, dnp=dnp)
    return ref


def c_(v, a, b, x, y, d=None):
    ref = nid('C')
    place(C, ref, v, FP_C10 if v == '10u' else FP_C, x, y, {'1': a, '2': b}, descr=d)
    return ref


text(15, 12, 'AM/FM RADIO CARD  -  Si4735-D60 tuner + STM32G0B1KBU6 + MCP2562: I2S audio to the audio card on the slot I2S rail, CAN for control', 3)
text(15, 20, 'Card for a DigiDash slot, 80 x 60 mm, standing on edge (docs/hardware/accessory_ports.md), any slot. '
     'CAN node 0x14 + (slot - 1), product 0x0009 (docs/hardware/radio_card.md). 5 V from the slot only; 3.3 V on board.\n'
     'Checked against the Si4730/31/34/35-D60 data sheet rev 1.2, AN332 (207003A) and AN383 (207075A). Open: an AM network for a car whip (not in AN383; JP2).', 1.6)

# --- slot
G('port')
place(HDR2x15, 'J1', 'DigiDash slot', 'Connector_PinHeader_2.54mm:PinHeader_2x15_P2.54mm_Horizontal', 45, 55,
      {'1': '+5V', '2': '+5V', '3': 'GND', '4': 'GND', '5': 'CANH', '6': 'CANL', '7': 'GND', '8': None,
       '9': None, '10': None, '11': 'SLOT_ID', '12': None, '13': None, '14': None,
       '15': 'I2S_BCK', '16': 'I2S_WS', '17': None, '18': 'I2S_RET', '19': 'GND', '20': 'GND',
       '21': None, '22': None, '23': None, '24': 'I2S_MCLK',
       '25': None, '26': None, '27': None, '28': None, '29': None, '30': None},
      descr='Right-angle 2x15 header on the bottom edge (the slot pinout, docs/hardware/accessory_ports.md). Used: 5 V, GND, CAN, '
            'slot ID, I2S rail BCK (15), WS (16) as inputs and RET (18) as a granted output, MCLK (24) only through the unfitted R to RCLK. Not used: MIC (17), 12 V, 3.3 V, WAKE, LAMP, REV, audio bus, AUX')
text(70, 40, 'Slot ID (pin 11): 10k to 3.3 V here, the slot\'s own resistor to GND on the main board, read on PA0 (ADC1_IN0). '
     'The rail is driven by the audio card only; this card only listens to BCK / WS and drives RET after a NET RAIL grant.', 1.4)
r_('10k', '+3V3', 'SLOT_ID', 25, 45, 'Slot ID pull-up (the main board has one resistor to GND per slot)')
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', 25, 85, {'1': '+5V'}, show_value=False)
place('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', 40, 85, {'1': 'GND'}, show_value=False)

# --- power
G('power')
text(15, 100, 'POWER: slot +5 V -> AP2112K-3.3 -> +3V3 (MCU, MCP2562 VIO, tuner VD, buffer, Y1 oscillator). Tuner VA (2.7-5.5 V) comes from +3V3 through a ferrite: +3V3A. '
     'The tuner draws about 10 mA on each rail in FM.', 1.4)
c_('10u', '+5V', 'GND', 25, 125, d='Slot 5 V bulk / LDO input')
place(LDO, 'U3', 'AP2112K-3.3', 'Package_TO_SOT_SMD:SOT-23-5', 60, 120,
      {'1': '+5V', '2': 'GND', '3': '+5V', '4': None, '5': '+3V3'},
      descr='Diodes AP2112K-3.3TRG1: 600 mA LDO, 3.3 V, SOT-23-5. EN tied to VIN')
c_('10u', '+3V3', 'GND', 90, 125, d='LDO output')
c_('100n', '+3V3', 'GND', 105, 125)
place(FB, 'FB1', 'BLM18 120R', 'Inductor_SMD:L_0603_1608Metric', 125, 120, {'1': '+3V3', '2': '+3V3A'},
      descr='Ferrite bead, 0603, about 120 R at 100 MHz, 0.5 A: keeps digital noise off the tuner analog supply')
c_('10u', '+3V3A', 'GND', 145, 125, d='Tuner analog supply bulk')
place('power:PWR_FLAG', '#FLG03', 'PWR_FLAG', '', 160, 115, {'1': '+3V3A'}, show_value=False)

# --- MCU
G('mcu')
text(170, 30, 'MCU: STM32G0B1KBU6 (UFQFPN-32, the GP pinout). Pin use (DS13560 rev 4, tables 12-16):\n'
     'PA11 (22) FDCAN1_RX AF3, PA12 (23) FDCAN1_TX AF3 (SYSCFG remap off).  PB6 (30) I2C1_SCL AF6, PB7 (31) I2C1_SDA AF6: the tuner.\n'
     'PB0 (15) tuner RST (output; pulled low, so the tuner stays in reset until the firmware starts).  PB1 (16) tuner INT (GPO2, input).\n'
     'PB2 (17) RET buffer enable, ACTIVE LOW (74LVC1G125 OE; 10k pull-up keeps it off at reset and while the MCU is blank).\n'
     'PA9 (19) USART1_TX AF1, PA10 (21) USART1_RX AF1: debug (J4, also the ROM bootloader).  PA0 (7) ADC1_IN0: slot ID.  PA5 (12) LED tuned, PA6 (13) LED stereo / RDS.\n'
     'PC14 (2) OSC_IN: 8 MHz oscillator, HSE bypass (no OSC_OUT on this package); PC15 (3) unused.  PA13 SWDIO, PA14 SWCLK (BOOT0 shares it), PF2 = NRST.', 1.4)
mcu = {'1': None, '2': 'HSE_IN', '3': None, '4': '+3V3', '5': 'GND', '6': 'NRST', '7': 'SLOT_ID', '8': None,
       '9': None, '10': None, '11': None, '12': 'LED_TUNED', '13': 'LED_STEREO', '14': None, '15': 'TUN_RST', '16': 'TUN_INT',
       '17': 'RET_OE', '18': None, '19': 'DBG_TX', '20': None, '21': 'DBG_RX', '22': 'CAN_RX', '23': 'CAN_TXD',
       '24': 'SWDIO', '25': 'SWCLK', '26': None, '27': None, '28': None, '29': None, '30': 'TUN_SCL', '31': 'TUN_SDA',
       '32': None, '33': 'GND'}
place(MCU, 'U1', 'STM32G0B1KBU6', 'Package_DFN_QFN:QFN-32-1EP_5x5mm_P0.5mm_EP3.45x3.45mm', 210, 120, mcu,
      descr='ST STM32G0B1KBU6: Cortex-M0+ 64 MHz, 128 KB flash, 144 KB RAM, FDCAN, UFQFPN-32 5 x 5 mm (exposed pad = VSS)')
c_('100n', '+3V3', 'GND', 175, 165, d='VDD / VDDA, at pin 4')
c_('4.7u', '+3V3', 'GND', 190, 165, d='VDD / VDDA bulk')
c_('100n', 'NRST', 'GND', 260, 165, d='NRST filter (the pin has its own pull-up)')
# The UFQFPN-32 G0B1 has no PF0 / PF1, so no HSE crystal driver: PC14 takes OSC_IN in bypass mode only and PC15 has no
# OSC_OUT (ST pin data: PC14 RCC_OSC_IN, PC15 RCC_OSC_EN / OSC32_OUT). A crystal here can never start; a CMOS oscillator
# into PC14 is the supported clock. As SensorCard2.
place(OSC, 'Y1', '8MHz', 'Oscillator:Oscillator_SMD_Abracon_ASE-4Pin_3.2x2.5mm', 150, 120,
      {'1': '+3V3', '2': 'GND', '3': 'HSE_IN', '4': '+3V3'},
      descr='8 MHz CMOS oscillator, 3.2 x 2.5 mm, 1.8-3.3 V, +-20 ppm (YXC OT32258MJBA4SL, LCSC C717687): pin 1 tri-state '
            '(high = on) tied to 3V3, pin 3 out to PC14 OSC_IN (HSE bypass)')
cnt['C'] += 1   # C8 was the first crystal load cap: skip it so the other capacitors keep their references
c_('100n', '+3V3', 'GND', 165, 135, d='Y1 supply, at pin 4')
r_('1k', 'LED_TUNED', 'LED_TUNED_A', 260, 95, 'LED current, about 1 mA')
place(LED, 'D1', 'TUNED', 'LED_SMD:LED_0603_1608Metric', 280, 95, {'2': 'LED_TUNED_A', '1': 'GND'},
      descr='Off = tuner off, blinking = seeking, steady = tuned')
r_('1k', 'LED_STEREO', 'LED_STEREO_A', 260, 110, 'LED current, about 1 mA')
place(LED, 'D3', 'STEREO/RDS', 'LED_SMD:LED_0603_1608Metric', 280, 110, {'2': 'LED_STEREO_A', '1': 'GND'},
      descr='On = stereo pilot found, blinks with RDS data')

# --- programming
G('prog')
text(15, 150, 'PROGRAMMING (the STM32 comes from assembly blank): J3 SWD for an ST-Link (pin 1 is the target-voltage sense, '
     'not a supply: power the card from the slot). J4 debug serial: USART1, 3.3 V logic; also the ST ROM bootloader.', 1.4)
place(HDR1x5, 'J3', 'SWD', FP_PINS % '1x05', 40, 185,
      {'1': '+3V3', '2': 'SWDIO', '3': 'SWCLK', '4': 'NRST', '5': 'GND'},
      descr='SWD: 1 3V3 (sense), 2 SWDIO, 3 SWCLK, 4 NRST, 5 GND. Right angle on the left edge')
place(HDR1x3, 'J4', 'Debug serial', FP_PINS % '1x03', 90, 185,
      {'1': 'GND', '2': 'DBG_TX', '3': 'DBG_RX'},
      descr='Debug serial (USART1, 3.3 V): 1 GND, 2 TX (out of the card), 3 RX (into the card). Right angle on the left edge')

# --- CAN
G('can')
text(15, 215, 'CAN: MCP2562 (VDD 5 V, VIO 3.3 V, STBY low). No 120 R: a card never terminates the bus.', 1.4)
place(CAN, 'U2', 'TJA1051T/3', 'Package_SO:SOIC-8_3.9x4.9mm_P1.27mm', 55, 245,
      {'1': 'CAN_TXD', '2': 'GND', '3': '+5V', '4': 'CAN_RXD', '5': '+3V3', '6': 'CANL', '7': 'CANH', '8': 'GND'})
r_('1k', 'CAN_RXD', 'CAN_RX', 95, 235, 'Series R on the receive line, as on the DigiDash board')
c_('100n', '+5V', 'GND', 115, 235, d='MCP2562 VDD')
c_('100n', '+3V3', 'GND', 130, 235, d='MCP2562 VIO')

# --- tuner
G('tuner')
text(15, 275, 'TUNER: Si4735-D60-GU (SSOP-24; same silicon as the -GM QFN, whose land pattern is not a stock KiCad footprint). '
     'Si4735 = AM / FM / SW / LW + RDS. NOT the Si4730 (no RDS).', 1.4)
place(TUNER, 'U4', 'Si4735-D60-GU', 'Package_SO:SSOP-24_3.9x8.7mm_P0.635mm', 330, 60,
      {'1': 'TUN_DOUT', '2': 'TUN_DFS', '3': 'TUN_DCLK', '4': 'TUN_INT', '5': None, '6': None, '7': None,
       '8': 'FMI', '9': 'GND', '10': 'GND', '11': 'GND', '12': 'AMI', '13': 'GND', '14': 'GND',
       '15': 'TUN_RST', '16': 'TUN_SEN', '17': 'TUN_SCL', '18': 'TUN_SDA', '19': 'RCLK', '20': '+3V3',
       '21': '+3V3A', '22': 'DBYP', '23': None, '24': None},
      descr='Skyworks (Silicon Labs) Si4735-D60-GU. Digital audio out is a SLAVE interface: DFS (pin 2) and DCLK (GPO3, pin 3) are inputs. '
            'Pins 10 / 11 unused: tie to GND (datasheet). 6 / 7 NC. GPO1 (5) left open: it must be high or open at reset (2-wire bus mode, internal pull-up). '
            'LOUT / ROUT unused: POWER_UP OPMODE 0xB0 (digital only; 0xB5 analog + digital is not offered on the Si4735 in FM, AN332 p.15)')
c_('22n', 'DBYP', '+3V3A', 365, 100, d='C1 (datasheet SSOP schematic p.18): DBYP to VA, X7R, close to VA')
c_('100n', '+3V3', 'GND', 380, 100, d='C4 in the datasheet: VD bypass, close to pin 20')
text(300, 105, 'Digital audio network (datasheet optional parts, AN383 tables 6-7): R1 604 on DOUT, R2 / R3 2k on DFS / DCLK, C9 2.2 pF C0G on the DFS line. '
     'I2S mode, slave: DCLK = rail BCK (64 fS), DFS = rail WS. Left channel when DFS is low.', 1.4)
r_('604', 'TUN_DOUT', 'TUN_DOUT_B', 300, 130, 'R1 (datasheet 600 R, AN383 table 6: 604 R E96): at the DOUT pin, towards the RET buffer')
r_('2k', 'TUN_DFS', 'I2S_WS', 300, 145, 'R2 (datasheet): at the DFS pin, WS from the rail')
r_('2k', 'TUN_DCLK', 'I2S_BCK', 300, 160, 'R3 (datasheet): at the DCLK pin, BCK from the rail')
c_('2.2p', 'I2S_WS', 'GND', 340, 145, d='C9 (datasheet, AN383 p.5): 2.2 pF C0G noise capacitor on the DFS line')
r_('100k', 'I2S_BCK', 'GND', 355, 145, 'Idle pull-down: the rail has no clocks until the audio card turns them on')
r_('100k', 'I2S_WS', 'GND', 370, 145, 'Idle pull-down: the rail has no clocks until the audio card turns them on')

text(300, 180, 'CONTROL (2-wire I2C): SCLK high at the rising edge of RST selects 2-wire; GPO1 high / GPO2 low at that edge (internal pull-up / pull-down). '
     'SEN sets the address (JP1: 1-2 bridged = SEN low = 0x11; 2-3 = 0x63, AN332 6.1). RST has 100k to GND so the tuner waits for the firmware.', 1.4)
r_('4.7k', '+3V3', 'TUN_SCL', 300, 205, 'I2C pull-up')
r_('4.7k', '+3V3', 'TUN_SDA', 315, 205, 'I2C pull-up')
r_('100k', 'TUN_RST', 'GND', 330, 205, 'Holds the tuner in reset until the MCU drives RST')
place(SJ3, 'JP1', 'Tuner address', 'Jumper:SolderJumper-3_P1.3mm_Bridged12_RoundedPad1.0x1.5mm', 355, 205,
      {'1': 'GND', '2': 'TUN_SEN', '3': '+3V3'},
      descr='SEN strap: 1-2 bridged (as delivered) = SEN low, 7-bit address 0x11; cut and bridge 2-3 = SEN high, 0x63 (AN332 section 6.1)')

# --- reference clock
G('clock')
text(15, 295, 'REFERENCE CLOCK: the tuner takes it from DCLK (the rail BCK, 3.072 MHz at 48 kHz): REFCLK_PRESCALE = 0x1060 (bit 12 RCLKSEL = DCLK, prescale 96), '
     'REFCLK_FREQ = 32000 (AN332 properties 0x0201 / 0x0202, p.37-38). So the tuner runs on the audio card\'s sample clock: no drift between them, and no oscillator. '
     'Its own crystal cannot be used (GPO3 is DCLK). Fallback, unfitted: R to RCLK from the rail MCLK (12.288 MHz, prescale 375 = 32768 Hz), whose 8th harmonic is 98.3 MHz.', 1.4)
r_('0', 'I2S_MCLK', 'RCLK', 60, 320, 'UNFITTED. Fallback reference: the rail MCLK into RCLK (REFCLK_PRESCALE 375, RCLKSEL = RCLK)', dnp=True)
r_('100k', 'RCLK', 'GND', 90, 320, 'Holds the unused RCLK input low (RCLKSEL = DCLK)')

# --- RET buffer
G('rail')
text(130, 295, 'RAIL (RET, pin 18): audio to the audio card. 74LVC1G125, OFF at power-up (OE pulled high; the MCU pulls it low only after a NET RAIL grant, '
     'and releases it within 10 ms of a withdrawal), 33 R in series at the pin.', 1.4)
place(BUF, 'U5', '74LVC1G125', 'Package_TO_SOT_SMD:SOT-353_SC-70-5', 160, 320,
      {'1': 'RET_OE', '2': 'TUN_DOUT_B', '3': 'GND', '4': 'RET_Y', '5': '+3V3'},
      descr='3-state buffer: pin 1 = OE (active low), 2 = A, 4 = Y. Every driver on the rail sits behind one that is off at reset')
r_('10k', '+3V3', 'RET_OE', 130, 340, 'Keeps the buffer OFF at reset, while the MCU is blank, and until the MCU drives PB2 low')
r_('33', 'RET_Y', 'I2S_RET', 195, 320, 'Series R at the rail pin (rail rule)')
c_('100n', '+3V3', 'GND', 175, 340, d='Buffer supply')

# --- antenna
G('antenna')
text(235, 275, 'ANTENNA: J2 SMA edge mount on the top edge. FM (AN383 section 7, fig. 23): the whip node has L1 56 nH to RFGND (matches a ~22 pF whip; '
     'tune on the bench for the car whip and its cable, or leave it off), then C2 1 nF C0G to FMI. ESD: D2 under 1 pF on the SMA centre (AN383 asks for <= 1 pF; '
     'FMI / AMI absolute max 0.4 Vpk). AM: the datasheet circuit, a ferrite loopstick (180-450 uH) on J5 between the AM node and ground, 0.47 uF to AMI. '
     'JP2 (open) would feed the SMA to AM too: AN383 has no network for a car whip on AM (its section 10 SW whip network is the starting point). '
     'Keep AM stray capacitance under 10 pF.', 1.4)
place(COAX, 'J2', 'Antenna (SMA)', 'Connector_Coaxial:SMA_Amphenol_132289_EdgeMount', 250, 310, {'1': 'FM_ANT', '2': 'GND'},
      descr='Amphenol RF 132289 SMA jack, edge mount, 1.6 mm board, on the top edge. For the car whip, a Motorola-to-SMA pigtail')
place(TVS, 'D2', 'TPD1E0B04', 'Diode_SMD:D_0402_1005Metric', 290, 330, {'1': 'FM_ANT', '2': 'GND'},
      descr='TI TPD1E0B04DPYR: 1-channel bidirectional ESD, 0.13 pF, X1SON 1.0 x 0.6 mm (0402 land pattern; check against the TI drawing). AN383 asks for <= 1 pF')
place(IND, 'L1', '56nH', 'Inductor_SMD:L_0603_1608Metric', 305, 345, {'1': 'FM_ANT', '2': 'GND'},
      descr='Murata LQW18AN56NJ00D, 56 nH wire-wound 0603, Q >= 15 at 100 MHz (AN383 fig. 23, table 16). FM antenna match to RFGND; bench-tune for the car whip')
c_('1n', 'FM_ANT', 'FMI', 320, 310, d='C2 (datasheet): FM coupling, C0G')
c_('470n', 'AM_ANT', 'AMI', 350, 310, d='C3 (datasheet): AM coupling')
place(HDR1x2, 'J5', 'AM loopstick', FP_PINS % '1x02', 380, 320, {'1': 'AM_ANT', '2': 'GND'},
      descr='AM ferrite loopstick (180-450 uH) between the AM node and ground (datasheet, AN383 section 8). Right angle on the top edge; a wire pair to a loopstick off the card')
place(SJ2, 'JP2', 'AM from whip', 'Jumper:SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm', 290, 355,
      {'1': 'FM_ANT', '2': 'AM_ANT'},
      descr='Open (as delivered) = AM only from the loopstick. Bridged = the SMA also feeds AM, with a network for a car whip that AN383 does not give (its SW whip network, section 10, is the start)')

# ----------------------------------------------------------------------------- write
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "A2")',
       '  (title_block (title "DigiDash AM/FM radio card") (date "2026-10-02") (rev "A")',
       '    (comment 1 "Si4735-D60 + STM32G0B1KBU6 + MCP2562: I2S to the audio card (slot I2S rail), CAN control")',
       '    (comment 2 "Card for a DigiDash slot (2x15, on edge), any slot"))',
       '  (lib_symbols']
for t in lib_text.values():
    out.append('\n'.join('    ' + ln for ln in t.split('\n')))
out.append('  )')
out += body
out += ['  (sheet_instances (path "/" (page "1")))', ')']
open(OUT, 'w').write('\n'.join(out) + '\n')
json.dump(refs_info, open(os.path.join(os.path.dirname(OUT), 'generate', 'refs.json'), 'w'), indent=1, sort_keys=True)
lone = {n: p for n, p in netmap.items() if len(p) < 2 and n not in ('SWDIO',)}
if lone:
    print('NETS WITH ONE PIN:', lone)
json.dump(netmap, open(os.path.join(os.path.dirname(OUT), 'generate', 'netlist.json'), 'w'), indent=1, sort_keys=True)
print('wrote', os.path.normpath(OUT), len(body), 'items', len(refs_info), 'parts')
