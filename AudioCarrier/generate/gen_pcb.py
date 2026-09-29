#!/usr/bin/env python3
"""Build AudioCarrier.kicad_pcb from the schematic's netlist with KiCad's pcbnew module.

Run with the system python that has pcbnew (KiCad 7 or newer):  /usr/bin/python3 gen_pcb.py [--route FREEROUTING_JAR]
Places the footprints, adds the outline, mounting holes, a GND pour and the antenna keep-out. With --route it also
runs Freerouting over a Specctra export and imports the result.
"""
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

import pcbnew
from pcbnew import FromMM, VECTOR2I, EDA_ANGLE

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
FPDIR = os.environ.get('KICAD_FOOTPRINTS', '/usr/share/kicad/footprints')
SCH = os.path.join(ROOT, 'AudioCarrier.kicad_sch')
PCB = os.path.join(ROOT, 'AudioCarrier.kicad_pcb')
mm = FromMM


def P(x, y):
    return VECTOR2I(mm(x), mm(y))


def netlist():
    xml = '/tmp/audiocarrier_netlist.xml'
    subprocess.check_call(['kicad-cli', 'sch', 'export', 'netlist', '--format', 'kicadxml', '-o', xml, SCH])
    r = ET.parse(xml).getroot()
    comps = {}
    for c in r.find('components'):
        comps[c.get('ref')] = (c.findtext('value'), c.findtext('footprint'))
    nets = {}
    for n in r.find('nets'):
        for node in n:
            nets[(node.get('ref'), node.get('pin'))] = n.get('name')
    return comps, nets


def carrier_header():
    """Carrier_Header_2x05 (docs/hardware/accessory_ports.md) built here: the stock KiCad 7 parser cannot read the
    KiCad 9 file in DigiDash_Accessory.pretty. Same pads, same asymmetric M3 holes."""
    fp = pcbnew.FOOTPRINT(None)
    fp.SetFPID(pcbnew.LIB_ID('DigiDash_Accessory', 'Carrier_Header_2x05'))
    fp.SetAttributes(pcbnew.FP_THROUGH_HOLE)

    def pad(name, x, y, shape, size, drill, ptype=pcbnew.PAD_ATTRIB_PTH):
        p = pcbnew.PAD(fp)
        p.SetName(name)
        p.SetAttribute(ptype)
        p.SetShape(shape)
        p.SetSize(VECTOR2I(mm(size), mm(size)))
        p.SetDrillSize(VECTOR2I(mm(drill), mm(drill)))
        p.SetLayerSet(p.PTHMask())
        p.SetPosition(P(x, y))
        p.SetPos0(P(x, y))
        fp.Add(p)

    for i in range(10):
        pad(str(i + 1), 2.54 * (i % 2), 2.54 * (i // 2),
            pcbnew.PAD_SHAPE_RECT if i == 0 else pcbnew.PAD_SHAPE_OVAL, 1.7, 1.0)
    pad('MP1', 1.27, -15.24, pcbnew.PAD_SHAPE_CIRCLE, 6.4, 3.2)
    pad('MP2', 8.89, 25.4, pcbnew.PAD_SHAPE_CIRCLE, 6.4, 3.2)
    for x1, y1, x2, y2, lay, w in [(-1.77, -1.77, 4.31, 11.93, pcbnew.F_CrtYd, 0.05), (-1.38, -1.38, 3.92, 11.54, pcbnew.F_SilkS, 0.12)]:
        r = pcbnew.FP_SHAPE(fp)
        r.SetShape(pcbnew.SHAPE_T_RECT)
        r.SetStart(P(x1, y1))
        r.SetEnd(P(x2, y2))
        r.SetStart0(P(x1, y1))
        r.SetEnd0(P(x2, y2))
        r.SetLayer(lay)
        r.SetWidth(mm(w))
        fp.Add(r)
    return fp


def load(fpid):
    lib, name = fpid.split(':')
    if lib == 'DigiDash_Accessory':
        return carrier_header()
    path = os.path.join(ROOT, 'AudioCarrier.pretty') if lib == 'AudioCarrier' else os.path.join(FPDIR, lib + '.pretty')
    fp = pcbnew.FootprintLoad(path, name)
    assert fp, fpid
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


# Fixed parts: ref -> (x, y, rotation). Everything else is packed into its group's region (mm, board coordinates).
W, H = 104.0, 70.0
FIXED = {
    'J1': (9, 22, 0), 'U1': (94.25, 14, 270), 'U7': (34, 9, 0),
    'U2': (34, 22, 0), 'J10': (25.5, 31, 0),
    'J6': (10, H - 4.5, 0), 'U3': (36, 60, 0),
    'U5': (58, 32, 0), 'U6': (85, 32, 0),
    'J2': (55, 54, 0), 'J3': (68.5, 54, 0), 'J4': (82, 54, 0), 'J5': (95.5, 54, 0),
    'U4': (55, 65, 0),
}
REGIONS = {
    'power': (24, 2, 66, 15), 'can': (38, 17, 66, 29), 'boot': (29, 30, 50, 47),
    'mic': (22, 50, 46, 69), 'dac_f': (47, 25, 71, 47), 'dac_r': (73, 25, 102.5, 47),
    'adc': (52, 60, 98, 69.5),
}
HOLES = [(100.3, 66.3), (47.5, 66.3)]


def bbox_key(ref):
    return ref


def bbox_mm(fp):
    b = fp.GetBoundingBox(False, False)
    return (b.GetLeft() / 1e6, b.GetTop() / 1e6, b.GetRight() / 1e6, b.GetBottom() / 1e6)


def overlap(a, b, m=0.35):
    return not (a[2] + m <= b[0] or b[2] + m <= a[0] or a[3] + m <= b[1] or b[3] + m <= a[1])


def import_ses(b, path):
    """Import a Freerouting .ses (pcbnew's own importer is not reachable from a standalone script)."""
    sys.path.insert(0, HERE)
    from sexp import parse
    t = parse(open(path).read())
    routes = [x for x in t if isinstance(x, list) and x[0] == 'routes'][0]
    res = [x for x in routes if isinstance(x, list) and x[0] == 'resolution'][0]
    per_um = float(res[2])            # units per um
    net_out = [x for x in routes if isinstance(x, list) and x[0] == 'network_out'][0]
    layers = {'F.Cu': pcbnew.F_Cu, 'B.Cu': pcbnew.B_Cu}

    def pt(x, y):                     # DSN y axis points up
        return VECTOR2I(int(float(x) / per_um * 1000), int(-float(y) / per_um * 1000))

    nt = 0
    nv = 0
    for net in net_out:
        if not (isinstance(net, list) and net[0] == 'net'):
            continue
        ni = b.FindNet(net[1])
        for item in net[2:]:
            if item[0] == 'wire':
                path_ = [x for x in item if isinstance(x, list) and x[0] == 'path'][0]
                lay, width = path_[1], float(path_[2]) / per_um * 1000
                xy = path_[3:]
                for i in range(0, len(xy) - 2, 2):
                    tr = pcbnew.PCB_TRACK(b)
                    tr.SetStart(pt(xy[i], xy[i + 1]))
                    tr.SetEnd(pt(xy[i + 2], xy[i + 3]))
                    tr.SetWidth(int(width))
                    tr.SetLayer(layers[lay])
                    tr.SetNet(ni)
                    b.Add(tr)
                    nt += 1
            elif item[0] == 'via':
                v = pcbnew.PCB_VIA(b)
                v.SetPosition(pt(item[2], item[3]))
                v.SetNet(ni)
                v.SetViaType(pcbnew.VIATYPE_THROUGH)
                v.SetWidth(mm(0.8))
                v.SetDrill(mm(0.4))
                b.Add(v)
                nv += 1
    print('imported', nt, 'track segments,', nv, 'vias')


def main():
    route = None
    if '--route' in sys.argv:
        route = sys.argv[sys.argv.index('--route') + 1]
    comps, nets = netlist()
    b = pcbnew.BOARD()
    ds = b.GetDesignSettings()
    ds.SetCopperLayerCount(2)
    ds.m_TrackMinWidth = mm(0.2)
    ds.m_MinThroughDrill = mm(0.2)
    ds.m_CopperEdgeClearance = mm(0.3)
    nc = ds.m_NetSettings.m_DefaultNetClass
    nc.SetClearance(mm(0.2))
    nc.SetTrackWidth(mm(0.3))
    nc.SetViaDiameter(mm(0.8))
    nc.SetViaDrill(mm(0.4))
    netobj = {}

    def net(name):
        if name not in netobj:
            n = pcbnew.NETINFO_ITEM(b, name)
            b.Add(n)
            netobj[name] = n
        return netobj[name]

    import json
    groups = json.load(open(os.path.join(HERE, 'refs.json')))
    placed = []     # bounding boxes of everything placed so far
    fps = {}
    for hx, hy in HOLES:
        placed.append((hx - 3.4, hy - 3.4, hx + 3.4, hy + 3.4))

    def build(ref):
        value, fpid = comps[ref]
        fp = load(fpid)
        fp.SetReference(ref)
        fp.SetValue(value)
        b.Add(fp)
        fp.Reference().SetLayer(pcbnew.F_SilkS)
        fp.Reference().SetTextSize(VECTOR2I(mm(0.8), mm(0.8)))
        fp.Reference().SetTextThickness(mm(0.12))
        fp.Value().SetLayer(pcbnew.F_Fab)
        for pad in fp.Pads():
            n = nets.get((ref, pad.GetName()))
            if pad.GetName() in ('MP1', 'MP2'):
                n = 'GND'
            if n:
                pad.SetNet(net(n))
        fps[ref] = fp
        return fp

    real = [r for r in comps if not r.startswith('#')]
    for ref in real:
        if ref in FIXED:
            x, y, rot = FIXED[ref]
            fp = build(ref)
            fp.SetPosition(P(x, y))
            if rot:
                fp.SetOrientation(EDA_ANGLE(rot, pcbnew.DEGREES_T))
            placed.append(bbox_mm(fp))
    rest = sorted([r for r in real if r not in FIXED], key=lambda r: (groups[r]['group'], -len(bbox_key(r)), r))
    for ref in rest:
        g = groups[ref]['group']
        x0, y0, x1, y1 = REGIONS[g]
        fp = build(ref)
        done = False
        yy = y0
        while yy <= y1 and not done:
            xx = x0
            while xx <= x1 and not done:
                fp.SetPosition(P(xx, yy))
                bb = bbox_mm(fp)
                if bb[0] >= x0 - 0.5 and bb[2] <= x1 + 0.5 and bb[1] >= y0 - 0.5 and bb[3] <= y1 + 0.5 and \
                        not any(overlap(bb, o) for o in placed):
                    placed.append(bb)
                    done = True
                xx += 0.5
            yy += 0.5
        if not done:
            raise SystemExit('no room for %s in region %s' % (ref, g))
    # extra mounting holes to GND
    for i, (hx, hy) in enumerate(HOLES):
        fp = pcbnew.FootprintLoad(os.path.join(FPDIR, 'MountingHole.pretty'), 'MountingHole_3.2mm_M3_Pad')
        fp.SetReference('H%d' % (i + 3))
        b.Add(fp)
        fp.SetPosition(P(hx, hy))
        for pad in fp.Pads():
            pad.SetNet(net('GND'))
    # outline
    # outline; the bottom edge leaves a gap for the TRS jack footprint, which carries its own notch on Edge.Cuts
    jx = FIXED['J6'][0]
    segs = [((0, 0), (W, 0)), ((W, 0), (W, H)), ((W, H), (jx + 6.5, H)), ((jx - 6.5, H), (0, H)), ((0, H), (0, 0))]
    for a_, b_ in segs:
        sh = pcbnew.PCB_SHAPE(b)
        sh.SetShape(pcbnew.SHAPE_T_SEGMENT)
        sh.SetLayer(pcbnew.Edge_Cuts)
        sh.SetWidth(mm(0.05))
        sh.SetStart(P(*a_))
        sh.SetEnd(P(*b_))
        b.Add(sh)
    # GND pours
    for lay in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(b)
        z.SetLayer(lay)
        z.SetNet(net('GND'))
        z.SetMinThickness(mm(0.25))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        o = z.Outline()
        o.NewOutline()
        for (x, y) in [(0.3, 0.3), (W - 0.3, 0.3), (W - 0.3, H - 0.3), (0.3, H - 0.3)]:
            o.Append(mm(x), mm(y))
        b.Add(z)
    # no copper pour under the antenna end of the DevKit (it hangs past the right edge)
    k = pcbnew.ZONE(b)
    k.SetIsRuleArea(True)
    k.SetDoNotAllowCopperPour(True)
    k.SetDoNotAllowTracks(False)
    k.SetDoNotAllowVias(False)
    k.SetDoNotAllowPads(False)
    k.SetDoNotAllowFootprints(False)
    ls = pcbnew.LSET()
    ls.AddLayer(pcbnew.F_Cu)
    ls.AddLayer(pcbnew.B_Cu)
    k.SetLayerSet(ls)
    o = k.Outline()
    o.NewOutline()
    for (x, y) in [(96, 0), (W, 0), (W, 24), (96, 24)]:
        o.Append(mm(x), mm(y))
    b.Add(k)

    pcbnew.SaveBoard(PCB, b)
    if route:
        dsn, ses = '/tmp/audiocarrier.dsn', '/tmp/audiocarrier.ses'
        b2 = pcbnew.LoadBoard(PCB)          # route without the pours: Freerouting handles GND as a normal net, the pour is refilled after
        for z in list(b2.Zones()):
            b2.Remove(z)
        pcbnew.ExportSpecctraDSN(b2, dsn)
        subprocess.check_call(['xvfb-run', '-a', 'java', '-jar', route, '-de', dsn, '-do', ses, '-mp', '100', '-mt', '1'])
        b = pcbnew.LoadBoard(PCB)
        import_ses(b, ses)
        pcbnew.SaveBoard(PCB, b)
    b = pcbnew.LoadBoard(PCB)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(PCB, b)
    print('wrote', PCB)


main()
