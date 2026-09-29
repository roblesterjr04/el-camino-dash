#!/usr/bin/env python3
"""Write the footprint the KiCad stock libraries do not have into AudioCarrier.pretty."""
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'AudioCarrier.pretty')
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


def smd(num, x, y, w, h):
    return ('  (pad "%s" smd rect (at %g %g) (size %g %g) (layers "F.Cu" "F.Paste" "F.Mask"))' % (num, x, y, w, h))


# ESP32-PICO-MINI-02: Espressif datasheet v1.7, figures 3 (pin layout) and 14 (recommended land pattern).
# 13.2 x 16.6 mm body, antenna at the -Y end (y -11 ... -5.6). 0.4 x 0.8 pads on a 0.8 mm pitch: 11 per side
# (1-11 left, top to bottom; 25-35 right, bottom to top), 13 per end row (12-24 bottom, left to right; 36-48 top,
# right to left), 0.7 mm corner pads 50-53 and a 3 x 3 grid of 1.45 mm ground pads (all pin 49).
# Same land pattern as KiCad's ESP32-C6-MINI-1, with a courtyard that covers the module only; the antenna
# keep-out is a rule area over the antenna end, and the module sits with that end on the board edge.
b = [text('reference', 'REF**', 0, 6.9, 'F.SilkS'), text('value', 'ESP32-PICO-MINI-02', 0, 8.2, 'F.Fab')]
for i in range(11):
    b.append(smd(1 + i, -5.9, -4 + 0.8 * i, 0.8, 0.4))
    b.append(smd(35 - i, 5.9, -4 + 0.8 * i, 0.8, 0.4))
for i in range(13):
    b.append(smd(12 + i, -4.8 + 0.8 * i, 4.9, 0.4, 0.8))
    b.append(smd(48 - i, -4.8 + 0.8 * i, -4.9, 0.4, 0.8))
for gx in (-1.975, 0, 1.975):
    for gy in (-1.975, 0, 1.975):
        b.append(smd(49, gx, gy, 1.45, 1.45))
for num, x, y in ((50, 5.95, -4.95), (51, 5.95, 4.95), (52, -5.95, 4.95), (53, -5.95, -4.95)):
    b.append(smd(num, x, y, 0.7, 0.7))
b += [rect(-6.6, -11, 6.6, 5.6, 'F.Fab', 0.1),
      '  (fp_line (start -6.6 -5.6) (end 6.6 -5.6) (stroke (width 0.1) (type solid)) (layer "F.Fab"))',
      rect(-6.75, -11.15, 6.75, 5.75, 'F.SilkS', 0.12),
      '  (fp_poly (pts (xy -7.1 -4) (xy -7.44 -3.76) (xy -7.44 -4.24)) (stroke (width 0.12) (type solid)) (fill solid) (layer "F.SilkS"))',
      rect(-6.85, -11.25, 6.85, 5.85, 'F.CrtYd', 0.05),
      text('user', 'ANTENNA', 0, -8.3, 'F.Fab', 0.8),
      '  (zone (net 0) (net_name "") (layers "*.Cu") (hatch edge 0.5) (connect_pads (clearance 0)) (min_thickness 0.25)'
      ' (keepout (tracks not_allowed) (vias not_allowed) (pads not_allowed) (copperpour not_allowed) (footprints allowed))'
      ' (fill (thermal_gap 0.5) (thermal_bridge_width 0.5))'
      ' (polygon (pts (xy -6.6 -11.3) (xy 6.6 -11.3) (xy 6.6 -5.6) (xy -6.6 -5.6))))']
write('ESP32-PICO-MINI-02',
      'Espressif ESP32-PICO-MINI-02 module (classic ESP32, Bluetooth Classic + BLE, Wi-Fi), 13.2 x 16.6 mm. Land pattern from the datasheet (figure 14). Antenna end at -Y, keep copper away from it',
      'ESP32 PICO MINI module', 'smd', b)
print('footprints written')
