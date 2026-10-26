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
# Card slot: 2x15 female socket, 2.54 mm, vertical (Sullins PPTC152LFBN-RC or equal). Numbered as KiCad's stock socket
# (pad 2 beside pad 1 at -2.54 mm, pads counting down the rows): on the main board's back, turned with pad 2 toward +x,
# pin 1 is at the top of the row, and each pad mates with the same-numbered pin of the card's right-angle header
# (PinHeader_2x15_P2.54mm_Horizontal) when the card stands in it, its parts side toward -x. (Until 2026-10-19 the pads
# were numbered the other way, pad 2 at +2.54 mm; that put a card's pin 1 on pad 29.) See
# docs/hardware/accessory_ports.md.
b = [text('reference', 'REF**', -1.27, -3.0, 'F.SilkS'), text('value', 'Slot_Socket_2x15', -1.27, 38.6, 'F.Fab')]
for m in range(15):
    y = 2.54 * m
    shape = 'rect' if m == 0 else 'circle'
    b.append('  (pad "%d" thru_hole %s (at 0 %g) (size 1.7 1.7) (drill 1) (layers "*.Cu" "*.Mask"))' % (2 * m + 1, shape, y))
    b.append(tht(2 * m + 2, -2.54, y, 1.7, 1.0))
b += [rect(-3.81, -1.27, 1.27, 36.83, 'F.Fab', 0.1), rect(-3.87, -1.33, 1.33, 36.89, 'F.SilkS', 0.12),
      '  (fp_line (start 1.8 -1.33) (end 1.8 1.27) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
      rect(-4.31, -1.77, 1.77, 37.33, 'F.CrtYd', 0.05),
      '  (model "${KICAD10_3DMODEL_DIR}/Connector_PinSocket_2.54mm.3dshapes/PinSocket_2x15_P2.54mm_Vertical.step"\n'
      '    (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0)))']
write('Slot_Socket_2x15_P2.54mm_Vertical', 'Card slot: 2x15 female socket, 2.54 mm, vertical, numbered as KiCad\'s stock socket '
      '(pad 2 at -2.54 mm): on the back, pad 2 toward +x and pin 1 at the top, it mates pin for pin with a right-angle 2x15 '
      'header on a card standing in it', 'socket 2x15 slot card', 'through_hole', b)

# Molded 1050 power inductor (COILMX MS1050-6R8M, 11.15 x 10.0 mm body, terminals 2.0 wide x 3.0 at the ends, centred
# +/-4.6): two 4.0 x 4.0 mm pads at +/-3.9, so each terminal (3.6 ... 5.6) sits on its pad with 0.3 mm to spare
# outward and the pad reaches 1.9 mm under the body. The datasheet asks for 4.1 x 4.95 mm pads at +/-4.75 (13.6 mm
# overall); on DigiDash2 that ran into C3, a GND via and the 12V_BATT track beside L1, and the smaller pads are the
# Wurth WE-HCI 1050 pattern it replaced on 2026-10-26 (3.85 x 4.0 at +/-3.83) grown a little, which the routing allows.
def smd(num, x, y, w, h):
    return ('  (pad "%s" smd rect (at %g %g) (size %g %g) (layers "F.Cu" "F.Paste" "F.Mask"))' % (num, x, y, w, h))


b = [text('reference', 'REF**', 0, -6.3, 'F.SilkS'), text('value', 'L_Molded_1050', 0, 6.3, 'F.Fab'),
     smd(1, -3.9, 0, 4.0, 4.0), smd(2, 3.9, 0, 4.0, 4.0),
     rect(-5.575, -5.0, 5.575, 5.0, 'F.Fab', 0.1),
     '  (fp_line (start -2.5 -5.11) (end 2.5 -5.11) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
     '  (fp_line (start -2.5 5.11) (end 2.5 5.11) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
     rect(-5.85, -5.3, 5.85, 5.3, 'F.CrtYd', 0.05)]
write('L_Molded_1050', 'Molded shielded power inductor, 1050 size (COILMX MS1050 series, 11.15 x 10.0 x 5.0 mm): pads 4.0 x 4.0 mm at +/-3.9 (terminals 2.0 x 3.0 at +/-4.6)',
      'inductor molded 1050 MS1050', 'smd', b)
print('footprints written')
