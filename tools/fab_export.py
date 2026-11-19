#!/usr/bin/env python3
"""Write a board's fab outputs into <BOARD_DIR>/fab: Gerbers + drill files (fab/gerbers, zipped), the placement file
and a PCBWay / JLC centroid. Coordinates are measured from the drill / place origin, which each board's gen_pcb.py
sets to the board's bottom-left corner. Not-fitted (DNP) parts are left out of the placement files, and on the boards in
HAND_THT (through-hole parts soldered by hand) the through-hole parts too.

    python3 tools/fab_export.py AudioCarrier [DigiDash2 ...]
    python3 tools/fab_export.py --placement-only GPSCard ...   # placement / centroid / CPL only, Gerbers untouched

fab/<board>_CPL_JLCPCB.csv is the JLCPCB pick-and-place (CPL) file: KiCad's rotations, normalised to 0-360. JLC's
reels don't always share KiCad's zero orientation (SOT-23, SOIC / TSSOP / QFN, diodes, electrolytics), so check the
polarised parts in JLC's placement preview before ordering and add any fix to JLC_ROT below so it sticks.

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
# Boards whose through-hole parts are soldered by hand: they stay out of the placement files (the fab BOM lists
# only the SMD parts too), so an assembly quote covers the SMD side only.
HAND_THT = {'ConsoleBoardRS', 'SensorCard2', 'GPSCard2', 'AudioCarrier'}
# Footprint-name regex -> degrees added to KiCad's rotation in the JLC CPL, filled in as JLC's preview shows a part
# landing turned. Empty until a board has been through JLC's placement check.
JLC_ROT = {}
LAYERS = 'F.Cu,In1.Cu,In2.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts'


def run(*args):
    subprocess.check_call([CLI, *args], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def jlc_rot(package, rot):
    for pat, add in JLC_ROT.items():
        if re.search(pat, package):
            rot += add
            break
    return '%g' % (rot % 360)


def export(board_dir, placement_only=False):
    d = os.path.join(ROOT, board_dir)
    name = os.path.basename(os.path.normpath(d))
    pcb = os.path.join(d, name + '.kicad_pcb')
    fab, gbr = os.path.join(d, 'fab'), os.path.join(d, 'fab', 'gerbers')
    os.makedirs(gbr, exist_ok=True)
    if not placement_only:
        export_gerbers(name, pcb, fab, gbr)
    export_placement(name, pcb, fab, gbr)


def export_gerbers(name, pcb, fab, gbr):
    for f in glob.glob(os.path.join(gbr, '*')):
        os.remove(f)
    layers = LAYERS if '(4 "In1.Cu"' in open(pcb).read() else LAYERS.replace('In1.Cu,In2.Cu,', '')
    run('pcb', 'export', 'gerbers', '--use-drill-file-origin', '--subtract-soldermask', '-l', layers, '-o', gbr, pcb)
    run('pcb', 'export', 'drill', '--drill-origin', 'plot', '--excellon-separate-th', '--generate-map',
        '--map-format', 'gerberx2', '-o', gbr + '/', pcb)
    with zipfile.ZipFile(os.path.join(fab, name + '_gerbers.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(glob.glob(os.path.join(gbr, '*'))):
            z.write(f, os.path.basename(f))


def export_placement(name, pcb, fab, gbr):
    place = os.path.join(fab, name + '_placement.csv')
    run('pcb', 'export', 'pos', '--format', 'csv', '--units', 'mm', '--side', 'both', '--use-drill-file-origin', '--exclude-dnp',
        *(['--smd-only'] if name in HAND_THT else []), '-o', place, pcb)
    rows = list(csv.DictReader(open(place)))
    with open(os.path.join(fab, name + '_centroid.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Designator', 'Mid X (mm)', 'Mid Y (mm)', 'Layer', 'Rotation'])
        for r in rows:
            w.writerow([r['Ref'], '%.3f' % float(r['PosX']), '%.3f' % float(r['PosY']),
                        'Top' if r['Side'] == 'top' else 'Bottom', '%g' % float(r['Rot'])])
    with open(os.path.join(fab, name + '_CPL_JLCPCB.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Designator', 'Val', 'Package', 'Mid X', 'Mid Y', 'Rotation', 'Layer'])
        for r in rows:
            w.writerow([r['Ref'], r['Val'], r['Package'], '%.3fmm' % float(r['PosX']), '%.3fmm' % float(r['PosY']),
                        jlc_rot(r['Package'], float(r['Rot'])), 'Top' if r['Side'] == 'top' else 'Bottom'])

    msg = '%s: %d Gerber / drill files, %d placed parts' % (name, len(os.listdir(gbr)), len(rows))
    bom = os.path.join(fab, name + '_BOM_PCBWay.csv')
    if os.path.exists(bom):
        listed = set()
        for r in csv.DictReader(open(bom)):
            if any((v or '').startswith('DNP') for v in r.values()):
                continue                # a DNP line: in the BOM, never placed
            listed |= set(re.split(r'[\s,;]+', r['Designator'].strip())) - {''}
        placed = {r['Ref'] for r in rows}
        if listed - placed or placed - listed:
            msg += '\n  PCBWay BOM out of date: not on the board %s; not in the BOM %s' % (
                sorted(listed - placed) or '-', sorted(placed - listed) or '-')
    print(msg)


if __name__ == '__main__':
    args = sys.argv[1:]
    only = '--placement-only' in args
    for a in [a for a in args if a != '--placement-only'] or ['DigiDash2', 'AudioCarrier']:
        export(a, only)
