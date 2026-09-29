#!/usr/bin/env python3
"""Write the two footprints the KiCad stock libraries do not have into AudioCarrier.pretty."""
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'AudioCarrier.pretty')


def rect(x1, y1, x2, y2, layer, w):
    return ('  (fp_rect (start %g %g) (end %g %g) (stroke (width %g) (type solid)) (fill none) (layer "%s"))' %
            (x1, y1, x2, y2, w, layer))


def text(kind, s, x, y, layer, size=1):
    return ('  (fp_text %s "%s" (at %g %g) (layer "%s") (effects (font (size %g %g) (thickness 0.15))))' %
            (kind, s, x, y, layer, size, size))


def write(name, descr, tags, body):
    txt = '(footprint "%s" (version 20221018) (generator gen_footprints)\n  (layer "F.Cu")\n  (descr "%s")\n  (tags "%s")\n  (attr through_hole)\n%s\n)\n' % (
        name, descr, tags, '\n'.join(body))
    open(os.path.join(OUT, name + '.kicad_mod'), 'w').write(txt)


def pad(num, x, y, size, drill, shape='circle'):
    return ('  (pad "%s" thru_hole %s (at %g %g) (size %g %g) (drill %g) (layers "*.Cu" "*.Mask"))' %
            (num, shape, x, y, size, size, drill))


# RCA jack, face mount (vertical): Same Sky (CUI) RCJ-02x, "Recommended PCB Layout" of the datasheet:
# tip pin (hole 1.7) at the origin, the two sleeve / bracket legs (holes 2.0) 10.3 mm apart, 1.6 mm toward +Y.
# Barrel 8.3 mm, flange 10.7 x 10.0, bracket 12.65 wide. Pad 1 = tip, pad 2 = sleeve (twice).
b = [text('reference', 'REF**', 0, -7.2, 'F.SilkS'), text('value', 'RCA_Jack_SameSky_RCJ-02_Vertical', 0, 7.2, 'F.Fab'),
     pad(1, 0, 0, 2.6, 1.7), pad(2, -5.15, 1.6, 3.4, 2.0), pad(2, 5.15, 1.6, 3.4, 2.0),
     rect(-5.35, -5.0, 5.35, 5.0, 'F.SilkS', 0.15),
     '  (fp_circle (center 0 0) (end 4.15 0) (stroke (width 0.15) (type solid)) (fill none) (layer "F.SilkS"))',
     rect(-6.5, -5.3, 6.5, 5.3, 'F.CrtYd', 0.05), text('user', 'TIP', 0, 2.6, 'F.SilkS', 0.7)]
write('RCA_Jack_SameSky_RCJ-02_Vertical',
      'Same Sky (CUI) RCJ-02x RCA jack, vertical / face mount, through hole. Pad pattern from the datasheet recommended layout. 22.1 mm tall. Pad 1 tip, pad 2 sleeve',
      'RCA RCJ-02 vertical', b)
print('footprints written')
