#!/usr/bin/env python3
"""Generate SensorCard.kicad_sch: the sensor pack as a DigiDash slot card (docs/hardware/accessory_ports.md, rev 2).

KiCad 7 file format (opens in 7-10). Needs the stock KiCad symbol libraries. Same helpers as DigiDash2/generate/gen_sch.py
(every connection is a net label or a power symbol on a short stub at the pin, parts packed in blocks, one block per
function), plus not-fitted parts (dnp=True): the sender-dependent pull-up / pull-down pairs. Also writes
generate/refs.json (each part's block and DNP flag, used by gen_pcb.py).
"""
import json
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, grab_symbol  # noqa: E402

SYMDIR = os.environ.get('KICAD_SYMBOLS', next((d for d in ('/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols',
                                                               '/usr/share/kicad/symbols') if os.path.isdir(d)), ''))
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'SensorCard.kicad_sch')
PROJECT = 'SensorCard'
NS = uuid.UUID('5e2c7a10-4b8d-4f3e-a6c1-0d9e8f7a3c04')
SHEET = str(uuid.uuid5(NS, 'sheet'))
_n = 0


def uid(tag=''):
    global _n
    _n += 1
    return str(uuid.uuid5(NS, '%s#%d' % (tag, _n)))


def f(v):
    return ('%.4f' % v).rstrip('0').rstrip('.')


# ----------------------------------------------------------------------------- symbols
lib_text, lib_pins, lib_props = {}, {}, {}


def register(lib_id, text):
    lib_text[lib_id] = text
    t = parse(text)
    pins, props = {}, {}

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
    lib_pins[lib_id], lib_props[lib_id] = pins, props


def stock(lib, name, as_name=None, value=None):
    """A stock symbol; with as_name, a stock symbol (or the parent of one that extends another) under a new id."""
    txt = grab_symbol(os.path.join(SYMDIR, lib + '.kicad_sym'), name).strip()
    if as_name is None:
        lib_id = '%s:%s' % (lib, name)
        txt = re.sub(r'^\(symbol "%s"' % re.escape(name), '(symbol "%s"' % lib_id, txt, count=1)
    else:
        lib_id = 'SensorCard:' + as_name
        txt = txt.replace('(symbol "%s"' % name, '(symbol "%s"' % lib_id, 1)
        txt = txt.replace('"%s_' % name, '"%s_' % as_name)
        if value:
            txt = re.sub(r'(\(property "Value" )"[^"]*"', r'\1"%s"' % value, txt, count=1)
    register(lib_id, txt)
    return lib_id


def box_symbol(lib_id, ref, value, left, right, w=15.24, footprint='', descr=''):
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
    for side, lst in ((0, left), (1, right)):
        for k, (num, nm, typ) in enumerate(lst):
            y = top - 2.54 - k * 2.54
            x, ang = (-w / 2 - 2.54, 0) if side == 0 else (w / 2 + 2.54, 180)
            o.append('    (pin %s line (at %s %s %d) (length 2.54) (name "%s" (effects (font (size 1.27 1.27)))) '
                     '(number "%s" (effects (font (size 1.27 1.27)))))' % (typ, f(x), f(y), ang, nm, num))
    o += ['  )', ')']
    register(lib_id, '\n'.join(o))
    return lib_id


# ----------------------------------------------------------------------------- placement on the sheet
body = []
pwr_n = [0]
refs_info = {}
ends = {}          # stub end position -> net, to catch two different nets meeting on the sheet
POWER = {'GND': ('power:GND', 'down'), '+5V': ('power:+5V', 'up'), '+3V3': ('power:+3V3', 'up')}


COLS = {1: 20, 2: 470}
col_y = {1: 45, 2: 45}
blocks = []


class Block:
    """A band of one sheet column that parts of one function are packed into, left to right, then down. Each new
    block in a column starts below the last one."""

    def __init__(self, name, col, title, w=420):
        if blocks:
            b = blocks[-1]
            col_y[b.col] = max(col_y[b.col], b.y + b.row_h + 25)
        self.name, self.col, self.x0, self.y0, self.w = name, col, COLS[col], col_y[col], w
        self.x, self.y, self.row_h = self.x0, self.y0 + 12, 0
        blocks.append(self)
        text(self.x0, self.y0, title, 2.5)

    def slot(self, lib_id, unit=1):
        pins = [v for v in lib_pins[lib_id].values() if v[5] in (0, unit)]
        xs = [p[0] for p in pins] or [0]
        ys = [-p[1] for p in pins] or [0]
        pad_x, pad_y = 22, 8
        w = max(xs) - min(xs) + 2 * pad_x
        h = max(ys) - min(ys) + 2 * pad_y
        if self.x + w > self.x0 + self.w and self.x > self.x0:
            self.x, self.y, self.row_h = self.x0, self.y + self.row_h, 0
        cx = self.x + pad_x - min(xs)
        cy = self.y + pad_y - min(ys)
        self.x += w
        self.row_h = max(self.row_h, h)
        return round(cx / 2.54) * 2.54, round(cy / 2.54) * 2.54


def stub(x, y, angle_toward_body, net):
    out = (angle_toward_body + 180) % 360
    dx, dy = {0: (2.54, 0), 180: (-2.54, 0), 90: (0, -2.54), 270: (0, 2.54)}[out]
    x2, y2 = x + dx, y + dy
    key = (round(x2, 2), round(y2, 2))
    assert ends.get(key, net) == net, ('stub ends meet', key, ends.get(key), net)
    ends[key] = net
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
    rot = 180 if (outward == 90 and nat == 'down') or (outward == 270 and nat == 'up') else 0
    pwr_n[0] += 1
    ref = '#PWR%03d' % pwr_n[0]
    body.append('''  (symbol (lib_id "%s") (at %s %s %d) (unit 1) (in_bom no) (on_board no) (dnp no) (uuid "%s")
    (property "Reference" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))
    (property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))
    (property "Datasheet" "" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))
    (pin "1" (uuid "%s"))
    (instances (project "%s" (path "/%s" (reference "%s") (unit 1))))
  )''' % (lib_id, f(x), f(y), rot, uid('p'), ref, f(x), f(y), net, f(x + 2.54), f(y), f(x), f(y), f(x), f(y),
          uid('pp'), PROJECT, SHEET, ref))


cur = [None]
cnt = {}


def nid(prefix):
    cnt[prefix] = cnt.get(prefix, 0) + 1
    return '%s%d' % (prefix, cnt[prefix])


used_refs = set()


def place(lib_id, ref, value, footprint, nets, descr=None, unit=1, first=True, at=None, dnp=False):
    """nets: {pin number: net | None (no-connect)}; every pin of the unit must be listed. dnp: not fitted."""
    blk = cur[0]
    if first:
        assert ref not in used_refs, ref
        used_refs.add(ref)
        refs_info[ref] = {'group': blk.name, 'fp': footprint, 'value': value, 'dnp': dnp}
    x, y = at if at else blk.slot(lib_id, unit)
    pins = {k: v for k, v in lib_pins[lib_id].items() if v[5] in (0, unit)}
    props = lib_props[lib_id]

    def pos(k, dflt):
        px, py = props.get(k, dflt)
        return f(x + px), f(y - py)

    rp, vp = pos('Reference', (0, 3)), pos('Value', (0, -3))
    lines = ['  (symbol (lib_id "%s") (at %s %s 0) (unit %d) (in_bom yes) (on_board yes) (dnp %s) (uuid "%s")' % (
        lib_id, f(x), f(y), unit, 'yes' if dnp else 'no', uid('s' + ref + str(unit))),
        '    (property "Reference" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))' % (ref, rp[0], rp[1]),
        '    (property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))' % (value, vp[0], vp[1]),
        '    (property "Footprint" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (footprint, f(x), f(y)),
        '    (property "Datasheet" "~" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (f(x), f(y))]
    if descr:
        lines.append('    (property "Description" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (
            descr.replace('"', "'"), f(x), f(y)))
    for num in pins:
        lines.append('    (pin "%s" (uuid "%s"))' % (num, uid('pin' + ref + str(unit) + num)))
    lines.append('    (instances (project "%s" (path "/%s" (reference "%s") (unit %d))))' % (PROJECT, SHEET, ref, unit))
    lines.append('  )')
    body.append('\n'.join(lines))
    seen = set()
    for num, net in nets.items():
        px, py, ang, _, _, _ = pins[num]
        sx, sy = x + px, y - py
        k = (round(sx, 2), round(sy, 2))
        assert ends.get(k, net) == net, ('pin lands on another net', ref, num, net, ends.get(k))
        ends[k] = net
        if k in seen:           # stacked pins (same position): one stub
            continue
        seen.add(k)
        if net is None:
            body.append('  (no_connect (at %s %s) (uuid "%s"))' % (f(sx), f(sy), uid('nc')))
        else:
            stub(sx, sy, ang, net)
    missing = set(pins) - set(nets)
    assert not missing, (ref, unit, sorted(missing))
    return ref


def text(x, y, s, size=1.6):
    s = s.replace('"', "'").replace('\n', '\\n')
    body.append('  (text "%s" (at %s %s 0) (effects (font (size %s %s)) (justify left top)) (uuid "%s"))' % (
        s, f(x), f(y), f(size), f(size), uid('t')))


# ----------------------------------------------------------------------------- libraries and footprints
R = stock('Device', 'R')
C = stock('Device', 'C')
PTC = stock('Device', 'Polyfuse')
LED = stock('Device', 'LED')
DS = stock('Device', 'D_Schottky')
DD = stock('Device', 'D')
XTAL = stock('Device', 'Crystal_GND24')
FB = stock('Device', 'FerriteBead_Small')
BAT54S = stock('Diode', 'BAT54S')
OPTO = stock('Isolator', 'PC817')
MCU = stock('MCU_Microchip_ATmega', 'ATmega640V-8A', 'ATmega2560-16A', 'ATmega2560-16AU')
CANC = stock('Interface_CAN_LIN', 'MCP2515-xSO')
XCVR = stock('Interface_CAN_LIN', 'MCP2562-E-SN')
FRAM = stock('Memory_EEPROM', '24LC16', 'MB85RC256V', 'MB85RC256V')
SJ3 = stock('Jumper', 'SolderJumper_3_Bridged12', 'SolderJumper_3_Bridged12', '5V / 12V')
SJ2B = stock('Jumper', 'SolderJumper_2_Bridged')
SJ2O = stock('Jumper', 'SolderJumper_2_Open')
HDR = {n: stock('Connector_Generic', 'Conn_01x%02d' % n) for n in (2, 3, 4, 6, 12)}
HDR2 = {n: stock('Connector_Generic', 'Conn_02x%02d_Odd_Even' % n) for n in (2, 3, 5, 15)}
for n in ('GND', '+5V'):
    stock('power', n)
stock('power', 'PWR_FLAG')

FP_R, FP_C, FP_C10 = 'Resistor_SMD:R_0603_1608Metric', 'Capacitor_SMD:C_0603_1608Metric', 'Capacitor_SMD:C_0805_2012Metric'
FP_R0805 = 'Resistor_SMD:R_0805_2012Metric'
FP_R1206 = 'Resistor_SMD:R_1206_3216Metric'
FP_R2010 = 'Resistor_SMD:R_2010_5025Metric'
FP_SJ2B = 'Jumper:SolderJumper-2_P1.3mm_Bridged_RoundedPad1.0x1.5mm'
FP_SJ2O = 'Jumper:SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm'
FP_SJ3 = 'Jumper:SolderJumper-3_P1.3mm_Bridged12_RoundedPad1.0x1.5mm'
FP_PTC_S = 'Fuse:Fuse_1206_3216Metric'
FP_SOT23 = 'Package_TO_SOT_SMD:SOT-23'
FP_SOD123 = 'Diode_SMD:D_SOD-123'
FP_SOIC8 = 'Package_SO:SOIC-8_3.9x4.9mm_P1.27mm'
FP_XTAL = 'Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm'
FP_FB = 'Inductor_SMD:L_0603_1608Metric'
FP_RA = 'Connector_PinHeader_2.54mm:PinHeader_%s_P2.54mm_Horizontal'     # right angle, on the left edge
FP_V = 'Connector_PinHeader_2.54mm:PinHeader_%s_P2.54mm_Vertical'


def r_(v, a, b, d=None, fp=FP_R, dnp=False):
    return place(R, nid('R'), v, fp, {'1': a, '2': b}, descr=d, dnp=dnp)


def c_(v, a, b, d=None, fp=None):
    if fp is None:
        fp = FP_C10 if v in ('10u', '4.7u') else FP_C
    return place(C, nid('C'), v, fp, {'1': a, '2': b}, descr=d)


def ptc(v, a, b, d=None, fp=FP_PTC_S):
    return place(PTC, nid('F'), v, fp, {'1': a, '2': b}, descr=d)


def clamp(sig, d):
    """BAT54S across an input: pin 1 (anode, lower diode) GND, pin 2 (cathode, upper diode) +5 V, pin 3 the signal."""
    return place(BAT54S, nid('D'), 'BAT54S', FP_SOT23, {'1': 'GND', '2': '+5V', '3': sig}, descr=d)


# fixed reference numbers first, so the auto-numbered parts do not take them
for p, n in (('J', 10), ('U', 5), ('Y', 2), ('JP', 16), ('D', 0), ('F', 0), ('R', 0), ('C', 0)):
    cnt[p] = n

# ============================================================================= sheet
text(20, 15, 'SENSOR CARD  -  the DigiDash sensor pack on a slot card: ATmega2560 + MCP2515 / MCP2562 (CAN node 0x04, fixed) + FRAM odometer', 5)
text(20, 26, 'Card for a DigiDash rev 2 slot, 80 x 60 mm, standing on edge (docs/hardware/accessory_ports.md). Any slot but slot 1 '
     '(kept for tall parts). Sender, speed and tach connectors on the top edge; ISP, FTDI and the sensor serial out on the left edge. '
     'Firmware: arduino/sketches/sensor_pack (port to this pin map, SensorCard/README.md).', 2)

# ----------------------------------------------------------------------------- slot
cur[0] = Block('port', 1, 'SLOT (J1, right-angle 2x15 on the bottom edge): 5 V, GND, CAN, switched 12 V (ignition sense, speed sensor feed), '
               'constant 12 V (battery sense), WAKE (hold the 5 V rail after key-off)')
place(HDR2[15], 'J1', 'DigiDash slot', FP_RA % '2x15',
      {'1': '+5V', '2': '+5V', '3': 'GND', '4': 'GND', '5': 'CANH', '6': 'CANL', '7': 'GND', '8': 'SLOT_12V',
       '9': None, '10': 'SLOT_BATT', '11': None, '12': 'WAKE', '13': None, '14': None,
       '15': None, '16': None, '17': None, '18': None, '19': 'GND', '20': 'GND',
       '21': None, '22': None, '23': None, '24': None,
       '25': None, '26': None, '27': None, '28': None, '29': None, '30': None},
      descr='Right-angle 2x15 header on the bottom edge (the slot pinout, docs/hardware/accessory_ports.md). Used: 5 V, GND, CAN, '
            'switched 12 V (8), constant 12 V (10), WAKE (12). Not used: 3.3 V, slot ID (the node is fixed), LAMP, REV, I2S, audio, AUX 12 V')
c_('10u', '+5V', 'GND', 'Slot 5 V bulk')
c_('100n', '+5V', 'GND', 'Slot 5 V')
text(cur[0].x0 + 200, cur[0].y0 + 6, 'IGN: slot switched 12 V (on with the key) -> 15k / 10k (HIGH from about 7.5 V, so cranking sag does not drop it), clamped.\\n'
     'BATT: slot constant 12 V -> 100k / 22k (16 V -> 2.9 V) on ADC8 (no JTAG pin, no fuse to set).\\n'
     'WAKE: PWR_HOLD (5 V) -> 4.7k / 10k (3.4 V) -> Schottky -> slot WAKE: keeps the buck on after key-off. WAKE -> 47k -> ADC9 (another card woke the rail).', 1.6)
r_('15k', 'SLOT_12V', 'IGN_SENSE', 'Ignition sense divider top (from the slot switched 12 V)')
r_('10k', 'IGN_SENSE', 'GND', 'Ignition sense divider bottom')
c_('100n', 'IGN_SENSE', 'GND', 'Ignition sense filter')
clamp('IGN_SENSE', 'Ignition sense clamp')
r_('100k', 'SLOT_BATT', 'BATT_SENSE', 'Battery sense top (16 V -> 2.9 V)')
r_('22k', 'BATT_SENSE', 'GND', 'Battery sense bottom')
c_('100n', 'BATT_SENSE', 'GND', 'Battery sense filter')
r_('4.7k', 'PWR_HOLD', 'HOLD_DIV', 'Hold divider top: 5 V -> 3.4 V (WAKE is a 3.3 V line)')
r_('10k', 'HOLD_DIV', 'GND', 'Hold divider bottom; also holds WAKE off while the 2560 is in reset')
place(DS, nid('D'), 'BAT42W', FP_SOD123, {'1': 'WAKE', '2': 'HOLD_DIV'},
      descr='Hold diode: this card can only drive WAKE high, never load it (other cards drive it too)')
r_('47k', 'WAKE', 'WAKE_SENSE', 'WAKE into the 2560 (series: the line may be up while the 2560 is unpowered)')
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', '', {'1': '+5V'})
place('power:PWR_FLAG', '#FLG02', 'PWR_FLAG', '', {'1': 'GND'})
place('power:PWR_FLAG', '#FLG03', 'PWR_FLAG', '', {'1': 'SLOT_12V'})
place('power:PWR_FLAG', '#FLG04', 'PWR_FLAG', '', {'1': 'SLOT_BATT'})
place('power:PWR_FLAG', '#FLG05', 'PWR_FLAG', '', {'1': 'AVCC'})

# ----------------------------------------------------------------------------- MCU
cur[0] = Block('mcu', 2, 'SENSOR PACK: ATmega2560, 16 MHz, 5 V (ADC ratiometric to the senders\' 5 V). Pin map: SensorCard/README.md')
m = {str(n): None for n in range(1, 101)}
m.update({'10': '+5V', '31': '+5V', '61': '+5V', '80': '+5V', '100': 'AVCC', '11': 'GND', '32': 'GND', '62': 'GND',
          '81': 'GND', '99': 'GND', '98': 'AREF', '30': 'RESET', '33': 'XTAL2', '34': 'XTAL1',
          '2': 'FTDI_RX', '3': 'FTDI_TX',                       # USART0: FTDI header (log, bootloader upload)
          '45': 'SENS_RX1', '46': 'SENS_TX1',                  # USART1: sensor frames out (J8, optional wire to the main board)
          '19': 'CAN_CS', '20': 'SCK', '21': 'MOSI', '22': 'MISO', '6': 'CAN_INT',
          '43': 'I2C_SCL', '44': 'I2C_SDA',                    # TWI: FRAM
          '35': 'TACH_SIG', '36': 'SPEED_SIG',                 # ICP4 / ICP5
          '97': 'FUEL_ADC', '96': 'ENGINE_TEMP_ADC', '95': 'OIL_PRESSURE_ADC', '94': 'AMB_TEMP_ADC',   # ADC0-3
          '89': 'BATT_SENSE', '88': 'WAKE_SENSE',              # ADC8, ADC9 (port K: no JTAG, pin-change capable)
          '87': 'SPARE_A10', '86': 'SPARE_A11', '85': 'SPARE_A12', '84': 'SPARE_A13',
          '78': 'ALT_L', '77': 'IGN_SENSE', '75': 'TEMP_MON', '74': 'PRESS_MON',
          '72': 'RPM_SEL_A', '71': 'RPM_SEL_B',
          '53': 'DEMO', '54': 'PWR_HOLD', '26': 'STATUS_LED',
          '37': 'SPARE_D47', '38': 'SPARE_D46', '39': 'SPARE_D45', '40': 'SPARE_D44'})
place(MCU, 'U1', 'ATmega2560-16AU', 'Package_QFP:TQFP-100_14x14mm_P0.5mm', m,
      descr='Sensor pack MCU. USART0 = FTDI header, USART1 = sensor frames (J8), SPI = MCP2515 + ISP, ICP4 / ICP5 = tach / speed')
for _ in range(4):
    c_('100n', '+5V', 'GND', 'U1 VCC pins')
c_('10u', '+5V', 'GND', 'U1 bulk')
place(FB, nid('FB'), '600R', FP_FB, {'1': '+5V', '2': 'AVCC'}, descr='AVCC filter')
c_('100n', 'AVCC', 'GND', 'AVCC')
c_('100n', 'AREF', 'GND', 'AREF (AVCC reference selected in firmware)')
place(XTAL, 'Y1', '16MHz', FP_XTAL, {'1': 'XTAL1', '2': 'GND', '3': 'XTAL2', '4': 'GND'}, descr='16 MHz, 3225, CL about 10 pF')
c_('18p', 'XTAL1', 'GND', 'Y1 load')
c_('18p', 'XTAL2', 'GND', 'Y1 load')
r_('10k', '+5V', 'RESET', 'Reset pull-up')
c_('100n', 'FTDI_DTR', 'RESET', 'Auto-reset from the FTDI header DTR')
place(LED, nid('D'), 'green', 'LED_SMD:LED_0603_1608Metric', {'1': 'LED_K', '2': 'STATUS_LED'}, descr='Status (PB7, Arduino D13)')
r_('1k', 'LED_K', 'GND', 'Status LED')
r_('10k', 'DEMO', 'GND', 'Demo mode pulled low (sensor frames are real data)')
place(HDR[2], 'J9', 'Demo', FP_V % '1x02', {'1': 'DEMO', '2': '+5V'},
      descr='Demo jumper: fitted = the sensor pack sends a fake sweep (bench only). 1 DEMO, 2 +5V')
place(HDR2[2], 'J5', 'RPM select', FP_V % '2x02', {'1': 'GND', '2': 'RPM_SEL_A', '3': 'GND', '4': 'RPM_SEL_B'},
      descr='RPM select jumpers (pulses per revolution; internal pull-ups): 1-2 = A, 3-4 = B')
place(HDR2[5], 'J10', 'Spare I/O', FP_V % '2x05',
      {'1': '+5V', '2': 'GND', '3': 'SPARE_A10', '4': 'SPARE_A11', '5': 'SPARE_A12', '6': 'SPARE_A13', '7': 'SPARE_D44',
       '8': 'SPARE_D45', '9': 'SPARE_D46', '10': 'SPARE_D47'},
      descr='Spare 2560 pins: ADC10-13 (port K: analog or digital, pin-change interrupts), D44-D47 (PWM). 5 V logic, no protection')
# CAN
place(CANC, 'U2', 'MCP2515-I/SO', 'Package_SO:SOIC-18W_7.5x11.6mm_P1.27mm',
      {'1': 'SNS_TXCAN', '2': 'SNS_RXCAN', '3': None, '4': None, '5': None, '6': None, '7': 'CAN_OSC2', '8': 'CAN_OSC1',
       '9': 'GND', '10': None, '11': None, '12': 'CAN_INT', '13': 'SCK', '14': 'MOSI', '15': 'MISO', '16': 'CAN_CS',
       '17': 'RESET', '18': '+5V'}, descr='CAN controller (node 0x04, fixed)')
c_('100n', '+5V', 'GND', 'U2')
r_('10k', '+5V', 'CAN_CS', 'Keeps U2 off the SPI bus while the 2560 is being programmed')
r_('10k', '+5V', 'CAN_INT', 'CAN_INT pull-up')
place(XTAL, 'Y2', '16MHz', FP_XTAL, {'1': 'CAN_OSC1', '2': 'GND', '3': 'CAN_OSC2', '4': 'GND'}, descr='MCP2515 clock, own crystal')
c_('18p', 'CAN_OSC1', 'GND', 'Y2 load')
c_('18p', 'CAN_OSC2', 'GND', 'Y2 load')
place(XCVR, 'U3', 'MCP2562-E/SN', FP_SOIC8,
      {'1': 'SNS_TXCAN', '2': 'GND', '3': '+5V', '4': 'SNS_RXCAN', '5': '+5V', '6': 'CANL', '7': 'CANH', '8': 'GND'},
      descr='CAN transceiver, VIO 5 V, STBY low. No termination: a card never terminates the bus')
c_('100n', '+5V', 'GND', 'U3 VDD / VIO')
# FRAM
place(FRAM, 'U4', 'MB85RC256V', FP_SOIC8,
      {'1': 'GND', '2': 'GND', '3': 'GND', '4': 'GND', '5': 'I2C_SDA', '6': 'I2C_SCL', '7': 'GND', '8': '+5V'},
      descr='32 KB I2C FRAM, address 0x50: odometer (A / B slots with CRC-8). Lives on the card: set a new card\'s mileage in firmware')
c_('100n', '+5V', 'GND', 'U4')
r_('4.7k', '+5V', 'I2C_SDA', 'TWI pull-up')
r_('4.7k', '+5V', 'I2C_SCL', 'TWI pull-up')

# ----------------------------------------------------------------------------- programming and serial
cur[0] = Block('prog', 1, 'PROGRAMMING AND SERIAL (right angle, left edge): J6 ISP, J7 FTDI (USART0), J8 sensor serial out (USART1: '
               'optional jumper wire to the main board J27, which fans it out to the three screens)')
place(HDR2[3], 'J6', 'AVR-ISP-6', FP_RA % '2x03',
      {'1': 'MISO', '2': '+5V', '3': 'SCK', '4': 'MOSI', '5': 'RESET', '6': 'GND'},
      descr='ISP (USBasp or AVRISP mkII; USBtiny is unreliable above 128 KB). Pin 2 is the card 5 V: power the card from its slot')
place(HDR[6], 'J7', 'FTDI', FP_RA % '1x06',
      {'1': 'GND', '2': 'GND', '3': None, '4': 'FTDI_RX', '5': 'FTDI_TX', '6': 'FTDI_DTR'},
      descr='FTDI 6-pin (5 V logic): 1 GND, 2 CTS (grounded), 3 VCC (not used: power from the slot), 4 adapter TX, 5 adapter RX, 6 DTR')
place(HDR[3], 'J8', 'Sensor serial out', FP_RA % '1x03',
      {'1': 'GND', '2': 'SER_OUT', '3': 'SER_IN'},
      descr='Sensor serial (USART1, 5 V, 115200 8N1): 1 GND, 2 TX (sensor frames out: jumper wire to the main board J27 pin 1), 3 RX (in; unused in the car)')
r_('100', 'SENS_TX1', 'SER_OUT', 'Series R on the serial out (a wire leaves the card)')
r_('1k', 'SER_IN', 'SENS_RX1', 'Series R on the serial in')
r_('10k', '+5V', 'SENS_RX1', 'USART1 RX idle high (nothing wired in the car)')

# ----------------------------------------------------------------------------- senders
cur[0] = Block('sensors', 1, 'SENDERS (J2, JST PH 12 pin, top edge). Each channel: a topology jumper (pull-up to +5 V or pull-down to GND) '
               'feeding a bank of resistors, each behind its own solder jumper. Bridge one value (two give them in parallel). 1k + 10 nF into the ADC.')
place(HDR[12], 'J2', 'Senders', 'Connector_JST:JST_PH_S12B-PH-K_1x12_P2.00mm_Horizontal',
      {'1': 'FUEL', '2': 'GND', '3': 'ENGINE_TEMP', '4': 'ENGINE_TEMP_VIN', '5': 'GND', '6': 'OIL_PRESSURE',
       '7': 'OIL_PRESSURE_VIN', '8': 'GND', '9': 'AMB_TEMP', '10': 'GND', '11': 'ALT_L_IN', '12': 'GND'},
      descr='Senders: 1 FUEL, 2 FUEL return, 3 ETEMP, 4 ETEMP V (5 V, 0.1 A PTC), 5 ETEMP return, 6 OILP, 7 OILP V (5 V, 0.1 A PTC), '
            '8 OILP return, 9 AMB, 10 AMB return, 11 ALT L (alternator lamp terminal, 12 V level), 12 GND')
text(cur[0].x0 + 200, cur[0].y0 + 6, 'As delivered: every channel pull-up (topology jumper 1-2); FUEL 100R (the factory 0-30R sender), ETEMP 1k, OILP 220R, AMB 100k bridged.\\n'
     'Pull-up mode: any resistive sender whose other end is ground (the block, the tank, or its return pin). Pull-down mode: a sender fed '
     'from the channel\'s V pin, or a 3-wire 0.5-4.5 V transducer (OILP 100k as a light load; or no value bridged).\\n'
     'The pull-ups come straight from +5 V, not through the PTCs (their drop would read as level error). Sender selection: SensorCard/DATASHEET.md.', 1.6)
# engine temperature and oil pressure: their sender feeds (2-wire / 3-wire senders) and fuse monitors
ptc('PTC 0.1A', '+5V', 'ENGINE_TEMP_VIN', 'Temperature sender feed (2-wire / 3-wire senders)')
r_('1k', 'ENGINE_TEMP_VIN', 'TEMP_MON', 'Temperature feed monitor')
r_('10k', 'TEMP_MON', 'GND', 'Monitor pull-down')
ptc('PTC 0.1A', '+5V', 'OIL_PRESSURE_VIN', 'Oil pressure sender feed (2-wire / 3-wire senders)')
r_('1k', 'OIL_PRESSURE_VIN', 'PRESS_MON', 'Oil feed monitor')
r_('10k', 'PRESS_MON', 'GND', 'Monitor pull-down')
# the banks: (channel net, short name, topology jumper, [(value, footprint, jumper ref, bridged as delivered, note)])
BANKS = [
    ('FUEL', 'FUEL', 'JP2', [('100', FP_R2010, 'JP6', True, '0-30R senders (the factory 1964 GM sender); 2010 for 0.25 W into a shorted sender'),
                              ('220', FP_R1206, 'JP7', False, '0-90R senders (GM 1965 on), 10-180R'),
                              ('470', FP_R0805, 'JP8', False, '240-33R senders')]),
    ('ENGINE_TEMP', 'ETEMP', 'JP3', [('220', FP_R1206, 'JP9', False, 'gauge senders that read tens of ohms hot'),
                                    ('1k', FP_R, 'JP10', True, 'senders of a few hundred ohms near 100 C (2-wire EFI CTS)'),
                                    ('10k', FP_R, 'JP11', False, '10k-class NTC thermistors')]),
    ('OIL_PRESSURE', 'OILP', 'JP4', [('220', FP_R1206, 'JP12', True, '10-180R and 240-33R senders'),
                                     ('1k', FP_R, 'JP13', False, 'higher-resistance senders'),
                                     ('100k', FP_R, 'JP14', False, 'pull-down mode: light load / open-wire detect for a 0.5-4.5 V transducer')]),
    ('AMB_TEMP', 'AMB', 'JP5', [('10k', FP_R, 'JP15', False, '10k NTC'),
                                ('100k', FP_R, 'JP16', True, '100k NTC')]),
]
for net, name, topo, bank in BANKS:
    place(SJ3, topo, '%s PU / PD' % name, FP_SJ3, {'1': '+5V', '2': net + '_BANK', '3': 'GND'},
          descr='%s topology: 1-2 (bridged as delivered) = pull-up to +5 V; cut 1-2 and bridge 2-3 = pull-down to GND' % name)
    for v, fp, jp, bridged, note in bank:
        place(SJ2B if bridged else SJ2O, jp, '%s %s' % (name, v), FP_SJ2B if bridged else FP_SJ2O,
              {'1': net + '_BANK', '2': '%s_%s' % (net, v.upper())},
              descr='%s %s select: %s as delivered' % (name, v, 'bridged' if bridged else 'open'))
        r_(v, '%s_%s' % (net, v.upper()), net, '%s bank, %s: %s' % (name, v, note), fp)
for net in ('FUEL', 'ENGINE_TEMP', 'OIL_PRESSURE', 'AMB_TEMP'):
    r_('1k', net, net + '_ADC', 'ADC series (protection)')
    c_('10n', net + '_ADC', 'GND', 'ADC filter')
# alternator L
r_('22k', 'ALT_L_IN', 'ALT_L', 'Alternator L divider top')
r_('10k', 'ALT_L', 'GND', 'Alternator L divider bottom')
c_('100n', 'ALT_L', 'GND', 'Alternator L filter')
clamp('ALT_L', 'Alternator L clamp')

# ----------------------------------------------------------------------------- speed
cur[0] = Block('speed', 2, 'SPEED (J3, JST PH 4, top edge): a hall / drop-in cable-replacement sensor. JP1 picks its supply: 5 V '
               '(1-2, as delivered) or the slot switched 12 V (2-3). Open-collector or push-pull output up to 12 V.')
place(HDR[4], 'J3', 'Speed', 'Connector_JST:JST_PH_S4B-PH-K_1x04_P2.00mm_Horizontal',
      {'1': 'SPEED_VIN', '2': 'SPEED', '3': 'GND', '4': 'GND'},
      descr='Speed sensor: 1 sensor power (5 V or 12 V by JP1, 0.1 A PTC), 2 signal, 3 GND, 4 GND / shield')
place(SJ3, 'JP1', '5V / 12V', 'Jumper:SolderJumper-3_P1.3mm_Bridged12_RoundedPad1.0x1.5mm',
      {'1': '+5V', '2': 'SPEED_VSEL', '3': 'SLOT_12V'},
      descr='Speed sensor supply: 1-2 (bridged as delivered) = 5 V; cut 1-2 and bridge 2-3 = slot switched 12 V')
ptc('PTC 0.1A', 'SPEED_VSEL', 'SPEED_VIN', 'Speed sensor feed')
c_('100n', 'SPEED_VIN', 'GND', 'Speed sensor feed, at J3', fp=FP_C10)
r_('10k', '+5V', 'SPEED', 'Speed pull-up (open-collector sensors)')
r_('10k', 'SPEED', 'SPEED_SIG', 'Speed input series: a 12 V output puts under 1 mA into the clamp')
clamp('SPEED_SIG', 'Speed input clamp')
c_('1n', 'SPEED_SIG', 'GND', 'Speed input filter (10 us with the 10k)')

# ----------------------------------------------------------------------------- tach
cur[0] = Block('tach', 2, 'TACH (J4, Micro-Fit 3.0, 2 pin, top edge, its own connector: coil-negative kick is hundreds of volts). '
               'PC817 isolation; keep the TACH net clear of everything else up to the two 1206 resistors.')
place(HDR[2], 'J4', 'Tach', 'Connector_Molex:Molex_Micro-Fit_3.0_43650-0200_1x02_P3.00mm_Horizontal',
      {'1': 'TACH', '2': 'GND'}, descr='Tach: 1 coil negative / HEI tach terminal / ignition box tach output, 2 GND')
r_('4.7k', 'TACH', 'TACH_R', 'Coil input, 1206 (two in series for the voltage)', FP_R1206)
r_('4.7k', 'TACH_R', 'TACH_LED', 'Coil input, 1206', FP_R1206)
place(OPTO, 'U5', 'PC817', 'Package_DIP:SMDIP-4_W7.62mm', {'1': 'TACH_LED', '2': 'GND', '3': 'GND', '4': 'TACH_OC'},
      descr='Tach isolation')
place(DD, nid('D'), '1N4148W', FP_SOD123, {'1': 'TACH_LED', '2': 'GND'}, descr='Reverse clamp across the LED (coil kickback)')
r_('22k', '+5V', 'TACH_OC', 'Optocoupler pull-up (22k: switches fully even at a low-CTR PC817 and 1 mA of LED current)')
r_('1k', 'TACH_OC', 'TACH_SIG', 'Tach input series')
c_('1n', 'TACH_SIG', 'GND', 'Tach input filter')

# ----------------------------------------------------------------------------- write
for b in blocks:
    col_y[b.col] = max(col_y[b.col], b.y + b.row_h + 25)
out = ['(kicad_sch (version 20230121) (generator eeschema)',
       '  (uuid "%s")' % SHEET,
       '  (paper "User" 920 %d)' % (max(col_y.values()) + 60),
       '  (title_block (title "DigiDash sensor card") (date "2026-10-01") (rev "A")',
       '    (comment 1 "ATmega2560 + MCP2515 / MCP2562 (CAN node 0x04) + FRAM: the sensor pack on a slot card")',
       '    (comment 2 "Card for a DigiDash rev 2 slot (2x15, on edge), any slot but slot 1"))',
       '  (lib_symbols']
for t in lib_text.values():
    out.append('\n'.join('    ' + ln for ln in t.split('\n')))
out.append('  )')
out += body
out += ['  (sheet_instances (path "/" (page "1")))', ')']
open(OUT, 'w').write('\n'.join(out) + '\n')
json.dump(refs_info, open(os.path.join(HERE, 'refs.json'), 'w'), indent=1, sort_keys=True)
print('wrote', os.path.normpath(OUT), len(body), 'items', len(refs_info), 'parts')
