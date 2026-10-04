#!/usr/bin/env python3
"""Write a board's fab outputs into <BOARD_DIR>/fab: Gerbers + drill files (fab/gerbers, zipped), the placement file
and a PCBWay / JLC centroid. Coordinates are measured from the drill / place origin, which each board's gen_pcb.py
sets to the board's bottom-left corner. Not-fitted (DNP) parts are left out of the placement files.

    python3 tools/fab_export.py AudioCarrier [DigiDash2 ...]

The PCBWay BOM (fab/<board>_BOM_PCBWay.csv) is kept by hand and not touched; the script only warns when its
designators no longer match the board.
"""
import csv
import glob
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
KICAD = '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli'
CLI = os.environ.get('KICAD_CLI', KICAD if os.path.exists(KICAD) else 'kicad-cli')
LAYERS = 'F.Cu,In1.Cu,In2.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts'


def run(*args):
    subprocess.check_call([CLI, *args], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def export(board_dir):
    d = os.path.join(ROOT, board_dir)
    name = os.path.basename(os.path.normpath(d))
    pcb = os.path.join(d, name + '.kicad_pcb')
    fab, gbr = os.path.join(d, 'fab'), os.path.join(d, 'fab', 'gerbers')
    os.makedirs(gbr, exist_ok=True)
    for f in glob.glob(os.path.join(gbr, '*')):
        os.remove(f)
    layers = LAYERS if re.search(r'\(\d+ "In1\.Cu"', open(pcb).read()) else LAYERS.replace('In1.Cu,In2.Cu,', '')   # any KiCad's layer numbering
    run('pcb', 'export', 'gerbers', '--use-drill-file-origin', '--subtract-soldermask', '-l', layers, '-o', gbr, pcb)
    run('pcb', 'export', 'drill', '--drill-origin', 'plot', '--excellon-separate-th', '--generate-map',
        '--map-format', 'gerberx2', '-o', gbr + '/', pcb)
    with zipfile.ZipFile(os.path.join(fab, name + '_gerbers.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(glob.glob(os.path.join(gbr, '*'))):
            z.write(f, os.path.basename(f))

    place = os.path.join(fab, name + '_placement.csv')
    try:
        run('pcb', 'export', 'pos', '--format', 'csv', '--units', 'mm', '--side', 'both', '--use-drill-file-origin', '--exclude-dnp',
            '-o', place, pcb)
    except subprocess.CalledProcessError:       # KiCad 7's CLI has no --exclude-dnp: fine for a board with no DNP parts
        run('pcb', 'export', 'pos', '--format', 'csv', '--units', 'mm', '--side', 'both', '--use-drill-file-origin',
            '-o', place, pcb)
    rows = list(csv.DictReader(open(place)))
    with open(os.path.join(fab, name + '_centroid.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Designator', 'Mid X (mm)', 'Mid Y (mm)', 'Layer', 'Rotation'])
        for r in rows:
            w.writerow([r['Ref'], '%.3f' % float(r['PosX']), '%.3f' % float(r['PosY']),
                        'Top' if r['Side'] == 'top' else 'Bottom', '%g' % float(r['Rot'])])

    msg = '%s: %d Gerber / drill files, %d placed parts' % (name, len(os.listdir(gbr)), len(rows))
    bom = os.path.join(fab, name + '_BOM_PCBWay.csv')
    if os.path.exists(bom):
        listed = set()
        for r in csv.DictReader(open(bom)):
            listed |= set(re.split(r'[\s,;]+', r['Designator'].strip())) - {''}
        placed = {r['Ref'] for r in rows}
        if listed - placed or placed - listed:
            msg += '\n  PCBWay BOM out of date: not on the board %s; not in the BOM %s' % (
                sorted(listed - placed) or '-', sorted(placed - listed) or '-')
    print(msg)


if __name__ == '__main__':
    for a in sys.argv[1:] or ['DigiDash2', 'AudioCarrier']:
        export(a)
