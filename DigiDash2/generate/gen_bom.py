#!/usr/bin/env python3
"""Write fab/DigiDash2_BOM_PCBWay.csv: the assembler's BOM, same columns as the cards' PCBWay BOMs. Run with KiCad's
python (it reads the board):

    $KIPY generate/gen_bom.py

Every fitted part is listed, SMD and through-hole (the connectors, the slot sockets and F1's holder): the assembler
fits them all. Not-fitted parts (C42, C43) are left out, as in the placement files. Parts with the same value and
footprint share a row. Where the other boards already use a part for the same value, it is the same part
number.
"""
import collections
import csv
import os
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
PCB = os.path.join(ROOT, 'DigiDash2.kicad_pcb')
OUT = os.path.join(ROOT, 'fab', 'DigiDash2_BOM_PCBWay.csv')

# (value, footprint) -> (maker, part number, description, package, note)
PARTS = {
    # capacitors
    ('100u 63V', 'CP_Elec_10x10.5'): ('Honor', 'RYRVT63V100UF10*10', 'Aluminium electrolytic 100 uF 63 V, SMD', '10 x 10.2 mm SMD',
                                       '63 V: above the SMCJ33A clamp (about 53 V). Panasonic EEE-FK1J101P (AEC-Q200) is the equal'),
    ('2.2u 100V', 'C_1210_3225Metric'): ('Murata', 'GRM32ER72A225KA35L', 'Capacitor 2.2u X7R 100 V', '1210', ''),
    ('47u 10V', 'C_1210_3225Metric'): ('Samsung', 'CL32A476KPJNNNE', 'Capacitor 47u X5R 10 V', '1210', ''),
    ('100n', 'C_0603_1608Metric'): ('Samsung', 'CL10B104KB8NNNC', 'Capacitor 100n X7R 50 V', '0603', ''),
    ('4.7n', 'C_0603_1608Metric'): ('Samsung', 'CL10B472KB8NNNC', 'Capacitor 4.7n X7R 50 V', '0603', ''),
    ('47p', 'C_0603_1608Metric'): ('Samsung', 'CL10C470JB8NNNC', 'Capacitor 47p C0G 50 V', '0603', ''),
    ('10u', 'C_0805_2012Metric'): ('Samsung', 'CL21B106KOQNNNE', 'Capacitor 10u X7R 16 V', '0805', ''),
    ('1u', 'C_0603_1608Metric'): ('Samsung', 'CL10B105KO8NNNC', 'Capacitor 1u X7R 16 V', '0603', ''),
    ('4.7u', 'C_0805_2012Metric'): ('Samsung', 'CL21A475KOFNNNE', 'Capacitor 4.7u X5R 16 V', '0805', ''),
    ('33p', 'C_0603_1608Metric'): ('Samsung', 'CL10C330JB8NNNC', 'Capacitor 33p C0G 50 V', '0603',
                                   'Y3 load caps: 2 x (20 pF CL - about 4 pF stray)'),
    ('2.2u', 'C_0603_1608Metric'): ('Samsung', 'CL10A225KO8NNNC', 'Capacitor 2.2u X5R 16 V', '0603', ''),
    # diodes
    ('NUP2105L', 'SOT-23'): ('onsemi', 'NUP2105LT1G', 'CAN bus ESD clamp', 'SOT-23', ''),
    ('BZT52C15', 'D_SOD-123'): ('Diodes Inc', 'BZT52C15-7-F', 'Zener 15 V', 'SOD-123', ''),
    ('BZT52C5V1', 'D_SOD-123'): ('Diodes Inc', 'BZT52C5V1-7-F', 'Zener 5.1 V', 'SOD-123', ''),
    ('SMCJ33A', 'D_SMC'): ('Littelfuse', 'SMCJ33A', 'TVS 33 V standoff, unidirectional, 1500 W', 'SMC', ''),
    ('B560C', 'D_SMC'): ('Diodes Inc', 'B560C-13-F', 'Schottky 5 A 60 V', 'SMC', ''),
    ('BAT54S', 'SOT-23'): ('Nexperia', 'BAT54S,215', 'Dual Schottky, series', 'SOT-23', ''),
    ('PESD5V0S1BA', 'D_SOD-323'): ('Nexperia', 'PESD5V0S1BA,115', 'ESD clamp 5 V, bidirectional', 'SOD-323', ''),
    # fuses (PTC)
    ('PTC 0.1A', 'Fuse_1206_3216Metric'): ('Littelfuse', '1206L010/60WR', 'PTC 0.1 A hold, 60 V', '1206', ''),
    ('PTC 0.2A', 'Fuse_1206_3216Metric'): ('Bourns', 'MF-NSMF020X-2', 'PTC 0.2 A hold, 30 V, AEC-Q200', '1206',
                                           'On the battery line: 30 V part'),
    ('PTC 0.5A', 'Fuse_1812_4532Metric'): ('PTTC', 'SMD1812P050TF/30', 'PTC 0.5 A hold, 30 V', '1812',
                                          '30 V part on the 5 V and the 12 V lines alike'),
    ('PTC 1.1A', 'Fuse_1812_4532Metric'): ('Littelfuse', '1812L110/33MR', 'PTC 1.1 A hold, 33 V', '1812', ''),
    # inductors, ferrite
    ('600R', 'L_0603_1608Metric'): ('Murata', 'BLM18KG601SN1D', 'Ferrite bead 600 R at 100 MHz', '0603', ''),
    ('6.8u', 'L_Molded_1050'): ('COILMX', 'MS1050-6R8M', 'Inductor 6.8 uH, Isat 14 A, Irms 8.5 A, molded shielded',
                                 '11.2 x 10 mm', 'Replaces Wurth 7443551720 (7.2 uH, WE-HCI 1050): the land pattern changed with it'),
    # transistors
    ('MMBT3904', 'SOT-23'): ('onsemi', 'MMBT3904LT1G', 'NPN 40 V', 'SOT-23', ''),
    ('SUD50P04-08', 'TO-252-2'): ('Vishay', 'SUD50P04-08-GE3', 'P-MOSFET 40 V, 8 mOhm', 'TO-252 (DPAK)', ''),
    # ICs
    ('TPS54560BDDA', 'TI_SO-PowerPAD-8_ThermalVias'): ('Texas Instruments', 'TPS54560BDDAR',
                                                       '5 A step-down, 4.5-60 V in', 'SO-8 PowerPAD', ''),
    ('TJA1051T/3', 'SOIC-8_3.9x4.9mm_P1.27mm'): ('NXP', 'TJA1051T/3/1J', 'CAN FD transceiver (pin-compatible with MCP2562-E/SN)', 'SOIC-8', ''),
    ('TVP5150AM1IPBS', 'TQFP-32_5x5mm_P0.5mm'): ('Texas Instruments', 'TVP5150AM1IPBSR', 'Video decoder', 'TQFP-32', ''),
    ('AP7361C-33E', 'SOT-223-3_TabPin2'): ('Diodes Inc', 'AP7361C-33E-13', 'LDO 3.3 V 1 A', 'SOT-223', ''),
    ('AP2112K-1.8', 'SOT-23-5'): ('Diodes Inc', 'AP2112K-1.8TRG1', 'LDO 1.8 V 600 mA', 'SOT-23-5', ''),
    ('14.31818MHz', 'Crystal_SMD_3225-4Pin_3.2x2.5mm'): ('YXC', 'X32251431818MSB4SI',
                                                         'Crystal 14.31818 MHz, +/-20 ppm, CL 20 pF', '3225 4-pad',
                                                         'Load caps C29 / C30 are 33p for its 20 pF'),
    ('TERM', 'SW_DIP_SPSTx02_Slide_KingTek_DSHP02TS_W7.62mm_P1.27mm'): (
        'KingTek', 'DSHP02TSGER', 'DIP switch, 2 position, SMD top slide, 1.27 mm pitch', 'SMD 4.1 x 5.4 mm',
        'Set position 1 (TERM) ON before shipping'),
    # through hole: connectors, slot sockets, fuse holder
    ('Center Screen - Master', 'PinHeader_2x20_P2.54mm_Vertical'): (
        'XFCN', 'PZ254V-12-40P', 'Pin header 2 x 20, 2.54 mm, vertical (male; the screen has the 40-pin socket)',
        'THT 2x20', 'FRONT side (screen side), the only parts on the front'),
    ('Left Screen', 'PinHeader_2x20_P2.54mm_Vertical'): (
        'XFCN', 'PZ254V-12-40P', 'Pin header 2 x 20, 2.54 mm, vertical (male; the screen has the 40-pin socket)',
        'THT 2x20', 'FRONT side (screen side), the only parts on the front'),
    ('Right Screen', 'PinHeader_2x20_P2.54mm_Vertical'): (
        'XFCN', 'PZ254V-12-40P', 'Pin header 2 x 20, 2.54 mm, vertical (male; the screen has the 40-pin socket)',
        'THT 2x20', 'FRONT side (screen side), the only parts on the front'),
    ('CAN debug', 'PinHeader_1x03_P2.54mm_Vertical'): (
        'Sullins', 'PRPC003SAAN-RC', 'Pin header 1 x 3, 2.54 mm, vertical', 'THT 1x3', ''),
    ('Sensor serial in', 'PinHeader_1x02_P2.54mm_Vertical'): (
        'Sullins', 'PRPC002SAAN-RC', 'Pin header 1 x 2, 2.54 mm, vertical', 'THT 1x2', ''),
    ('Vehicle power', 'Molex_Micro-Fit_3.0_43650-0615_1x06_P3.00mm_Vertical'): (
        'Molex', '43650-0615', 'Micro-Fit 3.0 header, 6 pin, 1 row, vertical', 'THT', ''),
    ('Trunk port', 'Molex_Micro-Fit_3.0_43650-0415_1x04_P3.00mm_Vertical'): (
        'Molex', '43650-0415', 'Micro-Fit 3.0 header, 4 pin, 1 row, vertical', 'THT', ''),
    ('AUX 12V in', 'Molex_Mini-Fit_Jr_5566-02A_2x01_P4.20mm_Vertical'): (
        'Molex', '39-28-1023', 'Mini-Fit Jr header 5566-02A, 2 pin, vertical, no pegs', 'THT', ''),
    ('Video in', 'RCA_RCJ-02x_FaceMount'): (
        'Same Sky', 'RCJ-024', 'RCA jack, vertical, yellow (video)', 'THT', ''),
    ('5A mini blade', 'Fuseholder_Blade_Mini_Keystone_3568'): (
        'Keystone', '3568', 'Fuse holder, mini blade (ATM), PCB', 'THT',
        'Holder only; fit a 5 A mini blade fuse (Littelfuse 0297005.WXNV) after soldering if supplied'),
}
# JST PH 4-pin, vertical, and the six slot sockets
for v in ('Serial', 'Backup Camera', 'Cable port 1', 'Cable port 2', 'Cable port 3', 'Cable port 4'):
    PARTS[(v, 'JST_PH_B4B-PH-K_1x04_P2.00mm_Vertical')] = ('JST', 'B4B-PH-K-S(LF)(SN)', 'JST PH header, 4 pin, vertical',
                                                           'THT', '')
for n in range(1, 7):
    PARTS[('Slot %d' % n, 'Slot_Socket_2x15_P2.54mm_Vertical')] = (
        'Connfly', 'DS1023-2*15SF11', 'Socket 2 x 15, 2.54 mm, female, vertical, 8.5 mm insulator (Sullins PPTC152LFBN-RC equal)', 'THT 2x15',
        'Pin 1 = square pad. Seat flat: the cards plug in square to the board')
# 1 % 0603 thick film, Yageo RC0603FR-07..L; one 0805 jumper
R_0603 = {'10k': '10K', '243k': '243K', '53.6k': '53K6', '10.2k': '10K2', '16.9k': '16K9', '442k': '442K',
          '90.9k': '90K9', '4.7k': '4K7', '22k': '22K', '1k': '1K', '120': '120R', '20k': '20K', '37.4': '37R4',
          '22': '22R', '100k': '100K', '3.3k': '3K3', '6.8k': '6K8', '12k': '12K', '47k': '47K', '27k': '27K'}
for v, code in R_0603.items():
    PARTS[(v, 'R_0603_1608Metric')] = ('Yageo', 'RC0603FR-07%sL' % code, 'Resistor %s 1%%' % v, '0603', '')
PARTS[('0', 'R_0805_2012Metric')] = ('Yageo', 'RC0805JR-070RL', 'Resistor 0 R (jumper)', '0805',
                                     'Anti-alias filter L as delivered: swap for a 2.7 uH 0805 to fit the filter')


def main():
    b = pcbnew.LoadBoard(PCB)
    rows = collections.OrderedDict()
    missing = []
    for f in sorted(b.GetFootprints(), key=lambda f: (f.GetReference().rstrip('0123456789'),
                                                     int(f.GetReference().lstrip('ABCDEFGHIJKLMNOPQRSTUVWXYZ') or 0))):
        if not f.GetAttributes() & (pcbnew.FP_SMD | pcbnew.FP_THROUGH_HOLE) or f.IsDNP() or f.IsExcludedFromBOM():
            continue
        key = (f.GetValue(), f.GetFPID().GetLibItemName().wx_str())
        if key not in PARTS:
            missing.append('%s %s %s' % (f.GetReference(), *key))
            continue
        row = rows.setdefault(key, dict(refs=[], sides=set(),
                                        type='THT' if f.GetAttributes() & pcbnew.FP_THROUGH_HOLE else 'SMD'))
        row['refs'].append(f.GetReference())
        row['sides'].add('Bottom' if f.GetLayer() == pcbnew.B_Cu else 'Top')
    if missing:
        sys.exit('no part for: ' + ', '.join(missing))

    with open(OUT, 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['Item #', 'Designator', 'Qty', 'Manufacturer', 'Mfg Part #', 'Description / Value',
                    'Package/Footprint', 'Type', 'Side', 'Your Instructions / Notes'])
        for n, (key, row) in enumerate(rows.items(), 1):
            maker, mpn, desc, pkg, note = PARTS[key]
            w.writerow([n, ' '.join(row['refs']), len(row['refs']), maker, mpn, '%s: %s' % (key[0], desc), pkg,
                        row['type'], '/'.join(sorted(row['sides'], reverse=True)), note])
    todo = [k[0] for k, r in rows.items() if not PARTS[k][1]]
    print('%s: %d lines, %d parts; still to choose: %s' % (os.path.relpath(OUT, ROOT), len(rows),
                                                           sum(len(r['refs']) for r in rows.values()),
                                                           ', '.join(todo) or 'none'))


if __name__ == '__main__':
    main()
