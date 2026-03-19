#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_PATH="image/el-camino-dash/config/el-camino-dash.yaml"
SOURCE_ROOT="${ELC_IMAGE_SOURCE_ROOT:-${PROJECT_DIR}/image/el-camino-dash}"
REMOTE_HOST="${ELC_IMAGE_BUILDER_HOST:-}"
REMOTE_DIR="${ELC_IMAGE_BUILDER_DIR:-~/el-camino-dash-build}"
ARTIFACT_DIR="${ELC_IMAGE_ARTIFACT_DIR:-${PROJECT_DIR}/build-artifacts/rpi-image-gen-work}"
SSH_PUBKEY_FILE="${ELC_IMAGE_SSH_PUBKEY_FILE:-}"
ARTEFACT_VERSION="${ELC_IMAGE_ARTEFACT_VERSION:-}"
FORCE_LOCAL=0
FORCE_DOCKER=0
DOCKER_IMAGE="${ELC_IMAGE_DOCKER_IMAGE:-el-camino/rpi-image-gen-builder:bookworm}"
DOCKER_WORK_VOLUME="${ELC_IMAGE_DOCKER_WORK_VOLUME:-elc-rpi-image-gen-work}"
DOCKER_FRESH_WORK="${ELC_IMAGE_DOCKER_FRESH_WORK:-y}"

usage() {
  cat <<'EOF'
Usage:
  bash scripts/build_image.sh [options] [config_path]

Options:
  --remote <user@host>      Run build on remote Linux builder over SSH.
  --remote-dir <path>       Remote working directory (default: ~/el-camino-dash-build).
  --artifact-dir <path>     Local path to pull back remote work directory artifacts.
  --source-root <path>      rpi-image-gen source root (must contain config/ and layer/).
  --ssh-pubkey-file <path>  Public key file for image SSH auth (default: auto-detect in ~/.ssh).
  --artefact-version <v>    Override IGconf_artefact_version (default: git describe without -dirty).
  --docker                  Run build in local Docker builder container.
  --docker-image <name>     Docker image tag (default: el-camino/rpi-image-gen-builder:bookworm).
  --docker-work-volume <n>  Docker volume for /workspace/work (default: elc-rpi-image-gen-work).
  --fresh-work              Use a fresh docker work volume for this run (default).
  --reuse-work              Reuse configured docker work volume.
  --local                   Force local build mode.
  -h, --help                Show this help.

Env fallbacks:
  ELC_IMAGE_BUILDER_HOST
  ELC_IMAGE_BUILDER_DIR
  ELC_IMAGE_ARTIFACT_DIR
  ELC_IMAGE_SOURCE_ROOT
  ELC_IMAGE_SSH_PUBKEY_FILE
  ELC_IMAGE_ARTEFACT_VERSION
  ELC_IMAGE_DOCKER_IMAGE
  ELC_IMAGE_DOCKER_WORK_VOLUME
  ELC_IMAGE_DOCKER_FRESH_WORK

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
    --source-root)
      SOURCE_ROOT="${2:-}"
      shift 2
      ;;
    --ssh-pubkey-file)
      SSH_PUBKEY_FILE="${2:-}"
      shift 2
      ;;
    --artefact-version)
      ARTEFACT_VERSION="${2:-}"
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
    --fresh-work)
      DOCKER_FRESH_WORK="y"
      shift
      ;;
    --reuse-work)
      DOCKER_FRESH_WORK="n"
      shift
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

if [[ "${SOURCE_ROOT}" != /* ]]; then
  SOURCE_ROOT="${PROJECT_DIR}/${SOURCE_ROOT}"
fi

if [[ "${ARTIFACT_DIR}" != /* ]]; then
  ARTIFACT_DIR="${PROJECT_DIR}/${ARTIFACT_DIR}"
fi

if [[ "${CONFIG_PATH}" != /* ]]; then
  CONFIG_ABS="${PROJECT_DIR}/${CONFIG_PATH}"
else
  CONFIG_ABS="${CONFIG_PATH}"
fi

if [[ ! -d "${SOURCE_ROOT}" ]]; then
  echo "Source root not found: ${SOURCE_ROOT}" >&2
  exit 1
fi

if [[ ! -f "${CONFIG_ABS}" ]]; then
  echo "Config not found: ${CONFIG_ABS}" >&2
  exit 1
fi

case "${CONFIG_ABS}" in
  "${SOURCE_ROOT}/"*)
    case "${CONFIG_ABS}" in
      "${SOURCE_ROOT}/config/"*)
        # rpi-image-gen searches <SRCROOT>/config for -c files, so pass path
        # relative to that directory.
        SOURCE_CONFIG_PATH="${CONFIG_ABS#${SOURCE_ROOT}/config/}"
        ;;
      *)
        SOURCE_CONFIG_PATH="${CONFIG_ABS#${SOURCE_ROOT}/}"
        ;;
    esac
    ;;
  *)
    echo "Config must be inside source root." >&2
    echo "Config: ${CONFIG_ABS}" >&2
    echo "Source root: ${SOURCE_ROOT}" >&2
    exit 1
    ;;
esac

case "${SOURCE_ROOT}" in
  "${PROJECT_DIR}/"*)
    SOURCE_ROOT_REL="${SOURCE_ROOT#${PROJECT_DIR}/}"
    ;;
  *)
    echo "Source root must be inside project for docker/remote modes." >&2
    echo "Source root: ${SOURCE_ROOT}" >&2
    exit 1
    ;;
esac

compute_artefact_version() {
  if [[ -n "${ARTEFACT_VERSION}" ]]; then
    return 0
  fi
  local described
  described="$(git -C "${PROJECT_DIR}" describe --tags --always --dirty 2>/dev/null || date +%Y-%m-%d)"
  ARTEFACT_VERSION="$(printf "%s" "${described}" | sed -E 's/-dirty$//')"
}

prepare_local_ssh_pubkey() {
  if ! grep -Eq "^[[:space:]]*pubkey_user1:" "${CONFIG_ABS}"; then
    return 0
  fi

  local selected="${SSH_PUBKEY_FILE}"
  if [[ -z "${selected}" ]]; then
    for candidate in \
      "${HOME}/.ssh/id_rsa.pub" \
      "${HOME}/.ssh/id_ed25519.pub" \
      "${HOME}/.ssh/id_ecdsa.pub"; do
      if [[ -f "${candidate}" ]]; then
        selected="${candidate}"
        break
      fi
    done
  fi

  if [[ -z "${selected}" || ! -f "${selected}" ]]; then
    echo "No SSH public key file found." >&2
    echo "Set --ssh-pubkey-file <path> or ELC_IMAGE_SSH_PUBKEY_FILE." >&2
    exit 1
  fi

  local key_dir="${PROJECT_DIR}/keys"
  local out_file="${key_dir}/pi_authorized_key.pub"
  mkdir -p "${key_dir}"
  cp "${selected}" "${out_file}"
  chmod 0644 "${out_file}"
  echo "Using SSH public key: ${selected}"
}

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

  exec "${bin}" build -S "${SOURCE_ROOT}" -c "${SOURCE_CONFIG_PATH}" -- "IGconf_artefact_version=${ARTEFACT_VERSION}"
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

  local work_volume="${DOCKER_WORK_VOLUME}"
  mkdir -p "${ARTIFACT_DIR}"
  if [[ "${DOCKER_FRESH_WORK}" == "y" ]]; then
    work_volume="${DOCKER_WORK_VOLUME}-$(date +%Y%m%d%H%M%S)"
    docker volume create "${work_volume}" >/dev/null
  else
    docker volume inspect "${work_volume}" >/dev/null 2>&1 || docker volume create "${work_volume}" >/dev/null
  fi

  echo "Running rpi-image-gen build in Docker (${DOCKER_IMAGE})..."
  docker run --rm --privileged \
    -v "${PROJECT_DIR}:/workspace" \
    -v "${work_volume}:/workspace/work" \
    -v "${ARTIFACT_DIR}:/workspace/artifacts" \
    -e TMPDIR=/workspace/work/tmp \
    -w /workspace \
    "${DOCKER_IMAGE}" \
    bash -lc "mkdir -p /workspace/work/tmp; rm -rf '/workspace/artifacts/deploy-${ARTEFACT_VERSION}'; if command -v rpi-image-gen >/dev/null 2>&1; then BIN=\$(command -v rpi-image-gen); elif [[ -x /opt/rpi-image-gen/rpi-image-gen ]]; then BIN=/opt/rpi-image-gen/rpi-image-gen; else echo 'rpi-image-gen not found in container'; exit 1; fi; \"\${BIN}\" build -S '/workspace/${SOURCE_ROOT_REL}' -c '${SOURCE_CONFIG_PATH}' -- 'IGconf_artefact_version=${ARTEFACT_VERSION}' 'IGconf_deploy_dir=/workspace/artifacts/deploy-${ARTEFACT_VERSION}'"
  local build_status=$?

  if [[ "${DOCKER_FRESH_WORK}" == "y" ]]; then
    docker volume rm "${work_volume}" >/dev/null 2>&1 || true
  fi

  if [[ ${build_status} -ne 0 ]]; then
    echo "Docker build failed. Inspect container output for details." >&2
    return ${build_status}
  fi

  echo "Docker build complete. Deploy artifacts written to ${ARTIFACT_DIR}."
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
  REMOTE_SOURCE_ROOT="${REMOTE_DIR}/${SOURCE_ROOT_REL}"
  ssh "${REMOTE_HOST}" bash -s -- "${REMOTE_SOURCE_ROOT}" "${SOURCE_CONFIG_PATH}" "${ARTEFACT_VERSION}" <<'EOF'
set -euo pipefail
REMOTE_SOURCE_ROOT="$1"
CONFIG_PATH="$2"
ARTEFACT_VERSION="$3"
cd "${REMOTE_SOURCE_ROOT}"

BIN=""
if command -v rpi-image-gen >/dev/null 2>&1; then
  BIN="$(command -v rpi-image-gen)"
fi

if [[ -z "${BIN}" ]]; then
  echo "rpi-image-gen not found on remote builder." >&2
  echo "Install rpi-image-gen on the remote builder host." >&2
  exit 1
fi

"${BIN}" build -S "${REMOTE_SOURCE_ROOT}" -c "${CONFIG_PATH}" -- "IGconf_artefact_version=${ARTEFACT_VERSION}"
EOF

  echo "Pulling back remote build artifacts to ${ARTIFACT_DIR}..."
  mkdir -p "${ARTIFACT_DIR}"
  rsync -az --delete --prune-empty-dirs \
    --include='deploy-*/' \
    --include='deploy-*/**' \
    --exclude='*' \
    "${REMOTE_HOST}:${REMOTE_DIR}/work/" "${ARTIFACT_DIR}/"
  echo "Remote build complete. Artifacts synced to: ${ARTIFACT_DIR}"
}

if [[ "${FORCE_LOCAL}" -eq 1 ]]; then
  compute_artefact_version
  prepare_local_ssh_pubkey
  run_local_build
fi

if [[ -n "${REMOTE_HOST}" ]]; then
  compute_artefact_version
  prepare_local_ssh_pubkey
  run_remote_build
elif [[ "${FORCE_DOCKER}" -eq 1 ]]; then
  compute_artefact_version
  prepare_local_ssh_pubkey
  run_docker_build
elif [[ "$(uname -s)" == "Linux" ]]; then
  compute_artefact_version
  prepare_local_ssh_pubkey
  run_local_build
elif command -v docker >/dev/null 2>&1; then
  echo "Non-Linux host detected ($(uname -s)); using Docker builder mode."
  compute_artefact_version
  prepare_local_ssh_pubkey
  run_docker_build
else
  echo "Non-Linux host detected ($(uname -s))." >&2
  echo "Use --docker (recommended) or --remote <user@host>." >&2
  exit 1
fi
