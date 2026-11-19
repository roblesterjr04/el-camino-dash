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


def pad(num, x, y, size, drill, shape='circle'):
    return ('  (pad "%s" thru_hole %s (at %g %g) (size %g %g) (drill %g) (layers "*.Cu" "*.Mask"))' %
            (num, shape, x, y, size, size, drill))


def line(x1, y1, x2, y2, layer, w):
    return '  (fp_line (start %g %g) (end %g %g) (stroke (width %g) (type solid)) (layer "%s"))' % (x1, y1, x2, y2, w, layer)


# ESP32-DevKitC-VE (ESP32-WROVER-E) in two 1 x 19 female sockets. Pin positions and numbering as Espressif's own
# footprint (espressif/kicad-libraries, Espressif.pretty/ESP32-DevKitC): origin at pin 1 (3V3), pins 1-19 (the
# DevKitC's J2: 3V3, EN, VP, VN, 34, 35, 32, 33, 25, 26, 27, 14, 12, GND, 13, D2, D3, CMD, 5V) down the x = 0 row
# from the antenna end, pins 20-38 (J3 bottom to top: CLK, D0, D1, 15, 2, 0, 4, 16, 17, 5, 18, 19, GND, 21, RX, TX, 22,
# 23, GND) up the x = 25.4 row. The rows are 25.4 mm apart. 1.0 mm holes for the sockets' 0.64 mm square pins.
# The module and its PCB antenna stand past the pin 1 end (-y): DK_ANT is how far the module can reach from pin 1
# (WROVER-E on the -VE: measure on the board; 13 mm is the generous value used here). The keep-out under it is no
# copper on any layer. Courtyards: the two socket strips only, so low SMD parts can sit under the DevKit (it stands
# 8.5 mm up on the sockets).
DK_ROW, DK_ANT, DK_END = 25.4, 13.0, 47.3
b = [text('reference', 'REF**', DK_ROW / 2, -3.0, 'F.SilkS'), text('value', 'ESP32-DevKitC-VE', DK_ROW / 2, 30.0, 'F.Fab')]
for k in range(19):
    b.append(pad(1 + k, 0, round(2.54 * k, 3), 1.7, 1.0, 'rect' if k == 0 else 'circle'))
    b.append(pad(20 + k, DK_ROW, round(45.72 - 2.54 * k, 3), 1.7, 1.0))
for x in (0, DK_ROW):
    b += [rect(x - 1.27, -1.27, x + 1.27, 46.99, 'F.SilkS', 0.12), rect(x - 1.27, -1.27, x + 1.27, 46.99, 'F.Fab', 0.1),
          rect(x - 1.52, -1.52, x + 1.52, 47.24, 'F.CrtYd', 0.05)]
mx1, mx2 = DK_ROW / 2 - 9.0, DK_ROW / 2 + 9.0          # the module: 18 mm wide, centred between the rows
b += [rect(-1.5, -1.1, DK_ROW + 1.5, DK_END, 'F.Fab', 0.1),
      rect(mx1, -DK_ANT, mx2, -1.1, 'F.Fab', 0.1),
      line(-0.6, -2.0, 0.6, -2.0, 'F.SilkS', 0.15),
      text('user', 'ANTENNA', DK_ROW / 2, -DK_ANT / 2 - 1.0, 'F.Fab', 0.8),
      text('user', 'USB', DK_ROW / 2, DK_END - 1.5, 'F.Fab', 0.8),
      '  (zone (net 0) (net_name "") (layers "*.Cu") (hatch edge 0.5) (connect_pads (clearance 0)) (min_thickness 0.25)'
      ' (keepout (tracks not_allowed) (vias not_allowed) (pads not_allowed) (copperpour not_allowed) (footprints allowed))'
      ' (fill (thermal_gap 0.5) (thermal_bridge_width 0.5))'
      ' (polygon (pts (xy %g %g) (xy %g %g) (xy %g %g) (xy %g %g))))' % (mx1, -DK_ANT - 0.5, mx2, -DK_ANT - 0.5, mx2, -1.6, mx1, -1.6)]
write('ESP32-DevKitC_Socket',
      'Espressif ESP32-DevKitC-VE (ESP32-WROVER-E) in two 1 x 19 2.54 mm female sockets, rows 25.4 mm apart (pin numbers as Espressif.pretty/ESP32-DevKitC). The module and antenna reach past pin 1: keep copper away',
      'ESP32 DevKitC socket', 'through_hole', b)


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
