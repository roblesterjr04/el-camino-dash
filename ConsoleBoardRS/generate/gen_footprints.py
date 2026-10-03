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


# Wayconn MJBA-640XXX: RJ11 6P4C top-entry jack, through hole (drawing MJBA-XXXXXX rev A). PCB layout (top view):
# contacts 1-4 staggered on a 1.27 mm pitch in two rows 2.54 apart (1 and 3 in the first row, 2 and 4 in the second),
# 0.90 mm holes; two 3.25 mm locating pegs 10.16 apart, 6.35 beyond the second row, symmetric about the contacts.
# Origin: the centre of the contact field's first row. The body is 13.46 x 16.38 mm and 16.5 mm tall; the drawing does
# not tie the body to the holes, so it is taken to start 1.75 mm before the first row and run back over the pegs, and
# the courtyard is drawn 0.5 mm wider than that all round.
b = [text('reference', 'REF**', 0, -3.0, 'F.SilkS', 0.7), text('value', 'MJBA-640', 0, 16.2, 'F.Fab', 0.7)]
for num, x, y in (('1', -1.905, 0), ('2', -0.635, 2.54), ('3', 0.635, 0), ('4', 1.905, 2.54)):
    b.append(tht(num, x, y, 0.9, 1.5, 'rect' if num == '1' else 'circle'))
for x in (-5.08, 5.08):
    b.append('  (pad "" np_thru_hole circle (at %g 8.89) (size 3.25 3.25) (drill 3.25) (layers "*.Cu" "*.Mask"))' % x)
b += [rect(-6.73, -1.75, 6.73, 14.63, 'F.Fab', 0.1),
      rect(-6.85, -1.87, 6.85, 14.75, 'F.SilkS', 0.12),
      rect(-7.23, -2.25, 7.23, 15.13, 'F.CrtYd', 0.05),
      text('user', 'PLUG FROM ABOVE', 0, 6.0, 'F.Fab', 0.6)]
write('RJ11_Wayconn_MJBA-640_Vertical',
      'Wayconn MJBA-640XXX RJ11 6P4C modular jack, top entry, through hole (drawing MJBA-XXXXXX rev A). Pads 1-4 = jack contacts 2-5. Body outline estimated',
      'RJ11 RJ14 6P4C modular jack vertical top entry Wayconn MJBA', 'through_hole', b)
print('wrote', os.path.normpath(OUT))
