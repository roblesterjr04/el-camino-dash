#!/usr/bin/env python3
"""Write Panel/fab/Panel_BOM_PCBWay.csv: the assembler's BOM for the whole panel.

The parts list is the panel's own placement file (fab/Panel_placement.csv, from tools/fab_export.py), so it holds
exactly what the machine places: no through-hole parts (gen_panel.py marks them DNP), no DNP parts, no pads-only
footprints. Maker and part number come from each board's own BOM, matched by designator; rows with the same part
number on several boards are merged. Parts missing from every board BOM, or listed without a part number, are
printed so they can be filled in.

    python3 Panel/generate/gen_bom.py
"""
import csv
import os
import re
import sys
from collections import OrderedDict

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
FAB = os.path.join(ROOT, 'Panel', 'fab')

# prefix -> (board directory, its PCBWay BOM)
SOURCES = OrderedDict([
    ('DD', ('DigiDash2', 'fab/DigiDash2_BOM_PCBWay.csv')),
    ('SE', ('SensorCard', 'fab/SensorCard_BOM_PCBWay.csv')),
    ('RA', ('RadioCard', 'fab/RadioCard_BOM_PCBWay.csv')),
    ('GP', ('GPSCard', 'fab/GPSCard_BOM_PCBWay.csv')),
    ('AU', ('AudioCarrier', 'fab/AudioCarrier_BOM_PCBWay.csv')),
    ('CO', ('ConsoleBoardRS', 'fab/ConsoleBoardRS_BOM_PCBWay.csv')),
])


def refs(cell):
    return [r for r in re.split(r'[\s,;]+', cell.strip()) if r]


def board_parts():
    """Prefixed designator -> dict(maker, mpn, desc, package, notes) from the boards' own BOMs."""
    info = {}
    for tag, (d, bom) in SOURCES.items():
        for row in csv.DictReader(open(os.path.join(ROOT, d, bom), newline='')):
            part = dict(maker=row['Manufacturer'].strip(), mpn=row['Mfg Part #'].strip(),
                        desc=row['Description / Value'].strip(), package=row['Package/Footprint'].strip(),
                        notes=row['Your Instructions / Notes'].strip())
            for r in refs(row['Designator']):
                info['%s-%s' % (tag, r)] = part
    return info


def main():
    placed = list(csv.DictReader(open(os.path.join(FAB, 'Panel_placement.csv'), newline='')))
    info = board_parts()
    rows = OrderedDict()
    no_bom, no_mpn = [], []
    for p in placed:
        ref, side = p['Ref'], 'Top' if p['Side'] == 'top' else 'Bottom'
        part = info.get(ref)
        if part is None:
            no_bom.append(ref)
            part = dict(maker='', mpn='', desc=p['Val'], package=p['Package'], notes='')
        if not part['mpn']:
            no_mpn.append(ref)
        key = ('mpn', part['maker'], part['mpn']) if part['mpn'] else ('val', part['desc'], part['package'])
        row = rows.setdefault(key, dict(part, refs=[], sides=set(), notes=[]))
        row['refs'].append(ref)
        row['sides'].add(side)
        if part['notes']:
            note = '%s: %s' % (ref.split('-')[0], part['notes'])
            if note not in row['notes']:
                row['notes'].append(note)

    out = os.path.join(FAB, 'Panel_BOM_PCBWay.csv')
    with open(out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Item #', 'Designator', 'Qty', 'Manufacturer', 'Mfg Part #', 'Description / Value',
                    'Package/Footprint', 'Type', 'Side', 'Your Instructions / Notes'])
        for n, row in enumerate(rows.values(), 1):
            w.writerow([n, ' '.join(row['refs']), len(row['refs']), row['maker'], row['mpn'], row['desc'],
                        row['package'], 'SMD', '/'.join(sorted(row['sides'], reverse=True)), ' | '.join(row['notes'])])

    print('%s: %d lines, %d placed parts' % (os.path.relpath(out, ROOT), len(rows), len(placed)))
    if no_bom:
        print('not in any board BOM (value + package only): %d: %s' % (len(no_bom), ' '.join(no_bom)))
    missing = [r for r in no_mpn if r not in no_bom]
    if missing:
        print('in a board BOM but no part number: %d: %s' % (len(missing), ' '.join(missing)))


if __name__ == '__main__':
    sys.exit(main())
