#!/usr/bin/env python3
"""Write the footprint the KiCad stock libraries do not have into ConsoleBoardRS.pretty."""
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ConsoleBoardRS.pretty')
os.makedirs(OUT, exist_ok=True)


def rect(x1, y1, x2, y2, layer, w):
    return ('  (fp_rect (start %g %g) (end %g %g) (stroke (width %g) (type solid)) (fill none) (layer "%s"))' %
            (x1, y1, x2, y2, w, layer))


def circle(x, y, r, layer, w):
    return ('  (fp_circle (center %g %g) (end %g %g) (stroke (width %g) (type solid)) (fill none) (layer "%s"))' %
            (x, y, x + r, y, w, layer))


def text(kind, s, x, y, layer, size=1):
    return ('  (fp_text %s "%s" (at %g %g) (layer "%s") (effects (font (size %g %g) (thickness 0.15))))' %
            (kind, s, x, y, layer, size, size))


def write(name, descr, tags, attr, body):
    txt = '(footprint "%s" (version 20221018) (generator gen_footprints)\n  (layer "F.Cu")\n  (descr "%s")\n  (tags "%s")\n  (attr %s)\n%s\n)\n' % (
        name, descr, tags, attr, '\n'.join(body))
    open(os.path.join(OUT, name + '.kicad_mod'), 'w').write(txt)


def tht(num, x, y, drill, dia, shape='circle'):
    return ('  (pad "%s" thru_hole %s (at %g %g) (size %g %g) (drill %g) (layers "*.Cu" "*.Mask"))' %
            (num, shape, x, y, dia, dia, drill))


# Molex 95522-2447: RJ9 4P4C modular jack, vertical (top entry), through hole, high profile (16.25 mm), with PCB
# locators. NOT YET CHECKED AGAINST THE MOLEX DRAWING (955222447_sd.pdf could not be fetched when this was written):
# - confirmed from Molex's part data: 1.27 mm termination pitch (contacts staggered in two rows), PCB locators,
#   16.25 mm tall, 1.57 mm board;
# - assumed: the usual vertical modular-jack pattern, contacts 1-4 staggered on 1.27 mm in two rows 2.54 apart
#   (1 and 3 in the first row, 2 and 4 in the second), 0.90 mm holes, and two 3.25 mm locating pegs 10.16 apart,
#   6.35 beyond the second row, symmetric about the contacts;
# - body: a conservative 15.24 x 16.64 mm envelope (the series' largest body, the 8P8C 95522-2887), taken to start
#   1.75 mm before the first contact row; the 4P4C body is narrower. Courtyard 0.5 mm bigger all round.
# Check every hole against the drawing (or a jack in hand) before ordering boards.
# Origin: the centre of the contact field's first row.
BW, B0, B1 = 15.24 / 2, -1.75, -1.75 + 16.64
b = [text('reference', 'REF**', 0, B0 - 1.25, 'F.SilkS', 0.7), text('value', '95522-2447', 0, B1 + 1.6, 'F.Fab', 0.7)]
for num, x, y in (('1', -1.905, 0), ('2', -0.635, 2.54), ('3', 0.635, 0), ('4', 1.905, 2.54)):
    b.append(tht(num, x, y, 0.9, 1.5, 'rect' if num == '1' else 'circle'))
for x in (-5.08, 5.08):
    b.append('  (pad "" np_thru_hole circle (at %g 8.89) (size 3.25 3.25) (drill 3.25) (layers "*.Cu" "*.Mask"))' % x)
b += [rect(-BW, B0, BW, B1, 'F.Fab', 0.1),
      rect(-BW - 0.12, B0 - 0.12, BW + 0.12, B1 + 0.12, 'F.SilkS', 0.12),
      rect(-BW - 0.5, B0 - 0.5, BW + 0.5, B1 + 0.5, 'F.CrtYd', 0.05),
      text('user', 'PLUG FROM ABOVE', 0, 6.0, 'F.Fab', 0.6)]
write('RJ9_Molex_95522-2447_Vertical',
      'Molex 95522-2447 RJ9 4P4C modular jack, vertical top entry, through hole, 16.25 mm tall. Pads 1-4 = jack contacts 1-4. '
      'Hole pattern assumed (standard staggered 1.27 mm layout), body is an envelope: check against Molex drawing 955222447_sd',
      'RJ9 RJ10 RJ22 4P4C modular jack vertical top entry Molex 95522', 'through_hole', b)
print('wrote', os.path.normpath(OUT))
