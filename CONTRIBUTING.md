# Contributing

**This repository is updated often, but issues are not monitored.** Questions, bug reports and feature requests
opened as issues will probably go unanswered.

**Pull requests are welcome** and are read: fixes, corrections to the docs, new cards, and main boards for other
vehicles. If you find a bug, the most useful report is a pull request that fixes it, or one that adds a failing test or
a note under the right README's *Known issues*.

Before you open one:

- Rebase on the current `main`; it moves often.
- Follow the ground rules in the [README's Contributing section](README.md#contributing): specs first, one firmware
  per board, run the host tests for what you touch, regenerate boards from `generate/`, and say what was and was not
  tested on hardware.
- For a new card, claim a product number by adding a row to the
  [products registry](docs/protocols/can_channels.md#products) in the same pull request.
- The guides for new hardware are [designing your own main board](docs/hardware/main_board_guide.md) and
  [the CAN bus, explained](docs/protocols/can_guide.md).

By contributing you agree that your work is released under the repository's licenses: MIT for software and
documentation ([`LICENSE`](LICENSE)) and CERN-OHL-P-2.0 for hardware ([`LICENSE-HARDWARE`](LICENSE-HARDWARE)).
