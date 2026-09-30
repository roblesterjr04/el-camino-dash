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


def smd(num, x, y, w, h, extra=''):
    return ('  (pad "%s" smd rect (at %g %g) (size %g %g) (layers "F.Cu" "F.Paste" "F.Mask")%s)' % (num, x, y, w, h, extra))


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
    b.append(smd(num, x, y, 0.7, 0.7, ' (zone_connect 2)'))     # solid: too small and too near the edge for spokes
# all ground pads are one net inside the module: tell KiCad, so the corner pads need no copper of their own
b.append('  (jumper_pad_groups ("%s"))' % '" "'.join(str(n) for n in [1, 2, 11, 14] + list(range(36, 54))))
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


def pad(num, x, y, size, drill):
    return ('  (pad "%s" thru_hole circle (at %g %g) (size %g %g) (drill %g) (layers "*.Cu" "*.Mask"))' %
            (num, x, y, size, size, drill))


def slot(num, x, y, w, h, dw, dh):
    return ('  (pad "%s" thru_hole oval (at %g %g) (size %g %g) (drill oval %g %g) (layers "*.Cu" "*.Mask"))' %
            (num, x, y, w, h, dw, dh))


# Switchcraft PJRAS2X2S01X: four RCA jacks (2 x 2), right angle, PC mount, screw mounting. Hole pattern from Switchcraft's
# customer drawing PJRAS2X2S__X (rev B, "P.C. board layout"). Origin: centre of the block, on the rear face of its front
# plate (the dashed line on the drawing); the body is at -y, the jack barrels (8.2 mm) and the plate (1.5 mm) at +y,
# so the board edge goes on y = 0. Seen from the front, as the plug side: left column x < 0.
#   pad 1 tip, upper left  (insert A, white)      pad 2 tip, lower left  (insert C, red)
#   pad 3 tip, upper right (insert B, white)      pad 4 tip, lower right (insert D, red)
#   pad 5 sleeve, left column                     pad 6 sleeve, right column
#   pads 7 / 8: front-plate tabs, left / right (tied to the sleeve, i.e. ground)
b = [text('reference', 'REF**', 0, -16.5, 'F.SilkS'), text('value', 'PJRAS2X2S01X', 0, 4.0, 'F.Fab'),
     pad(2, -10.75, -12.5, 2.4, 1.3), pad(1, -3.25, -12.5, 2.4, 1.3),
     pad(4, 3.25, -12.5, 2.4, 1.3), pad(3, 10.75, -12.5, 2.4, 1.3),
     pad(5, -3.25, -5.25, 2.6, 1.5), pad(6, 10.75, -5.25, 2.6, 1.5),
     slot(7, -13.15, -8.75, 2.6, 3.8, 1.7, 3.0), slot(8, 13.15, -8.75, 2.6, 3.8, 1.7, 3.0),
     rect(-13.5, -13.7, 13.5, 0, 'F.Fab', 0.1), rect(-14.0, 0, 14.0, 1.5, 'F.Fab', 0.1),
     '  (fp_line (start -14.2 0) (end 14.2 0) (stroke (width 0.15) (type solid)) (layer "F.SilkS"))',
     rect(-13.7, -14.0, 13.7, -0.3, 'F.SilkS', 0.12),
     rect(-14.4, -14.3, 14.4, 0.4, 'F.CrtYd', 0.05),
     text('user', 'PLUG SIDE', 0, 2.6, 'F.Fab', 0.8),
     '  (model "${KIPRJMOD}/AudioCarrier.pretty/PJRAS2X2.wrl" (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0)))']
write('RCA_Switchcraft_PJRAS2X2_RightAngle',
      'Switchcraft PJRAS2X2S01X, 4 x RCA jack (2 x 2), right angle PC mount. Pads from the customer drawing. Origin at the plate rear face (put the board edge at y = 0), body at -y. Body 27 x 13.7 mm, 28.5 mm tall; the barrels stand 8.2 mm and the plate 1.5 mm past the edge',
      'RCA phono 2x2 right angle', 'through_hole', b)


# Tinkersphere TS3328 "1/8 in (3.5 mm) audio jack, PCB mount": 5 flat pins (1.5 x 0.8 blades) and two round posts, right
# angle. Hole pattern from the drawing on its product page (photo 3: "P.C.B LAYOUT, TOP VIEW"). Origin: the middle of the
# body's front face; the body runs to +x (12 mm) and the barrel (nose, 6 mm dia, 2.5 mm long) stands out at -x, so the board
# edge goes on x = 0. Pad names are the drawing's pin numbers; the two "4" pads are one contact with two tabs.
def blade(num, x, y, along_x=True):
    w, h = (2.6, 1.9) if along_x else (1.9, 2.6)
    dw, dh = (1.9, 1.1) if along_x else (1.1, 1.9)
    return ('  (pad "%s" thru_hole oval (at %g %g) (size %g %g) (drill oval %g %g) (layers "*.Cu" "*.Mask"))' %
            (num, x, y, w, h, dw, dh))


def peg(x, y):
    return '  (pad "" np_thru_hole circle (at %g %g) (size 1.6 1.6) (drill 1.6) (layers "*.Cu" "*.Mask"))' % (x, y)


b = [text('reference', 'REF**', 6.0, -6.0, 'F.SilkS'), text('value', 'TS3328_3.5mm_jack', 6.0, 6.0, 'F.Fab'),
     blade(4, 3.5, -3.3), blade(4, 3.5, 3.3), blade(2, 11.5, -2.8), blade(3, 11.5, 2.8), blade(5, 12.0, 0, False),
     peg(2.0, 0), peg(9.0, 0),
     rect(0, -3.5, 12.0, 3.5, 'F.Fab', 0.1), rect(-2.5, -3.0, 0, 3.0, 'F.Fab', 0.1),
     '  (fp_line (start 0 -3.9) (end 0 3.9) (stroke (width 0.15) (type solid)) (layer "F.SilkS"))',
     rect(-0.2, -4.6, 12.6, 4.6, 'F.CrtYd', 0.05)]
write('Jack_3.5mm_TS3328_Horizontal',
      'Tinkersphere TS3328 3.5 mm audio jack, right angle, 5 flat pins + 2 posts. From the vendor drawing. Origin at the middle of the front face (board edge at x = 0), body at +x 12 x 7 mm x 6 mm high, barrel 2.5 mm out at -x. Pin functions are not documented on the page: verify (see the carrier README)',
      'audio jack 3.5mm TS3328', 'through_hole', b)


# a plain 3D model of the block for the viewer (VRML: 1 unit = 2.54 mm, footprint +y is VRML -y): body, plate, four barrels
def _wrl():
    k = 1 / 2.54

    def box(cx, cy, cz, sx, sy, sz, col):
        return ('Transform { translation %g %g %g children Shape { appearance Appearance { material Material { diffuseColor %s } } '
                'geometry Box { size %g %g %g } } }\n' % (cx * k, cy * k, cz * k, col, sx * k, sy * k, sz * k))

    def cyl(cx, cy, cz, r, h, col):     # cylinder axis along y (toward the plug side)
        return ('Transform { translation %g %g %g children Shape { appearance Appearance { material Material { diffuseColor %s } } '
                'geometry Cylinder { radius %g height %g } } }\n' % (cx * k, cy * k, cz * k, col, r * k, h * k))
    out = '#VRML V2.0 utf8\n'
    out += box(0, 13.7 / 2, 28.5 / 2, 27.0, 13.7, 28.5, '0.08 0.08 0.08')
    out += box(0, -0.75, 28.5 / 2, 28.0, 1.5, 28.5, '0.75 0.75 0.75')
    for x in (-7.0, 7.0):
        for zi, z in enumerate((14.25 + 7.0, 14.25 - 7.0)):
            out += cyl(x, -1.5 - 4.1, z, 5.25, 8.2, '0.6 0.6 0.6')
            out += cyl(x, -1.5 - 3.7, z, 3.0, 7.4, '0.9 0.9 0.9' if zi == 0 else '0.75 0.05 0.05')
    open(os.path.join(OUT, 'PJRAS2X2.wrl'), 'w').write(out)


_wrl()
print('footprints written')
