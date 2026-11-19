#!/usr/bin/env python3
"""Generate GPSCard2.kicad_sch: the GPS card, rev B, every part through-hole (KiCad 7 file format, opens in 7/8/9/10).

Rev B is the hand-assembled version of GPSCard/ (rev A, STM32G0B1 + SMD passives): same 80 x 60 mm slot card, same
slot and puck connectors, same CAN IDs, but a PIC18F26Q83 in a 28-pin DIP with its own CAN FD controller, an MCP2562
in DIP-8, a TO-92 regulator, axial resistors and radial capacitors, so it can be soldered by hand.

Same pattern as GPSCard/generate/gen_sch.py. Needs the stock KiCad symbol libraries (KICAD_SYMBOLS, default
/Applications/KiCad/... or /usr/share/kicad/symbols). Every connection is a net label or power symbol on a short stub
at the pin. Spec: docs/hardware/gps_card.md. Slot pinout: docs/hardware/accessory_ports.md (rev 2).
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
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'GPSCard2.kicad_sch')
PROJECT = 'GPSCard2'
NS = uuid.UUID('7b2e4c91-5d3a-4e8f-a6b0-2c9d1e8f4a17')
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
XTAL = stock('Device', 'Crystal')
HDR2x15 = stock('Connector_Generic', 'Conn_02x15_Odd_Even')
HDR1x6 = stock('Connector_Generic', 'Conn_01x06')
HDR1x4 = stock('Connector_Generic', 'Conn_01x04')
HDR1x3 = stock('Connector_Generic', 'Conn_01x03')
JMP3 = stock('Jumper', 'Jumper_3_Bridged12')
CAN = stock('Interface_CAN_LIN', 'MCP2562-E-P')
# KiCad has no PIC18F26Q83 symbol. The PIC18F2xK83 SPDIP-28 symbol has the same pins (checked against the Q83 data sheet
# DS40002253 pin allocation table: 1 MCLR/VPP/RE3, 2-7 RA0-RA5, 8 / 19 VSS, 9 RA7/OSC1, 10 RA6/OSC2, 11-18 RC0-RC7,
# 20 VDD, 21-28 RB0-RB7, 27 ICSPCLK, 28 ICSPDAT).
MCU = stock_as('MCU_Microchip_PIC18', 'PIC18F26K83-xSP', 'GPSCard2:PIC18F26Q83-I_SP', 'PIC18F26Q83-I/SP')
LDO = stock_as('Regulator_Linear', 'MCP1700x-330xxTO', 'GPSCard2:MCP1700-3302E_TO', 'MCP1700-3302E/TO')
for n in ('GND', '+5V', '+3V3', 'PWR_FLAG'):
    stock('power', n)

FP_R = 'Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal'
FP_C = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P2.50mm'

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
    place(C, ref, v, FP_C, x, y, {'1': a, '2': b}, descr=d)
    return ref


text(15, 12, 'GPS CARD rev B (all through-hole)  -  PIC18F26Q83 + MCP2562: reads a remote GPS puck (UART) and sends position / motion / time on CAN', 3)
text(15, 20, 'Card for a DigiDash slot, 80 x 60 mm, standing on edge (docs/hardware/accessory_ports.md), any slot. '
     'CAN node 0x14 + (slot - 1), product 0x0005, IDs 0x222-0x226 (docs/hardware/gps_card.md). 5 V from the slot only; 3.3 V on board. '
     'Every part is through-hole, for hand assembly.', 1.6)

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
     'the slot\'s own resistor to GND on the main board, read on RA1 (ANA1).', 1.4)
r_('10k', '+3V3', 'SLOT_ID', 25, 45, 'Slot ID pull-up (the main board has one resistor to GND per slot)')
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', 25, 85, {'1': '+5V'}, show_value=False)
place('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', 40, 85, {'1': 'GND'}, show_value=False)

# --- power
G('power')
text(15, 100, 'POWER: slot +5 V -> MCP1700-3302E/TO (250 mA LDO, TO-92: 1 GND, 2 VIN, 3 VOUT) -> 3.3 V for the MCU, the MCP2562 VIO '
     'and a 3.3 V puck. Ceramic caps in / out (the MCP1700 wants at least 1 uF ceramic on each side).', 1.4)
c_('10u', '+5V', 'GND', 25, 125, d='Slot 5 V bulk / LDO input, radial MLCC X5R')
place(LDO, 'U3', 'MCP1700-3302E/TO', 'Package_TO_SOT_THT:TO-92_Inline_Wide', 60, 120,
      {'1': 'GND', '2': '+5V', '3': '+3V3'},
      descr='Microchip MCP1700-3302E/TO: 3.3 V 250 mA LDO, TO-92 (1 GND, 2 VIN, 3 VOUT)')
c_('10u', '+3V3', 'GND', 90, 125, d='LDO output, radial MLCC X5R')

# --- MCU
G('mcu')
text(170, 30, 'MCU: PIC18F26Q83-I/SP (SPDIP-28, in a socket). Pin use (DS40002253 pin allocation and PPS tables; 28-pin PPS: '
     'UART1, UART2 and CAN on ports B and C only):\n'
     'RC6 (pin 17) U1TX by PPS, RC7 (pin 18) U1RX (its POR default): the puck.  RB4 (pin 25) U2TX, RB5 (pin 26) U2RX, both by PPS: '
     'debug serial (J4).\n'
     'RB2 (pin 23) CANTX by PPS, RB3 (pin 24) CANRX (its POR default).  RA1 (pin 3) ANA1: slot ID.  RC2 (pin 13): LED.\n'
     'RA7 / RA6 (pins 9 / 10): 16 MHz crystal, HS mode, 4x PLL = 64 MHz (RSTOSC = EXTOSC with 4x PLL, FEXTOSC = HS).\n'
     'Config: JTAGEN off (JTAG shares RA0, RA5, RB3, RB5), LVP on (default) so any PICkit / Snap programs it at 3.3 V.\n'
     'ICSP: MCLR (pin 1), ICSPCLK RB6 (pin 27), ICSPDAT RB7 (pin 28). No pull-ups or caps on RB6 / RB7, no cap on MCLR.', 1.4)
mcu = {'1': 'MCLR', '2': None, '3': 'SLOT_ID', '4': None, '5': None, '6': None, '7': None, '8': 'GND',
       '9': 'OSC1', '10': 'OSC2', '11': None, '12': None, '13': 'LED', '14': None, '15': None, '16': None,
       '17': 'PUCK_TXD', '18': 'PUCK_RXD', '19': 'GND', '20': '+3V3', '21': None, '22': None, '23': 'CAN_TXD',
       '24': 'CAN_RX', '25': 'DBG_TX', '26': 'DBG_RX', '27': 'ICSPCLK', '28': 'ICSPDAT'}
place(MCU, 'U1', 'PIC18F26Q83-I/SP', 'Package_DIP:DIP-28_W7.62mm_Socket', 210, 120, mcu,
      descr='Microchip PIC18F26Q83-I/SP: PIC18, 64 MHz, 64 KB flash, 4 KB RAM, CAN FD, 5 UARTs, 1.8-5.5 V, SPDIP-28 (in a 28-pin 0.3 in socket)')
c_('100n', '+3V3', 'GND', 175, 165, d='VDD (pin 20) decoupling, X7R, at the pin')
r_('10k', '+3V3', 'MCLR', 260, 150, 'MCLR pull-up (data sheet: 10k or less; no capacitor, the programmer drives MCLR)')
place(XTAL, 'Y1', '16MHz', 'Crystal:Crystal_HC49-4H_Vertical', 150, 120, {'1': 'OSC1', '2': 'OSC2'},
      descr='16 MHz crystal, HC-49/US (low profile), 18 pF load (Abracon ABL-16.000MHZ-B2 or equal)')
c_('27p', 'OSC1', 'GND', 140, 140, d='Crystal load, C0G (18 pF crystal: 2 x 27 pF with about 5 pF stray)')
c_('27p', 'OSC2', 'GND', 160, 140, d='Crystal load, C0G')
r_('1k', 'LED', 'LED_A', 260, 100, 'LED current, about 1 mA')
place(LED, 'D1', 'GPS', 'LED_THT:LED_D3.0mm', 280, 100, {'2': 'LED_A', '1': 'GND'},
      descr='Status LED, 3 mm green: off = nothing from the puck, blinking = searching, steady = 3D fix. Short lead (flat side) = cathode = pad 1')

# --- programming
G('prog')
text(15, 150, 'PROGRAMMING: J3 is the Microchip 6-pin ICSP header (PICkit 4 / 5, MPLAB Snap): 1 MCLR/VPP, 2 VDD, 3 GND, 4 ICSPDAT, '
     '5 ICSPCLK, 6 not used (LVP). Power the card from the slot; pin 2 lets the programmer sense 3.3 V. '
     'J4 debug serial: UART2, 3.3 V logic.', 1.4)
place(HDR1x6, 'J3', 'ICSP', 'Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Horizontal', 40, 185,
      {'1': 'MCLR', '2': '+3V3', '3': 'GND', '4': 'ICSPDAT', '5': 'ICSPCLK', '6': None},
      descr='ICSP (PICkit pinout): 1 MCLR/VPP, 2 VDD, 3 GND, 4 ICSPDAT, 5 ICSPCLK, 6 NC. Right angle on the left edge, pin 1 lowest')
place(HDR1x3, 'J4', 'Debug serial', 'Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Horizontal', 90, 185,
      {'1': 'GND', '2': 'DBG_TX', '3': 'DBG_RX'},
      descr='Debug serial (UART2, 3.3 V): 1 GND, 2 TX (out of the card), 3 RX (into the card). Right angle on the left edge')

# --- CAN
G('can')
text(15, 215, 'CAN: MCP2562-E/P (DIP-8; VDD 5 V, VIO 3.3 V, STBY low). No 120 R: a card never terminates the bus. '
     '500 kbit/s classic CAN from the PIC\'s CAN FD module.', 1.4)
place(CAN, 'U2', 'MCP2562-E/P', 'Package_DIP:DIP-8_W7.62mm', 55, 245,
      {'1': 'CAN_TXD', '2': 'GND', '3': '+5V', '4': 'CAN_RXD', '5': '+3V3', '6': 'CANL', '7': 'CANH', '8': 'GND'},
      descr='Microchip MCP2562-E/P: CAN FD transceiver, VDD 5 V, VIO 1.8-5.5 V, PDIP-8 (1 TXD, 2 VSS, 3 VDD, 4 RXD, 5 VIO, 6 CANL, 7 CANH, 8 STBY)')
r_('1k', 'CAN_RXD', 'CAN_RX', 95, 235, 'Series R on the receive line (also guards RB3 while JTAG still drives it as TDO on a blank chip)')
c_('100n', '+5V', 'GND', 115, 235, d='MCP2562 VDD')
c_('100n', '+3V3', 'GND', 130, 235, d='MCP2562 VIO')

# --- GPS puck connector
G('gps')
text(170, 200, 'GPS PUCK: J2, JST PH 4-pin side entry (S4B-PH-K), same pinout as the main board\'s J6: 1 puck power, 2 TX (out, to the '
     'puck\'s RX), 3 RX (in, from the puck\'s TX), 4 GND.\nPuck power: 3.3 V or 5 V by JP1, a 3-pin header with a shunt '
     '(1-2 = 3.3 V, 2-3 = 5 V), through a 0.25 A PTC. 100 R in series on TX and RX (no THT ESD array exists: the series '
     'resistors and the PIC\'s own clamps protect the lines).', 1.4)
place(JMP3, 'JP1', 'Puck 3V3 / 5V', 'Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical',
      185, 245, {'1': '+3V3', '2': 'PUCK_VSEL', '3': '+5V'},
      descr='Puck supply select: 3-pin header and a 2.54 mm shunt. 1-2 = 3.3 V (most pucks), 2-3 = 5 V')
place(PTC, 'F1', '0.25A', 'Fuse:Fuse_BelFuse_0ZRE0025FF_L9.6mm_W3.8mm', 210, 245, {'1': 'PUCK_VSEL', '2': 'PUCK_PWR'},
      descr='PTC, radial, 0.25 A hold (Bel Fuse 0ZRE0025FF)')
c_('1u', 'PUCK_PWR', 'GND', 225, 260, d='Puck supply, at J2, X7R')
r_('100', 'PUCK_TXD', 'GPS_TX', 240, 235, 'Series R, card TX to the puck')
r_('100', 'GPS_RX', 'PUCK_RXD', 240, 255, 'Series R, the puck\'s TX into the card')
place(HDR1x4, 'J2', 'GPS puck', 'Connector_JST:JST_PH_S4B-PH-K_1x04_P2.00mm_Horizontal', 310, 245,
      {'1': 'PUCK_PWR', '2': 'GPS_TX', '3': 'GPS_RX', '4': 'GND'},
      descr='JST S4B-PH-K (PH, 4 pin, side entry) on the top edge: 1 puck power, 2 TX out, 3 RX in, 4 GND (as J6 on the main board)')

# ----------------------------------------------------------------------------- write
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "A3")',
       '  (title_block (title "DigiDash GPS card, through-hole") (date "2026-11-19") (rev "B")',
       '    (comment 1 "PIC18F26Q83 + MCP2562: remote GPS puck (UART) -> CAN 0x222-0x226")',
       '    (comment 2 "Card for a DigiDash slot (2x15, on edge), any slot. Every part through-hole"))',
       '  (lib_symbols']
for t in lib_text.values():
    out.append('\n'.join('    ' + ln for ln in t.split('\n')))
out.append('  )')
out += body
out += ['  (sheet_instances (path "/" (page "1")))', ')']
open(OUT, 'w').write('\n'.join(out) + '\n')
json.dump(refs_info, open(os.path.join(os.path.dirname(OUT), 'generate', 'refs.json'), 'w'), indent=1, sort_keys=True)
print('wrote', os.path.normpath(OUT), len(body), 'items', len(refs_info), 'parts')
