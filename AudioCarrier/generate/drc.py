#!/usr/bin/env python3
"""Run KiCad's DRC (with the schematic parity check) on AudioCarrier.kicad_pcb and print a summary.

Uses kicad-cli (KiCad 8+). Expected result: no errors, 0 unconnected, silkscreen warnings only (J1's courtyard
reaches past the bottom edge on purpose: its pins stand in the slot).
"""
import collections
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
KICAD = '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli'
CLI = os.environ.get('KICAD_CLI', KICAD if os.path.exists(KICAD) else 'kicad-cli')
PCB = os.path.join(HERE, '..', 'AudioCarrier.kicad_pcb')
RPT = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'audiocarrier_drc.rpt')

subprocess.call([CLI, 'pcb', 'drc', '--severity-all', '--schematic-parity', '-o', RPT, PCB],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
txt = open(RPT).read()
print('\n'.join(re.findall(r'\*\* Found .*', txt)))
kinds = collections.Counter(re.findall(r'^\[(\w+)\]', txt, re.M))
for k, n in sorted(kinds.items()):
    print('%4d  %s' % (n, k))
print('errors:', len(re.findall(r'; error$', txt, re.M)), '(full report: %s)' % RPT)
