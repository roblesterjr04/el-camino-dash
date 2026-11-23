#!/usr/bin/env python3
"""Run KiCad's DRC (with the schematic parity check) on RadioCard2.kicad_pcb and print a summary.

Uses kicad-cli (KiCad 8+). Expected result: no errors but J2's, 0 unconnected, silkscreen warnings only (J1's courtyard
reaches past the bottom edge on purpose: its pins stand in the slot). J2 is an edge-launch SMA: its signal and ground
pads run to the top edge by design, so its copper_edge_clearance hits are listed as expected, not counted as errors
(kicad-cli takes no custom rule below the board-setup minimum, and these change UUIDs on every regeneration).
"""
import collections
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
KICAD = '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli'
CLI = os.environ.get('KICAD_CLI', KICAD if os.path.exists(KICAD) else 'kicad-cli')
PCB = os.path.join(HERE, '..', 'RadioCard2.kicad_pcb')
RPT = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'radiocard2_drc.rpt')

subprocess.call([CLI, 'pcb', 'drc', '--severity-all', '--schematic-parity', '-o', RPT, PCB],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
txt = open(RPT).read()
print('\n'.join(re.findall(r'\*\* Found .*', txt)))
kinds = collections.Counter(re.findall(r'^\[(\w+)\]', txt, re.M))
for k, n in sorted(kinds.items()):
    print('%4d  %s' % (n, k))
blocks = re.split(r'\n(?=\[)', txt)
edge_j2 = [bl for bl in blocks if bl.startswith('[copper_edge_clearance]') and ' of J2 ' in bl]
errors = len(re.findall(r'; error$', txt, re.M)) - len(edge_j2)
print('expected: %d copper_edge_clearance on J2 (edge-launch SMA)' % len(edge_j2))
print('errors:', errors, '(full report: %s)' % RPT)
