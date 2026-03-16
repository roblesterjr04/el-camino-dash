# El Camino Dash Image Project

This subtree defines an image-first provisioning path aligned with
[`rpi-image-gen`](https://github.com/raspberrypi/rpi-image-gen).

## Layout

- `config/el-camino-dash.yaml`: image build configuration entrypoint.
- `hooks/`: build hooks that provision packages, app payload, services, and boot config.
- `runtime-manifest.txt`: runtime files synced into `/opt/el-camino-dash`.
- `INSTALLER_MAPPING.md`: mapping from legacy installer steps to image layers.

## Build (from a host that has rpi-image-gen installed)

```bash
rpi-image-gen build -c image/el-camino-dash/config/el-camino-dash.yaml
```

## Build hook expectations

Hooks in this subtree are designed to run in a build environment and use these variables:

- `ELC_SOURCE_DIR`: source checkout root (defaults to current repo root).
- `ELC_ROOTFS_DIR`: mounted rootfs directory to populate.
- `ELC_TARGET_USER`: target runtime user (defaults to `pi`).

If your `rpi-image-gen` environment uses different variable names, map them in your layer config.
