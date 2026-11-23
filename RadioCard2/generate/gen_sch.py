#!/usr/bin/env python3
"""Generate RadioCard2.kicad_sch: the radio card, rev B, every part through-hole (KiCad 7 file format, opens in 7/8/9/10).

Rev B is the hand-assembled version of RadioCard/ (rev A: STM32G0B1 + Si4735 + SMD passives): same 80 x 60 mm slot
card, same slot use (CAN, I2S rail BCK / WS in, RET out), same CAN IDs, but a PIC18F26Q83 in a DIP-28 (as GPSCard2),
an MCP2562 in DIP-8, a TO-92 regulator, a 74HC125 in DIP-14 as the RET buffer, and the tuner on a plug-in module: the
owner's Si4737-C40-GU breakout (FMBreakout.brd, OSH Park), in a 1x8 and a 1x9 socket.

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
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'RadioCard2.kicad_sch')
PROJECT = 'RadioCard2'
NS = uuid.UUID('3d8f1a62-9c4e-4b7a-8e21-5f0c7d9b2a44')
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
SCHOTTKY = stock_as('Diode', '1N5711', 'RadioCard2:1N5711', '1N5711')
SPARK = stock('Device', 'SparkGap')
IND = stock('Device', 'L')
XTAL = stock('Device', 'Crystal')
HDR2x15 = stock('Connector_Generic', 'Conn_02x15_Odd_Even')
HDR1x9 = stock('Connector_Generic', 'Conn_01x09')
HDR1x8 = stock('Connector_Generic', 'Conn_01x08')
HDR1x6 = stock('Connector_Generic', 'Conn_01x06')
HDR1x3 = stock('Connector_Generic', 'Conn_01x03')
HDR1x2 = stock('Connector_Generic', 'Conn_01x02')
COAX = stock('Connector', 'Conn_Coaxial')
JMP3 = stock('Jumper', 'Jumper_3_Bridged12')
JMP2 = stock_as('Jumper', 'Jumper_2_Open', 'RadioCard2:Jumper_2_Open', 'AM from whip')
CAN = stock('Interface_CAN_LIN', 'MCP2562-E-P')
# KiCad has no PIC18F26Q83 symbol. The PIC18F2xK83 SPDIP-28 symbol has the same pins (as GPSCard2: checked against the
# Q83 data sheet DS40002253 pin allocation table).
MCU = stock_as('MCU_Microchip_PIC18', 'PIC18F26K83-xSP', 'RadioCard2:PIC18F26Q83-I_SP', 'PIC18F26Q83-I/SP')
LDO = stock_as('Regulator_Linear', 'MCP1700x-330xxTO', 'RadioCard2:MCP1700-3302E_TO', 'MCP1700-3302E/TO')
# 74HC125 = the 74LS125 pinout (four buffers, OE active low: 1 1OE, 2 1A, 3 1Y, ..., 7 GND, 14 VCC)
BUF = stock_as('74xx', '74LS125', 'RadioCard2:74HC125', '74HC125')
for n in ('GND', '+5V', '+3V3', 'PWR_FLAG'):
    stock('power', n)

FP_R = 'Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal'
FP_C = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P2.50mm'
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
    place(C, ref, v, FP_C, x, y, {'1': a, '2': b}, descr=d)
    return ref


text(15, 12, 'AM/FM RADIO CARD rev B (all through-hole)  -  Si4737 breakout + PIC18F26Q83 + MCP2562: I2S audio to the audio card on the slot I2S rail, CAN for control', 3)
text(15, 20, 'Card for a DigiDash slot, 80 x 60 mm, standing on edge (docs/hardware/accessory_ports.md), any slot. '
     'CAN node 0x14 + (slot - 1), product 0x0009 (docs/hardware/radio_card.md). 5 V from the slot only; 3.3 V on board.\n'
     'Every part is through-hole for hand assembly, except J2 (edge-mount SMA, hand-soldered) and D2 (antenna ESD clamp, 1 x 0.6 mm SMD), which is not fitted.', 1.6)

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
text(70, 40, 'Slot ID (pin 11): 10k to 3.3 V here, the slot\'s own resistor to GND on the main board, read on RA1 (ANA1). '
     'The rail is driven by the audio card only; this card only listens to BCK / WS and drives RET after a NET RAIL grant.', 1.4)
r_('10k', '+3V3', 'SLOT_ID', 25, 45, 'Slot ID pull-up (the main board has one resistor to GND per slot)')
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', 25, 85, {'1': '+5V'}, show_value=False)
place('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', 40, 85, {'1': 'GND'}, show_value=False)

# --- power
G('power')
text(15, 100, 'POWER: slot +5 V -> MCP1700-3302E/TO (250 mA LDO, TO-92: 1 GND, 2 VIN, 3 VOUT) -> +3V3 (PIC, MCP2562 VIO, 74HC125). '
     'The tuner module (VA and VD tied on the breakout) takes +3V3 through a ferrite bead: +3V3A, with 10 uF + 100 nF at its 3.3V pin '
     '(the breakout has no supply decoupling of its own). About 40 mA in all.', 1.4)
c_('10u', '+5V', 'GND', 25, 125, d='Slot 5 V bulk / LDO input, radial MLCC X5R')
place(LDO, 'U3', 'MCP1700-3302E/TO', 'Package_TO_SOT_THT:TO-92_Inline_Wide', 60, 120,
      {'1': 'GND', '2': '+5V', '3': '+3V3'},
      descr='Microchip MCP1700-3302E/TO: 3.3 V 250 mA LDO, TO-92 (1 GND, 2 VIN, 3 VOUT)')
c_('10u', '+3V3', 'GND', 90, 125, d='LDO output, radial MLCC X5R')
place(FB, 'FB1', 'Ferrite bead', 'Inductor_THT:L_Axial_L5.0mm_D3.6mm_P10.00mm_Horizontal_Murata_BL01RN1A2A2', 125, 120,
      {'1': '+3V3', '2': '+3V3A'},
      descr='Through-hole ferrite bead (Murata BL01RN1A2A2B or any bead of about 100 R at 100 MHz): keeps digital noise off the tuner supply')
c_('10u', '+3V3A', 'GND', 145, 125, d='Tuner supply bulk, at the module 3.3V pin, radial MLCC X5R')
c_('100n', '+3V3A', 'GND', 160, 125, d='Tuner supply bypass, at the module 3.3V pin (the breakout has none)')
place('power:PWR_FLAG', '#FLG03', 'PWR_FLAG', '', 175, 115, {'1': '+3V3A'}, show_value=False)

# --- MCU
G('mcu')
text(190, 30, 'MCU: PIC18F26Q83-I/SP (SPDIP-28, in a socket), as GPSCard2. Pin use (DS40002253 pin allocation and PPS tables):\n'
     'RC3 (pin 14) SCL1, RC4 (pin 15) SDA1 (the I2C module\'s default pins): the tuner.  RA2 (pin 4) tuner RST (100k to GND keeps it in reset).  '
     'RB0 (pin 21) tuner INT (GPO2, INT0).  RA4 (pin 6) RET buffer enable, ACTIVE LOW (10k pull-up keeps the buffer off at reset).\n'
     'RB2 (pin 23) CANTX by PPS, RB3 (pin 24) CANRX (its POR default).  RB4 (pin 25) U2TX, RB5 (pin 26) U2RX by PPS: debug serial (J4).\n'
     'RA1 (pin 3) ANA1: slot ID.  RC1 (pin 12) LED tuned, RC2 (pin 13) LED stereo / RDS.  RA7 / RA6 (pins 9 / 10): 16 MHz crystal, HS, 4x PLL = 64 MHz.\n'
     'Config: JTAGEN off (JTAG shares RA0, RA5, RB3, RB5), LVP on. ICSP: MCLR (pin 1), ICSPCLK RB6 (27), ICSPDAT RB7 (28).', 1.4)
mcu = {'1': 'MCLR', '2': None, '3': 'SLOT_ID', '4': 'TUN_RST', '5': None, '6': 'RET_OE', '7': None, '8': 'GND',
       '9': 'OSC1', '10': 'OSC2', '11': None, '12': 'LED_TUNED', '13': 'LED_STEREO', '14': 'TUN_SCL', '15': 'TUN_SDA',
       '16': None, '17': None, '18': None, '19': 'GND', '20': '+3V3', '21': 'TUN_INT', '22': None, '23': 'CAN_TXD',
       '24': 'CAN_RX', '25': 'DBG_TX', '26': 'DBG_RX', '27': 'ICSPCLK', '28': 'ICSPDAT'}
place(MCU, 'U1', 'PIC18F26Q83-I/SP', 'Package_DIP:DIP-28_W7.62mm_Socket', 230, 120, mcu,
      descr='Microchip PIC18F26Q83-I/SP: PIC18, 64 MHz, 64 KB flash, 4 KB RAM, CAN FD, I2C, 5 UARTs, 1.8-5.5 V, SPDIP-28 (in a 28-pin 0.3 in socket)')
c_('100n', '+3V3', 'GND', 195, 165, d='VDD (pin 20) decoupling, X7R, at the pin')
r_('10k', '+3V3', 'MCLR', 280, 150, 'MCLR pull-up (data sheet: 10k or less; no capacitor, the programmer drives MCLR)')
place(XTAL, 'Y1', '16MHz', 'Crystal:Crystal_HC49-4H_Vertical', 170, 120, {'1': 'OSC1', '2': 'OSC2'},
      descr='16 MHz crystal, HC-49/US (low profile), 18 pF load (Abracon ABL-16.000MHZ-B2 or equal)')
c_('27p', 'OSC1', 'GND', 160, 140, d='Crystal load, C0G (18 pF crystal: 2 x 27 pF with about 5 pF stray)')
c_('27p', 'OSC2', 'GND', 180, 140, d='Crystal load, C0G')
r_('1k', 'LED_TUNED', 'LED_TUNED_A', 280, 95, 'LED current, about 1 mA')
place(LED, 'D1', 'TUNED', 'LED_THT:LED_D3.0mm', 300, 95, {'2': 'LED_TUNED_A', '1': 'GND'},
      descr='3 mm green LED. Off = tuner off, blinking = seeking, steady = tuned. Short lead (flat side) = cathode = pad 1')
r_('1k', 'LED_STEREO', 'LED_STEREO_A', 280, 110, 'LED current, about 1 mA')
place(LED, 'D3', 'STEREO/RDS', 'LED_THT:LED_D3.0mm', 300, 110, {'2': 'LED_STEREO_A', '1': 'GND'},
      descr='3 mm green LED. On = stereo pilot found, blinks with RDS data')

# --- programming
G('prog')
text(15, 150, 'PROGRAMMING: J3 is the Microchip 6-pin ICSP header (PICkit 4 / 5, MPLAB Snap): 1 MCLR/VPP, 2 VDD, 3 GND, 4 ICSPDAT, '
     '5 ICSPCLK, 6 not used (LVP). Power the card from the slot; pin 2 lets the programmer sense 3.3 V. J4 debug serial: UART2, 3.3 V logic.', 1.4)
place(HDR1x6, 'J3', 'ICSP', FP_PINS % '1x06', 40, 185,
      {'1': 'MCLR', '2': '+3V3', '3': 'GND', '4': 'ICSPDAT', '5': 'ICSPCLK', '6': None},
      descr='ICSP (PICkit pinout): 1 MCLR/VPP, 2 VDD, 3 GND, 4 ICSPDAT, 5 ICSPCLK, 6 NC. Right angle on the left edge, pin 1 lowest')
place(HDR1x3, 'J4', 'Debug serial', FP_PINS % '1x03', 90, 185,
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

# --- tuner module
G('tuner')
text(330, 30, 'TUNER: the owner\'s Si4737-C40-GU breakout (FMBreakout.brd, 22.4 x 29.4 mm), plugged into J6 (1x8) and J7 (1x9) sockets, 17.78 mm apart. '
     'Si4737 = AM / FM / WB + RDS + digital audio out (no SW / LW; this card only uses FM and AM).\n'
     'Its pins as its silk reads (to check against the chip by continuity before first power): left header DOUT, DFS, P3 (GPO3 / DCLK), P2 (GPO2 / INT), '
     'P1 (GPO1), FMI, RFG, AMI; right header LOUT, ROUT, 3.3V, RCLK, SDIO, SCLK, SEN, RST, GND, both counted from the headphone-jack end.\n'
     'On the breakout: the DBYP capacitor, 10k pull-ups on SDIO / SCLK to its 3.3 V, the headphone jack on LOUT / ROUT. No crystal (the reference comes from DCLK), '
     'no supply decoupling, no antenna coupling capacitors: those are here.', 1.4)
place(HDR1x8, 'J6', 'Tuner L', 'Connector_PinSocket_2.54mm:PinSocket_1x08_P2.54mm_Vertical', 340, 75,
      {'1': 'TUN_DOUT', '2': 'TUN_DFS', '3': 'TUN_DCLK', '4': 'TUN_INT', '5': None, '6': 'FMI', '7': 'GND', '8': 'AMI'},
      descr='Tuner breakout, left header socket: 1 DOUT, 2 DFS, 3 P3 = GPO3 / DCLK, 4 P2 = GPO2 / INT, 5 P1 = GPO1 (open: it must be high or open at reset), '
            '6 FMI, 7 RFG (RF ground), 8 AMI. Pin 1 at the headphone-jack end. 1x8 female header, 2.54 mm, 8.5 mm')
place(HDR1x9, 'J7', 'Tuner R', 'Connector_PinSocket_2.54mm:PinSocket_1x09_P2.54mm_Vertical', 400, 75,
      {'1': None, '2': None, '3': '+3V3A', '4': 'RCLK', '5': 'TUN_SDA', '6': 'TUN_SCL', '7': 'TUN_SEN', '8': 'TUN_RST', '9': 'GND'},
      descr='Tuner breakout, right header socket: 1 LOUT, 2 ROUT (both open: digital out only), 3 3.3V (VA and VD), 4 RCLK, 5 SDIO, 6 SCLK, 7 SEN, '
            '8 RST, 9 GND. Pin 1 at the headphone-jack end. 1x9 female header, 2.54 mm, 8.5 mm')
text(330, 105, 'Digital audio network (Si4735/37 data sheet optional parts, AN383 tables 6-7): 604 R on DOUT, 2k on DFS and DCLK, 2.2 pF C0G on the DFS line. '
     'I2S mode, slave: DCLK = rail BCK (64 fS), DFS = rail WS. Left channel when DFS is low.', 1.4)
r_('604', 'TUN_DOUT', 'TUN_DOUT_B', 330, 130, 'DOUT series R (data sheet 600 R, AN383 table 6: 604 R E96), towards the RET buffer')
r_('2k', 'TUN_DFS', 'I2S_WS', 330, 145, 'DFS series R (data sheet), WS from the rail')
r_('2k', 'TUN_DCLK', 'I2S_BCK', 330, 160, 'DCLK series R (data sheet), BCK from the rail')
c_('2.2p', 'I2S_WS', 'GND', 370, 145, d='2.2 pF C0G noise capacitor on the DFS line (data sheet, AN383 p.5)')
r_('100k', 'I2S_BCK', 'GND', 385, 145, 'Idle pull-down: the rail has no clocks until the audio card turns them on')
r_('100k', 'I2S_WS', 'GND', 400, 145, 'Idle pull-down: the rail has no clocks until the audio card turns them on')
text(330, 180, 'CONTROL (2-wire I2C, 100 / 400 kHz): the breakout\'s 10k pull-ups to its 3.3 V are the only ones (no second pair here). '
     'SCLK high at the rising edge of RST selects 2-wire; GPO1 high / GPO2 low at that edge (internal pull-up / pull-down). '
     'SEN sets the address (JP1: shunt 1-2 = SEN low = 0x11; 2-3 = 0x63, AN332 6.1). RST has 100k to GND so the tuner waits for the firmware.', 1.4)
r_('100k', 'TUN_RST', 'GND', 340, 205, 'Holds the tuner in reset until the PIC drives RST')
place(JMP3, 'JP1', 'Tuner address', 'Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical', 370, 205,
      {'1': 'GND', '2': 'TUN_SEN', '3': '+3V3'},
      descr='SEN strap: 3-pin header and a 2.54 mm shunt. 1-2 = SEN low, 7-bit address 0x11 (the firmware default); 2-3 = SEN high, 0x63 (AN332 section 6.1)')

# --- reference clock
G('clock')
text(15, 295, 'REFERENCE CLOCK: the tuner takes it from DCLK (the rail BCK, 3.072 MHz at 48 kHz): REFCLK_PRESCALE = 0x1060 (bit 12 RCLKSEL = DCLK, prescale 96), '
     'REFCLK_FREQ = 32000 (AN332 properties 0x0201 / 0x0202). So the tuner runs on the audio card\'s sample clock, and needs no crystal (the breakout has none). '
     'Fallback, unfitted: 0 R to RCLK from the rail MCLK (12.288 MHz, prescale 375 = 32768 Hz).', 1.4)
r_('0', 'I2S_MCLK', 'RCLK', 60, 320, 'UNFITTED. Fallback reference: the rail MCLK into RCLK (REFCLK_PRESCALE 375, RCLKSEL = RCLK)', dnp=True)
r_('100k', 'RCLK', 'GND', 90, 320, 'Holds the unused RCLK input low (RCLKSEL = DCLK)')

# --- RET buffer
G('rail')
text(130, 295, 'RAIL (RET, pin 18): audio to the audio card. 74HC125 buffer A, OFF at power-up (OE pulled high; the PIC pulls it low only after a NET RAIL grant, '
     'and releases it within 10 ms of a withdrawal), 33 R in series at the pin. Buffers B-D unused: OE high (off), input to GND.', 1.4)
place(BUF, 'U5', '74HC125', 'Package_DIP:DIP-14_W7.62mm', 160, 320,
      {'1': 'RET_OE', '2': 'TUN_DOUT_B', '3': 'RET_Y'}, unit=1,
      descr='74HC125 (TI SN74HC125N or equal), DIP-14, at 3.3 V: four 3-state buffers, OE active low. Buffer A drives RET')
place(BUF, 'U5', '74HC125', 'Package_DIP:DIP-14_W7.62mm', 160, 345, {'4': '+3V3', '5': 'GND', '6': None}, unit=2, first=False)
place(BUF, 'U5', '74HC125', 'Package_DIP:DIP-14_W7.62mm', 185, 345, {'10': '+3V3', '9': 'GND', '8': None}, unit=3, first=False)
place(BUF, 'U5', '74HC125', 'Package_DIP:DIP-14_W7.62mm', 210, 345, {'13': '+3V3', '12': 'GND', '11': None}, unit=4, first=False)
place(BUF, 'U5', '74HC125', 'Package_DIP:DIP-14_W7.62mm', 235, 320, {'14': '+3V3', '7': 'GND'}, unit=5, first=False)
r_('10k', '+3V3', 'RET_OE', 130, 340, 'Keeps the buffer OFF at reset, while the PIC is blank, and until the PIC drives RA4 low')
r_('33', 'RET_Y', 'I2S_RET', 195, 320, 'Series R at the rail pin (rail rule)')
c_('100n', '+3V3', 'GND', 255, 320, d='74HC125 supply, at pin 14')

# --- antenna
G('antenna')
text(270, 275, 'ANTENNA: J2 SMA, edge mount (the one part soldered on both faces of the board edge), on the top edge. FM (AN383 section 7, fig. 23): L1 56 nH from the antenna node to RF ground '
     '(bench-tune for the car whip and its cable, or leave it off), then 1 nF C0G to FMI. D2 (ESD clamp, under 1 pF; FMI / AMI absolute max 0.4 Vpk) '
     'has no through-hole version: its pads are here, not fitted. In its place: D4 / D5, two 1N5711 Schottky diodes back to back (about 0.4 V clamp, about 4 pF together; retune L1 with them in), behind SG1, a 0.25 mm PCB spark gap. AM: a ferrite loopstick (180-450 uH) on J5 between the AM node and ground, 470 nF to AMI. '
     'JP2 (open) would feed the SMA to AM too. Keep AM stray capacitance under 10 pF.', 1.4)
place(COAX, 'J2', 'Antenna (SMA)', 'Connector_Coaxial:SMA_Amphenol_132289_EdgeMount', 280, 310, {'1': 'FM_ANT', '2': 'GND'},
      descr='Amphenol RF 132289 SMA jack, edge mount, 1.6 mm board, on the top edge (as rev A; hand-soldered, the owner has one). For the car whip, a Motorola-to-SMA pigtail')
place(TVS, 'D2', 'TPD1E0B04', 'Diode_SMD:D_0402_1005Metric', 320, 330, {'1': 'FM_ANT', '2': 'GND'}, dnp=True,
      descr='NOT FITTED (SMD, no through-hole version). TI TPD1E0B04DPYR: 1-channel bidirectional ESD, 0.13 pF, X1SON 1.0 x 0.6 mm (0402 land pattern)')
FP_DO35 = 'Diode_THT:D_DO-35_SOD27_P2.54mm_Vertical_CathodeUp'   # standing up: the RF corner is tight
place(SCHOTTKY, 'D4', '1N5711', FP_DO35, 320, 375, {'1': 'FM_ANT', '2': 'GND'},
      descr='ESD clamp, the through-hole stand-in for D2: 1N5711 Schottky (70 V, 2 pF), cathode to the antenna node. Clamps negative spikes at about -0.4 V')
place(SCHOTTKY, 'D5', '1N5711', FP_DO35, 345, 375, {'1': 'GND', '2': 'FM_ANT'},
      descr='ESD clamp, D4\'s partner the other way round: 1N5711, anode to the antenna node. Clamps positive spikes at about +0.4 V. D4 + D5 add about 4 pF: retune L1 with them fitted')
place(SPARK, 'SG1', 'Spark gap', 'RadioCard2:SparkGap_0.25mm', 370, 375, {'1': 'FM_ANT', '2': 'GND'},
      descr='PCB spark gap, 0.25 mm, bare copper (nothing to fit): takes the bulk of a large static hit off the SMA centre before D4 / D5')
place(IND, 'L1', '56nH', 'Inductor_THT:L_Axial_L5.3mm_D2.2mm_P7.62mm_Horizontal_Vishay_IM-1', 335, 345, {'1': 'FM_ANT', '2': 'GND'},
      descr='56 nH axial RF inductor, Q >= 15 at 100 MHz (AN383 fig. 23). Or about 5 turns of 0.5 mm wire on a 3 mm form. Bench-tune for the car whip')
c_('1n', 'FM_ANT', 'FMI', 350, 310, d='FM coupling, C0G')
c_('470n', 'AM_ANT', 'AMI', 380, 310, d='AM coupling, radial MLCC X7R')
place(HDR1x2, 'J5', 'AM loopstick', FP_PINS % '1x02', 410, 320, {'1': 'AM_ANT', '2': 'GND'},
      descr='AM ferrite loopstick (180-450 uH) between the AM node and ground. Right angle on the top edge; a wire pair to a loopstick off the card')
place(JMP2, 'JP2', 'AM from whip', 'Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical', 320, 355,
      {'1': 'FM_ANT', '2': 'AM_ANT'},
      descr='2-pin header, no shunt (as delivered) = AM only from the loopstick. A shunt = the SMA also feeds AM, with a network for a car whip that AN383 does not give')

# ----------------------------------------------------------------------------- write
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "A2")',
       '  (title_block (title "DigiDash AM/FM radio card, through-hole") (date "2026-11-20") (rev "B")',
       '    (comment 1 "Si4737 breakout + PIC18F26Q83 + MCP2562: I2S to the audio card (slot I2S rail), CAN control")',
       '    (comment 2 "Card for a DigiDash slot (2x15, on edge), any slot. Every part through-hole (D2 not fitted)"))',
       '  (lib_symbols']
for t in lib_text.values():
    out.append('\n'.join('    ' + ln for ln in t.split('\n')))
out.append('  )')
out += body
out += ['  (sheet_instances (path "/" (page "1")))', ')']
open(OUT, 'w').write('\n'.join(out) + '\n')
json.dump(refs_info, open(os.path.join(os.path.dirname(OUT), 'generate', 'refs.json'), 'w'), indent=1, sort_keys=True)
lone = {n: p for n, p in netmap.items() if len(p) < 2}
if lone:
    print('NETS WITH ONE PIN:', lone)
json.dump(netmap, open(os.path.join(os.path.dirname(OUT), 'generate', 'netlist.json'), 'w'), indent=1, sort_keys=True)
print('wrote', os.path.normpath(OUT), len(body), 'items', len(refs_info), 'parts')
