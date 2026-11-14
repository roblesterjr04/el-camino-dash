#!/usr/bin/env python3
"""Write DigiDash3_BOM.csv: the parts list for hand assembly (every part is through-hole), PCBWay / JLCPCB columns. Run with KiCad's
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
PCB = os.path.join(ROOT, 'DigiDash3.kicad_pcb')
OUT = os.path.join(ROOT, 'DigiDash3_BOM.csv')

# (value, footprint) -> (maker, part number, description, package, note)
PARTS = {
    # capacitors
    ('100u 63V', 'CP_Radial_D10.0mm_P5.00mm'): ('Nichicon', 'UPW1J101MPD', 'Aluminium electrolytic 100 uF 63 V, radial 10 x 12.5 mm, 5 mm pitch', 'radial', 'LCSC alternative: Rubycon 63YXF100MFFC10X12.5, C388845'),
    ('1000u 16V', 'CP_Radial_D10.0mm_P5.00mm'): ('Rubycon', '16YXJ1000MT810X16', 'Aluminium electrolytic 1000 uF 16 V, low impedance, radial 10 x 16 mm, 5 mm pitch', 'radial', 'Buck output. 16 mm tall, under the 23 mm inductors. Was CX 1000uF 16V 10x13 (LCSC C439782)'),
    ('10u 25V', 'CP_Radial_D5.0mm_P2.00mm'): ('Rubycon', '50YXJ10M5X11', 'Aluminium electrolytic 10 uF 50 V, radial 5 x 11 mm, 2 mm pitch', 'radial', ''),
    ('100n', 'C_Disc_D5.0mm_W2.5mm_P5.00mm'): ('Vishay', 'K104K15X7RF5TL2', 'Ceramic 100 nF X7R 50 V, 5 mm pitch', 'radial', 'DigiKey lists 2.5 mm lead spacing: confirm the 5 mm (F5) version, the footprint is 5 mm'),
    # diodes
    ('1.5KE33A', 'D_DO-201AD_P15.24mm_Horizontal'): ('Littelfuse', '1.5KE33A', 'TVS 33 V standoff, unidirectional, 1500 W, DO-201', 'axial', ''),
    ('SB560', 'D_DO-201AD_P15.24mm_Horizontal'): ('Vishay', 'SB560-E3/54', 'Schottky 5 A 60 V, DO-201AD (1N5822 fits too)', 'axial', ''),
    ('1N4744A', 'D_DO-41_SOD81_P10.16mm_Horizontal'): ('onsemi', '1N4744A', 'Zener 15 V 1 W, DO-41', 'axial', ''),
    ('SA24CA', 'D_DO-15_P10.16mm_Horizontal'): ('Littelfuse', 'SA24CA', 'TVS 24 V bidirectional, 500 W, DO-15 (CAN clamp)', 'axial', ''),
    ('BAT85', 'D_DO-35_SOD27_P7.62mm_Horizontal'): ('Vishay', 'BAT85S-TAP', 'Schottky 200 mA 30 V, DO-35', 'axial', ''),
    ('1N4148', 'D_DO-35_SOD27_P7.62mm_Horizontal'): ('onsemi', '1N4148', 'Signal diode, DO-35', 'axial', ''),
    # fuses (PTC)
    ('PTC 0.1A', 'PTC_Radial_P5.08mm'): ('Bourns', 'MF-R010', 'PTC 0.1 A hold, 60 V, radial', 'radial', ''),
    ('PTC 0.2A', 'PTC_Radial_P5.08mm'): ('Bourns', 'MF-R020', 'PTC 0.2 A hold, 60 V, radial', 'radial', 'On the battery line'),
    ('PTC 0.5A', 'PTC_Radial_P5.08mm'): ('Bourns', 'MF-R050', 'PTC 0.5 A hold, 60 V, radial', 'radial', ''),
    ('PTC 1.1A', 'PTC_Radial_P5.08mm'): ('Bourns', 'MF-R110', 'PTC 1.1 A hold, 30 V, radial', 'radial', ''),
    # inductors
    ('68u', 'L_Radial_D14.2mm_P10.00mm_Neosid_SD14'): ('Abracon', 'AIAP-03-680K', 'Inductor 68 uH, 3.3 A, 47 mOhm, radial drum core 11.4 x 23 mm', 'radial',
                                                       'Bend the leads out to the 10 mm pad pitch. 23 mm tall: the tallest part on the board. No stocked 100 uH part of 3 A or more fits a 14 mm / 10 mm footprint (2026-10-26)'),
    # transistors
    ('2N3904', 'TO-92_Inline'): ('onsemi', '2N3904BU', 'NPN 40 V, TO-92 (E B C)', 'TO-92', ''),
    ('IRF4905', 'TO-220-3_Horizontal_TabDown'): ('Infineon', 'IRF4905PBF', 'P-MOSFET 55 V, 20 mOhm, TO-220 lying flat', 'TO-220', ''),
    # ICs
    ('LM2576HVT-5.0', 'TO-220-5_Horizontal_TabDown'): ('HGSEMI', 'LM2576HVT-5.0', '5 V, 3 A step-down, 8-60 V in, TO-220-5 lying flat', 'TO-220-5', 'TI LM2576HVT-5.0/NOPB is on backorder at DigiKey and not for new designs; the HGSEMI / UMW parts are the same device'),
    ('MCP2562-E/P', 'DIP-8_W7.62mm'): ('Microchip', 'MCP2562-E/P', 'CAN FD transceiver, DIP-8', 'DIP-8', ''),
    ('LD1117V33', 'TO-220-3_Horizontal_TabDown'): ('STMicroelectronics', 'LD1117V33', 'LDO 3.3 V 800 mA, TO-220 lying flat', 'TO-220', ''),
    ('TERM', 'SW_DIP_SPSTx02_Slide_6.7x6.64mm_W7.62mm_P2.54mm_LowProfile'): (
        'CTS', '206-2', 'DIP switch, 2 position, slide, 2.54 mm pitch', 'DIP', 'Set position 1 (TERM) ON before shipping. LCSC alternative: KingTek DSWB02LHGET, C99421'),
    # headers, sockets
    ('Video module', 'PinHeader_2x10_P2.54mm_Vertical'): (
        'Sullins', 'PPTC102LFBN-RC', 'Socket 2 x 10, 2.54 mm, female, vertical, 8.5 mm (the module plugs in)', 'THT 2x10',
        'Fit a FEMALE socket here, not a pin header: the module carries the pins'),
    ('Video VCC', 'PinHeader_1x03_P2.54mm_Vertical'): ('Sullins', 'PRPC003SAAN-RC', 'Pin header 1 x 3, 2.54 mm, vertical, plus a shunt', 'THT 1x3', 'Shunt 1-2 = 5 V, 2-3 = 3.3 V. LCSC alternative: XFCN PZ254V-11-03P, C2937625'),
    ('Video aux', 'PinHeader_1x03_P2.54mm_Vertical'): ('Sullins', 'PRPC003SAAN-RC', 'Pin header 1 x 3, 2.54 mm, vertical', 'THT 1x3', 'Wires to the module: PCLK, RESET'),
    ('Center Screen - Master', 'PinHeader_2x20_P2.54mm_Vertical'): (
        'Sullins', 'PRPC020DAAN-RC', 'Pin header 2 x 20, 2.54 mm, vertical (male; the screen has the 40-pin socket)',
        'THT 2x20', 'FRONT side (screen side), the only parts on the front'),
    ('Left Screen', 'PinHeader_2x20_P2.54mm_Vertical'): (
        'Sullins', 'PRPC020DAAN-RC', 'Pin header 2 x 20, 2.54 mm, vertical (male; the screen has the 40-pin socket)',
        'THT 2x20', 'FRONT side (screen side), the only parts on the front'),
    ('Right Screen', 'PinHeader_2x20_P2.54mm_Vertical'): (
        'Sullins', 'PRPC020DAAN-RC', 'Pin header 2 x 20, 2.54 mm, vertical (male; the screen has the 40-pin socket)',
        'THT 2x20', 'FRONT side (screen side), the only parts on the front'),
    ('CAN debug', 'PinHeader_1x03_P2.54mm_Vertical'): ('Sullins', 'PRPC003SAAN-RC', 'Pin header 1 x 3, 2.54 mm, vertical', 'THT 1x3', ''),
    ('Sensor serial in', 'PinHeader_1x02_P2.54mm_Vertical'): ('Sullins', 'PRPC002SAAN-RC', 'Pin header 1 x 2, 2.54 mm, vertical', 'THT 1x2', 'LCSC alternative: XFCN PZ254V-11-02P, C492401'),
    ('Vehicle power', 'Molex_Micro-Fit_3.0_43650-0615_1x06_P3.00mm_Vertical'): (
        'Molex', '43650-0615', 'Micro-Fit 3.0 header, 6 pin, 1 row, vertical', 'THT', ''),
    ('Trunk port', 'Molex_Micro-Fit_3.0_43650-0415_1x04_P3.00mm_Vertical'): (
        'Molex', '43650-0415', 'Micro-Fit 3.0 header, 4 pin, 1 row, vertical', 'THT', ''),
    ('AUX 12V in', 'Molex_Mini-Fit_Jr_5566-02A_2x01_P4.20mm_Vertical'): (
        'Molex', '39-28-1023', 'Mini-Fit Jr header 5566-02A, 2 pin, vertical, no pegs', 'THT', ''),
    ('5A mini blade', 'Fuseholder_Blade_Mini_Keystone_3568'): (
        'Keystone', '3568', 'Fuse holder, mini blade (ATM), PCB', 'THT',
        'Holder only; fit a 5 A mini blade fuse (Littelfuse 0297005.WXNV)'),
    ('NetTie', 'NetTie-2_THT_Pad1.0mm'): ('', '', 'Net tie (copper on the board, no part)', '', ''),
}
# JST PH 4-pin, vertical, and the six slot sockets
for v in ('Serial', 'Backup Camera', 'Cable port 1', 'Cable port 2', 'Cable port 3', 'Cable port 4'):
    PARTS[(v, 'JST_PH_B4B-PH-K_1x04_P2.00mm_Vertical')] = ('JST', 'B4B-PH-K-S(LF)(SN)', 'JST PH header, 4 pin, vertical',
                                                           'THT', '')
for n in range(1, 7):
    PARTS[('Slot %d' % n, 'Slot_Socket_2x15_P2.54mm_Vertical')] = (
        'Connfly', 'DS1023-2*15SF11', 'Socket 2 x 15, 2.54 mm, female, vertical, 8.5 mm insulator', 'THT 2x15',
        'Pin 1 = square pad. Seat flat: the cards plug in square to the board. DigiKey equal: Sullins PPTC152LFBN-RC at $1.68 (LCSC $0.27, minimum 5)')
# 1 % 1/4 W metal film axial, Yageo MFR-25FBF52-..: '4.7k' -> 4K7, '120' -> 120R, '100k' -> 100K
def rcode(v):
    if v[-1] in 'kK':
        return (v[:-1].replace('.', 'K') if '.' in v else v[:-1] + 'K')
    return v + 'R'


R_CODE = {'47k': 'MFR-25FRF52-47K', '6.8k': 'MFR-25FRF52-6K8', '22k': 'MFR-25FRE52-22K'}   # the codes DigiKey stocks for these values
for v in ('10k', '4.7k', '22k', '1k', '120', '20k', '100k', '47k', '27k', '3.3k', '6.8k', '12k'):
    PARTS[(v, 'R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal')] = (
        'Yageo', R_CODE.get(v, 'MFR-25FBF52-%s' % rcode(v)), 'Resistor %s 1%%, 1/4 W metal film, axial' % v, 'axial', '')


# Where to buy (2026-10-26): vendor, product page, unit price at quantity 1 where the page showed one. DigiKey first;
# the generic Chinese sockets and the two parts DigiKey has on backorder come from LCSC.
DK = 'https://www.digikey.com/en/products/detail/'
SOURCE = {
    ('Nichicon', 'UPW1J101MPD'): ('DigiKey', DK + 'nichicon/UPW1J101MPD/589677', None),
    ('Vishay', 'K104K15X7RF5TL2'): ('DigiKey', DK + 'vishay-beyschlag-draloric-bc-components/K104K15X7RF5TL2/286538', None),
    ('Rubycon', '16YXJ1000MT810X16'): ('DigiKey', 'https://www.digikey.com/en/products/result?keywords=16YXJ1000MT810X16', None),
    ('Rubycon', '50YXJ10M5X11'): ('DigiKey', DK + 'rubycon/50YXJ10M5X11/3562914', None),
    ('onsemi', '1N4744A'): ('DigiKey', DK + 'onsemi/1N4744A/977267', 0.27),
    ('Littelfuse', '1.5KE33A'): ('DigiKey', DK + 'littelfuse-inc/1-5KE33A/688025', None),
    ('Vishay', 'SB560-E3/54'): ('DigiKey', DK + 'vishay-general-semiconductor-diodes-division/SB560-E3-54/1023528', None),
    ('Littelfuse', 'SA24CA'): ('DigiKey', DK + 'littelfuse-inc/SA24CA/557066', 0.30),
    ('onsemi', '1N4148'): ('DigiKey', DK + 'onsemi/1N4148/458603', 0.10),
    ('Vishay', 'BAT85S-TAP'): ('DigiKey', DK + 'vishay-general-semiconductor-diodes-division/BAT85S-TAP/3104127', 0.14),
    ('Keystone', '3568'): ('DigiKey', DK + 'keystone-electronics/3568/2137306', None),
    ('Bourns', 'MF-R050'): ('DigiKey', DK + 'bourns-inc/MF-R050/259965', None),
    ('Bourns', 'MF-R010'): ('DigiKey', DK + 'bourns-inc/MF-R010/259959', None),
    ('Bourns', 'MF-R110'): ('DigiKey', DK + 'bourns-inc/MF-R110/259970', 0.47),
    ('Bourns', 'MF-R020'): ('DigiKey', DK + 'bourns-inc/MF-R020/259961', None),
    ('Abracon', 'AIAP-03-680K'): ('LCSC', 'https://www.lcsc.com/product-detail/C186765.html', 1.14),
    ('onsemi', '2N3904BU'): ('DigiKey', DK + 'onsemi/2N3904BU/1413', 0.29),
    ('Infineon', 'IRF4905PBF'): ('DigiKey', DK + 'infineon-technologies/IRF4905PBF/812139', 2.53),
    ('Yageo', 'MFR-25FBF52-10K'): ('DigiKey', DK + 'yageo/MFR-25FBF52-10K/13219', 0.10),
    ('Yageo', 'MFR-25FBF52-100K'): ('DigiKey', DK + 'yageo/MFR-25FBF52-100K/13473', 0.10),
    ('Yageo', 'MFR-25FRF52-47K'): ('DigiKey', DK + 'yageo/MFR-25FRF52-47K/9139054', 0.10),
    ('Yageo', 'MFR-25FRE52-22K'): ('DigiKey', DK + 'yageo/MFR-25FRE52-22K/9138430', 0.10),
    ('Yageo', 'MFR-25FBF52-4K7'): ('DigiKey', 'https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-4K7', 0.10),
    ('Yageo', 'MFR-25FBF52-1K'): ('DigiKey', DK + 'yageo/MFR-25FBF52-1K/13011', 0.10),
    ('Yageo', 'MFR-25FBF52-120R'): ('DigiKey', DK + 'yageo/MFR-25FBF52-120R/9138050', 0.11),
    ('Yageo', 'MFR-25FBF52-20K'): ('DigiKey', DK + 'yageo/MFR-25FBF52-20K/13276', 0.10),
    ('Yageo', 'MFR-25FBF52-3K3'): ('DigiKey', DK + 'yageo/MFR-25FBF52-3K3/9138148', 0.10),
    ('Yageo', 'MFR-25FRF52-6K8'): ('DigiKey', DK + 'yageo/MFR-25FRF52-6K8/9139107', 0.10),
    ('Yageo', 'MFR-25FBF52-12K'): ('DigiKey', DK + 'yageo/MFR-25FBF52-12K/9138051', 0.10),
    ('Yageo', 'MFR-25FBF52-27K'): ('DigiKey', DK + 'yageo/MFR-25FBF52-27K/9138106', 0.10),
    ('CTS', '206-2'): ('DigiKey', DK + 'cts-electrocomponents/206-2/20733', None),
    ('HGSEMI', 'LM2576HVT-5.0'): ('LCSC', 'https://www.lcsc.com/product-detail/C434577.html', 1.14),
    ('Microchip', 'MCP2562-E/P'): ('DigiKey', DK + 'microchip-technology/MCP2562-E-P/4079966', 1.31),
    ('STMicroelectronics', 'LD1117V33'): ('DigiKey', DK + 'stmicroelectronics/LD1117V33/586012', 0.62),
    ('Sullins', 'PRPC020DAAN-RC'): ('DigiKey', DK + 'sullins-connector-solutions/PRPC020DAAN-RC/2775274', 1.34),
    ('JST', 'B4B-PH-K-S(LF)(SN)'): ('DigiKey', DK + 'jst-sales-america-inc/B4B-PH-K-S/926613', 0.24),
    ('Molex', '43650-0615'): ('DigiKey', DK + 'molex/0436500615/283465', 1.57),
    ('Sullins', 'PPTC102LFBN-RC'): ('DigiKey', DK + 'sullins-connector-solutions/PPTC102LFBN-RC/810216', 1.16),
    ('Sullins', 'PRPC003SAAN-RC'): ('DigiKey', DK + 'sullins-connector-solutions/PRPC003SAAN-RC/2775251', 0.32),
    ('Molex', '39-28-1023'): ('DigiKey', DK + 'molex/0039281023/61397', 0.49),
    ('Molex', '43650-0415'): ('DigiKey', DK + 'molex/0436500415/283463', 0.84),
    ('Sullins', 'PRPC002SAAN-RC'): ('DigiKey', DK + 'sullins-connector-solutions/PRPC002SAAN-RC/2775252', 0.30),
    ('Connfly', 'DS1023-2*15SF11'): ('LCSC', 'https://www.lcsc.com/product-detail/C132129.html', 0.27),
}
VENDOR_ORDER = ['DigiKey', 'LCSC', 'Mouser']


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

    def vendor_of(key):
        return SOURCE.get(PARTS[key][:2], ('', '', None))[0]
    ordered = sorted(rows.items(), key=lambda kv: (VENDOR_ORDER.index(vendor_of(kv[0])) if vendor_of(kv[0]) in VENDOR_ORDER else 99,
                                                   list(rows).index(kv[0])))
    with open(OUT, 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['Item #', 'Vendor', 'Designator', 'Qty', 'Manufacturer', 'Mfg Part #', 'Vendor link', 'Unit price (qty 1, $)',
                    'Line ($)', 'Description / Value', 'Package/Footprint', 'Type', 'Side', 'Your Instructions / Notes'])
        for n, (key, row) in enumerate(ordered, 1):
            maker, mpn, desc, pkg, note = PARTS[key]
            vendor, link, price = SOURCE.get((maker, mpn), ('', '', None))
            q = len(row['refs'])
            w.writerow([n, vendor, ' '.join(row['refs']), q, maker, mpn, link, '' if price is None else '%.2f' % price,
                        '' if price is None else '%.2f' % (price * q), '%s: %s' % (key[0], desc), pkg,
                        row['type'], '/'.join(sorted(row['sides'], reverse=True)), note])
    unsourced = [PARTS[k][1] for k in rows if (PARTS[k][0], PARTS[k][1]) not in SOURCE and PARTS[k][1]]
    if unsourced:
        print('no vendor link for: ' + ', '.join(unsourced))
    todo = [k[0] for k, r in rows.items() if not PARTS[k][1]]
    print('%s: %d lines, %d parts; still to choose: %s' % (os.path.relpath(OUT, ROOT), len(rows),
                                                           sum(len(r['refs']) for r in rows.values()),
                                                           ', '.join(todo) or 'none'))


if __name__ == '__main__':
    main()
