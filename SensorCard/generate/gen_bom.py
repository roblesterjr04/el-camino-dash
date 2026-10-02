#!/usr/bin/env python3
"""Write SensorCard_BOM.csv from the schematic's netlist (kicad-cli). Same columns as the GPS card's BOM.

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

MPN = {
    'U1': 'Microchip ATmega2560-16AU', 'U2': 'Microchip MCP2515-I/SO', 'U3': 'Microchip MCP2562-E/SN',
    'U4': 'Fujitsu MB85RC256VPNF-G-JNERE1', 'U5': 'Sharp PC817 (SMD gull-wing, e.g. PC817X2NSZ9F) or Everlight EL817S',
    'Y1': '16 MHz, 3225, CL 10 pF (e.g. Abracon ABM8-16.000MHZ-10-1-U-T)',
    'Y2': '16 MHz, 3225, CL 10 pF (e.g. Abracon ABM8-16.000MHZ-10-1-U-T)',
    'J1': '2x15 right-angle pin header, 2.54 mm, 6 mm mating pins', 'J2': 'JST S12B-PH-K-S', 'J3': 'JST S4B-PH-K-S',
    'J4': 'Molex 43650-0200', 'J5': '2x2 pin header, 2.54 mm, vertical', 'J6': '2x3 right-angle pin header, 2.54 mm',
    'J7': '1x6 right-angle pin header, 2.54 mm', 'J8': '1x3 right-angle pin header, 2.54 mm',
    'J9': '1x2 pin header, 2.54 mm, vertical (plus a jumper)', 'J10': '2x5 pin header, 2.54 mm, vertical',
}
BY_VALUE = {'BAT54S': 'Nexperia BAT54S', 'BAT42W': 'Vishay BAT42W', '1N4148W': '1N4148W', 'green': '0603 green LED',
            '600R': 'Murata BLM18KG601SN1D (600 R at 100 MHz)', 'PTC 0.1A': 'Bourns MF-NSMF010-2 (0.1 A hold)'}
GROUP_DESCR = {'BAT54S': 'Input clamps to GND / 5 V (ignition sense, alternator L, speed)',
               'PTC 0.1A': '0.1 A feed fuses: ETEMP V, OILP V, speed sensor power'}
ORDER = {'U': 0, 'Y': 1, 'J': 2, 'JP': 3, 'D': 4, 'FB': 5, 'F': 6, 'R': 7, 'C': 8}
WATTS = {'0603': '0.1 W', '0805': '0.125 W', '1206': '0.25 W', '2010': '0.5 W'}


def split(ref):
    m = re.match(r'([A-Z]+)(\d+)', ref)
    return m.group(1), int(m.group(2))


def package(fp):
    n = fp.split(':')[1]
    m = re.match(r'(?:R|C|L|LED|Fuse)_(\d{4})_', n)
    return m.group(1) if m else n


def main():
    xml = os.path.join(TMP, 'sensorcard_bom_netlist.xml')
    subprocess.check_call([CLI, 'sch', 'export', 'netlist', '--format', 'kicadxml', '-o', xml,
                           os.path.join(ROOT, 'SensorCard.kicad_sch')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
            mpn = '(PCB solder jumper, no part)'
        elif val in BY_VALUE:
            mpn = BY_VALUE[val]
        elif pre == 'R':
            mpn = '%s resistor, %s, 1%%, %s' % (val, pkg, WATTS.get(pkg, ''))
        elif pre == 'C':
            diel = 'C0G' if val.endswith('p') or val == '1n' else 'X7R'
            mpn = '%s capacitor, %s, %s%s' % (val, pkg, diel, ', 50 V' if val == '100n' else ', 10 V' if val == '10u' else '')
        else:
            mpn = val
        if len(refs) > 1:
            descr = GROUP_DESCR.get(val, ('%s %s' % (val, {'R': 'resistor', 'C': 'capacitor'}.get(pre, ''))).strip())
        note = ''
        if dnp:
            note = 'NOT FITTED as delivered (DNP)'
        elif ref == 'U1':
            note = 'Arrives blank: flash over J6 (ISP); fuses in DATASHEET.md'
        elif pre == 'JP':
            note = 'Bridged as delivered' if ('bridged as delivered' in descr.lower() or 'Bridged' in fp) else 'Open as delivered'
        elif pre == 'R' and 'bank' in descr:
            note = 'Sender bias bank: selected by a solder jumper'
        rows.append((dnp, ORDER.get(pre, 9), split(ref),
                     [len(refs), ' '.join(refs), mpn, descr, pkg, 'THT' if re.search(r'PinHeader|JST|Molex', fp) else 'SMD',
                      'Top', note]))
    rows.sort(key=lambda t: (t[0], t[1], t[2]))
    out = os.path.join(ROOT, 'SensorCard_BOM.csv')
    with open(out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Qty', 'Designators', 'Manufacturer part', 'Description', 'Package', 'Type', 'Side', 'Notes'])
        for r in rows:
            w.writerow(r[3])
    print('wrote %s: %d rows, %d parts' % (out, len(rows), sum(r[3][0] for r in rows)))


if __name__ == '__main__':
    main()
