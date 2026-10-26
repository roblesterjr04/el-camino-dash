# Panel order costing (2026-10-26)

Where the money goes in the PCBWay quote T-I24W1016483A (5 panels, $2,451.60) and what the cheapest realistic order looks
like. The re-sourced parts list is `Panel/fab/Panel_BOM_resourced.csv` (every line with a JLCPCB/LCSC part number, a
substitute where the original is out of stock or overpriced, and a Source column: JLC = fitted from JLCPCB stock, DK = bought
in the US and consigned, HAND = through-hole, bought and soldered by hand).

## Parts, per set

| | PCBWay quote | Re-sourced |
|---|---|---|
| Fitted by the assembler (SMD) | $175 | $76 (JLC stock) + $18 (consigned: Si4735, SMA jack, FRAM, USB-C, 3.5 mm jack, PTS810 switches) |
| Through-hole, by hand | $22 | $22 |
| **Total** | **$197.53** | **$116.18** |

Substitutes, applied to the boards on 2026-10-26: TLV9062 for the OPA2376 (mic preamp), TJA1051T/3 for the eight MCP2562,
PTTC / Littelfuse PTCs for the Bourns ones (same hold current and voltage), a COILMX MS1050-6R8M for the Wurth inductor
(L1's footprint is now `DigiDash2:L_Molded_1050`), a YXC 20 pF crystal for the 18 pF Abracon (C29 / C30 33p), Connfly /
XFCN sockets and headers for the Sullins ones. Not done: the generic SMA jacks (SMA-KWE) are through-hole right-angle
parts, not edge-mount, so the radio card keeps the Amphenol 132289, consigned (~$5). Re-sourced parts are $116 a set.
The expensive chips (ATmega2560 ~$13, STM32G0B1 ~$5.20, TVP5150 ~$9.40) are at their real price everywhere.

## Fees and duties

- JLCPCB Standard PCBA: setup $25.56 (one side) / $51.12 (two), stencil $8.21 / $16.42, $1.53 per BOM line per order,
  joints ~$0.0016. Fees are per order, so quantity is nearly free. Their assembly panel limit is 250 x 250 mm: DigiDash2
  goes as a single board (329 x 113) and the cards as a second panel, so two orders. Bare PCBs: 5 minimum.
- US duties: JLCPCB charges a flat 35 % DDP on PCB + assembly + parts (March 2026); PCBWay is similar. The PCBWay quote as it
  stands lands at about $3,400 with duties and shipping. Nothing avoids this except US assembly, which costs more than it saves.
- Consignment: DigiKey to Shenzhen by DHL ~$35 plus JLCPCB's 2 % / $10 minimum handling. Worth it only for the Si4735.

## Scenarios (best guess, landed in the US)

| Scenario | PCBs | Fees | Fitted parts | Duties 35 % | Consigned + logistics | Hand THT | Ship | Total |
|---|---|---|---|---|---|---|---|---|
| JLCPCB, 2 full sets (DD order + 5-card order) | $250 | $300 | $166 | $250 | $79 | $54 | $65 | **~$1,160** |
| JLCPCB, 1 full set | $250 | $298 | $83 | $221 | $66 | $32 | $65 | **~$1,010** |
| JLCPCB, 1 set of DigiDash2 + Sensor + Console | $200 | $212 | $57 | $164 | $58 | $24 | $55 | **~$770** |
| PCBWay, 2 panels, re-sourced BOM, THT by hand | $220 | ~$525 | ~$210 | $334 | — | $54 | $80 | **~$1,420** |
| PCBWay quote as sent, 5 panels | $338 | $1,141 | $988 | $858 | — | — | ~$100 | **~$3,400** |

PCB prices are estimates (JLCPCB 4-layer: DigiDash2 x5 ~$140, five-card panel x5 ~$110). Fees are from the published table and
the line counts in the BOM (54 lines on the DigiDash2 order, 73 on the cards order, 39 for Sensor + Console alone).

## Levers still open

- Leave the audio card out of the first cards order: it is the only two-sided board, so the order drops to one side
  (-$34 setup and stencil) and ~15 BOM lines (-$23) and $10 of parts.
- Use JLCPCB global sourcing for the five consigned lines instead of shipping them yourself (-$40 to -$50 if their fee is small).
- Merge resistor and capacitor values that appear on only one or two boards: each line is $1.53 per order it appears in.
- Ask JLCPCB whether the whole panel can go as one "single PCB" with internal mouse bites (460 x 500 limit): one order
  instead of two saves ~$120 of fees.
