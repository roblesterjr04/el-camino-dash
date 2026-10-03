#!/usr/bin/env python3
"""Write the footprint the KiCad stock libraries do not have into ConsoleBoard4Key.pretty."""
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ConsoleBoard4Key.pretty')
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


# E-Switch TL1240YQ: illuminated 6 x 6 mm tact switch, yellow LED (10 mA, 2.5 V), drawing P010363 rev E.
# P.C.B. layout (top view): switch pins on a 6.50 x 4.50 rectangle (3 and 4 on top, 1 and 2 below; 1-2 and 3-4 are
# each one contact: the switch closes 1/2 to 3/4), LED pins on the centre line 5.40 apart, + (anode) on top, - below.
# Holes 1.00 mm. The legs bend out to 7.9 mm across at the board, so the courtyard is that wide; the body is 6.0 x 6.6.
# Pads are 1.5 mm (0.25 mm ring): the switch sits in an 8 mm strip at the board edge.
b = [text('reference', 'REF**', 0, -4.6, 'F.Fab', 0.7), text('value', 'TL1240YQ', 0, 4.6, 'F.Fab', 0.7)]
for num, x, y in (('1', -3.25, 2.25), ('2', 3.25, 2.25), ('3', -3.25, -2.25), ('4', 3.25, -2.25)):
    b.append(tht(num, x, y, 1.0, 1.5))
b.append(tht('A', 0, -2.7, 1.0, 1.5, 'rect'))
b.append(tht('K', 0, 2.7, 1.0, 1.5))
b += [rect(-3.0, -3.0, 3.0, 3.0, 'F.Fab', 0.1),
      circle(0, 0, 1.5, 'F.Fab', 0.1),
      '  (fp_line (start -3 -3.8) (end 3 -3.8) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',   # top and bottom
      '  (fp_line (start -3 3.8) (end 3 3.8) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',     # edges, clear of the pads
      rect(-4.1, -3.55, 4.1, 3.55, 'F.CrtYd', 0.05),
      text('user', '+', 0.9, -2.7, 'F.Fab', 0.6)]
write('SW_TL1240YQ',
      'E-Switch TL1240YQ illuminated tact switch, 6 x 6 mm, through hole, yellow LED (drawing P010363 rev E). Pins 1/2 and 3/4 switch, A LED anode, K LED cathode',
      'switch tactile illuminated LED E-Switch TL1240', 'through_hole', b)
print('wrote', os.path.normpath(OUT))
