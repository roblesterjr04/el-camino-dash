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


# ESP32-WROVER-E (-N8R8): Espressif ESP32-WROVER-E / -IE datasheet v2.4, figure 12 (dimensions) and figure 15 (recommended
# PCB land pattern). 18.0 x 31.4 mm, antenna at the -Y end (the "antenna area", 6.22 mm deep). 38 castellated pins on a
# 1.27 mm pitch, 19 per side: 1-19 down the left side (pin 1 7.44 mm from the antenna end, pin 19 1.1 mm from the far
# end), 20-38 up the right side. Pads 1.5 x 0.9, their outer end 0.5 mm past the module edge (centres 17.5 mm apart).
# Pin 39 is the ground EPAD: 3 x 3 pads of 0.9 mm in a 3.7 mm square, centred 7.5 mm from the left edge and 16.16 mm from
# the far end. Here it is one 3.7 mm copper pad with the nine 0.9 mm paste openings, and no vias in it (no via in a pad
# on this board): it reaches the ground pours by its spokes. The keep-out (no copper, any layer) covers the antenna area.
M_W, M_L, ANT = 18.0, 31.4, 6.22
TOP = -M_L / 2
b = [text('reference', 'REF**', 0, TOP + 9.5, 'F.SilkS'), text('value', 'ESP32-WROVER-E', 0, 6.0, 'F.Fab')]
for k in range(19):
    b.append(smd(1 + k, -8.75, round(TOP + 7.44 + 1.27 * k, 3), 1.5, 0.9))
    b.append(smd(20 + k, 8.75, round(-TOP - 1.1 - 1.27 * k, 3), 1.5, 0.9))
ex, ey = -M_W / 2 + 7.5, -TOP - 16.16
b.append('  (pad "39" smd rect (at %g %g) (size 3.7 3.7) (layers "F.Cu" "F.Mask"))' % (ex, round(ey, 3)))
for gx in (-1.4, 0, 1.4):
    for gy in (-1.4, 0, 1.4):
        b.append('  (pad "" smd rect (at %g %g) (size 0.9 0.9) (layers "F.Paste"))' % (round(ex + gx, 3), round(ey + gy, 3)))
s = 0.12
xs, y1, y2 = M_W / 2 + 0.11, TOP - 0.11, -TOP + 0.11
b += [rect(-M_W / 2, TOP, M_W / 2, -TOP, 'F.Fab', 0.1),
      '  (fp_line (start %g %g) (end %g %g) (stroke (width 0.1) (type solid)) (layer "F.Fab"))' % (-M_W / 2, TOP + ANT, M_W / 2, TOP + ANT),
      '  (fp_line (start %g %g) (end %g %g) (stroke (width %g) (type solid)) (layer "F.SilkS"))' % (-xs, y1, xs, y1, s),
      '  (fp_line (start %g %g) (end %g %g) (stroke (width %g) (type solid)) (layer "F.SilkS"))' % (-xs, y2, xs, y2, s),
      '  (fp_line (start %g %g) (end %g %g) (stroke (width %g) (type solid)) (layer "F.SilkS"))' % (-xs, y1, -xs, TOP + 6.9, s),
      '  (fp_line (start %g %g) (end %g %g) (stroke (width %g) (type solid)) (layer "F.SilkS"))' % (xs, y1, xs, TOP + 6.9, s),
      '  (fp_poly (pts (xy -10.0 %g) (xy -10.4 %g) (xy -10.4 %g)) (stroke (width 0.12) (type solid)) (fill solid) (layer "F.SilkS"))' % (
          TOP + 7.44, TOP + 7.14, TOP + 7.74),
      rect(-9.75, TOP - 0.25, 9.75, -TOP + 0.25, 'F.CrtYd', 0.05),
      text('user', 'ANTENNA', 0, TOP + 3.1, 'F.Fab', 0.8),
      '  (zone (net 0) (net_name "") (layers "*.Cu") (hatch edge 0.5) (connect_pads (clearance 0)) (min_thickness 0.25)'
      ' (keepout (tracks not_allowed) (vias not_allowed) (pads not_allowed) (copperpour not_allowed) (footprints allowed))'
      ' (fill (thermal_gap 0.5) (thermal_bridge_width 0.5))'
      ' (polygon (pts (xy %g %g) (xy %g %g) (xy %g %g) (xy %g %g))))' % (-M_W / 2, TOP - 0.3, M_W / 2, TOP - 0.3, M_W / 2, TOP + ANT,
                                                                       -M_W / 2, TOP + ANT)]
write('ESP32-WROVER-E',
      'Espressif ESP32-WROVER-E module (classic ESP32, Bluetooth Classic + BLE, Wi-Fi, PCB antenna), 18.0 x 31.4 mm. Land pattern from the datasheet v2.4 (figure 15). Antenna end at -Y: keep copper away from it',
      'ESP32 WROVER-E module', 'smd', b)


def pad(num, x, y, size, drill):
    return ('  (pad "%s" thru_hole circle (at %g %g) (size %g %g) (drill %g) (layers "*.Cu" "*.Mask"))' %
            (num, x, y, size, size, drill))


def slot(num, x, y, w, h, dw, dh):
    return ('  (pad "%s" thru_hole oval (at %g %g) (size %g %g) (drill oval %g %g) (layers "*.Cu" "*.Mask"))' %
            (num, x, y, w, h, dw, dh))


# Kycon KLP42X-61-WW/RR: four RCA jacks (2 x 2), right angle, PC mount, snap-in tabs. From Kycon's KLP42X-61-xx/xx
# drawing (third-angle: the bottom view is not mirrored, so left / right are as seen from the plug side). Origin: centre
# of the block, on the rear face of its 1.0 mm front plate; the body (to 10.0 mm behind it) is at -y, the plate and the
# barrels (7.8 mm past the plate) at +y, so the board edge goes on y = 0. Pin depths on the drawing are from the plate's
# front face (1.0 mm in front of the origin). Columns 18 mm apart; each has three pins 4 mm apart (1.1 mm blades):
#   pad 1 sleeve, left column (x -9, 7.0 deep)      pad 4 sleeve, right column (x 9)
#   pad 2 tip, upper left (X, white, 11.5 deep)     pad 5 tip, upper right (X1, white), x 5
#   pad 3 tip, lower left (Z, red, 10.25 deep)      pad 6 tip, lower right (Z1, red), x 13
#   pads 7 / 8: the snap-in side tabs, left / right (2.4 mm wide, centred 7.5 deep, barb to 17.6 mm out; tied to ground)
b = [text('reference', 'REF**', 0, -13.0, 'F.SilkS'), text('value', 'KLP42X-61-WW/RR', 0, 4.0, 'F.Fab'),
     pad(1, -9.0, -6.0, 2.3, 1.4), pad(2, -13.0, -10.5, 2.3, 1.4), pad(3, -5.0, -9.25, 2.3, 1.4),
     pad(4, 9.0, -6.0, 2.3, 1.4), pad(5, 5.0, -10.5, 2.3, 1.4), pad(6, 13.0, -9.25, 2.3, 1.4),
     slot(7, -16.7, -6.5, 2.9, 4.0, 1.9, 3.0), slot(8, 16.7, -6.5, 2.9, 4.0, 1.9, 3.0),
     rect(-17.0, -10.0, 17.0, 0, 'F.Fab', 0.1), rect(-17.0, 0, 17.0, 1.0, 'F.Fab', 0.1),
     '  (fp_line (start -17.2 0) (end 17.2 0) (stroke (width 0.15) (type solid)) (layer "F.SilkS"))',
     rect(-17.2, -11.9, 17.2, -0.3, 'F.SilkS', 0.12),
     rect(-18.4, -12.2, 18.4, 0.4, 'F.CrtYd', 0.05),
     text('user', 'PLUG SIDE', 0, 2.6, 'F.Fab', 0.8),
     '  (model "${KIPRJMOD}/AudioCarrier.pretty/KLP42X61.wrl" (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0)))']
write('RCA_Kycon_KLP42X-61_RightAngle',
      'Kycon KLP42X-61, 4 x RCA jack (2 x 2), right angle PC mount. Pads from the KLP42X-61-xx/xx drawing. Origin at the plate rear face (put the board edge at y = 0), body at -y. Body 34 x 10 mm, 26.5 mm tall; the plate stands 1.0 mm and the barrels 8.8 mm past the edge',
      'RCA phono 2x2 right angle', 'through_hole', b)


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
    out += box(0, 10.0 / 2, 26.5 / 2, 34.0, 10.0, 26.5, '0.08 0.08 0.08')
    out += box(0, -0.5, 26.5 / 2, 34.0, 1.0, 26.5, '0.75 0.75 0.75')
    for x in (-9.0, 9.0):
        for zi, z in enumerate((20.7, 6.7)):
            out += cyl(x, -1.0 - 0.7, z, 5.1, 1.4, '0.6 0.6 0.6')
            out += cyl(x, -1.0 - 3.9, z, 4.15, 7.8, '0.9 0.9 0.9' if zi == 0 else '0.75 0.05 0.05')
    open(os.path.join(OUT, 'KLP42X61.wrl'), 'w').write(out)


_wrl()
print('footprints written')
