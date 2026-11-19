# Housing

The cluster housing and one small printed part. The numbers the housing is built around (board, screens, cards,
connector openings) are in [`docs/hardware/housing_envelope.md`](../docs/hardware/housing_envelope.md).

**Status:** housing model v6 (2026-11-17), checked for collisions in CAD against the board model, the cards and the
camera module ([`housing_envelope.md`](../docs/hardware/housing_envelope.md#housing-model-onshape-cluster-housing-fs-v6-2026-11-17)).
Printed or fitted: not stated. The camera module's mounting values are guesses until the module is in hand.

| File | What |
| --- | --- |
| `cluster_housing.fs` | Onshape FeatureScript, feature "Gauge cluster housing": the front housing (screen bezel, two halves), the two-piece rear shell with the card pod, camera nacelle and port well, and the card bay hatch. The card in each slot is a feature parameter |
| `turn_signal_puck.py` | A 23.2 × 9.3 mm disc that holds a 5 mm LED for a turn-signal indicator, its wires leaving through the side. Written with [build123d](https://github.com/gumyr/build123d) |
| `turn_signal_puck.step`, `.stl` | The puck, written by the script |

## The housing FeatureScript

The `.fs` file is a copy of a Feature Studio in Onshape: edit it there and copy it back here. To use it:

1. Create a Feature Studio in an Onshape document and paste the file in.
2. Import a main board STEP (`DigiDash3/boardmodel.step`; `DigiDash2/boardmodel.step` has the same outline) into its
   own Part Studio.
3. Replace the `BoardV4::import(...)` line at the top with an import of that Part Studio. The line in this file points
   into the original document and will not resolve anywhere else.
4. Add the "Gauge cluster housing" feature to a Part Studio. The feature instance stores every parameter value, so a
   default changed in the code only takes effect once the instance's value is changed too.

Coordinates, as the file's header says: X right, Z up, Y the depth (the screens' front glass at Y = 0), with the
center screen's centre at the origin.

## The puck

```bash
$PY Housing/turn_signal_puck.py      # writes turn_signal_puck.step and .stl next to the script; needs build123d
```
