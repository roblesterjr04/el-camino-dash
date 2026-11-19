# Console board, 4 keys (abandoned)

**Status: abandoned on 2026-10-03; became [`ConsoleBoardRS/`](../ConsoleBoardRS/README.md).** Only
`ConsoleBoard4Key.kicad_pro` is tracked here. The schematic, board, footprints and fab files were removed when the
design was renamed, and are in the git history.

What it was (from the git history): a variant of [`ConsoleBoard`](../ConsoleBoard/README.md) for a housing
111.5 × 38.5 mm outside. A 108 × 35 mm board with the same ESP32-S3, SSD1322 OLED module, socket and pinout, and four
E-Switch TL1240YQ lit keys in a column left of the screen instead of the Back / View keys and loose LEDs. Two later
commits that day put the RetroSound knobs on RJ11 jacks. Then the four keys, their LEDs and the LED driver were
removed (the knobs give six buttons and two encoders), the screen was centred, and the board was renamed
ConsoleBoardRS.

**Firmware:** none of its own; the console firmware is [`firmware/esp32s3_console`](../firmware/esp32s3_console/README.md).

To see the last full version: `git show e1e93e6 --stat` lists its files; `git checkout e1e93e6 -- ConsoleBoard4Key`
restores them.
