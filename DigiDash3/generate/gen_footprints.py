#!/usr/bin/env python3
"""Write the footprints the stock KiCad libraries do not have into DigiDash3.pretty."""
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'DigiDash3.pretty')
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
    # 1.5 mm pads on 1.0 mm holes (0.25 mm ring; rev 2 had 1.7): 1.04 mm between neighbours, two 0.25 mm tracks
    b.append('  (pad "%d" thru_hole %s (at 0 %g) (size 1.5 1.5) (drill 1) (layers "*.Cu" "*.Mask"))' % (2 * m + 1, shape, y))
    b.append(tht(2 * m + 2, -2.54, y, 1.5, 1.0))
b += [rect(-3.81, -1.27, 1.27, 36.83, 'F.Fab', 0.1), rect(-3.87, -1.33, 1.33, 36.89, 'F.SilkS', 0.12),
      '  (fp_line (start 1.8 -1.33) (end 1.8 1.27) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
      rect(-4.31, -1.77, 1.77, 37.33, 'F.CrtYd', 0.05),
      '  (model "${KICAD10_3DMODEL_DIR}/Connector_PinSocket_2.54mm.3dshapes/PinSocket_2x15_P2.54mm_Vertical.step"\n'
      '    (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0)))']
write('Slot_Socket_2x15_P2.54mm_Vertical', 'Card slot: 2x15 female socket, 2.54 mm, vertical, numbered as KiCad\'s stock socket '
      '(pad 2 at -2.54 mm): on the back, pad 2 toward +x and pin 1 at the top, it mates pin for pin with a right-angle 2x15 '
      'header on a card standing in it', 'socket 2x15 slot card', 'through_hole', b)

# Radial PTC (Bourns MF-R series: MF-R010 / R020 / R050 / R110, 5.1 mm lead spacing, bodies 5-9 mm wide and
# 7.5-9 mm tall): two 1.0 mm holes 5.08 mm apart, the body outline drawn for the widest (MF-R110, 8.9 x 3.3 mm).
b = [text('reference', 'REF**', 0, -3.6, 'F.SilkS'), text('value', 'PTC_Radial', 0, 3.6, 'F.Fab'),
     tht(1, -2.54, 0, 1.8, 1.0), tht(2, 2.54, 0, 1.8, 1.0),
     rect(-4.5, -1.7, 4.5, 1.7, 'F.Fab', 0.1),
     '  (fp_line (start -4.6 -1.8) (end -3.6 -1.8) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
     '  (fp_line (start 3.6 -1.8) (end 4.6 -1.8) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
     '  (fp_line (start -4.6 1.8) (end -3.6 1.8) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
     '  (fp_line (start 3.6 1.8) (end 4.6 1.8) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
     rect(-4.85, -2.05, 4.85, 2.05, 'F.CrtYd', 0.05)]
write('PTC_Radial_P5.08mm', 'Radial PTC resettable fuse, 5.08 mm lead spacing (Bourns MF-R series or equal); outline for the MF-R110',
      'PTC polyfuse radial MF-R', 'through_hole', b)
print('footprints written')
