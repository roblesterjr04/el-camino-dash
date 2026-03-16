# Installer to Image Mapping

This document maps legacy `install.sh` behavior to the new image-centric flow.

## Layer Responsibilities

- `base-packages`: install apt dependencies required by dashboard runtime.
- `boot-config`: configure Raspberry Pi boot firmware settings and kernel cmdline.
- `ui-session`: configure LightDM autologin and user X session bootstrap.
- `app-runtime`: place application files under `/opt/el-camino-dash`.
- `systemd`: install tracked unit files and enable the HDMI runtime path.
- `boot-splash`: install Plymouth splash assets and activate the theme.

## Legacy Step Mapping

| Legacy step (`install.sh`) | New home | Notes |
|---|---|---|
| Pre-clean apt cache | build environment concern | no app-specific layer needed |
| apt package install | `layer/base-packages` | baked into image |
| boot config edits | `layer/boot-config` | baked into image |
| LightDM + `.xsession` | `layer/ui-session` | baked into image |
| project rsync to `~/gauge-cluster` | `layer/app-runtime` | now copied to `/opt/el-camino-dash` |
| venv creation + pip install | `layer/app-runtime` | image creates `.venv` at build time |
| Arduino compile/flash | excluded from base image | hardware-conditional post-provision task |
| systemd unit generation | `layer/systemd` | tracked files in repo, no heredocs |
| mode selection service toggles | `layer/systemd` | HDMI enabled, SPI masked |
| boot splash setup script | `layer/boot-splash` | baked into image |
| Navit/BT checks | docs + optional verify script | not required for build success |
| user group membership | `layer/base-packages` | baked into image |

## Explicit Exclusions

The image build intentionally does not run these behaviors:

- Live Arduino flashing over `/dev/serial0` or USB.
- Runtime process stopping for flash safety.
- Interactive Bluetooth pairing sessions.
- Repetitive apt cache cleanup logic.

These remain optional operational steps after a device is provisioned.
