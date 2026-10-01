#!/usr/bin/env python3
"""Generate GPSCard.kicad_sch (KiCad 7 file format, opens in 7/8/9/10).

Same pattern as AudioCarrier/generate/gen_sch.py. Needs the stock KiCad symbol libraries (KICAD_SYMBOLS, default
/Applications/KiCad/... or /usr/share/kicad/symbols). Every connection is a net label or power symbol on a short stub
at the pin, so the file is a netlist you can read; move things around in eeschema as you like.

Spec: docs/hardware/gps_card.md. Slot pinout: docs/hardware/accessory_ports.md (rev 2).
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
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'GPSCard.kicad_sch')
PROJECT = 'GPSCard'
NS = uuid.UUID('3f6d2b1e-8a4c-4f0b-9d21-6c5e0a7b9e05')
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
LED = stock('Device', 'LED')
PTC = stock('Device', 'Polyfuse')
XTAL = stock('Device', 'Crystal_GND24')
HDR2x15 = stock('Connector_Generic', 'Conn_02x15_Odd_Even')
HDR1x5 = stock('Connector_Generic', 'Conn_01x05')
HDR1x4 = stock('Connector_Generic', 'Conn_01x04')
HDR1x3 = stock('Connector_Generic', 'Conn_01x03')
SJ3 = stock_as('Jumper', 'SolderJumper_3_Bridged12', 'GPSCard:SolderJumper_3_Bridged12', 'Puck 3V3 / 5V')
CAN = stock('Interface_CAN_LIN', 'MCP2562-E-SN')
LDO = stock_as('Regulator_Linear', 'AP2112K-3.3', 'GPSCard:AP2112K-3.3', 'AP2112K-3.3')
ESD = stock_as('Power_Protection', 'USBLC6-2SC6', 'GPSCard:USBLC6-2SC6', 'USBLC6-2SC6')
MCU = stock_as('MCU_ST_STM32G0', 'STM32G0B1KBUx', 'GPSCard:STM32G0B1KBU6', 'STM32G0B1KBU6')
for n in ('GND', '+5V', '+3V3', 'PWR_FLAG'):
    stock('power', n)

FP_R = 'Resistor_SMD:R_0603_1608Metric'
FP_C = 'Capacitor_SMD:C_0603_1608Metric'
FP_C10 = 'Capacitor_SMD:C_0805_2012Metric'

cnt = {}


def nid(prefix):
    cnt[prefix] = cnt.get(prefix, 0) + 1
    return '%s%d' % (prefix, cnt[prefix])


def r_(v, a, b, x, y, d=None):
    ref = nid('R')
    place(R, ref, v, FP_R, x, y, {'1': a, '2': b}, descr=d)
    return ref


def c_(v, a, b, x, y, d=None):
    ref = nid('C')
    place(C, ref, v, FP_C10 if v == '10u' else FP_C, x, y, {'1': a, '2': b}, descr=d)
    return ref


text(15, 12, 'GPS CARD  -  STM32G0B1KBU6 + MCP2562: reads a remote GPS puck (UART) and sends position / motion / time on CAN', 3)
text(15, 20, 'Card for a DigiDash slot, 80 x 60 mm, standing on edge (docs/hardware/accessory_ports.md), any slot. '
     'CAN node 0x05, IDs 0x222-0x226 (docs/hardware/gps_card.md). 5 V from the slot only; 3.3 V on board. No radio, no RF parts.', 1.6)

# --- slot
G('port')
place(HDR2x15, 'J1', 'DigiDash slot', 'Connector_PinHeader_2.54mm:PinHeader_2x15_P2.54mm_Horizontal', 45, 55,
      {'1': '+5V', '2': '+5V', '3': 'GND', '4': 'GND', '5': 'CANH', '6': 'CANL', '7': 'GND', '8': None,
       '9': None, '10': None, '11': 'SLOT_ID', '12': None, '13': None, '14': None,
       '15': None, '16': None, '17': None, '18': None, '19': 'GND', '20': 'GND',
       '21': None, '22': None, '23': None, '24': None,
       '25': None, '26': None, '27': None, '28': None, '29': None, '30': None},
      descr='Right-angle 2x15 header on the bottom edge (the slot pinout, docs/hardware/accessory_ports.md). Used: 5 V, GND, CAN, '
            'slot ID. Not used: switched / constant 12 V, 3.3 V, WAKE, LAMP, REV, the I2S rail, the audio bus, AUX 12 V')
text(70, 40, 'Only 5 V is used (the card draws well under 0.1 A plus the puck). Slot ID (pin 11): 10k to 3.3 V here, '
     'the slot\'s own resistor to GND on the main board, read on PA0 (ADC1_IN0).', 1.4)
r_('10k', '+3V3', 'SLOT_ID', 25, 45, 'Slot ID pull-up (the main board has one resistor to GND per slot)')
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', 25, 85, {'1': '+5V'}, show_value=False)
place('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', 40, 85, {'1': 'GND'}, show_value=False)

# --- power
G('power')
text(15, 100, 'POWER: slot +5 V -> AP2112K-3.3 (MCU, MCP2562 VIO, a 3.3 V puck). 1 uF min. ceramic in / out per the datasheet.', 1.4)
c_('10u', '+5V', 'GND', 25, 125, d='Slot 5 V bulk / LDO input')
place(LDO, 'U3', 'AP2112K-3.3', 'Package_TO_SOT_SMD:SOT-23-5', 60, 120,
      {'1': '+5V', '2': 'GND', '3': '+5V', '4': None, '5': '+3V3'},
      descr='Diodes AP2112K-3.3TRG1: 600 mA LDO, 3.3 V, SOT-23-5. EN tied to VIN')
c_('10u', '+3V3', 'GND', 90, 125, d='LDO output')
c_('100n', '+3V3', 'GND', 105, 125)

# --- MCU
G('mcu')
text(170, 30, 'MCU: STM32G0B1KBU6 (UFQFPN-32). Pin use (datasheet DS13560 alternate-function tables):\n'
     'PA11 (pin 22) FDCAN1_RX AF9, PA12 (pin 23) FDCAN1_TX AF9 (leave the PA11/PA12 -> PA9/PA10 remap off).\n'
     'PA2 (pin 9) USART2_TX AF1, PA3 (pin 10) USART2_RX AF1: the puck.  PA9 (pin 19) USART1_TX AF1, PA10 (pin 21) USART1_RX AF1: debug (J4, also the ROM bootloader UART).\n'
     'PA0 (pin 7) ADC1_IN0: slot ID.  PA5 (pin 12): LED.  PC14 / PC15 (pins 2 / 3) OSC_IN / OSC_OUT: 8 MHz HSE.  PA13 SWDIO, PA14 SWCLK (BOOT0 shares it), PF2 = NRST.', 1.4)
mcu = {'1': None, '2': 'HSE_IN', '3': 'HSE_OUT', '4': '+3V3', '5': 'GND', '6': 'NRST', '7': 'SLOT_ID', '8': None,
       '9': 'PUCK_TXD', '10': 'PUCK_RXD', '11': None, '12': 'LED', '13': None, '14': None, '15': None, '16': None,
       '17': None, '18': None, '19': 'DBG_TX', '20': None, '21': 'DBG_RX', '22': 'CAN_RX', '23': 'CAN_TXD',
       '24': 'SWDIO', '25': 'SWCLK', '26': None, '27': None, '28': None, '29': None, '30': None, '31': None,
       '32': None, '33': 'GND'}
place(MCU, 'U1', 'STM32G0B1KBU6', 'Package_DFN_QFN:QFN-32-1EP_5x5mm_P0.5mm_EP3.45x3.45mm', 210, 120, mcu,
      descr='ST STM32G0B1KBU6: Cortex-M0+ 64 MHz, 128 KB flash, 144 KB RAM, FDCAN, UFQFPN-32 5 x 5 mm (exposed pad = VSS)')
c_('100n', '+3V3', 'GND', 175, 165, d='VDD / VDDA, at pin 4')
c_('4.7u', '+3V3', 'GND', 190, 165, d='VDD / VDDA bulk')
c_('100n', 'NRST', 'GND', 260, 165, d='NRST filter (the pin has its own pull-up)')
place(XTAL, 'Y1', '8MHz', 'Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm', 150, 120,
      {'1': 'HSE_IN', '3': 'HSE_OUT', '2': 'GND', '4': 'GND'},
      descr='8 MHz crystal, 3.2 x 2.5 mm, CL 10 pF, +-20 ppm or better (HSE, for CAN bit timing)')
c_('12p', 'HSE_IN', 'GND', 135, 135, d='Crystal load (CL 10 pF crystal; 2 x (CL - stray))')
c_('12p', 'HSE_OUT', 'GND', 165, 135, d='Crystal load (CL 10 pF crystal; 2 x (CL - stray))')
r_('1k', 'LED', 'LED_A', 260, 100, 'LED current, about 1 mA')
place(LED, 'D1', 'GPS', 'LED_SMD:LED_0603_1608Metric', 280, 100, {'2': 'LED_A', '1': 'GND'},
      descr='Status LED: off = nothing from the puck, blinking = searching, steady = 3D fix')

# --- programming
G('prog')
text(15, 150, 'PROGRAMMING (the STM32 comes from assembly blank): J3 SWD for an ST-Link (pin 1 is the target-voltage sense, '
     'not a supply: power the card from the slot). J4 debug serial: USART1, 3.3 V logic; also the ST ROM bootloader.', 1.4)
place(HDR1x5, 'J3', 'SWD', 'Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Horizontal', 40, 185,
      {'1': '+3V3', '2': 'SWDIO', '3': 'SWCLK', '4': 'NRST', '5': 'GND'},
      descr='SWD: 1 3V3 (sense), 2 SWDIO, 3 SWCLK, 4 NRST, 5 GND. Right angle on the left edge')
place(HDR1x3, 'J4', 'Debug serial', 'Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Horizontal', 90, 185,
      {'1': 'GND', '2': 'DBG_TX', '3': 'DBG_RX'},
      descr='Debug serial (USART1, 3.3 V): 1 GND, 2 TX (out of the card), 3 RX (into the card). Right angle on the left edge')

# --- CAN
G('can')
text(15, 215, 'CAN: MCP2562 (VDD 5 V, VIO 3.3 V, STBY low). No 120 R: a card never terminates the bus.', 1.4)
place(CAN, 'U2', 'MCP2562-E/SN', 'Package_SO:SOIC-8_3.9x4.9mm_P1.27mm', 55, 245,
      {'1': 'CAN_TXD', '2': 'GND', '3': '+5V', '4': 'CAN_RXD', '5': '+3V3', '6': 'CANL', '7': 'CANH', '8': 'GND'})
r_('1k', 'CAN_RXD', 'CAN_RX', 95, 235, 'Series R on the receive line, as on the DigiDash board')
c_('100n', '+5V', 'GND', 115, 235, d='MCP2562 VDD')
c_('100n', '+3V3', 'GND', 130, 235, d='MCP2562 VIO')

# --- GPS puck connector
G('gps')
text(170, 200, 'GPS PUCK: J2, JST PH 4-pin side entry (S4B-PH-K), same pinout as the main board\'s J6: 1 puck power, 2 TX (out, to the '
     'puck\'s RX), 3 RX (in, from the puck\'s TX), 4 GND.\nPuck power: 3.3 V or 5 V by JP1 (1-2 bridged = 3.3 V as delivered; cut it '
     'and bridge 2-3 for 5 V), through a 0.2 A PTC. 100 R in series on TX and RX, USBLC6-2SC6 (low capacitance) on TX, RX and the supply.', 1.4)
place(SJ3, 'JP1', 'Puck 3V3 / 5V', 'Jumper:SolderJumper-3_P1.3mm_Bridged12_RoundedPad1.0x1.5mm',
      185, 245, {'1': '+3V3', '2': 'PUCK_VSEL', '3': '+5V'},
      descr='Puck supply select: 1-2 (bridged as delivered) = 3.3 V; cut 1-2 and bridge 2-3 = 5 V')
place(PTC, 'F1', '0.2A', 'Fuse:Fuse_1206_3216Metric', 210, 245, {'1': 'PUCK_VSEL', '2': 'PUCK_PWR'},
      descr='PTC, 0.2 A hold, 1206 (Bourns MF-NSMF020-2 or equal)')
c_('1u', 'PUCK_PWR', 'GND', 225, 260, d='Puck supply, at J2')
r_('100', 'PUCK_TXD', 'GPS_TX', 240, 235, 'Series R, card TX to the puck')
r_('100', 'GPS_RX', 'PUCK_RXD', 240, 255, 'Series R, the puck\'s TX into the card')
place(ESD, 'U4', 'USBLC6-2SC6', 'Package_TO_SOT_SMD:SOT-23-6', 275, 245,
      {'1': 'GPS_TX', '6': 'GPS_TX', '3': 'GPS_RX', '4': 'GPS_RX', '5': 'PUCK_PWR', '2': 'GND'},
      descr='ST USBLC6-2SC6: low-capacitance ESD array on the cable lines (TX, RX, puck supply on VBUS)')
place(HDR1x4, 'J2', 'GPS puck', 'Connector_JST:JST_PH_S4B-PH-K_1x04_P2.00mm_Horizontal', 310, 245,
      {'1': 'PUCK_PWR', '2': 'GPS_TX', '3': 'GPS_RX', '4': 'GND'},
      descr='JST S4B-PH-K (PH, 4 pin, side entry) on the top edge: 1 puck power, 2 TX out, 3 RX in, 4 GND (as J6 on the main board)')

# ----------------------------------------------------------------------------- write
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "A3")',
       '  (title_block (title "DigiDash GPS card") (date "2026-10-01") (rev "A")',
       '    (comment 1 "STM32G0B1KBU6 + MCP2562: remote GPS puck (UART) -> CAN 0x222-0x226")',
       '    (comment 2 "Card for a DigiDash slot (2x15, on edge), any slot"))',
       '  (lib_symbols']
for t in lib_text.values():
    out.append('\n'.join('    ' + ln for ln in t.split('\n')))
out.append('  )')
out += body
out += ['  (sheet_instances (path "/" (page "1")))', ')']
open(OUT, 'w').write('\n'.join(out) + '\n')
json.dump(refs_info, open(os.path.join(os.path.dirname(OUT), 'generate', 'refs.json'), 'w'), indent=1, sort_keys=True)
print('wrote', os.path.normpath(OUT), len(body), 'items', len(refs_info), 'parts')
