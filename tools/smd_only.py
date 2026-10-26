#!/usr/bin/env python3
"""Write the assembler's SMD-only files for a board into fab/assembly/: the PCBWay BOM, the JLCPCB CPL, the placement
and centroid files with every through-hole line taken out (you solder those yourself). The full files in fab/ stay
as they are: they are the shopping list.

    python3 tools/smd_only.py SensorCard RadioCard ...
"""
import csv
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def main(boards):
    for b in boards:
        fab = os.path.join(ROOT, b, 'fab')
        out = os.path.join(fab, 'assembly')
        os.makedirs(out, exist_ok=True)
        bom = list(csv.DictReader(open(os.path.join(fab, b + '_BOM_PCBWay.csv'), newline='')))
        tht = {r for row in bom if row['Type'].strip().upper().startswith('THT') for r in row['Designator'].split()}
        smd = [row for row in bom if not row['Type'].strip().upper().startswith('THT')]
        with open(os.path.join(out, b + '_BOM_SMD.csv'), 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(bom[0].keys()))
            w.writeheader()
            for n, row in enumerate(smd, 1):
                row = dict(row, **{'Item #': n})
                w.writerow(row)
        kept = 0
        for name in (b + '_CPL_JLCPCB.csv', b + '_placement.csv', b + '_centroid.csv'):
            src = os.path.join(fab, name)
            if not os.path.exists(src):
                continue
            rows = list(csv.reader(open(src, newline='')))
            head, body = rows[0], rows[1:]
            ref_col = next(i for i, h in enumerate(head) if h.strip().lower() in ('designator', 'ref', 'reference', 'refdes'))
            body = [r for r in body if r and r[ref_col].strip() not in tht]
            kept = len(body)
            with open(os.path.join(out, name.replace('.csv', '_SMD.csv')), 'w', newline='') as f:
                csv.writer(f).writerows([head] + body)
        print('%s: %d SMD lines (%d parts) for the assembler, %d through-hole parts left to you: %s' % (
            b, len(smd), sum(int(r['Qty']) for r in smd), len(tht), ' '.join(sorted(tht))))


if __name__ == '__main__':
    main(sys.argv[1:] or ['SensorCard', 'RadioCard', 'GPSCard', 'AudioCarrier', 'ConsoleBoardRS'])
