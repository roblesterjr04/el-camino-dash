"""Turn-signal LED puck: a 23.2 x 9.3 mm disc that holds a 5 mm LED, its wires leaving through the side.

z = 0 is the back face, z = THICK the front. The LED goes in from the back, its flange sits on a shoulder, and the dome
points out the front. Under the flange, a slot open to the back face takes the bent legs and the two wires out to the
side wall.

    $PY Housing/turn_signal_puck.py      # writes turn_signal_puck.step and .stl next to this file (needs build123d)
"""
from pathlib import Path

from build123d import Axis, Box, Cylinder, Pos, chamfer, export_step, export_stl

DIA = 23.2          # puck
THICK = 9.3
LED_BORE = 5.2      # 5 mm LED body, with print clearance
FLANGE_BORE = 6.2   # its 5.8 mm flange
FLANGE_T = 1.0
WIRE_DEPTH = 2.5    # leg bends and wires, below the flange
WIRE_W = 4.0        # two wires side by side, with heatshrink
LED_H = 8.6         # base of the flange to the top of the dome
EDGE = 0.5          # chamfer on the outer edges

seat = WIRE_DEPTH + FLANGE_T  # z of the shoulder the flange sits on

puck = Pos(0, 0, THICK / 2) * Cylinder(DIA / 2, THICK)
puck = chamfer(puck.edges().filter_by(Axis.Z, reverse=True), EDGE)
puck -= Pos(0, 0, THICK / 2) * Cylinder(LED_BORE / 2, THICK)
puck -= Pos(0, 0, seat / 2) * Cylinder(FLANGE_BORE / 2, seat)
puck -= Pos(DIA / 4, 0, WIRE_DEPTH / 2) * Box(DIA / 2 + 1, WIRE_W, WIRE_DEPTH)

if __name__ == '__main__':
    out = Path(__file__).with_suffix('')
    export_step(puck, f'{out}.step')
    export_stl(puck, f'{out}.stl')
    bb = puck.bounding_box()
    print(f'{bb.size.X:.2f} x {bb.size.Y:.2f} x {bb.size.Z:.2f} mm, volume {puck.volume:.0f} mm3; '
          f'LED dome stands {WIRE_DEPTH + LED_H - THICK:.1f} mm past the front face')
