#!/usr/bin/python3
"""Run KiCad's DRC on AudioCarrier.kicad_pcb (pcbnew python, KiCad 7+) and print the report."""
import os
import sys
import pcbnew
here = os.path.dirname(os.path.abspath(__file__))
b = pcbnew.LoadBoard(os.path.join(here, '..', 'AudioCarrier.kicad_pcb'))
out = '/tmp/audiocarrier_drc.rpt'
pcbnew.WriteDRCReport(b, out, pcbnew.EDA_UNITS_MILLIMETRES, True)
print(open(out).read())
