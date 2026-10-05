#!/usr/bin/env python3
"""Write BenchBoard_BOM.csv and fab/BenchBoard_BOM_PCBWay.csv from the schematic's netlist (kicad-cli).

    python3 generate/gen_bom.py

Identical resistors and capacitors are grouped; everything else gets a row of its own. Every placed part has a maker
and a part number (a part without one stops the script). Mounting holes and net ties are not parts: they are left
out. The jumper shunts are loose parts (not placed): they are listed in the notes.
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

MPN = {     # ref: (maker, part number)
    'U1': ('Texas Instruments', 'TPS563201DDCR'), 'U2': ('Diodes Inc', 'AP2112K-3.3TRG1'),
    'U3': ('Microchip', 'MCP2562-E/SN'), 'L1': ('Bourns', 'SRN6045TA-3R3Y'), 'D1': ('Vishay', 'SS34-E3/57T'),
    'D2': ('Lite-On', 'LTST-C191KGKT'), 'D3': ('Lite-On', 'LTST-C191KSKT'),
    'J1': ('CUI Devices', 'PJ-063AH'), 'J3': ('Sullins', 'PPTC152LFBN-RC'), 'J4': ('Sullins', 'PPTC152LFBN-RC'),
    'J9': ('Sullins', 'PRPC020DBAN-M71RC'), 'J12': ('JST', 'S4B-PH-K-S'), 'J14': ('Sullins', 'PRPC006SAAN-RC'),
    'J15': ('CUI Devices', 'SJ1-3523N'),
}
BY_FP = {   # footprint name: (maker, part number), for the plain headers
    'PinHeader_1x02_P2.54mm_Vertical': ('Sullins', 'PRPC002SAAN-RC'),
    'PinHeader_1x03_P2.54mm_Vertical': ('Sullins', 'PRPC003SAAN-RC'),
}
BY_VALUE = {'2N7002': ('Nexperia', '2N7002,215')}
FUSES = {('0.5A', '/SW12V'): ('Littelfuse', '1206L050/15YR'),   # 12 V: MF-NSMF050-2 is only rated to 13.2 V
         ('0.5A', '+5V'): ('Bourns', 'MF-NSMF050-2'), ('0.1A', '+3V3'): ('Bourns', 'MF-NSMF010-2'),
         ('0.2A', '/VIN'): ('Bourns', 'MF-NSMF020-2'), ('1.5A', '/VIN'): ('Bourns', 'MF-MSMF150/24X-2')}
CAPS = {('10u', '1206'): 'CL31A106KAHNNNE', ('10u', '0805'): 'CL21A106KPFNNNE', ('22u', '0805'): 'CL21A226MPQNNNE',
        ('100n', '0603'): 'CL10B104KB8NNNC'}
NOTES = {'J1': 'Bench adapter: 12 V, 3 A, centre positive (17 V max: the buck\'s limit)',
         'J3': 'Same socket as DigiDash2\'s slots', 'J4': 'Same socket as DigiDash2\'s slots',
         'J9': 'Right angle, on the bottom edge, pins pointing off the board (the screen plugs onto them)',
         'D1': 'Reverse-polarity diode', 'L1': 'Isat 7.8 A', 'J15': 'Left edge, opening off the board',
         'J12': 'Right edge, opening off the board'}
SHUNTS = 'Plus 6 loose 2.54 mm jumper shunts (Sullins QPC02SXGN-RC), not placed: IGN, LAMP, REV, TERM, ROLE, spare'


def split(ref):
    m = re.match(r'([A-Z]+)(\d+)', ref)
    return m.group(1), int(m.group(2))


def package(fp):
    n = fp.split(':')[1]
    m = re.match(r'(?:R|C|L|LED|Fuse)_(\d{4})_', n)
    return m.group(1) if m else n


def yageo(val):
    """Yageo RC0603FR-07, 1 %: '4.7k' -> 4K7, '54.9k' -> 54K9, '22k' -> 22K, '120' -> 120R."""
    if val[-1] in 'kK':
        code = val[:-1].replace('.', 'K') if '.' in val else val[:-1] + 'K'
    else:
        code = val + 'R'
    return ('Yageo', 'RC0603FR-07%sL' % code)


def main():
    xml = os.path.join(TMP, 'benchboard_bom_netlist.xml')
    subprocess.check_call([CLI, 'sch', 'export', 'netlist', '--format', 'kicadxml', '-o', xml,
                           os.path.join(ROOT, 'BenchBoard.kicad_sch')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    root = ET.parse(xml).getroot()
    pin1 = {}
    for n in root.find('nets'):
        for node in n:
            if node.get('pin') == '1':
                pin1[node.get('ref')] = n.get('name')
    parts = []
    for c in root.find('components'):
        ref = c.get('ref')
        if ref.startswith('#') or re.match(r'(H|NT)\d', ref):
            continue
        parts.append((ref, c.findtext('value'), c.findtext('footprint'), c.findtext('description') or ''))
    groups = collections.OrderedDict()
    for ref, val, fp, descr in sorted(parts, key=lambda t: split(t[0])):
        pre = split(ref)[0]
        key = (pre, val, fp) if pre in ('R', 'C') else (ref,)
        groups.setdefault(key, []).append((ref, val, fp, descr))
    order = {'U': 0, 'L': 1, 'D': 2, 'Q': 3, 'J': 4, 'F': 5, 'R': 6, 'C': 7}
    rows = []
    for items in groups.values():
        ref, val, fp, descr = items[0]
        refs = [i[0] for i in items]
        pre, pkg = split(ref)[0], package(fp)
        if ref in MPN:
            mpn = MPN[ref]
        elif fp.split(':')[1] in BY_FP:
            mpn = BY_FP[fp.split(':')[1]]
        elif val in BY_VALUE:
            mpn = BY_VALUE[val]
        elif pre == 'F':
            mpn = FUSES[(val, pin1[ref])]
        elif pre == 'R':
            mpn = yageo(val)
        elif pre == 'C':
            mpn = ('Samsung', CAPS[(val, pkg)])
        else:
            raise SystemExit('no part number for %s (%s, %s)' % (ref, val, fp))
        if len(refs) > 1:
            descr = '%s %s' % (val, {'R': 'resistor, 1 %', 'C': 'capacitor'}[pre])
        note = NOTES.get(ref, '')
        rows.append((order.get(pre, 9), split(ref), [len(refs), ' '.join(refs), mpn, '%s: %s' % (val, descr) if pre in 'RC' and len(refs) == 1 else descr,
                                                   pkg, 'THT' if re.search(r'PinHeader|PinSocket|JST|Jack|BarrelJack', fp) else 'SMD', 'Top', note]))
    rows.sort(key=lambda t: (t[0], t[1]))
    out = os.path.join(ROOT, 'BenchBoard_BOM.csv')
    with open(out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Qty', 'Designators', 'Manufacturer part', 'Description', 'Package', 'Type', 'Side', 'Notes'])
        for r in rows:
            q, refs, (maker, part) = r[2][:3]
            w.writerow([q, refs, '%s %s' % (maker, part)] + r[2][3:])
        w.writerow(['', '', '', SHUNTS, '', '', '', ''])
    print('wrote %s: %d rows, %d parts' % (out, len(rows), sum(r[2][0] for r in rows)))
    out = os.path.join(ROOT, 'fab', 'BenchBoard_BOM_PCBWay.csv')
    with open(out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Item #', 'Designator', 'Qty', 'Manufacturer', 'Mfg Part #', 'Description / Value', 'Package/Footprint',
                    'Type', 'Side', 'Your Instructions / Notes'])
        for i, r in enumerate(rows, 1):
            q, refs, (maker, part), descr, pkg, typ, side, note = r[2]
            w.writerow([i, refs, q, maker, part, descr, pkg, typ, side, note])
    print('wrote %s: %d rows, %d parts. %s' % (out, len(rows), sum(r[2][0] for r in rows), SHUNTS))


if __name__ == '__main__':
    main()
