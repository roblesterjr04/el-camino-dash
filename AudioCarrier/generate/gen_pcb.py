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
W, H = 120.0, 84.0
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


# ref -> (x, y, rotation degrees)
XK = [34, 56, 78, 100]
POS = {
    'J1': (9, 19.5, 0),
    'U1': (118, 5, 270),
    'U2': (30, 8, 0), 'R1': (44, 10, 0), 'C2': (44, 15, 0), 'C3': (44, 20, 0),
    'D1': (52, 6, 0), 'C1': (56, 20, 0),
    'JP1': (4, 58, 0), 'JP2': (10, 58, 0), 'JP3': (4, 70, 0),
}
CHAN = ['FL', 'FR', 'RL', 'RR']
for k in range(4):
    x = XK[k]
    POS['U%d' % (3 + k)] = (x, 42, 0)
    POS['J%d' % (2 + k)] = (x + 5, 44, 0)
    POS['C%d' % (10 + k)] = (x + 10, 46, 0)
    if k in (1, 3):
        POS['R%d' % (10 + k)] = (x + 9, 51, 0)
    POS['R%d' % (14 + k)] = (x + 9, 55, 0)
    POS['R%d' % (18 + k)] = (x + 9, 59, 0)
    POS['C%d' % (14 + k)] = (x + 9, 63, 0)
    POS['J%d' % (6 + k)] = (x + 8, 78, 0)


def main():
    route = None
    if '--route' in sys.argv:
        route = sys.argv[sys.argv.index('--route') + 1]
    comps, nets = netlist()
    b = pcbnew.BOARD()
    ds = b.GetDesignSettings()
    ds.SetCopperLayerCount(2)
    ds.m_TrackMinWidth = mm(0.2)
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

    for ref, (value, fpid) in sorted(comps.items()):
        if ref.startswith('#'):
            continue
        fp = load(fpid)
        fp.SetReference(ref)
        fp.SetValue(value)
        x, y, rot = POS[ref]
        b.Add(fp)
        fp.SetPosition(P(x, y))
        if rot:
            fp.SetOrientation(EDA_ANGLE(rot, pcbnew.DEGREES_T))
        fp.Reference().SetLayer(pcbnew.F_SilkS)
        fp.Reference().SetTextSize(VECTOR2I(mm(1), mm(1)))
        fp.Value().SetLayer(pcbnew.F_Fab)
        for pad in fp.Pads():
            n = nets.get((ref, pad.GetName()))
            if pad.GetName() in ('MP1', 'MP2'):
                n = 'GND'
            if n and not n.startswith('unconnected-'):
                pad.SetNet(net(n))
            elif n:
                pad.SetNet(net(n))
    # extra mounting holes to GND
    for i, (hx, hy) in enumerate([(116, 37), (4, 80)]):
        fp = pcbnew.FootprintLoad(os.path.join(FPDIR, 'MountingHole.pretty'), 'MountingHole_3.2mm_M3_Pad')
        fp.SetReference('H%d' % (i + 3))
        b.Add(fp)
        fp.SetPosition(P(hx, hy))
        for pad in fp.Pads():
            pad.SetNet(net('GND'))
    # outline
    pts = [(0, 0), (W, 0), (W, H), (0, H)]
    for i in range(4):
        s = pcbnew.PCB_SHAPE(b)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(mm(0.05))
        s.SetStart(P(*pts[i]))
        s.SetEnd(P(*pts[(i + 1) % 4]))
        b.Add(s)
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
    for (x, y) in [(106, 0), (W, 0), (W, 34), (106, 34)]:
        o.Append(mm(x), mm(y))
    b.Add(k)

    pcbnew.SaveBoard(PCB, b)
    if route:
        dsn, ses = '/tmp/audiocarrier.dsn', '/tmp/audiocarrier.ses'
        pcbnew.ExportSpecctraDSN(b, dsn)
        subprocess.check_call(['java', '-jar', route, '-de', dsn, '-do', ses, '-mp', '40', '--gui.enabled=false'])
        b = pcbnew.LoadBoard(PCB)
        pcbnew.ImportSpecctraSES(b, ses)
        pcbnew.SaveBoard(PCB, b)
    b = pcbnew.LoadBoard(PCB)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(PCB, b)
    print('wrote', PCB)


main()
