#!/usr/bin/env python3
"""Write SensorCard2_BOM.csv from the schematic's netlist (kicad-cli). Same columns as the GPS card's BOM.

    python3 generate/gen_bom.py

Identical resistors and capacitors are grouped; chips, connectors and jumpers get a row each. Not-fitted (DNP) parts,
if any, are listed last with a note. Manufacturer parts for the chips and connectors are in MPN below.
"""
import collections
import csv
import os
import re
import subprocess
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
KICAD = '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli'
CLI = os.environ.get('KICAD_CLI', KICAD if os.path.exists(KICAD) else 'kicad-cli')
TMP = os.environ.get('TMPDIR', '/tmp')

MPN = {     # maker, part number
    'U1': ('STMicroelectronics', 'STM32G0B1KBU6'), 'U3': ('NXP', 'TJA1051T/3/1J'), 'U4': ('Fujitsu', 'MB85RC256VPNF-G-JNERE1'),
    'U5': ('Everlight', 'EL817S1(C)(TU)-F'),          # PC817 in the 4-SMD gull wing (LCSC C106900)
    'U6': ('Diodes Inc', 'AP2112K-3.3TRG1'), 'U7': ('Texas Instruments', 'SN74LVC1T45DCKR'),
    'Y1': ('YXC', 'OT32258MJBA4SL'),                   # 8 MHz CMOS oscillator, 3225 (LCSC C717687): HSE bypass on PC14
    'J1': ('Sullins', 'PRPC015DBAN-M71RC'), 'J2': ('JST', 'S12B-PH-K-S'), 'J3': ('JST', 'S4B-PH-K-S'),
    'J4': ('Molex', '43650-0200'), 'J5': ('XFCN', 'PZ254V-12-4P'), 'J6': ('Sullins', 'PRPC005SBAN-M71RC'),
    'J7': ('Sullins', 'PRPC006SBAN-M71RC'), 'J8': ('Sullins', 'PREC003SBAN-M71RC'),
    'J9': ('Sullins', 'PRPC002SAAN-RC'), 'J10': ('Sullins', 'PRPC004SAAN-RC'),
}
BY_VALUE = {'BAT54S': ('Nexperia', 'BAT54S,215'), 'BAT42W': ('Vishay', 'BAT42W-E3-08'),
            '1N4148W': ('Diodes Inc', '1N4148W-7-F'), 'green': ('Lite-On', 'LTST-C191KGKT'),
            'PTC 0.1A': ('Littelfuse', '1206L010/60WR')}
CAPS = {('10u', '0805'): 'CL21B106KOQNNNE', ('4.7u', '0805'): 'CL21A475KOFNNNE', ('1u', '0603'): 'CL10B105KO8NNNC',
        ('100n', '0603'): 'CL10B104KB8NNNC', ('100n', '0805'): 'CL21B104KBCNNNC',
        ('10n', '0603'): 'CL10B103KB8NNNC', ('15p', '0603'): 'CL10C150JB8NNNC', ('1n', '0603'): 'CL10C102JB8NNNC'}
NOTES = {'Y1': 'LCSC C717687. An oscillator, not a crystal: the 32-pin STM32G0B1 has no OSC_OUT (HSE bypass on PC14 only)',
         'U1': 'Arrives blank: flash over J6 (SWD, ST-Link)', 'U5': 'LCSC C106900. Any PC817 in the 4-SMD gull wing',
         'U7': 'Level shifter: the sensor serial out at 5 V for the main board\'s dividers',
         'J9': 'Plus one 2.54 mm jumper shunt, not fitted: bench only',
         'J1': 'Same part as the audio and GPS cards; on the bottom edge, pins pointing off the board'}
GROUP_DESCR = {'BAT54S': 'Input clamps to GND / 3.3 V (ignition sense, alternator L, speed, the senders)',
               'PTC 0.1A': '0.1 A, 60 V feed fuses, 1206: ETEMP V, OILP V, speed sensor power'}


def resistor(val, pkg):
    """Yageo RC series, 1 %: '4.7k' -> 4K7, '22k' -> 22K, '100' -> 100R."""
    if val[-1] in 'kK':
        code = val[:-1].replace('.', 'K') if '.' in val else val[:-1] + 'K'
    else:
        code = val + 'R'
    return ('Yageo', 'RC%s%s-07%sL' % (pkg, 'FK' if pkg == '2010' else 'FR', code))


ORDER = {'U': 0, 'Y': 1, 'J': 2, 'JP': 3, 'D': 4, 'FB': 5, 'F': 6, 'R': 7, 'C': 8}


def split(ref):
    m = re.match(r'([A-Z]+)(\d+)', ref)
    return m.group(1), int(m.group(2))


def package(fp):
    n = fp.split(':')[1]
    m = re.match(r'(?:R|C|L|LED|Fuse)_(\d{4})_', n)
    return m.group(1) if m else n


def main():
    xml = os.path.join(TMP, 'sensorcard2_bom_netlist.xml')
    subprocess.check_call([CLI, 'sch', 'export', 'netlist', '--format', 'kicadxml', '-o', xml,
                           os.path.join(ROOT, 'SensorCard2.kicad_sch')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    parts = []
    for c in ET.parse(xml).getroot().find('components'):
        ref = c.get('ref')
        if ref.startswith('#'):
            continue
        props = {p.get('name'): p.get('value') for p in c.findall('property')}
        parts.append((ref, c.findtext('value'), c.findtext('footprint'), c.findtext('description') or '', 'dnp' in props))
    groups = collections.OrderedDict()
    for ref, val, fp, descr, dnp in sorted(parts, key=lambda t: (t[4], split(t[0]))):
        pre = split(ref)[0]
        key = (ref,) if (ref in MPN or pre == 'JP' or dnp) else (pre, val, fp)
        groups.setdefault(key, []).append((ref, val, fp, descr, dnp))
    rows = []
    for items in groups.values():
        ref, val, fp, descr, dnp = items[0]
        refs = [i[0] for i in items]
        pre, pkg = split(ref)[0], package(fp)
        if ref in MPN:
            mpn = MPN[ref]
        elif pre == 'JP':
            mpn = ('', '(PCB solder jumper, no part)')
        elif val in BY_VALUE:
            mpn = BY_VALUE[val]
        elif pre == 'R':
            mpn = resistor(val, pkg)
        elif pre == 'C':
            mpn = ('Samsung', CAPS[(val, pkg)])
        else:
            raise SystemExit('no part number for %s (%s, %s)' % (ref, val, fp))
        if len(refs) > 1:
            descr = GROUP_DESCR.get(val, ('%s %s' % (val, {'R': 'resistor', 'C': 'capacitor'}.get(pre, ''))).strip())
        note = ''
        if dnp:
            note = 'NOT FITTED as delivered (DNP)'
        elif ref == 'U1':
            note = NOTES['U1']
        elif pre == 'JP':
            note = 'Bridged as delivered' if ('bridged as delivered' in descr.lower() or 'Bridged' in fp) else 'Open as delivered'
        elif pre == 'R' and 'bank' in descr:
            note = 'Sender bias bank: selected by a solder jumper'
        elif ref in NOTES:
            note = NOTES[ref]
        rows.append((dnp, ORDER.get(pre, 9), split(ref),
                     [len(refs), ' '.join(refs), mpn, descr, pkg, 'THT' if re.search(r'PinHeader|JST|Molex', fp) else 'SMD',
                      'Top', note]))
    rows.sort(key=lambda t: (t[0], t[1], t[2]))
    out = os.path.join(ROOT, 'SensorCard2_BOM.csv')
    with open(out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Qty', 'Designators', 'Manufacturer part', 'Description', 'Package', 'Type', 'Side', 'Notes'])
        for r in rows:
            q, refs, (maker, part) = r[3][:3]
            w.writerow([q, refs, ('%s %s' % (maker, part)).strip()] + r[3][3:])
    print('wrote %s: %d rows, %d parts' % (out, len(rows), sum(r[3][0] for r in rows)))
    out = os.path.join(ROOT, 'fab', 'SensorCard2_BOM_PCBWay.csv')
    # the assembler's BOM: SMD only (the through-hole headers and connectors are hand-soldered, like the console's)
    placed = [r for r in rows if not r[0] and not r[3][1].startswith('JP') and r[3][5] != 'THT']
    with open(out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Item #', 'Designator', 'Qty', 'Manufacturer', 'Mfg Part #', 'Description / Value', 'Package/Footprint',
                    'Type', 'Side', 'Your Instructions / Notes'])
        for i, r in enumerate(placed, 1):
            q, refs, (maker, part), descr, pkg, typ, side, note = r[3]
            w.writerow([i, refs, q, maker, part, descr, pkg, typ, side, note])
    print('wrote %s: %d rows, %d parts' % (out, len(placed), sum(r[3][0] for r in placed)))


if __name__ == '__main__':
    main()
