#!/usr/bin/env python3
"""Generate BenchBoard.kicad_sch (KiCad 7 file format, opens in 7/8/9/10).

Same pattern as GPSCard/generate/gen_sch.py: every connection is a net label or power symbol on a short stub at the
pin, so the file is a netlist you can read. Needs the stock KiCad symbol libraries (KICAD_SYMBOLS).

The bench board stands in for DigiDash2 on the bench: two card slots (the rev 2 slot pinout,
docs/hardware/accessory_ports.md) and one round screen (the DigiDash2 screen header's signals, on a right-angle
2x20 header at the edge), on one CAN bus with a USB-CAN adapter. 12 V in, 5 V / 3 A buck, 3.3 V LDO.
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
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'BenchBoard.kicad_sch')
PROJECT = 'BenchBoard'
NS = uuid.UUID('7c1e4b2a-93d5-4e8f-a0b6-2d9f5c3e8a17')
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
POWER = {'GND': ('power:GND', 'down'), '+5V': ('power:+5V', 'up'), '+3V3': ('power:+3V3', 'up')}


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
        refs_info[ref] = {'group': cur_group[0], 'fp': footprint, 'value': value, 'nets': {k: v for k, v in nets.items()}}
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
L = stock('Device', 'L')
LED = stock('Device', 'LED')
PTC = stock('Device', 'Polyfuse')
SCHOTTKY = stock('Device', 'D_Schottky')
TIE = stock('Device', 'NetTie_2')
HOLE = stock('Mechanical', 'MountingHole')
JACK = stock('Connector', 'Barrel_Jack')
AUDIO = stock('Connector_Audio', 'AudioJack3')
HDR2x15 = stock('Connector_Generic', 'Conn_02x15_Odd_Even')
HDR2x20 = stock('Connector_Generic', 'Conn_02x20_Odd_Even')
HDR1x6 = stock('Connector_Generic', 'Conn_01x06')
HDR1x4 = stock('Connector_Generic', 'Conn_01x04')
HDR1x3 = stock('Connector_Generic', 'Conn_01x03')
HDR1x2 = stock('Connector_Generic', 'Conn_01x02')
CAN = stock('Interface_CAN_LIN', 'MCP2562-E-SN')
LDO = stock_as('Regulator_Linear', 'AP2112K-3.3', 'BenchBoard:AP2112K-3.3', 'AP2112K-3.3')
BUCK = stock_as('Regulator_Switching', 'TPS563201', 'BenchBoard:TPS563201', 'TPS563201')
FET = stock_as('Transistor_FET', '2N7002', 'BenchBoard:2N7002', '2N7002')
for n in ('GND', '+5V', '+3V3', 'PWR_FLAG'):
    stock('power', n)

FP_R = 'Resistor_SMD:R_0603_1608Metric'
FP_C = 'Capacitor_SMD:C_0603_1608Metric'
FP_C0805 = 'Capacitor_SMD:C_0805_2012Metric'
FP_C1206 = 'Capacitor_SMD:C_1206_3216Metric'
FP_LED = 'LED_SMD:LED_0603_1608Metric'
FP_SOT23 = 'Package_TO_SOT_SMD:SOT-23'
FP_V = 'Connector_PinHeader_2.54mm:PinHeader_1x%02d_P2.54mm_Vertical'

cnt = {}


def nid(prefix):
    cnt[prefix] = cnt.get(prefix, 0) + 1
    return '%s%d' % (prefix, cnt[prefix])


def r_(v, a, b, x, y, d=None):
    ref = nid('R')
    place(R, ref, v, FP_R, x, y, {'1': a, '2': b}, descr=d)
    return ref


def c_(v, a, b, x, y, d=None, fp=None):
    ref = nid('C')
    place(C, ref, v, fp or FP_C, x, y, {'1': a, '2': b}, descr=d)
    return ref


def ptc_(v, a, b, x, y, d=None, fp='Fuse:Fuse_1206_3216Metric'):
    ref = nid('F')
    place(PTC, ref, v, fp, x, y, {'1': a, '2': b}, descr=d)
    return ref


def fet_(gate, drain, x, y, d):
    ref = nid('Q')
    place(FET, ref, '2N7002', FP_SOT23, x, y, {'1': gate, '2': 'GND', '3': drain}, descr=d)
    return ref


def hdr_(sym, n, value, x, y, nets, d, fp=None):
    ref = nid('J')
    place(sym, ref, value, fp or FP_V % n, x, y, nets, descr=d)
    return ref


text(15, 12, 'DIGIDASH BENCH BOARD  -  two card slots + one round screen on one CAN bus, for the bench (stands in for DigiDash2)', 3)
text(15, 20, 'Slot pinout: docs/hardware/accessory_ports.md (rev 2). Screen header: DigiDash2 J5 / J1 / J4 signals on a right-angle 2x20 '
     'at the board edge (the Waveshare ESP32-P4-WIFI6-Touch-LCD-4C plugs on, standing up). 12 V in (bench adapter, 17 V max).', 1.6)

# --- power in
G('power')
text(15, 30, 'POWER: 12 V barrel jack -> SS34 (reverse polarity) -> VIN. VIN feeds the slots\' constant 12 V and AUX 12 V, the IGN jumper '
     '(switched 12 V: fitted = key on), and a TPS563201 buck to 5 V / 3 A (TI\'s typical application, Table 7-2 values for 5 V). '
     'AP2112K-3.3 from 5 V.', 1.4)
place(JACK, nid('J'), '12V IN', 'Connector_BarrelJack:BarrelJack_CUI_PJ-063AH_Horizontal', 25, 55,
      {'1': 'VIN_RAW', '2': 'GND'}, descr='12 V in: 5.5 / 2.1 mm barrel jack, centre positive (CUI PJ-063AH)')
place(SCHOTTKY, nid('D'), 'SS34', 'Diode_SMD:D_SMA', 45, 50, {'2': 'VIN_RAW', '1': 'VIN'},
      descr='Reverse-polarity diode, 3 A Schottky (SS34, SMA)')
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', 45, 65, {'1': 'VIN_RAW'}, show_value=False)
place('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', 55, 65, {'1': 'VIN'}, show_value=False)
place('power:PWR_FLAG', '#FLG03', 'PWR_FLAG', '', 65, 65, {'1': 'GND'}, show_value=False)
c_('10u', 'VIN', 'GND', 70, 50, d='Buck input (TI C1), 25 V', fp=FP_C1206)
c_('10u', 'VIN', 'GND', 80, 50, d='Buck input (TI C2), 25 V', fp=FP_C1206)
c_('100n', 'VIN', 'GND', 90, 50, d='Buck input (TI C3), at VIN')
place(BUCK, 'U1', 'TPS563201', 'Package_TO_SOT_SMD:SOT-23-6', 115, 55,
      {'1': 'GND', '2': 'BUCK_SW', '3': 'VIN', '4': 'BUCK_FB', '5': 'BUCK_EN', '6': 'BUCK_BST'},
      descr='TI TPS563201DDCR: 3 A synchronous buck, 4.5-17 V in, SOT-23-6. 5 V out')
r_('100k', 'VIN', 'BUCK_EN', 100, 70, 'EN pull-up (EN is rated to 19 V)')
c_('100n', 'BUCK_BST', 'BUCK_SW', 135, 45, d='Bootstrap, VBST to SW (TI C7)')
place(L, 'L1', '3.3u', 'Inductor_SMD:L_Bourns_SRN6045TA', 150, 55, {'1': 'BUCK_SW', '2': '+5V'},
      descr='3.3 uH, 6 x 6 mm, Isat > 4 A (Bourns SRN6045TA-3R3M). TI Table 7-2: 3.3 uH for 5 V')
r_('54.9k', '+5V', 'BUCK_FB', 135, 70, 'Feedback top (TI R1, 5 V: 0.768 x (1 + 54.9 / 10) = 4.98 V)')
r_('10k', 'BUCK_FB', 'GND', 150, 70, 'Feedback bottom (TI R2)')
c_('22u', '+5V', 'GND', 165, 50, d='Buck output (TI C8)', fp=FP_C0805)
c_('22u', '+5V', 'GND', 175, 50, d='Buck output (TI C9)', fp=FP_C0805)
place('power:PWR_FLAG', '#FLG04', 'PWR_FLAG', '', 165, 65, {'1': '+5V'}, show_value=False)
place(LDO, 'U2', 'AP2112K-3.3', 'Package_TO_SOT_SMD:SOT-23-5', 200, 55,
      {'1': '+5V', '2': 'GND', '3': '+5V', '4': None, '5': '+3V3'},
      descr='Diodes AP2112K-3.3TRG1: 600 mA LDO, 3.3 V, EN tied to VIN')
c_('10u', '+3V3', 'GND', 220, 50, d='LDO output', fp=FP_C0805)
c_('100n', '+3V3', 'GND', 230, 50)
r_('1k', '+5V', 'PWR_LED_A', 245, 50, 'Power LED current')
place(LED, nid('D'), 'PWR', FP_LED, 260, 50, {'2': 'PWR_LED_A', '1': 'GND'}, descr='Power LED: 5 V present')
hdr_(HDR1x2, 2, 'IGN', 270, 70, {'1': 'VIN', '2': 'SW12V'},
     'IGN jumper: fitted = key on (switched 12 V to the slots, pin 8); removed = key off')
r_('10k', 'SW12V', 'GND', 285, 70, 'Bleeds switched 12 V to 0 V with IGN removed')

# --- slots
G('slot')
text(15, 90, 'CARD SLOTS: slot 1 and slot 2 of DigiDash2 (J31 / J32), 2x15 sockets 25.4 mm apart, cards on edge with their parts '
     'side toward slot 1\'s open end. Per slot, as DigiDash2: 0.5 A PTC on 5 V and switched 12 V, 0.1 A on 3.3 V, 0.2 A on '
     'constant 12 V, a slot-ID resistor (1k slot 1, 3.3k slot 2: nodes 0x14 / 0x15). Shared: CAN, WAKE, LAMP, REV, the I2S '
     'rail, the audio bus, AUX 12 V (one 1.5 A PTC; AUX GND meets GND at NT1, audio GND at NT2).', 1.4)
for k, (x0, rid) in enumerate(((30, '1k'), (150, '3.3k')), 1):
    s = 'S%d' % k
    nets = {'1': s + '_5V', '2': s + '_5V', '3': 'GND', '4': 'GND', '5': 'CANH', '6': 'CANL', '7': 'GND', '8': s + '_12V',
            '9': s + '_3V3', '10': s + '_BATT', '11': s + '_ID', '12': 'WAKE', '13': 'LAMP_LVL', '14': 'REV_LVL',
            '15': 'I2S_BCK', '16': 'I2S_WS', '17': 'I2S_MIC', '18': 'I2S_RET', '19': 'GND', '20': 'GND',
            '21': 'AUDIO_L', '22': 'AUDIO_R', '23': 'AUDIO_GND', '24': 'I2S_MCLK',
            '25': 'AUX_12V', '26': 'AUX_12V', '27': 'AUX_12V', '28': 'AUX_GND', '29': 'AUX_GND', '30': 'AUX_GND'}
    hdr_(HDR2x15, 15, 'Slot %d' % k, x0 + 40, 130, nets,
         'Card slot %d: 2x15 female socket, 2.54 mm, vertical (the DigiDash2 slot pinout)' % k,
         fp='Connector_PinSocket_2.54mm:PinSocket_2x15_P2.54mm_Vertical')
    ptc_('0.5A', '+5V', s + '_5V', x0, 115, 'Slot %d 5 V, 0.5 A hold' % k)
    ptc_('0.5A', 'SW12V', s + '_12V', x0, 125, 'Slot %d switched 12 V, 0.5 A hold' % k)
    ptc_('0.1A', '+3V3', s + '_3V3', x0, 135, 'Slot %d 3.3 V, 0.1 A hold' % k)
    ptc_('0.2A', 'VIN', s + '_BATT', x0, 145, 'Slot %d constant 12 V, 0.2 A hold' % k)
    r_(rid, s + '_ID', 'GND', x0, 155, 'Slot %d ID (as DigiDash2: the card reads it with its own pull-up)' % k)
ptc_('1.5A', 'VIN', 'AUX_12V', 30, 170, 'AUX 12 V to both slots, 1.5 A hold (1812)', fp='Fuse:Fuse_1812_4532Metric')
place(TIE, 'NT1', 'NetTie', 'NetTie:NetTie-2_SMD_Pad0.5mm', 60, 170, {'1': 'GND', '2': 'AUX_GND'},
      descr='AUX GND meets GND here only (as DigiDash2 NT1)')
place(TIE, 'NT2', 'NetTie', 'NetTie:NetTie-2_SMD_Pad0.5mm', 80, 170, {'1': 'GND', '2': 'AUDIO_GND'},
      descr='Audio GND meets GND here only (as DigiDash2 NT2)')
c_('10u', '+5V', 'GND', 100, 170, d='5 V bulk at the slots', fp=FP_C0805)
c_('100n', '+3V3', 'GND', 115, 170, d='3.3 V at the slots')

# --- WAKE, LAMP, REV
G('ctrl')
text(15, 185, 'WAKE: 100k pull-down (as DigiDash2); a card holding it high lights the WAKE LED (2N7002, so the LED loads nothing). '
     'LAMP / REV jumpers: fitted = dash lights on / in reverse. Each drives the slots\' level (3.3 V through 1k) and, through a '
     '2N7002, pulls the screen\'s pin low (GPIO24 illumination: low = dim; GPIO25 reverse trigger: low = reverse).', 1.4)
r_('100k', 'WAKE', 'GND', 30, 205, 'WAKE pull-down (as DigiDash2 R47)')
fet_('WAKE', 'WAKE_K', 45, 205, 'WAKE LED driver')
r_('1k', '+5V', 'WAKE_A', 60, 200, 'WAKE LED current')
place(LED, nid('D'), 'WAKE', FP_LED, 75, 200, {'2': 'WAKE_A', '1': 'WAKE_K'}, descr='WAKE LED: a card holds WAKE high')
hdr_(HDR1x2, 2, 'LAMP', 100, 205, {'1': '+3V3', '2': 'LAMP_SW'}, 'LAMP jumper: fitted = dash lights on')
r_('100k', 'LAMP_SW', 'GND', 115, 210, 'LAMP off without the jumper')
r_('1k', 'LAMP_SW', 'LAMP_LVL', 130, 200, 'LAMP level to the slots (pin 13)')
fet_('LAMP_SW', 'SCR_ILLUM', 145, 210, 'Screen illumination input (GPIO24, active low = dim)')
hdr_(HDR1x2, 2, 'REV', 170, 205, {'1': '+3V3', '2': 'REV_SW'}, 'REV jumper: fitted = in reverse')
r_('100k', 'REV_SW', 'GND', 185, 210, 'REV off without the jumper')
r_('1k', 'REV_SW', 'REV_LVL', 200, 200, 'REV level to the slots (pin 14)')
fet_('REV_SW', 'SCR_REV', 215, 210, 'Screen reverse trigger (GPIO25, active low)')

# --- CAN
G('can')
text(15, 225, 'CAN: one bus for the slots, the screen and the USB-CAN header (H L G). TERM jumper: fitted = 120 R here; '
     'terminate the adapter end too. The screen\'s MCP2562 is as on DigiDash2 (VDD 5 V, VIO 3.3 V, STBY low, 10k on TXD).', 1.4)
hdr_(HDR1x3, 3, 'CAN', 30, 250, {'1': 'CANH', '2': 'CANL', '3': 'GND'}, 'USB-CAN adapter: 1 CANH, 2 CANL, 3 GND')
r_('120', 'CANH', 'CAN_TERM', 50, 245, 'Bus termination, with the TERM jumper')
hdr_(HDR1x2, 2, 'TERM', 65, 250, {'1': 'CAN_TERM', '2': 'CANL'}, 'TERM jumper: fitted = 120 R across the bus')
place(CAN, 'U3', 'MCP2562-E/SN', 'Package_SO:SOIC-8_3.9x4.9mm_P1.27mm', 110, 255,
      {'1': 'SCR_CAN_TX', '2': 'GND', '3': '+5V', '4': 'SCR_CAN_RX', '5': '+3V3', '6': 'CANL', '7': 'CANH', '8': 'GND'},
      descr='Microchip MCP2562-E/SN: the screen\'s CAN transceiver (as DigiDash2 U9)')
r_('10k', '+3V3', 'SCR_CAN_TX', 140, 245, 'TXD held recessive while the screen boots (as DigiDash2 R16)')
c_('100n', '+5V', 'GND', 150, 260, d='MCP2562 VDD')
c_('100n', '+3V3', 'GND', 160, 260, d='MCP2562 VIO')

# --- screen
G('screen')
# Waveshare 40-pin header (Raspberry Pi numbering, read off the board's silkscreen): the pins the DigiDash2 screen
# headers use. The bench board's right-angle header meets the screen's socket with its columns reversed (the screen
# stands up off the edge, facing away), so pad p meets screen pin 40 - p (odd p) / 42 - p (even p).
WS = {2: '+5V', 4: '+5V', 6: 'GND', 9: 'GND', 14: 'GND', 20: 'GND', 25: 'GND', 30: 'GND', 34: 'GND', 39: 'GND',
      17: 'SCR_3V3', 18: 'SCR_ROLE', 16: 'SCR_SENS_RX', 19: 'SCR_SER_TX', 21: 'SCR_SER_RX',
      24: 'SCR_CAN_RX', 32: 'SCR_CAN_TX', 27: 'SCR_ILLUM', 28: 'SCR_REV', 33: 'CL_SCL', 37: 'CL_SDA'}


def screen_pin(pad):
    return 40 - pad if pad % 2 else 42 - pad


scr = {str(p): WS.get(screen_pin(p)) for p in range(1, 41)}
text(15, 275, 'SCREEN: right-angle 2x20 at the board edge; the screen (Waveshare ESP32-P4-WIFI6-Touch-LCD-4C) plugs on and stands '
     'up, facing away. Its pin numbers (Raspberry Pi order, on its silkscreen) meet this header\'s pads reversed: pad p = screen pin '
     '40 - p (odd) / 42 - p (even).\nScreen pins used (DigiDash2 J5 / J1 / J4 signals): 2 / 4 5 V; 6 9 14 20 25 30 34 39 GND; '
     '17 its 3V3 (role strap only); 18 GPIO4 role strap; 16 GPIO5 sensor serial in; 19 GPIO3 TX / 21 GPIO2 RX (serial port, '
     'firmware GPS_UART_TX / _RX); 24 GPIO36 CAN RX; 32 GPIO34 CAN TX; 27 GPIO24 illumination; 28 GPIO25 reverse; '
     '33 GPIO48 / 37 GPIO47 cluster I2C. Video pins: not used.', 1.4)
hdr_(HDR2x20, 20, 'Screen', 250, 320, scr,
     'Screen: 2x20 right-angle male header, 2.54 mm, at the board edge (pad p = screen pin 40 - p odd / 42 - p even)',
     fp='Connector_PinHeader_2.54mm:PinHeader_2x20_P2.54mm_Horizontal')
hdr_(HDR1x3, 3, 'ROLE', 30, 300, {'1': 'ROLE_L', '2': 'SCR_ROLE', '3': 'ROLE_R'},
     'ROLE jumper: 1-2 = left, 2-3 = right, none = center (the DigiDash2 strap on GPIO4)')
r_('1k', 'ROLE_L', 'GND', 50, 295, 'Left strap: 1k to GND (as DigiDash2 R37)')
r_('1k', 'ROLE_R', 'SCR_3V3', 50, 310, 'Right strap: 1k to the screen\'s 3V3 (as DigiDash2)')
hdr_(HDR1x2, 2, 'SENS', 30, 325, {'1': 'SENS_IN', '2': 'GND'},
     'Sensor serial in: a jumper wire from the sensor card\'s J8 (pin 2 TX, pin 1 GND); 5 V logic')
r_('10k', 'SENS_IN', 'SCR_SENS_RX', 50, 325, 'Sensor serial divider top (as DigiDash2)')
r_('20k', 'SCR_SENS_RX', 'GND', 65, 330, 'Sensor serial divider bottom: 5 V to 3.3 V')
hdr_(HDR1x4, 4, 'SERIAL', 90, 300, {'1': '+3V3', '2': 'SCR_SER_TX', '3': 'SCR_SER_RX', '4': 'GND'},
     'Serial port (GPS puck), JST PH 4 pin side entry, as DigiDash2 J6: 1 3V3, 2 TX out (screen GPIO3), 3 RX in (screen GPIO2), 4 GND',
     fp='Connector_JST:JST_PH_S4B-PH-K_1x04_P2.00mm_Horizontal')
hdr_(HDR1x3, 3, 'I2C', 130, 300, {'1': 'CL_SCL', '2': 'CL_SDA', '3': 'GND'},
     'Cluster I2C (screen GPIO48 SCL, GPIO47 SDA): to a second screen or a probe')
r_('4.7k', '+3V3', 'CL_SCL', 150, 295, 'Cluster I2C pull-up (as DigiDash2)')
r_('4.7k', '+3V3', 'CL_SDA', 165, 295, 'Cluster I2C pull-up (as DigiDash2)')

# --- I2S rail, audio
G('audio')
text(15, 345, 'I2S RAIL (slot pins 15-18, 24): bused between the slots, as DigiDash2, with a probe header. AUDIO BUS (21 L, 22 R, 23 GND): '
     'to a 3.5 mm jack (headphones or an amplifier).', 1.4)
hdr_(HDR1x6, 6, 'I2S', 30, 370, {'1': 'I2S_BCK', '2': 'I2S_WS', '3': 'I2S_MIC', '4': 'I2S_RET', '5': 'I2S_MCLK', '6': 'GND'},
     'I2S rail probe: 1 BCK, 2 WS, 3 MIC, 4 RET, 5 MCLK, 6 GND')
place(AUDIO, nid('J'), 'AUDIO', 'Connector_Audio:Jack_3.5mm_CUI_SJ1-3523N_Horizontal', 80, 370,
      {'T': 'AUDIO_L', 'R': 'AUDIO_R', 'S': 'AUDIO_GND'}, descr='Audio bus out: 3.5 mm stereo jack (tip L, ring R, sleeve audio GND)')

# --- mounting
G('misc')
for k in range(4):
    place(HOLE, nid('H'), 'M3', 'MountingHole:MountingHole_3.2mm_M3', 120 + 15 * k, 370, {},
          descr='M3, for 45 mm standoffs (the screen hangs about 35 mm below the header)')

# ----------------------------------------------------------------------------- write
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "A3")',
       '  (title_block (title "DigiDash bench board") (date "2026-10-05") (rev "A")',
       '    (comment 1 "Two card slots + one round screen on one CAN bus, 12 V in")',
       '    (comment 2 "Stands in for DigiDash2 on the bench"))',
       '  (lib_symbols']
for t in lib_text.values():
    out.append('\n'.join('    ' + ln for ln in t.split('\n')))
out.append('  )')
out += body
out += ['  (sheet_instances (path "/" (page "1")))', ')']
open(OUT, 'w').write('\n'.join(out) + '\n')
json.dump(refs_info, open(os.path.join(os.path.dirname(OUT), 'generate', 'refs.json'), 'w'), indent=1, sort_keys=True)
print('wrote', os.path.normpath(OUT), len(body), 'items', len(refs_info), 'parts')
