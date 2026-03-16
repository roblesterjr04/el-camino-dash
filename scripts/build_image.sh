#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_PATH="image/el-camino-dash/config/el-camino-dash.yaml"
REMOTE_HOST="${ELC_IMAGE_BUILDER_HOST:-}"
REMOTE_DIR="${ELC_IMAGE_BUILDER_DIR:-~/el-camino-dash-build}"
ARTIFACT_DIR="${ELC_IMAGE_ARTIFACT_DIR:-${PROJECT_DIR}/build-artifacts/rpi-image-gen-work}"
FORCE_LOCAL=0
FORCE_DOCKER=0
DOCKER_IMAGE="${ELC_IMAGE_DOCKER_IMAGE:-el-camino/rpi-image-gen-builder:bookworm}"
DOCKER_WORK_VOLUME="${ELC_IMAGE_DOCKER_WORK_VOLUME:-elc-rpi-image-gen-work}"

usage() {
  cat <<'EOF'
Usage:
  bash scripts/build_image.sh [options] [config_path]

Options:
  --remote <user@host>      Run build on remote Linux builder over SSH.
  --remote-dir <path>       Remote working directory (default: ~/el-camino-dash-build).
  --artifact-dir <path>     Local path to pull back remote work directory artifacts.
  --docker                  Run build in local Docker builder container.
  --docker-image <name>     Docker image tag (default: el-camino/rpi-image-gen-builder:bookworm).
  --docker-work-volume <n>  Docker volume for /workspace/work (default: elc-rpi-image-gen-work).
  --local                   Force local build mode.
  -h, --help                Show this help.

Env fallbacks:
  ELC_IMAGE_BUILDER_HOST
  ELC_IMAGE_BUILDER_DIR
  ELC_IMAGE_ARTIFACT_DIR
  ELC_IMAGE_DOCKER_IMAGE
  ELC_IMAGE_DOCKER_WORK_VOLUME

Examples:
  bash scripts/build_image.sh --remote pi@builder.local
  bash scripts/build_image.sh --docker
  bash scripts/build_image.sh --remote pi@builder.local --remote-dir /home/pi/builds/elcamino
  ELC_IMAGE_BUILDER_HOST=pi@builder.local bash scripts/build_image.sh
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --remote)
      REMOTE_HOST="${2:-}"
      shift 2
      ;;
    --remote-dir)
      REMOTE_DIR="${2:-}"
      shift 2
      ;;
    --artifact-dir)
      ARTIFACT_DIR="${2:-}"
      shift 2
      ;;
    --docker)
      FORCE_DOCKER=1
      shift
      ;;
    --docker-image)
      DOCKER_IMAGE="${2:-}"
      shift 2
      ;;
    --docker-work-volume)
      DOCKER_WORK_VOLUME="${2:-}"
      shift 2
      ;;
    --local)
      FORCE_LOCAL=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      CONFIG_PATH="$1"
      shift
      ;;
  esac
done

if [[ ! -f "${PROJECT_DIR}/${CONFIG_PATH}" ]]; then
  echo "Config not found: ${CONFIG_PATH}" >&2
  exit 1
fi

run_local_build() {
  local bin=""
  if command -v rpi-image-gen >/dev/null 2>&1; then
    bin="$(command -v rpi-image-gen)"
  fi

  if [[ -z "${bin}" ]]; then
    echo "rpi-image-gen is required but was not found." >&2
    echo "Install it on this Linux host, or use --docker / --remote." >&2
    echo "Reference: https://github.com/raspberrypi/rpi-image-gen" >&2
    exit 1
  fi

  exec "${bin}" build -S "${PROJECT_DIR}" -c "${CONFIG_PATH}"
}

run_docker_build() {
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required for --docker mode." >&2
    exit 1
  fi

  if ! docker image inspect "${DOCKER_IMAGE}" >/dev/null 2>&1; then
    echo "Docker builder image not found locally, building ${DOCKER_IMAGE}..."
    docker build \
      -f "${PROJECT_DIR}/docker/rpi-image-gen-builder/Dockerfile" \
      -t "${DOCKER_IMAGE}" \
      "${PROJECT_DIR}"
  fi

  docker volume inspect "${DOCKER_WORK_VOLUME}" >/dev/null 2>&1 || docker volume create "${DOCKER_WORK_VOLUME}" >/dev/null
  echo "Running rpi-image-gen build in Docker (${DOCKER_IMAGE})..."
  docker run --rm --privileged \
    -v "${PROJECT_DIR}:/workspace" \
    -v "${DOCKER_WORK_VOLUME}:/workspace/work" \
    -w /workspace \
    "${DOCKER_IMAGE}" \
    bash -lc "if command -v rpi-image-gen >/dev/null 2>&1; then BIN=\$(command -v rpi-image-gen); elif [[ -x /opt/rpi-image-gen/rpi-image-gen ]]; then BIN=/opt/rpi-image-gen/rpi-image-gen; else echo 'rpi-image-gen not found in container'; exit 1; fi; \"\${BIN}\" build -S /workspace -c '${CONFIG_PATH}'"
  local build_status=$?

  echo "Syncing docker work volume to ${ARTIFACT_DIR}..."
  mkdir -p "${ARTIFACT_DIR}"
  docker run --rm \
    -v "${DOCKER_WORK_VOLUME}:/from" \
    -v "${ARTIFACT_DIR}:/to" \
    alpine:3.20 \
    sh -lc '
      set -eu
      rm -rf /to/*
      mkdir -p /to
      # Copy only image/deploy outputs to avoid device-node copy errors
      for path in /from/deploy-* /from/image-*; do
        [ -e "$path" ] || continue
        cp -a "$path" /to/
      done
    '

  if [[ ${build_status} -ne 0 ]]; then
    echo "Docker build failed. Work artifacts/log context copied to ${ARTIFACT_DIR}." >&2
    return ${build_status}
  fi

  echo "Docker build complete. Artifacts copied to ${ARTIFACT_DIR}."
}

run_remote_build() {
  if [[ -z "${REMOTE_HOST}" ]]; then
    echo "Remote host is required for remote build mode." >&2
    echo "Set --remote <user@host> or ELC_IMAGE_BUILDER_HOST." >&2
    exit 1
  fi

  for cmd in rsync ssh; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
      echo "${cmd} is required for remote builds." >&2
      exit 1
    fi
  done

  echo "Syncing project to remote builder ${REMOTE_HOST}:${REMOTE_DIR}..."
  ssh "${REMOTE_HOST}" "mkdir -p '${REMOTE_DIR}'"
  rsync -az --delete \
    --exclude ".git/" \
    --exclude ".venv/" \
    --exclude ".cursor/" \
    --exclude "__pycache__/" \
    --exclude "*.pyc" \
    --exclude "build-artifacts/" \
    "${PROJECT_DIR}/" "${REMOTE_HOST}:${REMOTE_DIR}/"

  echo "Running rpi-image-gen build on remote builder..."
  ssh "${REMOTE_HOST}" bash -s -- "${REMOTE_DIR}" "${CONFIG_PATH}" <<'EOF'
set -euo pipefail
REMOTE_DIR="$1"
CONFIG_PATH="$2"
cd "${REMOTE_DIR}"

BIN=""
if command -v rpi-image-gen >/dev/null 2>&1; then
  BIN="$(command -v rpi-image-gen)"
fi

if [[ -z "${BIN}" ]]; then
  echo "rpi-image-gen not found on remote builder." >&2
  echo "Install rpi-image-gen on the remote builder host." >&2
  exit 1
fi

"${BIN}" build -S "${REMOTE_DIR}" -c "${CONFIG_PATH}"
EOF

  echo "Pulling back remote build artifacts to ${ARTIFACT_DIR}..."
  mkdir -p "${ARTIFACT_DIR}"
  rsync -az "${REMOTE_HOST}:${REMOTE_DIR}/work/" "${ARTIFACT_DIR}/"
  echo "Remote build complete. Artifacts synced to: ${ARTIFACT_DIR}"
}

if [[ "${FORCE_LOCAL}" -eq 1 ]]; then
  run_local_build
fi

if [[ -n "${REMOTE_HOST}" ]]; then
  run_remote_build
elif [[ "${FORCE_DOCKER}" -eq 1 ]]; then
  run_docker_build
elif [[ "$(uname -s)" == "Linux" ]]; then
  run_local_build
elif command -v docker >/dev/null 2>&1; then
  echo "Non-Linux host detected ($(uname -s)); using Docker builder mode."
  run_docker_build
else
  echo "Non-Linux host detected ($(uname -s))." >&2
  echo "Use --docker (recommended) or --remote <user@host>." >&2
  exit 1
fi
