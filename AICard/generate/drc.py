#!/usr/bin/env python3
"""Run KiCad's DRC on AICard.kicad_pcb and print a summary. Needs a Python with pcbnew (KiCad's own, or python3-pcbnew).

KiCad 7's DRC has no schematic-parity check; KiCad 8+ does (kicad-cli pcb drc --schematic-parity), which the LTE and audio
cards were checked with. Expected result: no errors, 0 unconnected; warnings only (J1, J3 and J4 stick out of the outline
on purpose: the slot pins, the debug pins and the USB-C plug side).
"""
import collections
import os
import re
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.join(HERE, '..', 'AICard.kicad_pcb')
RPT = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'aicard_drc.rpt')

b = pcbnew.LoadBoard(PCB)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.BuildConnectivity()
pcbnew.WriteDRCReport(b, RPT, pcbnew.EDA_UNITS_MILLIMETRES, True)
txt = open(RPT).read()
print('\n'.join(re.findall(r'\*\* Found .*', txt)))
kinds = collections.Counter(re.findall(r'^\[(\w+)\]', txt, re.M))
for k, n in sorted(kinds.items()):
    print('%4d  %s' % (n, k))
errors = len(re.findall(r'(?:; |Severity: )error$', txt, re.M))      # KiCad 8+: '; error', KiCad 7: 'Severity: error'
print('errors:', errors, '(full report: %s)' % RPT)
sys.exit(1 if errors else 0)
