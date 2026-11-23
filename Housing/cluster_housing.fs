FeatureScript 3083;
import(path : "onshape/std/common.fs", version : "3083.0");
BoardV4::import(path : "b779ac360a00653b1f8efd1f", version : "d1ccd6621638a5a789dc223e");

// Gauge cluster housing - v6 (2026-11-17): DigiDash3 board model, one sculpted rear shell (crowned hull, flared card
// pod with a crowned hatch, nacelle over the inductors and the camera module, dished port well), index ribs so a card
// only reaches its socket with its parts facing slot 1, the J8 camera module and its RCA jack.
// v6.1 (2026-11-22): the hatch's audio opening follows the rev J card (RCA block y 27.5-62.5, was 26.5-55.4; jack opening
// widened for the plug), and the reference audio card carries its RCA block, barrels and jack for the clash check.
// v5 (no front lip, rectangular board moved down, six 60x80 accessory cards in a card cage).
// v5 (2026-10-23): cards beside their pin row (+3.34 mm), cage notched for the cards' left-edge headers, slot 1's end
// plate opened for the audio RCA block, slot panels cut for each card's top-edge connectors (per-slot card choice).
// Coordinates: X right, Z up, Y = depth (front glass of the screens at Y = 0, +Y goes back).
// Centre screen centre is the origin. Board (KiCad/STEP) maps as  X = bx - 184.5,  Z = by + 82.73.

const B_POS = { (millimeter) : [0, 121.92, 400] } as LengthBoundSpec;
const B_RISE = { (millimeter) : [-100, 7.625, 100] } as LengthBoundSpec;
const B_STAND = { (millimeter) : [0, 10.26, 60] } as LengthBoundSpec;

const BOARD_T = 1.51 * millimeter;
const BOARD_BACK_PARTS = 10.59 * millimeter;      // tallest part on the board's back (electrolytics)
const BOARD_W = 329 * millimeter;
const BOARD_H = 113 * millimeter;
const BOARD_CZ = 6.23 * millimeter;               // board centre above the centre screen centre, BEFORE the header move
// board mounting holes H1-H4, moved into the necks between the screens (X, Z above board centre), mm.
// KiCad: (238.5, 25.25) (130.5, 25.25) (224.5, 127.75) (144.5, 127.75)
const BOARD_HOLES = [[54.0, 51.25], [-54.0, 51.25], [40.0, -51.25], [-40.0, -51.25]];
// accessory card headers: six 1x15 rows (cluster X of each row centre); rows span Z -9.3..28.8 before the header move
const CARD_X = [-144.5, -119.1, -93.7, -68.3, -42.9, -17.5];
const CARD_ZC = 9.75;
// The cards stand beside their socket's pin row, not on it: the right-angle header's mating pins lie on the parts side, so
// the card body is at +2.54 .. +4.14 mm in X from the pin-row centre (card centre +3.34: the "Card offset from pin row").
// Top-edge connectors per card, cut through its slot panel: [board y0, board y1, X lo, X hi] in mm, X from the card's
// centre plane (its parts face is at -0.8 and faces -X). From the card PCBs and docs/hardware/housing_envelope.md.
export enum SlotCard
{
    annotation { "Name" : "Empty (blank panel)" }
    EMPTY,
    annotation { "Name" : "Audio" }
    AUDIO,
    annotation { "Name" : "Sensor" }
    SENSOR,
    annotation { "Name" : "GPS" }
    GPS,
    annotation { "Name" : "Radio" }
    RADIO
}

function cardCutouts(card is SlotCard) returns array
{
    if (card == SlotCard.AUDIO)    // 2 x 2 RCA block (board y 28-62, 26.5 mm off the face; card rev J, block centre at
                                   // card x 48), 3.5 mm jack (y 17.4-25.7; opening sized for a 10 mm plug overmold)
        return [[27.5, 62.5, -29.3, -0.8], [16.4, 26.7, -8.8, 1.9]];
    if (card == SlotCard.SENSOR)   // JST PH 12 senders, JST PH 4 speed, Micro-Fit 2 tach
        return [[52.5, 79.5, -5.6, -0.8], [37.5, 48.5, -5.6, -0.8], [16.1, 26.8, -10.8, -0.8]];
    if (card == SlotCard.GPS)      // JST PH 4 to the puck
        return [[47.5, 58.5, -5.6, -0.8]];
    if (card == SlotCard.RADIO)    // edge-mount SMA (straddles the card), AM loopstick header
        return [[42.3, 53.6, -2.9, 2.9], [55.6, 61.8, -3.3, -0.8]];
    return [];
}

// Top-edge connector bodies, added to the reference cards so the clash check sees them:
// [board y0, board y1, X lo, X hi, depth into the card from its top edge, reach past the top edge] in mm.
// For cylinders (the RCA barrels): [board y, X of the axis, radius, reach past the top edge, "cyl"].
function cardParts(card is SlotCard) returns array
{
    if (card == SlotCard.AUDIO)    // Kycon KLP42X-61 body + plate, its four barrels, Same Sky SJ1-3533NG
        return [[28.0, 62.0, -27.3, -0.8, 10.0, 1.0],
                [54.0, -7.5, 4.15, 8.8, "cyl"], [36.0, -7.5, 4.15, 8.8, "cyl"],
                [54.0, -21.5, 4.15, 8.8, "cyl"], [36.0, -21.5, 4.15, 8.8, "cyl"],
                [17.4, 25.7, -6.3, -0.8, 14.0, 4.0]];
    return [];
}

// Left-edge programming / debug headers (SWD, serial, ISP, FTDI; the LTE card's USB-C) stand 6.5 mm past the card's
// left edge (board y 93 -> 99.5), between 21.7 and 58.4 mm behind the main board, up to 5.1 mm off the parts face and
// with solder tails on the back: the cage's bottom wall is notched there, open to the rear (X from the card centre).
const HDR_Z0 = 20.7;
const HDR_X0 = -6.9;
const HDR_X1 = 2.8;

// Z of a board y (mm, down from the board's top edge) on a card whose left edge (board y 93) is at Z cz0
function zOfY(cz0 is ValueWithUnits, y) returns ValueWithUnits
{
    return cz0 + (93 - y) * millimeter;
}
// rear-facing ports on the main board (KiCad x0, x1, y0, y1 in mm): six JST PH vertical headers
const JST = [[300.0, 310.0, 33.7, 38.3], [318.0, 328.0, 33.7, 38.3], [281.0, 291.0, 48.7, 53.3], [299.0, 309.0, 48.7, 53.3], [317.0, 327.0, 48.7, 53.3], [300.0, 310.0, 62.7, 67.3]];
const JST_H = 6.09;
// other rear-facing ports (KiCad x0, x1, y0, y1, reference height): 4-pin and 6-pin 3.0 mm-pitch vertical headers
// (footprints only, no 3D model: body sized from the pin and peg holes), the 2-pin 5.5 mm-pitch aux 12 V input,
// and the Keystone 3568 mini blade fuse holder (opening so the fuse can be swapped from the back)
const PORTS = [[277.3, 294.7, 32.6, 42.6, 10], [251.7, 275.1, 47.6, 57.6, 10], [267.0, 279.0, 79.0, 89.0, 10], [284.0, 300.0, 80.6, 87.4, 7.46]];
// DigiDash3: the two 68 uH inductors L1 / L2 (KiCad sheet x0, x1, y0, y1 and height off the board, from the STEP)
const IND = [173.4, 215.6, 71.4, 85.6, 24.7];
// camera module socket J8 (2 x 10): KiCad sheet x of its middle and y of the middle between its two pin rows
const CAM = [219.55, 91.75];
// the cards' left-edge headers stand this far past the card edge (board y 93 -> 99.5)
const HDR_DROP = 6.5;
const PLATE_W = 85.5 * millimeter;                // screen rear plate
const PLATE_H = 65 * millimeter;
const SCREEN_BACK = 9.5 * millimeter;             // module back face -> deepest screen part (rear PCB / plate)
const SCREEN_BOSS_H = 4 * millimeter;             // the screen's four M4 bosses stand 4 mm off the module back (7.1 mm dia)
const SCREEN_BOSS_R = 3.56 * millimeter;
const M4_PITCH = 37.5 * millimeter;               // screen M4 holes at (+-37.5, +-37.5)

function mkBox(context is Context, id is Id, x0, x1, y0, y1, z0, z1) returns Query
{
    fCuboid(context, id, { "corner1" : vector(x0, y0, z0), "corner2" : vector(x1, y1, z1) });
    return qCreatedBy(id, EntityType.BODY);
}

// cylinder along Y at (x, z)
function cylY(context is Context, id is Id, x, z, y0, y1, r) returns Query
{
    fCylinder(context, id, { "bottomCenter" : vector(x, y0, z), "topCenter" : vector(x, y1, z), "radius" : r });
    return qCreatedBy(id, EntityType.BODY);
}

function coneY(context is Context, id is Id, x, z, y0, y1, r0, r1) returns Query
{
    fCone(context, id, { "bottomCenter" : vector(x, y0, z), "topCenter" : vector(x, y1, z), "bottomRadius" : r0, "topRadius" : r1 });
    return qCreatedBy(id, EntityType.BODY);
}

function boolAdd(context is Context, id is Id, target is Query, tools is array)
{
    opBoolean(context, id, { "tools" : qUnion(concatenateArrays([[target], tools])), "operationType" : BooleanOperationType.UNION });
}

function boolCut(context is Context, id is Id, target is Query, tools is array)
{
    opBoolean(context, id, { "tools" : qUnion(tools), "targets" : target, "operationType" : BooleanOperationType.SUBTRACTION });
}

function copyBody(context is Context, id is Id, q is Query) returns Query
{
    opPattern(context, id, { "entities" : q, "transforms" : [identityTransform()], "instanceNames" : ["c"] });
    return qCreatedBy(id, EntityType.BODY);
}

function cylP(context is Context, id is Id, p0 is Vector, p1 is Vector, r) returns Query
{
    fCylinder(context, id, { "bottomCenter" : p0, "topCenter" : p1, "radius" : r });
    return qCreatedBy(id, EntityType.BODY);
}

function coneP(context is Context, id is Id, p0 is Vector, p1 is Vector, r0, r1) returns Query
{
    fCone(context, id, { "bottomCenter" : p0, "topCenter" : p1, "bottomRadius" : r0, "topRadius" : r1 });
    return qCreatedBy(id, EntityType.BODY);
}

// polygon given as (y, z) points on the plane X = x0, extruded +X
function prismX(context is Context, id is Id, x0, pts is array, depth) returns Query
{
    const pl = plane(vector(x0, 0 * meter, 0 * meter), vector(1, 0, 0), vector(0, 1, 0));
    const sk = newSketchOnPlane(context, id + "sk", { "sketchPlane" : pl });
    skPolyline(sk, "poly", { "points" : concatenateArrays([pts, [pts[0]]]) });
    skSolve(sk);
    opExtrude(context, id + "ex", { "entities" : qSketchRegion(id + "sk"), "direction" : pl.normal, "endBound" : BoundingType.BLIND, "endDepth" : depth });
    opDeleteBodies(context, id + "del", { "entities" : qCreatedBy(id + "sk", EntityType.BODY) });
    return qCreatedBy(id + "ex", EntityType.BODY);
}


// ---------------- v6 shape helpers ----------------
// plane Y = y, normal +Y; sketch u = X, v = -Z
function rrPlane(y) returns Plane
{
    return plane(vector(0 * meter, y, 0 * meter), vector(0, 1, 0), vector(1, 0, 0));
}

function sv(x, z) returns Vector
{
    return vector(x, -z);
}

// rounded rectangle [x0, x1, z0, z1, r] grown outward by d
function rrGrow(p is array, d) returns array
{
    return [p[0] - d, p[1] + d, p[2] - d, p[3] + d, max(p[4] + d, 0.5 * millimeter)];
}

function rrSketch(sk is Sketch, nm is string, p is array)
{
    const x0 = p[0];
    const x1 = p[1];
    const z0 = p[2];
    const z1 = p[3];
    const rr = min(p[4], min((x1 - x0) / 2, (z1 - z0) / 2) - 0.01 * millimeter);
    const c = rr * (1 - sqrt(0.5));
    skLineSegment(sk, nm ~ "l0", { "start" : sv(x0 + rr, z0), "end" : sv(x1 - rr, z0) });
    skArc(sk, nm ~ "a0", { "start" : sv(x1 - rr, z0), "mid" : sv(x1 - c, z0 + c), "end" : sv(x1, z0 + rr) });
    skLineSegment(sk, nm ~ "l1", { "start" : sv(x1, z0 + rr), "end" : sv(x1, z1 - rr) });
    skArc(sk, nm ~ "a1", { "start" : sv(x1, z1 - rr), "mid" : sv(x1 - c, z1 - c), "end" : sv(x1 - rr, z1) });
    skLineSegment(sk, nm ~ "l2", { "start" : sv(x1 - rr, z1), "end" : sv(x0 + rr, z1) });
    skArc(sk, nm ~ "a2", { "start" : sv(x0 + rr, z1), "mid" : sv(x0 + c, z1 - c), "end" : sv(x0, z1 - rr) });
    skLineSegment(sk, nm ~ "l3", { "start" : sv(x0, z1 - rr), "end" : sv(x0, z0 + rr) });
    skArc(sk, nm ~ "a3", { "start" : sv(x0, z0 + rr), "mid" : sv(x0 + c, z0 + c), "end" : sv(x0 + rr, z0) });
}

function rrExtrude(context is Context, id is Id, p is array, ya, yb) returns Query
{
    const sk = newSketchOnPlane(context, id + "sk", { "sketchPlane" : rrPlane(ya) });
    rrSketch(sk, "p", p);
    skSolve(sk);
    opExtrude(context, id + "ex", { "entities" : qSketchRegion(id + "sk"), "direction" : vector(0, 1, 0), "endBound" : BoundingType.BLIND, "endDepth" : yb - ya });
    opDeleteBodies(context, id + "del", { "entities" : qCreatedBy(id + "sk", EntityType.BODY) });
    return qCreatedBy(id + "ex", EntityType.BODY);
}

// loft through rounded rectangles at the given Y levels
function rrLoft(context is Context, id is Id, profs is array, ys is array) returns Query
{
    var subs = [];
    for (var k = 0; k < size(profs); k += 1)
    {
        const sk = newSketchOnPlane(context, id + ("sk" ~ k), { "sketchPlane" : rrPlane(ys[k]) });
        rrSketch(sk, "p", profs[k]);
        skSolve(sk);
        subs = append(subs, qSketchRegion(id + ("sk" ~ k)));
    }
    opLoft(context, id + "lo", { "profileSubqueries" : subs, "bodyType" : ToolBodyType.SOLID });
    for (var k = 0; k < size(profs); k += 1)
    {
        opDeleteBodies(context, id + ("del" ~ k), { "entities" : qCreatedBy(id + ("sk" ~ k), EntityType.BODY) });
    }
    return qCreatedBy(id + "lo", EntityType.BODY);
}

// the housing's outline (three screen rings and the board rectangle) grown by g, from y0 to y1
function outlineBody(context is Context, id is Id, screens is array, rx, rz0, rz1, wall, g, y0, y1) returns Query
{
    const B = mkBox(context, id + "r", -rx - wall - g, rx + wall + g, y0, y1, rz0 - wall - g, rz1 + wall + g);
    var cs = [];
    var k = 0;
    for (var s in screens)
    {
        cs = append(cs, cylY(context, id + ("c" ~ k), s[0], s[1], y0, y1, s[2] + wall + g));
        k += 1;
    }
    boolAdd(context, id + "u", B, cs);
    return B;
}

// keep only the part of target inside tool (tool is kept)
function clipTo(context is Context, id is Id, target is Query, tool is Query)
{
    const bx = mkBox(context, id + "bx", -3 * meter, 3 * meter, -3 * meter, 3 * meter, -3 * meter, 3 * meter);
    opBoolean(context, id + "inv", { "tools" : tool, "targets" : bx, "operationType" : BooleanOperationType.SUBTRACTION, "keepTools" : true });
    opBoolean(context, id + "cut", { "tools" : bx, "targets" : target, "operationType" : BooleanOperationType.SUBTRACTION });
}

// round every edge of body except those lying on the given Y planes (all at once, else edge by edge)
function softEdges(context is Context, id is Id, body is Query, keepYs is array, r is ValueWithUnits)
{
    var es = qOwnedByBody(body, EntityType.EDGE);
    for (var y in keepYs)
    {
        es = qSubtraction(es, qCoincidesWithPlane(es, rrPlane(y)));
    }
    var done = false;
    try silent
    {
        opFillet(context, id + "all", { "entities" : es, "radius" : r });
        done = true;
    }
    if (!done)
    {
        var k = 0;
        for (var e in evaluateQuery(context, es))
        {
            try silent
            {
                opFillet(context, id + ("e" ~ k), { "entities" : e, "radius" : r });
            }
            k += 1;
        }
    }
}

function bbox(boxes is array) returns array
{
    var b = boxes[0];
    for (var q in boxes)
    {
        b = [min(b[0], q[0]), max(b[1], q[1]), min(b[2], q[2]), max(b[3], q[3])];
    }
    return b;
}

function minPocketGap(screens is array, x, z) returns ValueWithUnits
{
    var g = 1 * meter;
    for (var s in screens)
    {
        g = min(g, sqrt((x - s[0]) * (x - s[0]) + (z - s[1]) * (z - s[1])) - s[2]);
    }
    return g;
}

annotation { "Feature Type Name" : "Gauge cluster housing", "Feature Type Description" : "Three round screens (0o0) flush with the front face, rectangular main board (screen headers moved up so the board sits lower), six accessory cards in a screw-in card cage. Two printable halves, a two-piece sculpted rear shell with the card pod, the camera nacelle and the port well, and a crowned card bay hatch." }
export const gaugeClusterHousing = defineFeature(function(context is Context, id is Id, definition is map)
    precondition
    {
        annotation { "Group Name" : "Screens", "Collapsed By Default" : false }
        {
            annotation { "Name" : "Side screen diameter" }
            isLength(definition.sideD, { (millimeter) : [50, 126, 300] } as LengthBoundSpec);
            annotation { "Name" : "Centre screen diameter" }
            isLength(definition.centerD, { (millimeter) : [50, 115, 300] } as LengthBoundSpec);
            annotation { "Name" : "Side screen spacing (centre to centre)" }
            isLength(definition.sideX, B_POS);
            annotation { "Name" : "Centre screen rise above sides" }
            isLength(definition.centerRise, B_RISE);
            annotation { "Name" : "Screen module thickness (glass to back face)" }
            isLength(definition.pocketD, { (millimeter) : [2, 6, 40] } as LengthBoundSpec);
            annotation { "Name" : "Pocket radial clearance" }
            isLength(definition.pocketClr, { (millimeter) : [0, 0.3, 3] } as LengthBoundSpec);
            annotation { "Name" : "Screen proud of housing face" }
            isLength(definition.proud, { (millimeter) : [0, 0, 5] } as LengthBoundSpec);
            annotation { "Name" : "Back-stop ledge width" }
            isLength(definition.ledgeW, { (millimeter) : [0, 3.5, 6] } as LengthBoundSpec);
        }
        annotation { "Group Name" : "Board", "Collapsed By Default" : false }
        {
            annotation { "Name" : "Screen header move up the board (board drops)" }
            isLength(definition.headerMove, { (millimeter) : [0, 12, 30] } as LengthBoundSpec);
            annotation { "Name" : "Standoff (screen boss face to board front)" }
            isLength(definition.standoff, B_STAND);
            annotation { "Name" : "Board edge clearance" }
            isLength(definition.edgeClr, { (millimeter) : [0, 1, 20] } as LengthBoundSpec);
            annotation { "Name" : "Room below board (cover screws)" }
            isLength(definition.botM, { (millimeter) : [0, 8, 30] } as LengthBoundSpec);
            annotation { "Name" : "Clearance behind tallest part" }
            isLength(definition.backClr, { (millimeter) : [0, 1.5, 30] } as LengthBoundSpec);
        }
        annotation { "Group Name" : "Bezel mounts", "Collapsed By Default" : false }
        {
            annotation { "Name" : "Mount line inset from housing end edge" }
            isLength(definition.mountInset, { (millimeter) : [5, 20.5, 60] } as LengthBoundSpec);
            annotation { "Name" : "Top-to-bottom boss spacing (centred on side gauge line)" }
            isLength(definition.mountSpan, { (millimeter) : [50, 131.5, 250] } as LengthBoundSpec);
            annotation { "Name" : "Bezel boss diameter" }
            isLength(definition.mountBossD, { (millimeter) : [4, 10, 25] } as LengthBoundSpec);
            annotation { "Name" : "Boss height past the glass plane" }
            isLength(definition.mountBossH, { (millimeter) : [0, 7.5, 15] } as LengthBoundSpec);
            annotation { "Name" : "Screw slot width" }
            isLength(definition.mountSlotW, { (millimeter) : [2, 4.5, 8] } as LengthBoundSpec);
            annotation { "Name" : "Slot travel (each way)" }
            isLength(definition.mountTravel, { (millimeter) : [0, 4, 15] } as LengthBoundSpec);
            annotation { "Name" : "Tab thickness behind the boss" }
            isLength(definition.mountTabT, { (millimeter) : [2, 4, 10] } as LengthBoundSpec);
            annotation { "Name" : "Screw access channel diameter" }
            isLength(definition.mountAccessD, { (millimeter) : [4, 9, 20] } as LengthBoundSpec);
            annotation { "Name" : "Tab width" }
            isLength(definition.mountTabW, { (millimeter) : [8, 26, 40] } as LengthBoundSpec);
        }
        annotation { "Group Name" : "Rear ports", "Collapsed By Default" : false }
        {
            annotation { "Name" : "Port opening margin" }
            isLength(definition.portMargin, { (millimeter) : [0, 1, 5] } as LengthBoundSpec);
            annotation { "Name" : "RCA opening diameter" }
            isLength(definition.rcaDia, { (millimeter) : [5, 13, 30] } as LengthBoundSpec);
        }
        annotation { "Group Name" : "Accessory cards", "Collapsed By Default" : false }
        {
            annotation { "Name" : "Card height (off the main board)" }
            isLength(definition.cardH, { (millimeter) : [10, 60, 150] } as LengthBoundSpec);
            annotation { "Name" : "Card length (along the header)" }
            isLength(definition.cardL, { (millimeter) : [20, 80, 150] } as LengthBoundSpec);
            annotation { "Name" : "Card thickness" }
            isLength(definition.cardT, { (millimeter) : [0.8, 1.6, 3] } as LengthBoundSpec);
            annotation { "Name" : "Card lift (main board back to card edge)" }
            isLength(definition.cardLift, { (millimeter) : [0, 9, 30] } as LengthBoundSpec);
            annotation { "Name" : "Card offset from pin row (X)" }
            isLength(definition.cardXOff, { (millimeter) : [-10, 3.34, 10] } as LengthBoundSpec);
            annotation { "Name" : "Card offset along header (Z)" }
            isLength(definition.cardZOff, { (millimeter) : [-30, 0, 30] } as LengthBoundSpec);
            annotation { "Name" : "Guide slot width (between rails)" }
            isLength(definition.slotW, { (millimeter) : [1, 2.2, 5] } as LengthBoundSpec);
            annotation { "Name" : "Guide rail height" }
            isLength(definition.slotD, { (millimeter) : [0.5, 2, 6] } as LengthBoundSpec);
            annotation { "Name" : "Cage wall thickness" }
            isLength(definition.barT, { (millimeter) : [0.8, 1.6, 6] } as LengthBoundSpec);
            annotation { "Name" : "Slot 1 card", "Default" : SlotCard.AUDIO }
            definition.slot1 is SlotCard;
            annotation { "Name" : "Slot 2 card", "Default" : SlotCard.SENSOR }
            definition.slot2 is SlotCard;
            annotation { "Name" : "Slot 3 card", "Default" : SlotCard.GPS }
            definition.slot3 is SlotCard;
            annotation { "Name" : "Slot 4 card", "Default" : SlotCard.RADIO }
            definition.slot4 is SlotCard;
            annotation { "Name" : "Slot 5 card", "Default" : SlotCard.EMPTY }
            definition.slot5 is SlotCard;
            annotation { "Name" : "Slot 6 card", "Default" : SlotCard.EMPTY }
            definition.slot6 is SlotCard;
        }
        annotation { "Group Name" : "Shell", "Collapsed By Default" : false }
        {
            annotation { "Name" : "Wall thickness" }
            isLength(definition.wall, { (millimeter) : [1, 2.5, 8] } as LengthBoundSpec);
            annotation { "Name" : "Rear shell wall thickness" }
            isLength(definition.coverT, { (millimeter) : [1, 2.4, 8] } as LengthBoundSpec);
            annotation { "Name" : "Screw boss diameter" }
            isLength(definition.bossD, { (millimeter) : [4, 7, 14] } as LengthBoundSpec);
            annotation { "Name" : "Board post diameter" }
            isLength(definition.postD, { (millimeter) : [4, 6, 12] } as LengthBoundSpec);
            annotation { "Name" : "Pilot hole (M3)" }
            isLength(definition.pilotDia, { (millimeter) : [1, 2.5, 6] } as LengthBoundSpec);
            annotation { "Name" : "Clearance hole (M3)" }
            isLength(definition.clearDia, { (millimeter) : [2, 3.4, 7] } as LengthBoundSpec);
            annotation { "Name" : "Show reference board and screens" }
            definition.showRef is boolean;
            annotation { "Name" : "Use the KiCad board model (boardmodel-v4) as the reference board", "Default" : true }
            definition.showBoard is boolean;
        }
        annotation { "Group Name" : "Rear shell", "Collapsed By Default" : false }
        {
            annotation { "Name" : "Hull depth behind the housing" }
            isLength(definition.hullH, { (millimeter) : [4, 12, 40] } as LengthBoundSpec);
            annotation { "Name" : "Hull crown (top to bottom)" }
            isLength(definition.hullCrown, { (millimeter) : [0.5, 5, 10] } as LengthBoundSpec);
            annotation { "Name" : "Card pod flare at its base" }
            isLength(definition.podFlare, { (millimeter) : [0, 12, 25] } as LengthBoundSpec);
            annotation { "Name" : "Card pod corner radius" }
            isLength(definition.podR, { (millimeter) : [3, 8, 20] } as LengthBoundSpec);
            annotation { "Name" : "Edge rounding" }
            isLength(definition.shellR, { (millimeter) : [1, 6, 12] } as LengthBoundSpec);
            annotation { "Name" : "Hatch crown" }
            isLength(definition.hatchCrown, { (millimeter) : [0.5, 7, 12] } as LengthBoundSpec);
            annotation { "Name" : "Port well floor (under side, off the board's back)" }
            isLength(definition.wellFloor, { (millimeter) : [8, 15, 30] } as LengthBoundSpec);
            annotation { "Name" : "Rear shell split (X)" }
            isLength(definition.coverSplit, { (millimeter) : [-60, 4.5, 60] } as LengthBoundSpec);
        }
        annotation { "Group Name" : "Camera module (J8)", "Collapsed By Default" : false }
        {
            annotation { "Name" : "Module parts face the board's bottom edge", "Default" : true }
            definition.camFaceDown is boolean;
            annotation { "Name" : "Module width (along J8)" }
            isLength(definition.camW, { (millimeter) : [20, 29, 60] } as LengthBoundSpec);
            annotation { "Name" : "Module length (off the board)" }
            isLength(definition.camL, { (millimeter) : [20, 37.65, 80] } as LengthBoundSpec);
            annotation { "Name" : "Module lift (board back to its edge)" }
            isLength(definition.camLift, { (millimeter) : [0, 9, 30] } as LengthBoundSpec);
            annotation { "Name" : "RCA axis off the module's parts face" }
            isLength(definition.rcaAx, { (millimeter) : [3, 7, 15] } as LengthBoundSpec);
            annotation { "Name" : "RCA body size" }
            isLength(definition.rcaBody, { (millimeter) : [6, 13.5, 20] } as LengthBoundSpec);
            annotation { "Name" : "RCA nose past the module edge" }
            isLength(definition.rcaNose, { (millimeter) : [0, 4, 15] } as LengthBoundSpec);
        }
    }
    {
        const mm = millimeter;
        const wall = definition.wall;
        const pD = definition.pocketD;
        const y0 = definition.proud;
        const sx = definition.sideX;
        const sz = -definition.centerRise;
        const rC = definition.centerD / 2 + definition.pocketClr;
        const rS = definition.sideD / 2 + definition.pocketClr;
        const bossR = definition.bossD / 2;
        const postR = definition.postD / 2;
        const big = 400 * mm;
        const dy = definition.headerMove;

        // board (moves down by the header move)
        const bcz = BOARD_CZ - dy;
        const bx1 = BOARD_W / 2;
        const bz0 = bcz - BOARD_H / 2;
        const bz1 = bcz + BOARD_H / 2;
        const bFront = pD + SCREEN_BOSS_H + definition.standoff;
        const bBack = bFront + BOARD_T;
        // cards
        const cardZc = CARD_ZC * mm - dy + definition.cardZOff;
        const cz0 = cardZc - definition.cardL / 2;
        const cz1 = cardZc + definition.cardL / 2;
        const cy0 = bBack + definition.cardLift;
        const cy1 = cy0 + definition.cardH;
        const backY = bBack + BOARD_BACK_PARTS + definition.backClr;     // shallow back: only the card bay sticks out further

        const rx = bx1 + definition.edgeClr;
        const rz0 = bz0 - definition.edgeClr - definition.botM;
        const rz1 = bz1 + definition.edgeClr;
        const screens = [[-sx, sz, rS], [0 * mm, 0 * mm, rC], [sx, sz, rS]];

        // ---------------- shell ----------------
        var outer = [];
        var i = 0;
        for (var s in screens)
        {
            outer = append(outer, cylY(context, id + ("oc" ~ i), s[0], s[1], y0, backY, s[2] + wall));
            i += 1;
        }
        const H = mkBox(context, id + "orect", -rx - wall, rx + wall, y0, backY, rz0 - wall, rz1 + wall);
        boolAdd(context, id + "ounion", H, outer);
        var cuts = [];
        i = 0;
        for (var s in screens)
        {
            cuts = append(cuts, cylY(context, id + ("pk" ~ i), s[0], s[1], y0 - 1 * mm, backY + 1 * mm, s[2]));
            i += 1;
        }
        cuts = append(cuts, mkBox(context, id + "irect", -rx, rx, pD, backY + 1 * mm, rz0, rz1));
        boolCut(context, id + "cavity", H, cuts);

        // ---------------- back-stop ledges ----------------
        var adds = [];
        if (definition.ledgeW > 0.05 * mm)
        {
            const W = definition.ledgeW;
            i = 0;
            for (var s in screens)
            {
                const L = cylY(context, id + ("lg" ~ i), s[0], s[1], pD, pD + W + 1 * mm, s[2] + 0.2 * mm);
                var lc = [coneY(context, id + ("lgc" ~ i), s[0], s[1], pD - 0.001 * mm, pD + W, s[2] + 0.001 * mm, s[2] - W),
                          cylY(context, id + ("lgi" ~ i), s[0], s[1], pD + W - 0.01 * mm, pD + W + 2 * mm, s[2] - W)];
                if (i == 1)
                {
                    // no ledge across the top of the split: the wall it would hang from belongs to the other half there
                    lc = append(lc, mkBox(context, id + "lgsplit", -6.5 * mm, 6.5 * mm, pD - 1 * mm, pD + W + 2 * mm, rz1, big));
                }
                boolCut(context, id + ("lgcut" ~ i), L, lc);
                adds = append(adds, L);
                // backing collar: a full-thickness ring behind the frame, sitting on the solid face around the pocket,
                // so the ledge is never hanging off a thin edge where the board cavity opens up behind it
                const col = cylY(context, id + ("lgo" ~ i), s[0], s[1], pD, pD + W + 2 * mm, s[2] + wall);
                // keep the collar out of the neighbouring pockets too, so it never bridges over open space at the necks
                var cc = [];
                var jn = 0;
                for (var s2 in screens)
                {
                    cc = append(cc, cylY(context, id + ("lgoi" ~ i ~ "x" ~ jn), s2[0], s2[1], pD - 1 * mm, pD + W + 3 * mm, s2[2]));
                    jn += 1;
                }
                boolCut(context, id + ("lgocut" ~ i), col, cc);
                adds = append(adds, col);
                i += 1;
            }
        }

        // ---------------- board posts at H1-H4 ----------------
        var posts = [];
        var pg = 1 * meter;
        for (var h in BOARD_HOLES)
        {
            posts = append(posts, [h[0] * mm, bcz + h[1] * mm]);
            pg = min(pg, minPocketGap(screens, h[0] * mm, bcz + h[1] * mm) - postR);
        }
        if (pg < 0.3 * mm)
            reportFeatureWarning(context, id, "Board posts hit the screen pockets: check 'Screen header move'");
        var pilots = [];
        i = 0;
        for (var c in posts)
        {
            adds = append(adds, cylY(context, id + ("post" ~ i), c[0], c[1], y0, bFront, postR));
            pilots = append(pilots, cylY(context, id + ("postp" ~ i), c[0], c[1], bFront - 9 * mm, bFront + 1 * mm, definition.pilotDia / 2));
            i += 1;
        }

        // ---------------- rear cover bosses ----------------
        const inset = max(0 * mm, bossR - wall + 0.8 * mm);
        const rSi = rS - inset;
        const zct = sqrt((rC - inset) * (rC - inset) - 100 * mm * mm);
        const zbt = rz0 - wall + 0.8 * mm + bossR;
        var pts = [[10 * mm, zct], [-10 * mm, zct], [40 * mm, zbt], [-40 * mm, zbt]];
        for (var sg in [-1, 1])
        {
            for (var a in [0, 30, -30, -90])
            {
                pts = append(pts, [sg * (sx + rSi * cos(a * degree)), sz + rSi * sin(a * degree)]);
            }
        }
        i = 0;
        for (var p in pts)
        {
            if (minPocketGap(screens, p[0], p[1]) > bossR + 0.3 * mm)
            {
                adds = append(adds, cylY(context, id + ("bb" ~ i), p[0], p[1], y0, backY, bossR));
            }
            else
            {
                const yb = pD + definition.ledgeW + 1 * mm;
                adds = append(adds, coneY(context, id + ("bc" ~ i), p[0], p[1], yb, yb + bossR, 0.05 * mm, bossR));
                adds = append(adds, cylY(context, id + ("bb" ~ i), p[0], p[1], yb + bossR, backY, bossR));
            }
            pilots = append(pilots, cylY(context, id + ("bp" ~ i), p[0], p[1], backY - 10 * mm, backY + 1 * mm, definition.pilotDia / 2));
            i += 1;
        }
        // ---------------- card cage mounting bosses (on the top and bottom walls, behind the board) ----------------
        const cageY0 = backY + definition.coverT;                      // cage sits on the outside of the rear cover
        const zTopB = rz1 + wall - 0.8 * mm - bossR;
        const zBotB = rz0 - wall + 0.8 * mm + bossR;
        const cageBosses = [[-90 * mm, zTopB], [-62 * mm, zTopB], [-80 * mm, zBotB], [-30 * mm, zBotB]];   // rear shell screws. v6: the top pair moved to board x 74.5 / 102.5, clear of DigiDash3's PTCs F24, F16, F28, J27 and H1 on the top edge; bottom pair clear of the header notches
        i = 0;
        for (var p in cageBosses)
        {
            const onBoard = abs(p[1] - bcz) < BOARD_H / 2 + bossR;
            var ys = y0;
            if (onBoard)
                ys = bBack + 0.5 * mm;
            else if (minPocketGap(screens, p[0], p[1]) < bossR + 0.3 * mm)
                ys = pD + definition.ledgeW + 1 * mm;
            if (ys > y0)
            {
                adds = append(adds, coneY(context, id + ("gbc" ~ i), p[0], p[1], ys, ys + bossR, 0.05 * mm, bossR));
                adds = append(adds, cylY(context, id + ("gbb" ~ i), p[0], p[1], ys + bossR, backY, bossR));
            }
            else
            {
                adds = append(adds, cylY(context, id + ("gbb" ~ i), p[0], p[1], y0, backY, bossR));
            }
            pilots = append(pilots, cylY(context, id + ("gbp" ~ i), p[0], p[1], backY - 9 * mm, backY + 1 * mm, definition.pilotDia / 2));
            i += 1;
        }
        // ---------------- split laps: the halves overlap at the top and bottom of the split and screw together ----------------
        // top: left half owns the outer layer, right half the inner layer; M3 x 8 flat-head screws down from the top.
        // bottom: left half owns the outer layer, right half the inner layer; M3 x 10 flat-head screws up from the bottom.
        const lw = 6 * mm;
        const zbTop = rz0 + definition.botM;
        const zbS = rz0 - wall + (wall + definition.botM) / 2;
        const ztTopOuter = rC + wall;
        const ztS = rz1 + (ztTopOuter - rz1) / 2;
        const yp0 = pD + definition.ledgeW + 1 * mm;
        const lapX1 = 30 * mm;   // the right half's inner lap runs out to its own wall
        const padT = prismX(context, id + "padT", -lw, [vector(yp0, rC + 0.5 * mm), vector(backY, rC + 0.5 * mm), vector(backY, rz1), vector(yp0 + (rC + 0.5 * mm - rz1), rz1)], lw + 12 * mm);
        const padB = mkBox(context, id + "padB", -lw, lapX1, y0, backY, rz0 - 0.5 * mm, zbTop);
        adds = append(adds, padT);
        // outboard of X 12 the housing top drops to the band's top surface, so the right half's tongue continues there at reduced height
        const ztB = rz1 + wall - 0.3 * mm;
        adds = append(adds, prismX(context, id + "padT2", 10.5 * mm, [vector(yp0 + 0.4 * mm, ztB), vector(backY - 0.01 * mm, ztB), vector(backY - 0.01 * mm, rz1 + 0.01 * mm), vector(yp0 + 0.4 * mm + (ztB - rz1), rz1 + 0.01 * mm)], lapX1 - 10.5 * mm));
        adds = append(adds, padB);

        // ---------------- bezel mounting tabs: one above and one below each end, on the old cluster's bosses ----------------
        // The bosses stand proud of the bezel rings, so each tab's contact face sits back from the housing face by the boss
        // height, and the housing is cleared around each boss from the front face back to that contact face.
        // Slots and boss clearances run left-right (X) for adjustment. Each tab has a 45 deg underside back to the housing
        // so it prints from the bed without supports.
        const xm = rx + wall - definition.mountInset;
        const tw = definition.mountTabW / 2;
        const yF = y0 + definition.mountBossH;                               // tab contact face (bears on the boss tops)
        const ty1 = yF + definition.mountTabT;                               // tab back face
        const pr = definition.mountBossD / 2 + 1 * mm;                       // boss clearance radius
        const tr = definition.mountTravel;
        var mCuts = [];
        var mTabs = [];
        var mi = 0;
        for (var sg in [-1, 1])
        {
            for (var d in [1, -1])
            {
                const zb = sz + d * definition.mountSpan / 2;                // boss centre
                const hs = d > 0 ? rz1 + wall : rz0 - wall;                    // housing outer top / bottom surface
                const far = zb + d * (pr + 2 * mm);
                const inner = zb - d * (pr + 2 * mm);
                const xc = sg * xm;
                mTabs = append(mTabs, prismX(context, id + ("mt" ~ mi), xc - tw,
                            [vector(y0, inner), vector(ty1, inner), vector(ty1, far), vector(yF, far), vector(yF, hs + d * (yF - y0)), vector(y0, hs)], 2 * tw));
                // 45 deg gussets at both tab edges, from the tab's back onto the housing's top / bottom surface
                const gl = min(abs(far - hs) - 2 * mm, 12 * mm);
                if (gl > 1 * mm)
                {
                    var ge = 0;
                    for (var ex in [-1, 1])
                    {
                        mTabs = append(mTabs, prismX(context, id + ("mg" ~ mi ~ "x" ~ ge), xc + ex * (tw - 1 * mm) - 1 * mm,
                                    [vector(ty1 - 0.5 * mm, hs - d * 0.5 * mm), vector(ty1 - 0.5 * mm, hs + d * gl), vector(ty1, hs + d * gl), vector(ty1 + gl, hs - d * 0.5 * mm)], 2 * mm));
                        ge += 1;
                    }
                }
                // boss clearance (front face back to the contact face) and screw slot (through), both oval left-right
                for (var kk in [0, 1])
                {
                    const rr = kk == 0 ? pr : definition.mountSlotW / 2;
                    const yb = kk == 0 ? yF : ty1 + 1 * mm;
                    mCuts = append(mCuts, cylY(context, id + ("mc" ~ mi ~ "a" ~ kk), xc - tr, zb, y0 - 1 * mm, yb, rr));
                    mCuts = append(mCuts, cylY(context, id + ("mc" ~ mi ~ "b" ~ kk), xc + tr, zb, y0 - 1 * mm, yb, rr));
                    if (tr > 0.01 * mm)
                        mCuts = append(mCuts, mkBox(context, id + ("mc" ~ mi ~ "c" ~ kk), xc - tr, xc + tr, y0 - 1 * mm, yb, zb - rr, zb + rr));
                }
                if (true)
                {
                    // the bottom screws sit behind the housing's bottom wall (the top ones graze the top edge): open a channel from the tab back to the rear
                    // (left-right oval like the slot) so a screw and driver can reach them
                    const ar = definition.mountAccessD / 2;
                    mCuts = append(mCuts, cylY(context, id + ("ma" ~ mi ~ "a"), xc - tr, zb, ty1, backY + 1 * mm, ar));
                    mCuts = append(mCuts, cylY(context, id + ("ma" ~ mi ~ "b"), xc + tr, zb, ty1, backY + 1 * mm, ar));
                    if (tr > 0.01 * mm)
                        mCuts = append(mCuts, mkBox(context, id + ("ma" ~ mi ~ "c"), xc - tr, xc + tr, ty1, backY + 1 * mm, zb - ar, zb + ar));
                    // a 1.6 mm wall round the inside of the channel, from the tab back to the rear panel, so the screw way is
                    // sealed off from the inside of the housing; it stops at the housing's outer surface, leaving the
                    // channel open on the outside (it rises off the tab, so it prints without support)
                    const aw = ar + 1.6 * mm;
                    var wl = [cylY(context, id + ("mw" ~ mi ~ "a"), xc - tr, zb, ty1 - 0.5 * mm, backY, aw),
                              cylY(context, id + ("mw" ~ mi ~ "b"), xc + tr, zb, ty1 - 0.5 * mm, backY, aw)];
                    if (tr > 0.01 * mm)
                        wl = append(wl, mkBox(context, id + ("mw" ~ mi ~ "c"), xc - tr, xc + tr, ty1 - 0.5 * mm, backY, zb - aw, zb + aw));
                    boolCut(context, id + ("mwo" ~ mi), qUnion(wl), [mkBox(context, id + ("mwk" ~ mi), -big, big, -big, big, d > 0 ? hs : -big, d > 0 ? big : hs)]);
                    adds = concatenateArrays([adds, wl]);
                }
                mi += 1;
            }
        }
        // trim the tabs back to the screen pocket outline over their full depth, so nothing of a tab
        // sits inside the round pocket (the screen drops in past them)
        var tcut = [];
        var tj = 0;
        for (var s2 in screens)
        {
            tcut = append(tcut, cylY(context, id + ("mtp" ~ tj), s2[0], s2[1], y0 - 2 * mm, backY + 2 * mm, s2[2] + 0.5 * mm));
            tj += 1;
        }
        boolCut(context, id + "mtrim", qUnion(mTabs), tcut);
        adds = concatenateArrays([adds, mTabs]);
        boolAdd(context, id + "adds", H, adds);
        // keep the screen pockets clear where a tab overlaps them
        var pi2 = 0;
        for (var s2 in screens)
        {
            mCuts = append(mCuts, cylY(context, id + ("mpk" ~ pi2), s2[0], s2[1], y0 - 1 * mm, pD, s2[2]));
            pi2 += 1;
        }
        boolCut(context, id + "mountcut", H, mCuts);
        boolCut(context, id + "pilots", H, pilots);
        var lapCuts = [];
        i = 0;
        for (var y in [yp0 + (rC + 0.5 * mm - rz1) + 2.5 * mm, backY - 4 * mm])
        {
            lapCuts = append(lapCuts, cylP(context, id + ("ltc" ~ i), vector(0 * mm, y, ztS), vector(0 * mm, y, ztTopOuter + 1 * mm), definition.clearDia / 2));
            lapCuts = append(lapCuts, cylP(context, id + ("ltp" ~ i), vector(0 * mm, y, rz1 + 0.8 * mm), vector(0 * mm, y, ztS + 0.1 * mm), definition.pilotDia / 2));
            lapCuts = append(lapCuts, coneP(context, id + ("lts" ~ i), vector(0 * mm, y, ztTopOuter - 1.7 * mm), vector(0 * mm, y, ztTopOuter + 0.2 * mm), definition.clearDia / 2, 3.3 * mm));
            i += 1;
        }
        const zBo = rz0 - wall;
        i = 0;
        for (var y in [5 * mm, backY / 2, backY - 4 * mm])
        {
            lapCuts = append(lapCuts, cylP(context, id + ("lbc" ~ i), vector(0 * mm, y, zBo - 1 * mm), vector(0 * mm, y, zbS), definition.clearDia / 2));
            lapCuts = append(lapCuts, cylP(context, id + ("lbp" ~ i), vector(0 * mm, y, zbS - 0.1 * mm), vector(0 * mm, y, zbTop - 0.8 * mm), definition.pilotDia / 2));
            lapCuts = append(lapCuts, coneP(context, id + ("lbs" ~ i), vector(0 * mm, y, zBo - 0.2 * mm), vector(0 * mm, y, zBo + 1.7 * mm), 3.3 * mm, definition.clearDia / 2));
            i += 1;
        }
        boolCut(context, id + "lapcut", H, lapCuts);


        // ================= rear shell (v6): one sculpted shell over the whole back =================
        // A low crowned hull over the full outline, a flared card pod on the left (the cards, their guides and a crowned
        // hatch), a tapered nacelle over the two inductors and the camera module, and a dished port well over the rear
        // connector group. Outer and inner skins are built from the same profiles (the inner inset by the wall) and
        // rounded, then the inner is taken out of the outer.
        const t = definition.coverT;
        var cxs = [];
        for (var x in CARD_X)
        {
            cxs = append(cxs, x * mm + definition.cardXOff);
        }
        const wt = definition.barT;
        const tp0 = cz1 + 0.5 * mm;                                    // top guide wall inner face, 0.5 mm past the card edge
        const tp1 = tp0 + wt;
        const bp1 = cz0 - 0.5 * mm;                                    // bottom guide wall inner face
        const bp0 = bp1 - wt;
        const rh = definition.slotD;
        const rwd = 1.2 * mm;
        const rimY = cy1 + 0.5 * mm;                                   // card bay rim = hatch underside, 0.5 mm over the card tops
        const hullY1 = backY + definition.hullH;
        const flare = definition.podFlare;

        // ---- card pod: cavity = the six cards, the audio card's RCA block past slot 1, the left-edge headers below ----
        const podIn = [cxs[0] - 0.8 * mm - 26.5 * mm - 1.5 * mm, cxs[size(cxs) - 1] + 10.8 * mm, cz0 - HDR_DROP * mm - 0.5 * mm, tp0, definition.podR - t];
        const P2 = rrGrow(podIn, t);
        const podYs = [backY, backY + 0.5 * (rimY - backY), rimY];
        const podO = rrLoft(context, id + "podO", [rrGrow(P2, flare), rrGrow(P2, flare * 0.45), P2], podYs);
        const podI = rrLoft(context, id + "podI", [rrGrow(podIn, flare), rrGrow(podIn, flare * 0.45), podIn], [backY - 1 * mm, podYs[1], rimY + 1 * mm]);

        // ---- camera module on J8 (right-angle pins, standing straight back) ----
        const camX = (CAM[0] - 184.5) * mm;
        const camZp = bcz + (82.73 - 6.23 - CAM[1]) * mm;             // centre between J8's two pin rows
        const sg = definition.camFaceDown ? -1 : 1;                   // which way the module's parts face (Z)
        const mFace = camZp - sg * 2.54 * mm;                          // parts face (the PCB sits beside the pin rows, like the cards)
        const mBack = camZp - sg * 4.14 * mm;
        const mzA = mFace + sg * definition.rcaBody;                   // RCA body / parts side
        const mzB = mBack - sg * 2.5 * mm;                             // solder tails
        const mz0 = min(mzA, mzB);
        const mz1 = max(mzA, mzB);
        const mX0 = camX - definition.camW / 2;
        const mX1 = camX + definition.camW / 2;
        const modY0 = bBack + definition.camLift;
        const modY1 = modY0 + definition.camL;
        const rcaZ = mFace + sg * definition.rcaAx;
        // inductors L1 / L2 (KiCad sheet x0, x1, y0, y1, height)
        const iX0 = (IND[0] - 184.5) * mm;
        const iX1 = (IND[1] - 184.5) * mm;
        const iZ0 = bcz + (82.73 - 6.23 - IND[3]) * mm;
        const iZ1 = bcz + (82.73 - 6.23 - IND[2]) * mm;
        const n2Y = modY1 + 1 * mm + t;
        const n1Y = bBack + IND[4] * mm + 1.5 * mm + t;
        const N2 = [mX0 - 1 * mm - t, mX1 + 1 * mm + t, mz0 - 1 * mm - t, mz1 + 1 * mm + t, 9 * mm];
        const N1 = [min(iX0, mX0) - 1.5 * mm - t, max(iX1, mX1) + 1.5 * mm + t, min(iZ0, mz0) - 1.5 * mm - t, max(iZ1, mz1) + 1.5 * mm + t, 14 * mm];
        const N0 = rrGrow(N1, 8 * mm);
        const nacO = rrLoft(context, id + "nacO", [N0, N1, N2], [backY, n1Y, n2Y]);
        const nacI = rrLoft(context, id + "nacI", [rrGrow(N0, -t), rrGrow(N1, -t), rrGrow(N2, -t)], [backY - 1 * mm, n1Y - t, n2Y - t]);

        // ---- hull: the housing outline, crowned both ways ----
        const hullO = outlineBody(context, id + "hO", screens, rx, rz0, rz1, wall, 0 * mm, backY, hullY1);
        const hullI = outlineBody(context, id + "hI", screens, rx, rz0, rz1, wall, -t, backY - 1 * mm, hullY1);
        const aH = 72 * mm;
        const sH = definition.hullCrown;
        const zmH = (rz0 + rz1) / 2;
        const RA = (aH * aH + sH * sH) / (2 * sH);
        const bH = 190 * mm;
        const sHx = definition.hullCrown * 0.8;
        const RB = (bH * bH + sHx * sHx) / (2 * sHx);
        clipTo(context, id + "hcA", hullO, cylP(context, id + "hcAc", vector(-1 * meter, hullY1 - RA, zmH), vector(1 * meter, hullY1 - RA, zmH), RA));
        clipTo(context, id + "hcB", hullO, cylP(context, id + "hcBc", vector(0 * mm, hullY1 - RB, -1 * meter), vector(0 * mm, hullY1 - RB, 1 * meter), RB));
        clipTo(context, id + "hcAi", hullI, cylP(context, id + "hcAic", vector(-1 * meter, hullY1 - RA, zmH), vector(1 * meter, hullY1 - RA, zmH), RA - t));
        clipTo(context, id + "hcBi", hullI, cylP(context, id + "hcBic", vector(0 * mm, hullY1 - RB, -1 * meter), vector(0 * mm, hullY1 - RB, 1 * meter), RB - t));
        opDeleteBodies(context, id + "hcdel", { "entities" : qUnion([qCreatedBy(id + "hcAc", EntityType.BODY), qCreatedBy(id + "hcBc", EntityType.BODY),
                    qCreatedBy(id + "hcAic", EntityType.BODY), qCreatedBy(id + "hcBic", EntityType.BODY)]) });

        // keep the pod inside the bezel's silhouette
        clipTo(context, id + "podclip", podO, outlineBody(context, id + "olO", screens, rx, rz0, rz1, wall, 0 * mm, backY - 2 * mm, 400 * mm));
        clipTo(context, id + "podclipi", podI, outlineBody(context, id + "olI", screens, rx, rz0, rz1, wall, -t, backY - 2 * mm, 400 * mm));
        opDeleteBodies(context, id + "oldel", { "entities" : qUnion([qCreatedBy(id + "olO", EntityType.BODY), qCreatedBy(id + "olI", EntityType.BODY)]) });

        boolAdd(context, id + "Ou", hullO, [podO, nacO]);
        boolAdd(context, id + "Iu", hullI, [podI, nacI]);
        const O = hullO;
        const I = hullI;
        softEdges(context, id + "Of", O, [backY, rimY], definition.shellR);
        softEdges(context, id + "If", I, [backY - 1 * mm, rimY + 1 * mm], max(definition.shellR - t, 0.6 * mm));
        const Oc = copyBody(context, id + "Oc", O);
        boolCut(context, id + "shell", O, [I]);
        const S = O;

        // ---- port well: a dished pocket over the rear connector group, with a floor just over the connectors ----
        var upper = [];
        var lower = [];
        for (var j in concatenateArrays([JST, PORTS]))
        {
            const b = [(j[0] - 184.5) * mm, (j[1] - 184.5) * mm, bcz + (82.73 - 6.23 - j[3]) * mm, bcz + (82.73 - 6.23 - j[2]) * mm];
            if (b[2] > N0[3])
                upper = append(upper, b);
            else
                lower = append(lower, b);
        }
        const gm = definition.portMargin + 2 * mm;
        const R1 = rrGrow(append(bbox(upper), 6 * mm), gm);
        const lb = rrGrow(append(bbox(lower), 6 * mm), gm);
        const R2 = [lb[0], max(lb[1], R1[1]), lb[2], R1[3], 6 * mm];
        const floorIn = bBack + definition.wellFloor;
        const floorOut = floorIn + t;
        var wellAdd = [rrExtrude(context, id + "wr1", rrGrow(R1, t), floorIn, 300 * mm), rrExtrude(context, id + "wr2", rrGrow(R2, t), floorIn, 300 * mm)];
        boolAdd(context, id + "wru", wellAdd[0], [wellAdd[1]]);
        const Oc2 = copyBody(context, id + "Oc2", Oc);
        clipTo(context, id + "wrc", wellAdd[0], Oc2);
        opDeleteBodies(context, id + "Oc2del", { "entities" : Oc2 });
        boolAdd(context, id + "wadd", S, [wellAdd[0]]);
        const W = rrExtrude(context, id + "w1", R1, floorOut, 300 * mm);
        boolAdd(context, id + "wu", W, [rrExtrude(context, id + "w2", R2, floorOut, 300 * mm)]);
        try silent
        {
            opFillet(context, id + "wf", { "entities" : qCoincidesWithPlane(qOwnedByBody(W, EntityType.EDGE), rrPlane(floorOut)), "radius" : 4 * mm });
        }
        boolCut(context, id + "wcut", S, [W]);

        // ---- card guides: thin walls on the card ends with a rail pair per card, an index rib per slot ----
        var g = [mkBox(context, id + "gtop", podIn[0], podIn[1], backY, rimY, tp0, tp1),
                 mkBox(context, id + "gbot", podIn[0], podIn[1], backY, rimY, bp0, bp1)];
        i = 0;
        for (var x in cxs)
        {
            for (var sd in [-1, 1])
            {
                const r0 = x + sd * definition.slotW / 2;
                const r1 = r0 + sd * rwd;
                g = append(g, mkBox(context, id + ("grt" ~ i), min(r0, r1), max(r0, r1), backY, rimY, tp0 - rh, tp1));
                g = append(g, mkBox(context, id + ("grb" ~ i), min(r0, r1), max(r0, r1), backY, rimY, bp0, bp1 + rh));
                i += 1;
            }
            i += 1;
        }
        // Index ribs. A card plugged in back to front (parts facing away from slot 1) can only meet its socket with its
        // body 3.34 mm on the other side of the pin row, i.e. centred 6.68 mm left of the guide. A rib on the top wall fills
        // that spot in every slot, so the reversed card cannot reach its socket; it is 4 mm clear of a correct card's
        // parts face. (On the bottom wall the left-edge header notch is there.)
        i = 0;
        for (var x in cxs)
        {
            const xr = x - 6.68 * mm;
            g = append(g, mkBox(context, id + ("gix" ~ i), xr - 1.8 * mm, xr + 1.8 * mm, backY, rimY, tp0 - rh, tp1));
            i += 1;
        }
        // webs from the bottom guide wall down to the pod skin, between the header notches
        var webs = [];
        for (var k = 0; k <= size(cxs); k += 1)
        {
            const xl = k == 0 ? podIn[0] + 1 * mm : cxs[k - 1] + HDR_X1 * mm + 1 * mm;
            const xh = k == size(cxs) ? podIn[1] - 1 * mm : cxs[k] + HDR_X0 * mm - 1 * mm;
            const xc = (xl + xh) / 2;
            webs = append(webs, mkBox(context, id + ("gw" ~ k), xc - 0.8 * mm, xc + 0.8 * mm, backY + 3 * mm, rimY, podIn[2] - flare - t, bp0 + 0.1 * mm));
        }
        // and from the top guide wall up to the skin, between each slot's rails and the next slot's index rib
        for (var k = 0; k < size(cxs); k += 1)
        {
            const xc = k < size(cxs) - 1 ? cxs[k] + 9.6 * mm : (cxs[k] + 3.3 * mm + podIn[1]) / 2;
            webs = append(webs, mkBox(context, id + ("gwt" ~ k), xc - 0.8 * mm, xc + 0.8 * mm, backY + 3 * mm, rimY, tp1 - 0.1 * mm, tp1 + flare + t));
        }
        // hatch screw bosses in the pod's corners, outside the cards (45 deg cone underneath)
        const hb = [[podIn[0] + 3 * mm, tp0 - 3 * mm], [podIn[0] + 3 * mm, podIn[2] + 3 * mm], [podIn[1] - 3 * mm, tp0 - 3 * mm], [podIn[1] - 3 * mm, podIn[2] + 3 * mm]];
        i = 0;
        for (var p in hb)
        {
            g = append(g, cylY(context, id + ("hbb" ~ i), p[0], p[1], rimY - 12 * mm, rimY, 3.5 * mm));
            g = append(g, coneY(context, id + ("hbc" ~ i), p[0], p[1], rimY - 15.5 * mm, rimY - 12 * mm, 0.05 * mm, 3.5 * mm));
            i += 1;
        }
        // camera module: a slot in the nacelle roof that takes the module's top edge (either side of the RCA jack)
        const bLo = min(mFace, mBack);
        const bHi = max(mFace, mBack);
        i = 0;
        for (var xs in [[mX0 + 0.5 * mm, mX0 + 6 * mm], [mX1 - 6 * mm, mX1 - 0.5 * mm]])
        {
            g = append(g, mkBox(context, id + ("mgA" ~ i), xs[0], xs[1], modY1 - 2.5 * mm, n2Y - t + 0.5 * mm, bLo - 0.3 * mm - 1.5 * mm, bLo - 0.3 * mm));
            g = append(g, mkBox(context, id + ("mgB" ~ i), xs[0], xs[1], modY1 - 2.5 * mm, n2Y - t + 0.5 * mm, bHi + 0.3 * mm, bHi + 0.3 * mm + 1.5 * mm));
            i += 1;
        }
        // screw towers down to the housing's cover bosses, outside the pod, nacelle and port well
        var towerPts = [];
        for (var p in concatenateArrays([pts, cageBosses]))
        {
            var free = true;
            for (var f in [rrGrow(podIn, t + 5 * mm), rrGrow(N0, 3 * mm), rrGrow(R1, t + 5 * mm), rrGrow(R2, t + 5 * mm)])
            {
                if (p[0] > f[0] && p[0] < f[1] && p[1] > f[2] && p[1] < f[3])
                    free = false;
            }
            if (free)
                towerPts = append(towerPts, p);
        }
        i = 0;
        var towers = [];
        for (var p in towerPts)
        {
            towers = append(towers, cylY(context, id + ("towers" ~ i), p[0], p[1], backY, hullY1 + 5 * mm, 4.5 * mm));
            i += 1;
        }
        // bezel-screw access channels' walls
        const ar = definition.mountAccessD / 2;
        var scuts = [];
        var mai = 0;
        for (var mq in [[-1, -1], [1, -1], [-1, 1], [1, 1]])
        {
            const xc = mq[0] * (rx + wall - definition.mountInset);
            const zb = sz + mq[1] * definition.mountSpan / 2;
            towers = append(towers, cylY(context, id + ("cwa" ~ mai), xc - tr, zb, backY, hullY1 + 5 * mm, ar + 1.6 * mm));
            towers = append(towers, cylY(context, id + ("cwb" ~ mai), xc + tr, zb, backY, hullY1 + 5 * mm, ar + 1.6 * mm));
            towers = append(towers, mkBox(context, id + ("cwc" ~ mai), xc - tr, xc + tr, backY, hullY1 + 5 * mm, zb - ar - 1.6 * mm, zb + ar + 1.6 * mm));
            scuts = append(scuts, cylY(context, id + ("cma" ~ mai), xc - tr, zb, backY - 1 * mm, 300 * mm, ar));
            scuts = append(scuts, cylY(context, id + ("cmb" ~ mai), xc + tr, zb, backY - 1 * mm, 300 * mm, ar));
            scuts = append(scuts, mkBox(context, id + ("cmc" ~ mai), xc - tr, xc + tr, backY - 1 * mm, 300 * mm, zb - ar, zb + ar));
            mai += 1;
        }
        towers = concatenateArrays([towers, webs]);
        for (var k = 0; k < size(towers); k += 1)
        {
            clipTo(context, id + ("twc" ~ k), towers[k], Oc);
        }
        boolAdd(context, id + "gu", S, concatenateArrays([g, towers]));

        // ---- openings ----
        i = 0;
        for (var p in towerPts)
        {
            scuts = append(scuts, cylY(context, id + ("th" ~ i), p[0], p[1], backY - 1 * mm, 300 * mm, definition.clearDia / 2));
            scuts = append(scuts, cylY(context, id + ("tcb" ~ i), p[0], p[1], backY + 3 * mm, 300 * mm, 3.2 * mm));
            i += 1;
        }
        // the cards' left-edge headers slide down notches in the bottom guide wall and its rails
        i = 0;
        for (var x in cxs)
        {
            scuts = append(scuts, mkBox(context, id + ("gnh" ~ i), x + HDR_X0 * mm, x + HDR_X1 * mm, bBack + HDR_Z0 * mm, rimY + 1 * mm, bp0 - 1 * mm, bp1 + rh + 1 * mm));
            i += 1;
        }
        // keep the inductor L1 clear where it passes under the pod's right end
        scuts = append(scuts, mkBox(context, id + "l1clr", iX0 - 1.5 * mm, iX1 + 1.5 * mm, backY - 1 * mm, bBack + IND[4] * mm + 1.5 * mm, iZ0 - 1.5 * mm, iZ1 + 1.5 * mm));
        // hatch boss pilots
        i = 0;
        for (var p in hb)
        {
            scuts = append(scuts, cylY(context, id + ("hbp" ~ i), p[0], p[1], rimY - 10 * mm, rimY + 1 * mm, definition.pilotDia / 2));
            i += 1;
        }
        // rear connectors (through the well floor)
        const pm = definition.portMargin;
        i = 0;
        for (var j in concatenateArrays([JST, PORTS]))
        {
            scuts = append(scuts, mkBox(context, id + ("cp" ~ i), (j[0] - 184.5) * mm - pm, (j[1] - 184.5) * mm + pm, backY - 1 * mm, 300 * mm,
                        bcz + (82.73 - 6.23 - j[3]) * mm - pm, bcz + (82.73 - 6.23 - j[2]) * mm + pm));
            i += 1;
        }
        // camera RCA through the nacelle's end
        scuts = append(scuts, cylY(context, id + "crca", camX, rcaZ, n2Y - t - 1 * mm, 300 * mm, definition.rcaDia / 2));
        boolCut(context, id + "scut", S, scuts);
        opDeleteBodies(context, id + "Ocdel", { "entities" : Oc });

        // ---- card bay hatch: crowned both ways, screwed at the pod's four corners, cut for each slot's connectors ----
        const hTop = rimY + t + definition.hatchCrown * 1.5;
        const Hh = rrExtrude(context, id + "hat", P2, rimY, hTop + 1 * mm);
        clipTo(context, id + "hatclip", Hh, outlineBody(context, id + "olH", screens, rx, rz0, rz1, wall, 0 * mm, backY, 400 * mm));
        opDeleteBodies(context, id + "olHdel", { "entities" : qCreatedBy(id + "olH", EntityType.BODY) });
        const aZ = (P2[3] - P2[2]) / 2;
        const sZh = definition.hatchCrown;
        const zmh = (P2[2] + P2[3]) / 2;
        const RAh = (aZ * aZ + sZh * sZh) / (2 * sZh);
        const bX = (P2[1] - P2[0]) / 2;
        const sXh = definition.hatchCrown * 0.5;
        const xmh = (P2[0] + P2[1]) / 2;
        const RBh = (bX * bX + sXh * sXh) / (2 * sXh);
        clipTo(context, id + "hA", Hh, cylP(context, id + "hAc", vector(-1 * meter, hTop - RAh, zmh), vector(1 * meter, hTop - RAh, zmh), RAh));
        clipTo(context, id + "hB", Hh, cylP(context, id + "hBc", vector(xmh, hTop - RBh, -1 * meter), vector(xmh, hTop - RBh, 1 * meter), RBh));
        opDeleteBodies(context, id + "hcdl", { "entities" : qUnion([qCreatedBy(id + "hAc", EntityType.BODY), qCreatedBy(id + "hBc", EntityType.BODY)]) });
        softEdges(context, id + "hf", Hh, [rimY], 4 * mm);
        // hollow it: a pocket under the crown, offset by the wall, leaving a 3 mm flat band on the pod rim
        const Hfull = copyBody(context, id + "hfull", Hh);
        const hp = rrExtrude(context, id + "hpk", rrGrow(P2, -t - 3 * mm), rimY - 1 * mm, hTop);
        clipTo(context, id + "hpA", hp, cylP(context, id + "hpAc", vector(-1 * meter, hTop - RAh, zmh), vector(1 * meter, hTop - RAh, zmh), RAh - t));
        clipTo(context, id + "hpB", hp, cylP(context, id + "hpBc", vector(xmh, hTop - RBh, -1 * meter), vector(xmh, hTop - RBh, 1 * meter), RBh - t));
        opDeleteBodies(context, id + "hpdl", { "entities" : qUnion([qCreatedBy(id + "hpAc", EntityType.BODY), qCreatedBy(id + "hpBc", EntityType.BODY)]) });
        boolCut(context, id + "hpcut", Hh, [hp]);
        // ribs that bear on each card's top edge (on its back half, clear of the parts face) and pads round the screws
        var hr = [];
        i = 0;
        for (var x in cxs)
        {
            hr = append(hr, mkBox(context, id + ("hrb" ~ i), x - 0.5 * mm, x + 2.5 * mm, cy1 + 0.3 * mm, hTop + 1 * mm, cz0 + 1 * mm, cz1 - 1 * mm));
            i += 1;
        }
        i = 0;
        for (var p in hb)
        {
            hr = append(hr, cylY(context, id + ("hpd" ~ i), p[0], p[1], rimY, hTop + 1 * mm, 4.2 * mm));
            i += 1;
        }
        for (var k = 0; k < size(hr); k += 1)
        {
            clipTo(context, id + ("hrc" ~ k), hr[k], Hfull);
        }
        boolAdd(context, id + "hru", Hh, hr);
        opDeleteBodies(context, id + "hfdel", { "entities" : Hfull });
        var hcuts = [];
        const slotCards = [definition.slot1, definition.slot2, definition.slot3, definition.slot4, definition.slot5, definition.slot6];
        i = 0;
        for (var x in cxs)
        {
            const card = slotCards[i];
            var k = 0;
            for (var co in cardCutouts(card))
            {
                hcuts = append(hcuts, mkBox(context, id + ("hco" ~ i ~ "x" ~ k), x + co[2] * mm - pm, x + co[3] * mm + pm,
                            rimY - 1 * mm, 300 * mm, zOfY(cz0, co[1]) - pm, zOfY(cz0, co[0]) + pm));
                k += 1;
            }
            i += 1;
        }
        i = 0;
        for (var p in hb)
        {
            hcuts = append(hcuts, cylY(context, id + ("hh" ~ i), p[0], p[1], rimY - 1 * mm, 300 * mm, definition.clearDia / 2));
            i += 1;
        }
        boolCut(context, id + "hcut", Hh, hcuts);
        setProperty(context, { "entities" : Hh, "propertyType" : PropertyType.NAME, "value" : "Card bay hatch" });

        // ---- split: the housing halves at X 0, the rear shell at the cover split ----
        const bands = [[-big, zbS, lw], [zbS, zbTop, -lw], [zbTop, rz1, 0 * mm], [rz1, ztS, -lw], [ztS, big, lw]];
        var rightBoxes = [];
        var leftBoxes = [];
        i = 0;
        for (var b in bands)
        {
            rightBoxes = append(rightBoxes, mkBox(context, id + ("kr" ~ i), b[2], big, -big, big, b[0], b[1]));
            leftBoxes = append(leftBoxes, mkBox(context, id + ("kl" ~ i), -big, b[2], -big, big, b[0], b[1]));
            i += 1;
        }
        const Hr = copyBody(context, id + "Hr", H);
        boolCut(context, id + "Hcut", H, rightBoxes);
        boolCut(context, id + "Hrcut", Hr, leftBoxes);
        const cs = definition.coverSplit;
        const Sr = copyBody(context, id + "Sr", S);
        boolCut(context, id + "Scut", S, [mkBox(context, id + "k3", cs, big, -big, big, -big, big)]);
        boolCut(context, id + "Srcut", Sr, [mkBox(context, id + "k6", -big, cs, -big, big, -big, big)]);

        setProperty(context, { "entities" : H, "propertyType" : PropertyType.NAME, "value" : "Housing left half" });
        setProperty(context, { "entities" : Hr, "propertyType" : PropertyType.NAME, "value" : "Housing right half" });
        setProperty(context, { "entities" : S, "propertyType" : PropertyType.NAME, "value" : "Rear shell left (card pod)" });
        setProperty(context, { "entities" : Sr, "propertyType" : PropertyType.NAME, "value" : "Rear shell right (nacelle, port well)" });

        // ---------------- reference geometry ----------------
        var refs = [];
        i = 0;
        for (var s in screens)
        {
            const d = (i == 1 ? definition.centerD : definition.sideD);
            const scr = cylY(context, id + ("rs" ~ i), s[0], s[1], y0 - definition.proud, pD, d / 2);
            const pl = mkBox(context, id + ("rp" ~ i), s[0] - PLATE_W / 2, s[0] + PLATE_W / 2, pD, pD + SCREEN_BACK, s[1] - PLATE_H / 2, s[1] + PLATE_H / 2);
            var sb = [pl];
            var jb = 0;
            for (var q in (i == 1 ? [] : [[1, 1], [1, -1], [-1, 1], [-1, -1]]))
            {
                sb = append(sb, cylY(context, id + ("rsb" ~ i ~ "x" ~ jb), s[0] + q[0] * M4_PITCH, s[1] + q[1] * M4_PITCH, pD - 0.01 * mm, pD + SCREEN_BOSS_H, SCREEN_BOSS_R));
                jb += 1;
            }
            boolAdd(context, id + ("ru" ~ i), scr, sb);
            setProperty(context, { "entities" : scr, "propertyType" : PropertyType.NAME, "value" : "REF screen " ~ (i == 1 ? "centre" : (i == 0 ? "left" : "right")) });
            refs = append(refs, scr);
            i += 1;
        }
        i = 0;
        for (var x in cxs)
        {
            const card = mkBox(context, id + ("rcard" ~ i), x - definition.cardT / 2, x + definition.cardT / 2, cy0, cy1, cz0, cz1);
            var cp = [];
            var k = 0;
            for (var p in cardParts(slotCards[i]))
            {
                if (size(p) == 5)
                    cp = append(cp, cylY(context, id + ("rcp" ~ i ~ "x" ~ k), x + p[1] * mm, zOfY(cz0, p[0]), cy1 - 0.1 * mm, cy1 + p[3] * mm, p[2] * mm));
                else
                    cp = append(cp, mkBox(context, id + ("rcp" ~ i ~ "x" ~ k), x + p[2] * mm, x + p[3] * mm, cy1 - p[4] * mm, cy1 + p[5] * mm,
                                zOfY(cz0, p[1]), zOfY(cz0, p[0])));
                k += 1;
            }
            if (size(cp) > 0)
                boolAdd(context, id + ("rcpu" ~ i), card, cp);
            setProperty(context, { "entities" : card, "propertyType" : PropertyType.NAME, "value" : "REF card " ~ (i + 1) });
            refs = append(refs, card);
            i += 1;
        }
        // camera module: PCB and its right-angle RCA jack
        const mp = mkBox(context, id + "rcam", mX0, mX1, modY0, modY1, bLo, bHi);
        const mr = cylY(context, id + "rcamj", camX, rcaZ, modY1 - definition.rcaBody, modY1 + definition.rcaNose, 4.2 * mm);
        const mrb = mkBox(context, id + "rcamb", camX - definition.rcaBody / 2, camX + definition.rcaBody / 2, modY1 - definition.rcaBody, modY1 - 0.5 * mm,
                    min(mFace, mFace + sg * definition.rcaBody), max(mFace, mFace + sg * definition.rcaBody));
        boolAdd(context, id + "rcamu", mp, [mr, mrb]);
        setProperty(context, { "entities" : mp, "propertyType" : PropertyType.NAME, "value" : "REF camera module" });
        refs = append(refs, mp);
        // the KiCad board model (boardmodel-v4), placed on the board's position
        if (definition.showBoard)
        {
            const inst = newInstantiator(id + "brd");
            addInstance(inst, BoardV4::build, { "transform" : transform(rotationMatrix3d(vector(1, 0, 0), 90 * degree), vector(-184.5 * mm, bBack, bcz + 76.5 * mm)) });
            instantiate(context, inst);
            if (!definition.showRef)
                opDeleteBodies(context, id + "delbrd", { "entities" : qCreatedBy(id + "brd", EntityType.BODY) });
        }
        else
        {
            const board = mkBox(context, id + "rboard", -bx1, bx1, bFront, bBack, bz0, bz1);
            setProperty(context, { "entities" : board, "propertyType" : PropertyType.NAME, "value" : "REF board" });
            refs = append(refs, board);
        }

        if (!definition.showRef)
            opDeleteBodies(context, id + "delref", { "entities" : qUnion(refs) });
    });
