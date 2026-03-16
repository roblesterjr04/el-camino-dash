# Provision and Development Workflows

## Provision (image build)

No dedicated build host is required if you have Docker.

### Option A: Docker builder (recommended for macOS)

```bash
bash scripts/build_image.sh --docker
```

This builds/uses `docker/rpi-image-gen-builder/Dockerfile` and runs `rpi-image-gen` inside a privileged Linux container.
Docker mode builds in a Linux Docker volume (`/workspace/work`) to avoid macOS bind-mount filesystem issues, then copies artifacts to `build-artifacts/rpi-image-gen-work/`.

### Option B: Remote Linux builder over SSH

```bash
bash scripts/build_image.sh --remote pi@<linux-builder>
```

### Option C: Local Linux host with `rpi-image-gen` installed in PATH

```bash
bash scripts/build_image.sh --local
```

After build:

- Docker/local builds place artifacts in repo `work/` (rpi-image-gen default).
- Remote builds sync artifacts back to `build-artifacts/rpi-image-gen-work/`.
- Flash generated image to SD card and boot target Pi.

Optional:

- Set `ELC_IMAGE_BUILDER_HOST=pi@<linux-builder>` and run `bash scripts/build_image.sh`.
- Set `ELC_IMAGE_DOCKER_IMAGE` to override the Docker builder image tag.

## Develop (fast iteration)

Deploy app/runtime updates to a provisioned Pi:

```bash
bash scripts/deploy_to_pi.sh pi@<host>
```

Useful options:

- `--with-pip`: refresh `.venv` dependencies.
- `--install-services`: install/update tracked units from `deploy/systemd/`.
- `--logs`: print recent HDMI service logs after deploy.
- `--no-restart`: sync only (skip service restart).

## Runtime paths

- App root on target: `/opt/el-camino-dash`
- Systemd units in repo: `deploy/systemd/`
- Image config and hooks: `image/el-camino-dash/`
