#!/usr/bin/env python3
"""Narrow fine-pitch SMD pads on a routed board so that neighbouring pads are at least 0.22 mm apart, the smallest gap
PCBWay can hold a solder-mask dam in. Without a dam between them, pads of a 0.5 mm QFP or 0.65 mm VSSOP share one mask
opening and bridge more easily in assembly.

For each footprint, pads of one size in a row are taken at the row's pitch (the distance to the nearest pad of the same
size); where pitch - width < 0.22 mm and the pitch is 0.8 mm or less, the width becomes pitch - 0.22 (length unchanged,
centre unchanged). A pad is never narrowed by more than a quarter of its width. Edits the .kicad_pcb in place (the
footprints then differ from the library copy: KiCad's "footprint differs from library" warning is expected), so run it
again after regenerating a board:

    python3 tools/mask_dams.py DigiDash2 SensorCard ...      # board directories
"""
import math
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
DAM = 0.22          # mm, smallest pad-to-pad gap that keeps a solder-mask dam (PCBWay)
MAX_PITCH = 0.8     # mm, only fine-pitch rows
MAX_CUT = 0.25      # never narrow a pad by more than this fraction of its width

PAD = re.compile(r'(\(pad "[^"]*" smd \w+\n\t\t\t\(at )([-\d.]+) ([-\d.]+)((?: [-\d.]+)?\)\n\t\t\t\(size )([\d.]+) ([\d.]+)(\))')


def fix(board):
    path = os.path.join(ROOT, board, board + '.kicad_pcb')
    s = open(path).read()
    out, last, changed = [], 0, []
    for fm in re.finditer(r'\n\t\(footprint .*?\n\t\)', s, re.S):
        blk = fm.group(0)
        ref = re.search(r'\(property "Reference" "([^"]+)"', blk).group(1)
        pads = [(m, float(m.group(2)), float(m.group(3)), float(m.group(5)), float(m.group(6))) for m in PAD.finditer(blk)]
        new = {}
        for i, (m, x, y, a, b) in enumerate(pads):
            same = [math.hypot(x - x2, y - y2) for j, (m2, x2, y2, a2, b2) in enumerate(pads)
                    if j != i and {a2, b2} == {a, b}]
            if not same:
                continue
            pitch = min(same)
            w = min(a, b)
            target = round(pitch - DAM, 3)
            if pitch <= MAX_PITCH and w - pitch > -DAM + 1e-6 and target >= w * (1 - MAX_CUT):
                na, nb = (target, b) if a < b else (a, target)
                new[m.start()] = (m, na, nb)
        if new:
            for st in sorted(new, reverse=True):
                m, na, nb = new[st]
                rep = m.group(1) + m.group(2) + ' ' + m.group(3) + m.group(4) + '%g %g' % (na, nb) + m.group(7)
                blk = blk[:m.start()] + rep + blk[m.end():]
            m0, na0, nb0 = next(iter(new.values()))
            changed.append('%s: %d pads to %g mm wide' % (ref, len(new), min(na0, nb0)))
        out.append(s[last:fm.start()]); out.append(blk); last = fm.end()
    out.append(s[last:])
    open(path, 'w').write(''.join(out))
    print('%s: %s' % (board, '; '.join(changed) or 'nothing under %.2f mm' % DAM))


if __name__ == '__main__':
    for b in sys.argv[1:] or ['DigiDash2', 'GPSCard', 'AudioCarrier', 'SensorCard', 'RadioCard', 'ConsoleBoardRS']:
        fix(b)
