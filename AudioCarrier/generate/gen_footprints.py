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


# ESP32-DevKitC-32 in two 1x19 sockets: rows 25.4 mm apart, 2.54 pitch, USB end at +Y.
b = [text('reference', 'REF**', 12.7, -6.5, 'F.SilkS'), text('value', 'ESP32-DevKitC-32_Sockets', 12.7, 54, 'F.Fab')]
for i in range(19):
    b.append(pad(i + 1, 0, i * 2.54, 1.7, 1.0, 'rect' if i == 0 else 'oval'))
    b.append(pad(i + 20, 25.4, i * 2.54, 1.7, 1.0, 'rect' if i == 0 else 'oval'))
b += [rect(-1.3, -4.5, 26.7, 50.5, 'F.SilkS', 0.15),
      rect(-1.55, -4.75, 26.95, 50.75, 'F.CrtYd', 0.05),
      text('user', 'antenna end', 12.7, -3, 'F.SilkS', 0.8), text('user', 'USB end', 12.7, 49.5, 'F.SilkS', 0.8),
      '  (fp_line (start -2 -1) (end -2 1) (stroke (width 0.15) (type solid)) (layer "F.SilkS"))']
write('ESP32-DevKitC-32_Sockets',
      'ESP32-DevKitC-32 (Espressif V4 / HiLetgo, 2x19) in two 1x19 2.54 mm sockets, rows 25.4 mm apart. Pins 1-19 left column, 20-38 right column, pin 1 at the antenna end (top), USB at the bottom. Check the row spacing of the board on hand',
      'ESP32 DevKitC', b)

# RCA jack: pad geometry from the RCJ-022 footprint used for J11 / J12 on the DigiDash board.
# Tip = pad 1, sleeve = pad 2 (twice). Body outline is the one from that footprint.
b = [text('reference', 'REF**', 0, -7.5, 'F.SilkS'), text('value', 'RCA_Jack_RCJ-022', 0, 7.5, 'F.Fab'),
     pad(1, 0, 1.6, 2.55, 1.7), pad(2, -5.15, 0, 3, 2), pad(2, 5.15, 0, 3, 2),
     rect(-7.4, -5.24, 6.95, 5.24, 'F.SilkS', 0.15), rect(-7.65, -5.49, 7.2, 5.49, 'F.CrtYd', 0.05),
     text('user', 'TIP', 0, 3.6, 'F.SilkS', 0.8)]
write('RCA_Jack_RCJ-022',
      'Single RCA jack, CUI RCJ-022 pad pattern as on the DigiDash board (J11 / J12). Pad 1 = tip, pad 2 = sleeve. Verify the mechanical outline and the plug direction against the datasheet before ordering',
      'RCA CUI RCJ-022', b)
print('footprints written')
