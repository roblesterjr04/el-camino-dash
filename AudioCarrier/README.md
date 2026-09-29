# Audio carrier (work in progress, superseded design)

First cut of the audio accessory board: ESP32-DevKitC + four MAX98357A breakouts (front L/R, rear L/R, GAIN tied to +5 V)
with a resistive line-level tap into four RCA jacks. The PCB is placed but **not routed**.

This is being replaced by a smaller design: bare ESP32-WROOM-32E module + TI PCM4104 4-channel DAC, face-mounted RCA
jacks. That needs the PCM4104 datasheet pinout, which is not in the repo yet. Regenerate this version with
`generate/gen_sch.py`, `generate/gen_footprints.py`, `generate/gen_pcb.py` (KiCad 7 tools; see the scripts).
