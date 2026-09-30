#!/usr/bin/env python3
"""Write the footprints the stock KiCad libraries do not have into DigiDash2.pretty."""
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'DigiDash2.pretty')
os.makedirs(OUT, exist_ok=True)


def rect(x1, y1, x2, y2, layer, w):
    return ('  (fp_rect (start %g %g) (end %g %g) (stroke (width %g) (type solid)) (fill none) (layer "%s"))' %
            (x1, y1, x2, y2, w, layer))


def text(kind, s, x, y, layer, size=1):
    return ('  (fp_text %s "%s" (at %g %g) (layer "%s") (effects (font (size %g %g) (thickness 0.15))))' %
            (kind, s, x, y, layer, size, size))


def write(name, descr, tags, attr, body):
    txt = '(footprint "%s" (version 20221018) (generator gen_footprints)\n  (layer "F.Cu")\n  (descr "%s")\n  (tags "%s")\n  (attr %s)\n%s\n)\n' % (
        name, descr, tags, attr, '\n'.join(body))
    open(os.path.join(OUT, name + '.kicad_mod'), 'w').write(txt)


def tht(num, x, y, size, drill):
    return ('  (pad "%s" thru_hole circle (at %g %g) (size %g %g) (drill %g) (layers "*.Cu" "*.Mask"))' %
            (num, x, y, size, size, drill))


# RCA jack, face mount (the jack points away from the board): Same Sky (CUI) RCJ-02x, the datasheet's recommended
# PCB layout, as on rev 1's J11 / J12. Pad 1 = tip (centre pin), pad 2 = sleeve (the two bracket legs).
b = [text('reference', 'REF**', 0, -7.2, 'F.SilkS'), text('value', 'RCA_RCJ-02x', 0, 7.2, 'F.Fab'),
     tht(1, 0, 0, 2.55, 1.7), tht(2, -5.15, 1.6, 3.0, 2.0), tht(2, 5.15, 1.6, 3.0, 2.0),
     rect(-5.35, -5.0, 5.35, 5.0, 'F.SilkS', 0.15),
     '  (fp_circle (center 0 0) (end 4.15 0) (stroke (width 0.15) (type solid)) (fill none) (layer "F.SilkS"))',
     rect(-6.97, -5.3, 6.97, 5.3, 'F.CrtYd', 0.05)]
write('RCA_RCJ-02x_FaceMount', 'Same Sky (CUI) RCJ-02x RCA jack, face mount, through hole, 22.1 mm tall. Pad 1 tip, pad 2 sleeve',
      'RCA RCJ-02', 'through_hole', b)
print('footprints written')
