# Housing envelope: main board, screens and slot cards

A reference for designing the dash housing. It covers where the DigiDash2 main board, its three screens and its
expansion cards sit, and what has to reach outside. All numbers come from the generated boards (`DigiDash2/`, the card
directories) and Waveshare's screen drawings, as of 2026-10-19. Heights come from KiCad's 3D models unless marked ≈.

## Coordinate frame

Every position below uses **one frame, fixed to the main board**:

- **x:** along the board, **to the right as seen from the front** (the screen side, the driver's view). x = 0 is
  the board's left edge, x = 329 its right edge.
- **y:** **down** the board. y = 0 is the top edge, y = 113 the bottom edge.
- **z:** **rearward, through the board**, away from the driver. z = 0 is the **back surface** of the main board, so
  the front surface is at z = −1.6. Cards and back-side parts are at z > 0; the screens are at z < −1.6.

**KiCad sheet coordinates** (what KiCad shows for the opened board) are these plus (20, 20): the board's top-left
corner sits at (20, 20) on the sheet, so board (110.5, 5.25) is sheet (130.5, 25.25).

Seen from the back, x runs right to left. Slot 1 is at the **left** end seen from the front, which is the right end
seen from the back.

```
 side view (looking along +x, from the left end)            top view (looking down, -y), front at the bottom

   z ->  rear                                                  rear (+z)
   |                                                           ^
   |   card 60 tall          top edge z 68.5                   |  card  card  card  card  card  card
   |   +--------+  <- connectors face +z                       |   |     |     |     |     |     |      ~68.5
   |   |        |                                              |   |     |     |     |     |     |
   |   |  card  |                                              |   |     |     |     |     |     |
   |   +--------+  bottom edge z 8.5                           |   ^8.5 mm sockets
   |    socket 8.5                                       ======+==========================================  board (z 0)
 ==+========== main board, back at z 0                         |   ( screens on the front, z < -1.6 )
   |  screens on the front                                     +--> x (front view's right)
```

## Main board

| Item | Value |
| --- | --- |
| Outline | **329.0 × 113.0 mm**, 1.6 mm thick, rectangular, square corners |
| Front (z < −1.6) | The three screen headers and the three screens. Nothing else |
| Back (z > 0) | Every other part, the six card sockets and the cards |
| Board mounting holes | **H1–H4, M3** (3.2 mm, plated, tied to GND) at **(110.5, 5.25), (218.5, 5.25), (124.5, 107.75), (204.5, 107.75)**. They sit in the gaps between the screens, above and below where they meet, and each pair is mirrored about x 164.5. The housing's posts land on the front and the M3 screws go in from the back. A 7 mm circle is kept clear on both sides. The posts in the housing's Onshape model are at these spots (2026-10-19) |
| Screen holes | **H5–H16, M4 clearance** (4.3 mm, no copper). The screens bolt to the board through these (see below) |

### Screens (front)

All three screens mount face out on the front of the main board. Their carrier's 2×20 header plugs into J5 / J1 / J4,
and four M4 screws from the back go into each screen's threaded bosses.

| Screen | Model | Display centre (x, y) | Outer Ø | Lens Ø | M4 holes (x, y) |
| --- | --- | --- | --- | --- | --- |
| Left (J5) | Waveshare ESP32-P4-WIFI6-Touch-LCD-**4C** | (42.58, 58.355) | 126.0 | 123.5 | (5.08, 20.855), (80.08, 20.855), (5.08, 95.855), (80.08, 95.855) |
| Center (J1) | Waveshare ESP32-P4-WIFI6-Touch-LCD-**3.4C** | (164.5, 50.735) | 115.0 | ≈114 | (127, 13.235), (202, 13.235), (127, 88.235), (202, 88.235) |
| Right (J4) | Waveshare ESP32-P4-WIFI6-Touch-LCD-**4C** | (286.42, 58.355) | 126.0 | 123.5 | (248.92, 20.855), (323.92, 20.855), (248.92, 95.855), (323.92, 95.855) |

The screens moved 12 mm toward the top edge on 2026-10-19, at the housing's request. The positions above are the
current ones.

- **Hole pattern:** each screen's holes are a **75 × 75 mm square** centred on its display. The screen centre is
  34.384 mm right of and 25.745 mm above its header's pin 1.
- **Overhang:** the screens overhang the board. The left screen reaches x −20.4 and the right one x 349.4 (20.4 mm past
  each end). The side screens reach from y −4.6 to 121.4 (4.6 mm above the top edge, 8.4 mm below the bottom), and the
  center screen from y −6.8 to 108.2 (6.8 mm above the top edge).
- **Spacing:** the left and center screens' rims are about 1.7 mm apart, and the same on the right. The bezel web
  between them can't be thicker than that unless it sits in front of the glass.
- **Screen depth:** about 15.1 mm overall, measured from the glass front (6.0 mm display plus the carrier). The gap
  between the carrier and the main board's front surface, which sets the standoff length, **has not been measured**.
  It depends on the mated height of the 2×20 header and socket (the board's 2×20 header model stands 8.6 mm).
  **Measure this on the bench** before fixing the glass plane.
- **Screws:** M4 screw heads sit on the back (z > 0). The board keeps 4.5 mm around each hole clear of parts on both
  sides. The heads of H5, H6, H9 and H11 are under card envelopes, below the cards' 8.5 mm bottom edge.

### Back side: parts outside the cards

The tallest items on the back, other than the cards. Where it matters, the cable direction is given.

| Ref | What | x | y | Height (z) | Cable / access |
| --- | --- | --- | --- | --- | --- |
| J16 | CAN tap (1 × 3 header, for a USB-CAN adapter) | 183.1–191.8 | 77.2–80.8 | 8.6 | Service only, from the rear |
| J27 | Sensor serial jumper (1 × 2 header) | 91.7–97.8 | 4.2–7.8 | 8.6 | A wire from the sensor card's J8 |
| SW1 | CAN termination slide switch | 65.1–74.6 | 87.4–92.2 | low | **Under slot 3's card**: reachable only with that card out |
| J31–J36 | Card sockets | see [Slots](#slots) | 33.4–72.6 | 8.5 | — |

### Power section and rear connector group (back side)

East of the card cage, the back has two zones. The housing can step down from the deep card shell over the low power
section to a shallower connector shell at the right end.

**Power section** (x ≈ 155–240, y ≈ 39–63): the 5 V buck and its parts, and the switched-12 V FET. It needs **no
openings**. Its tallest parts are two 10 × 10.5 electrolytics (10.5 mm): C1 at x 159.6–173.2, y 41.4–52.4, and C18 at
x 234.4–248.0, y 68.1–79.2. The inductor L1 stands 4.7 mm, and the rest is under 3 mm.

**Rear connector group** (x 232.0–308.5, y 12.6–67.7): **every cable connector is vertical on the back, in one
group**, so every cable plugs in from the rear (+z) and nothing enters through the board's edges. The housing needs
no clearance past the top and bottom edges for plugs. It does need a rear opening, or one cable entry, over the
group, and room behind it for the mated plugs and their wire bends: allow about 30–40 mm behind the board. There
are 5–8 mm between plugs, for fingers and the Micro-Fit latches.

| Row | Ref | What | Part (all vertical, through-hole) | x | y | Height (z) |
| --- | --- | --- | --- | --- | --- | --- |
| Top | J25 | Trunk port (CAN + switched 12 V to the rear of the car) | Molex Micro-Fit 3.0 4-pin, 43650-0415 | 257.6–274.4 | 12.6–19.4 | ≈10 |
| Top | J6 | Serial port | JST PH 4-pin, B4B-PH-K | 279.5–290.5 | 13.2–18.8 | 6.0 |
| Top | J24 | Cable port 4 | JST PH 4-pin, B4B-PH-K | 297.5–308.5 | 13.2–18.8 | 6.0 |
| 2nd | J7 | **Vehicle power in** (battery, ground, ignition, dash lights) | Molex Micro-Fit 3.0 6-pin, 43650-0615 | 232.0–254.8 | 27.6–34.4 | ≈10 |
| 2nd | J17 | Cable port 1 (CAN + 5 V to the console and other nodes) | JST PH 4-pin, B4B-PH-K | 260.5–271.5 | 28.2–33.8 | 6.0 |
| 2nd | J22 | Cable port 2 | JST PH 4-pin, B4B-PH-K | 278.5–289.5 | 28.2–33.8 | 6.0 |
| 2nd | J23 | Cable port 3 | JST PH 4-pin, B4B-PH-K | 296.5–307.5 | 28.2–33.8 | 6.0 |
| 3rd | Q2 | Vehicle power reverse-polarity FET (not a connector) | TO-252 | 237.8–249.0 | 41.5–48.7 | low |
| 3rd | J11 | Backup camera video | RCA jack, face mount (Same Sky RCJ-02x), pin 1 at (263, 45) | 256.0–270.0 | 39.7–50.3 | **22.1** |
| 3rd | J9 | Backup camera power / reverse | JST PH 4-pin, B4B-PH-K | 279.5–290.5 | 42.2–47.8 | 6.0 |
| Bottom | Q6 | AUX reverse-polarity FET (not a connector) | TO-252 | 231.4–242.6 | 60.5–67.7 | low |
| Bottom | J13 | **AUX 12 V in** | Molex Mini-Fit Jr 2-pin, 5566-02A | 246.5–258.5 | 60.8–67.2 | ≈13 |
| Bottom | F1 | **AUX 5 A blade fuse** (mini), Keystone 3568 holder | — | 263.7–280.3 | 60.4–67.6 | 7.4 holder, ≈20 with the fuse. **User-replaceable** from the rear |

The cable plugs are unchanged from the side-entry versions: JST PH plugs are PHR-4, and the Micro-Fit and Mini-Fit
plugs are the same series. **JST PH has only a friction lock**, so the housing should clamp or tie the cables close to
the group: in a car a vertical plug pulls straight out toward the rear.

## Slots

Six 2 × 15 female sockets (2.54 mm pitch, 8.5 mm tall) on the back, in a row on the left, **25.4 mm apart**.
Each one's pins run along y, from y 35.22 to 70.78 (centre y 53).

| Slot | Socket | Pin rows at x | Slot centre x |
| --- | --- | --- | --- |
| 1 | J31 | 18.73 / 21.27 | 20.0 |
| 2 | J32 | 44.13 / 46.67 | 45.4 |
| 3 | J33 | 69.53 / 72.07 | 70.8 |
| 4 | J34 | 94.93 / 97.47 | 96.2 |
| 5 | J35 | 120.33 / 122.87 | 121.6 |
| 6 | J36 | 145.73 / 148.27 | 147.0 |

To the right of slot 6 (x > about 154) the back carries no cards: the video, power and harness parts, then empty
ground plane from about x 250 to the right end.

## Cards

Every card is the same shape: **80 × 60 mm, 1.6 mm thick**. It stands **on edge, perpendicular to the main board**,
like a PC expansion card, with a right-angle 2 × 15 header on its bottom edge plugged into a slot.

### Where a card sits (slot centre c)

| | Value |
| --- | --- |
| Card plane | Parallel to the y–z plane (vertical, running front to back) |
| Along y | **y 13 to 93**: 80 mm, centred on the socket |
| Along z | **Bottom edge at z ≈ 8.5** (it rests on the socket), **top edge at z ≈ 68.5** |
| Along x (card thickness) | **Parts face at x = c + 2.54, back face at x = c + 4.14** (≈ ±0.3, from the header's pin rows) |
| Parts side | Faces **−x**, toward slot 1 / the board's left end. **Up to 20 mm** tall, so parts reach x ≥ c − 17.46 |
| Back side | Faces +x. Up to 2.5 mm including solder tails, so it reaches x ≤ c + 6.64 |
| Between cards | Neighbouring envelopes leave about 1.3 mm |
| Slot 1 exception | Slot 1's parts side faces off the end of the row, so its parts height has **no limit**. The audio card uses this |

The **per-slot card envelope**, the box each card may fill, including connectors that stand past the card edges
(listed per card below):

| Slot | x (parts side → back side) | Card body x | y | z |
| --- | --- | --- | --- | --- |
| 1 | −6.0 (audio card RCA block) → 26.64 | 22.54–24.14 | 13–93 | 8.5–68.5 |
| 2 | 27.94 → 52.04 | 47.94–49.54 | 13–93 | 8.5–68.5 |
| 3 | 53.34 → 77.44 | 73.34–74.94 | 13–93 | 8.5–68.5 |
| 4 | 78.74 → 102.84 | 98.74–100.34 | 13–93 | 8.5–68.5 |
| 5 | 104.14 → 128.24 | 124.14–125.74 | 13–93 | 8.5–68.5 |
| 6 | 129.54 → 153.64 | 149.54–151.14 | 13–93 | 8.5–68.5 |

- **Slot 1 reaches past the board:** the audio card's RCA block stands 28.5 mm off its card, to x ≈ −6, which is
  6 mm past the main board's left edge.
- **Under the cards** (z 0 to 8.5), the main board keeps parts under about 7 mm.

### Card edges, in the board frame

A card's own drawings show its parts side with its top edge up. In that view:

- The card's **top edge** is at z 68.5, and connectors there face rearward (+z).
- The card's **left edge** (card x = 0) is at **y 93**, the end nearer the main board's bottom edge.
- The card's **right edge** (card x = 80) is at **y 13**.
- A point at card x = u is at board **y = 93 − u**. A part that stands h mm off the card's parts face reaches
  **x = c + 2.54 − h**.

**Left-edge programming headers:** most cards carry right-angle programming and debug headers (SWD, serial, USB-C) on
their left edge. Their pins stand about 6.5 mm past it, to **y ≈ 99.5**, between z ≈ 20 and 58. Leave room there to
plug in a programmer with the card in place, or accept that a card comes out to be flashed.

**Card retention:** the cards are held only by their 30-pin socket. In a car the housing should **support each card's
top edge or its free right edge (y 13)**: a slotted card guide or a retaining bar at z ≈ 68.5. Keep it clear of the
top-edge connectors listed below.

### What each card brings out

Slot 1 is the audio card's. The sensor card can go in any slot but 1. The other cards can go in any slot from 2 to 6,
so the housing should treat slots 2–6 the same. The y and z values below hold in any slot; x follows the slot.

| Card | Item | y | Past the top edge (z) | Off the parts face | Notes |
| --- | --- | --- | --- | --- | --- |
| **Audio** (slot 1) | 2 × 2 RCA block (front / rear line out), right angle | 26.5–55.4 | to z 70.0 | **28.5** | Plugs from the rear |
| | 3.5 mm jack (mic / aux in) | 16.3–25.6 | to z 71.0 | ≈6 | Plugs from the rear |
| | ESP32 module, **PCB antenna** at the card's top corner | 78–93 | z 51–68.5 | 2.6 | Bluetooth: no metal close to this corner |
| | Programming header J10 (vertical 1 × 5) | 78–92 | z 10–14 | 8.6 | Reached from the open end of the row |
| **GPS** | JST PH 4-pin to the GPS puck, side entry | 47.5–58.4 | to z 68.9 | 4.8 | Plugs from the rear |
| | SWD + debug serial, right angle on the left edge | to 99.5 | z 30.7–54.4 | 2.5 | |
| **Sensor** | JST PH 12-pin (senders, speed, tach), side entry | 52.5–79.4 | to z 68.9 | 4.8 | Plugs from the rear |
| | JST PH 4-pin, side entry | 37.5–48.4 | to z 68.9 | 4.8 | |
| | Micro-Fit 3.0 2-pin | 16.1–26.8 | to z 68.8 | ≈10 | |
| | ISP, debug and serial headers, right angle on the left edge | to 99.5 | z 21.7–58.3 | ≤5.1 | J8 also takes the jumper wire to the main board's J27 |
| **Radio** | **SMA**, edge mount | 42.4–53.6 | **to z 80.4** (11.9 past) | straddles the card ±2.9 | FM antenna via a DIN-to-SMA pigtail |
| | AM loopstick header (1 × 2, right angle) | 55.6–61.8 | to z 75.0 | 2.5 | |
| | SWD + debug serial, right angle on the left edge | to 99.5 | z 30.7–54.4 | 2.5 | |
| **LTE** (not on the current panel) | LTE core board on 8.5 mm sockets | — | — | **≈14** | |
| | LTE antenna: IPEX-to-SMA **bulkhead pigtail to a case panel** | — | — | — | The housing needs an SMA bulkhead hole |
| | ESP32-S3 module, **PCB antenna** at the card's top corner | ≈78–93 | z ≈51–68.5 | 3.1 | Wi-Fi: no metal close to it (or the -1U module with an external antenna if the case is metal) |
| | USB-C (programming), on the left edge | 85.3–98.1 | z 25.6–39.4 | — | |

## Housing model (Onshape, "Cluster housing FS" v5, 2026-10-23)

Checked against this page and the card PCBs, and fixed where they disagreed:

- **Cards beside their pin row.** The cage rails, the rear cover's card slots and the reference cards were centred on
  each socket's pin row. The card body is at +2.54 … +4.14 mm (centre +3.34), so every card hit the rear cover. Now
  the "Card offset from pin row" is 3.34 mm and the "Card position in its slot" is 9.34 mm, so the slot panels stay where
  they were.
- **Header notches.** The cage's bottom wall and its rails are notched for every card's left-edge headers, from
  20.7 mm behind the board to the rear and from 6.9 mm on the parts side to 2.8 mm on the back. That covers the 2 × 3
  ISP header, the solder tails and the LTE USB-C. A card slides in with them fitted. The lower-left cage ear moved
  from X −95 to −80 (under the upper one): it and its gusset sat in slot 3's notch.
- **Slot 1 end plate** is open where the audio card's RCA block passes (board y 25.5–56.4, 53 mm behind the board to
  the rear). Slot panel 1 reaches 10.5 mm past the cage end to cover the block.
- **Slot panels** have openings for each card's top-edge connectors, plus the 1 mm port margin. Pick the card in each
  slot in the feature dialog ("Slot 1 card" … "Slot 6 card"; as delivered: 1 Audio, 2 Sensor, 3 GPS, 4 Radio, 5–6
  empty). Cards: the audio RCA block and 3.5 mm jack; the sensor PH 12, PH 4 and Micro-Fit tach; the GPS PH 4; the
  radio SMA and AM header.

Collision test (solid test boxes against every housing part, 2026-10-23): the six cards, their left-edge headers, the
RCA block and every top-edge connector are clear, and no housing parts interfere with each other. Control boxes (a
card at the old position, a header beside its notch, a plug at a blank panel) all clash, as they should.

## Open items for the housing

- **Standoff length** between the screens and the main board: measure it on the bench (see [Screens](#screens-front)).
- **Rear access:** the card connectors face +z at z ≈ 68.5–80.4. Every main-board cable connector, the RCA jack and
  the AUX fuse are in the rear connector group at the right end (x 232–308.5, y 12.6–67.7), facing +z. Nothing plugs
  in through the board's edges. The power section between the cards and the group needs no openings.
- **Antennas:** the audio and LTE cards have PCB antennas at the top corner of the card. The radio's SMA and the
  LTE pigtail lead out to the case.
