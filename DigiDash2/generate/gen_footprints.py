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
# Card slot: 2x15 female socket, 2.54 mm, vertical (Sullins PPTC152LFBN-RC or equal). The pads are numbered like a
# pin header (pad 2 beside pad 1 at +2.54 mm, pads counting down the rows), not like KiCad's stock socket, so each pad
# mates with the same-numbered pin of the card's right-angle header (PinHeader_2x15_P2.54mm_Horizontal) when the card
# stands in it. See docs/hardware/accessory_ports.md.
b = [text('reference', 'REF**', 1.27, -3.0, 'F.SilkS'), text('value', 'Slot_Socket_2x15', 1.27, 38.6, 'F.Fab')]
for m in range(15):
    y = 2.54 * m
    shape = 'rect' if m == 0 else 'circle'
    b.append('  (pad "%d" thru_hole %s (at 0 %g) (size 1.7 1.7) (drill 1) (layers "*.Cu" "*.Mask"))' % (2 * m + 1, shape, y))
    b.append(tht(2 * m + 2, 2.54, y, 1.7, 1.0))
b += [rect(-1.27, -1.27, 3.81, 36.83, 'F.Fab', 0.1), rect(-1.33, -1.33, 3.87, 36.89, 'F.SilkS', 0.12),
      '  (fp_line (start -1.8 -1.33) (end -1.8 1.27) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
      rect(-1.77, -1.77, 4.31, 37.33, 'F.CrtYd', 0.05),
      '  (model "${KICAD10_3DMODEL_DIR}/Connector_PinSocket_2.54mm.3dshapes/PinSocket_2x15_P2.54mm_Vertical.step"\n'
      '    (offset (xyz 2.54 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0)))']
write('Slot_Socket_2x15_P2.54mm_Vertical', 'Card slot: 2x15 female socket, 2.54 mm, vertical, pads numbered to mate pin for pin with a '
      'right-angle 2x15 header on a card standing in it (pad 2 at +2.54 mm)', 'socket 2x15 slot card', 'through_hole', b)
print('footprints written')
