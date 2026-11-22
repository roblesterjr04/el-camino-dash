#!/usr/bin/env python3
"""AICard_BOM.csv from the schematic's netlist (kicad-cli, KiCad 7 or later): one row per value + footprint, with the
designators, a suggested part where the value alone does not say it, and notes. Edit PART / NOTES for your sourcing."""
import csv
import os
import subprocess
import xml.etree.ElementTree as ET
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
CLI = os.environ.get('KICAD_CLI', 'kicad-cli')
TMP = os.environ.get('TMPDIR', '/tmp')

PART = {  # value -> suggested part (the schematic's values are descriptive)
    'ESP32-S3-WROOM-1U-N16R8': 'Espressif ESP32-S3-WROOM-1U-N16R8 (16 MB flash, 8 MB octal PSRAM, IPEX)',
    'AP7361C-33E': 'Diodes AP7361C-33E-13 (3.3 V, 1 A LDO, SOT-223)',
    'MCP2562-E/SN': 'Microchip MCP2562-E/SN',
    'USBLC6-2SC6': 'ST USBLC6-2SC6',
    '74LVC125APW': 'Nexperia 74LVC125APW (TSSOP-14)',
    'TPS54560BDDA': 'TI TPS54560BDDAR',
    '6.8u': 'Bourns SRP7028A-6R8M (Isat >= 5 A, Irms >= 3 A: check)',
    'B360B': 'Diodes B360B-13-F (3 A, 60 V Schottky, SMB)',
    'BZT52C5V1': 'Nexperia BZT52C5V1 (5.1 V zener, SOD-123)',
    'DMP6350S': 'Diodes DMP6350S-7 (P-FET, -60 V, SOT-23)',
    '2N7002': 'onsemi 2N7002 (N-FET, 60 V, SOT-23)',
    '10k NTC': 'Murata NCP18XH103F03RB (10k, beta 3950, 0603) or equal',
    '470u 10V': 'Panasonic EEE-FT1A471AP or equal (470 uF, 10 V, 8 x 10 mm, low ESR)',
    'DigiDash slot': 'right-angle 2 x 15 pin header, 2.54 mm, 6 mm mating pins (e.g. Samtec TSW-115-08-G-D-RA)',
    'Compute module': 'female 2 x 20 socket, 2.54 mm, 8.5 mm insulator (e.g. Sullins PPPC202LFBN-RC)',
    'USB-C': 'GCT USB4105-GF-A (USB 2.0 Type-C, 16-pin, horizontal)',
    'Debug serial': '1 x 3 right-angle pin header, 2.54 mm',
    'RESET': 'C&K PTS810 SJM 250 SMTR LFS (SPST tactile)', 'BOOT': 'C&K PTS810 SJM 250 SMTR LFS (SPST tactile)',
    'M2.5': 'M2.5 standoff + screw for the compute module (plain 2.7 mm holes in the card)',
}
NOTES = {
    'Compute module': 'The module itself is a separate part: Radxa Zero 3W 8 GB with eMMC (or a Pi-header compatible board). Not on the BOM',
    'M2.5': 'The hole positions are provisional until the module is measured',
    '10k NTC': 'Beta 3950: the firmware converts with it (ai_pins.h AI_NTC_BETA)',
    'TPS54560BDDA': 'Same circuit as the main board and the LTE card; check the loop with the 470 uF and the real load',
    'DMP6350S': 'Q1: 60 V rating for load-dump on AUX 12 V; Vgs limited to half the supply by the 47k / 47k divider',
}


def main():
    xml = os.path.join(TMP, 'aicard_bom_netlist.xml')
    subprocess.check_call([CLI, 'sch', 'export', 'netlist', '--format', 'kicadxml', '-o', xml,
                           os.path.join(ROOT, 'AICard.kicad_sch')], stderr=subprocess.DEVNULL)
    groups = defaultdict(list)
    desc = {}
    for c in ET.parse(xml).getroot().find('components'):
        ref = c.get('ref')
        if ref.startswith('#'):
            continue
        fp = (c.findtext('footprint') or '').split(':')[-1]
        key = (c.findtext('value'), fp)
        groups[key].append(ref)
        f = {e.get('name'): (e.text or '') for e in c.find('fields')} if c.find('fields') is not None else {}
        desc.setdefault(key, []).append(f.get('Description', '') or c.findtext('value'))

    def sortkey(refs):
        return (refs[0].rstrip('0123456789'), int(''.join(ch for ch in refs[0] if ch.isdigit()) or 0))
    rows = []
    for (value, fp), refs in groups.items():
        refs.sort(key=lambda r: (r.rstrip('0123456789'), int(''.join(ch for ch in r if ch.isdigit()) or 0)))
        pkg = fp.split('_')[1] if fp.startswith(('R_', 'C_', 'LED_')) else fp
        smd = 'Mech' if 'MountingHole' in fp else 'THT' if any(k in fp for k in ('PinHeader', 'PinSocket')) else 'SMD'
        rows.append((refs, value, fp, pkg, smd))
    rows.sort(key=lambda r: sortkey(r[0]))
    with open(os.path.join(ROOT, 'AICard_BOM.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Qty', 'Designators', 'Manufacturer part', 'Description', 'Package', 'Type', 'Side', 'Notes'])
        for refs, value, fp, pkg, smd in rows:
            part = PART.get(value)
            if part is None:  # passives: the value, with the voltage class where the schematic's value implies one
                part = value
            ds = list(dict.fromkeys(desc[(value, fp)]))
            w.writerow([len(refs), ' '.join(refs), part, '; '.join(ds[:4]) + (' ...' if len(ds) > 4 else ''), pkg, smd, 'Top', NOTES.get(value, '')])
    print('wrote AICard_BOM.csv:', sum(len(r[0]) for r in rows), 'parts in', len(rows), 'lines')


if __name__ == '__main__':
    main()
