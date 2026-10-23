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


# Assmann WSW A-2014-3-4-N-T-R: RJ9 (RJ10 / RJ22) 4P4C modular jack, vertical (top entry), through hole, 16.38 mm tall,
# two PCB locating pegs. From Assmann drawing ASS 7331 CO rev00 (seen from the component side), turned 180 degrees here
# (no mirroring) so the body runs toward +y from the contacts, as the old envelope did:
# - contacts: 0.90 mm holes on 1.27 mm, staggered in two rows 2.54 apart: 1 and 3 in the row nearer the pegs (y = 0),
#   2 and 4 in the far row (y = -2.54); pin 1 at x = -1.905, then 2 -0.635, 3 +0.635, 4 +1.905. Pads 1-4 = jack
#   contacts 1-4;
# - pegs: two 3.20 mm non-plated holes, 7.62 apart, centred on the contacts, 6.35 from the near row (8.89 from the far);
# - body: 11.43 wide (across the rows) x 16.50 long, from 6.50 beyond the pegs (y = 12.85) to 0.76 beyond the far row
#   (y = -3.65; the main body ends at -3.05, 0.51 beyond it). Leads 3.25 mm. Courtyard 0.5 mm bigger all round.
# Origin: the centre of the near contact row.
BW, B0, B1 = 11.43 / 2, -3.65, 12.85
b = [text('reference', 'REF**', 0, B0 - 1.25, 'F.SilkS', 0.7), text('value', 'A-2014-3-4-N-T-R', 0, B1 + 1.6, 'F.Fab', 0.7)]
for num, x, y in (('1', -1.905, 0), ('2', -0.635, -2.54), ('3', 0.635, 0), ('4', 1.905, -2.54)):
    b.append(tht(num, x, y, 0.9, 1.5, 'rect' if num == '1' else 'circle'))
for x in (-3.81, 3.81):
    b.append('  (pad "" np_thru_hole circle (at %g 6.35) (size 3.2 3.2) (drill 3.2) (layers "*.Cu" "*.Mask"))' % x)
b += [rect(-BW, B0, BW, B1, 'F.Fab', 0.1),
      rect(-BW - 0.12, B0 - 0.12, BW + 0.12, B1 + 0.12, 'F.SilkS', 0.12),
      rect(-BW - 0.5, B0 - 0.5, BW + 0.5, B1 + 0.5, 'F.CrtYd', 0.05),
      text('user', 'PLUG FROM ABOVE', 0, 10.0, 'F.Fab', 0.6)]
write('RJ9_Assmann_A-2014-3-4-N-T-R_Vertical',
      'Assmann WSW A-2014-3-4-N-T-R RJ9 4P4C modular jack, vertical top entry, through hole, 16.38 mm tall, locating pegs. '
      'Pads 1-4 = jack contacts 1-4. From Assmann drawing ASS 7331 CO rev00',
      'RJ9 RJ10 RJ22 4P4C modular jack vertical top entry Assmann A-2014-3-4-N-T-R', 'through_hole', b)
print('wrote', os.path.normpath(OUT))
