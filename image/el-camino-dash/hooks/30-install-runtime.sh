#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_rootfs

manifest="${SOURCE_DIR}/image/el-camino-dash/runtime-manifest.txt"
if [[ ! -f "${manifest}" ]]; then
  echo "Missing runtime manifest: ${manifest}" >&2
  exit 1
fi

target_runtime="$(in_rootfs "${RUNTIME_DIR}")"
mkdir -p "${target_runtime}"

while IFS= read -r entry; do
  [[ -z "${entry}" ]] && continue
  src="${SOURCE_DIR}/${entry}"
  if [[ ! -e "${src}" ]]; then
    echo "Runtime manifest entry not found: ${entry}" >&2
    exit 1
  fi
  if [[ -d "${src}" ]]; then
    rsync -a --delete \
      --exclude "__pycache__/" \
      --exclude "*.pyc" \
      "${src}/" "${target_runtime}/${entry}/"
  else
    install -D -m 0644 "${src}" "${target_runtime}/${entry}"
  fi
done < "${manifest}"

install -m 0755 "${SOURCE_DIR}/scripts/gauge-mcp-monitor" "$(in_rootfs /usr/local/bin/gauge-mcp-monitor)"
install -m 0755 "${SOURCE_DIR}/scripts/start-gauge-hdmi-x11.sh" "$(in_rootfs /usr/local/bin/start-gauge-hdmi-x11)"

# Install SSH authorized key explicitly to avoid relying on upstream key mapping.
auth_key_src="${SOURCE_DIR}/keys/pi_authorized_key.pub"
target_home="$(in_rootfs "/home/${TARGET_USER}")"
target_ssh_dir="${target_home}/.ssh"
target_auth_keys="${target_ssh_dir}/authorized_keys"
if [[ -f "${auth_key_src}" ]]; then
  install -d -m 0700 "${target_ssh_dir}"
  install -m 0600 "${auth_key_src}" "${target_auth_keys}"
  chroot "${ROOTFS_DIR}" chown -R "${TARGET_USER}:${TARGET_USER}" "/home/${TARGET_USER}/.ssh" || true
else
  echo "WARNING: SSH key not found at ${auth_key_src}; key auth may fail." >&2
fi

# Enforce key-based SSH authentication on target image.
sshd_dropin_dir="$(in_rootfs /etc/ssh/sshd_config.d)"
install -d -m 0755 "${sshd_dropin_dir}"
cat > "${sshd_dropin_dir}/10-elc-keyauth.conf" <<'EOF'
PubkeyAuthentication yes
AuthorizedKeysFile .ssh/authorized_keys
PasswordAuthentication no
KbdInteractiveAuthentication no
ChallengeResponseAuthentication no
EOF

# Build runtime venv in image for first boot readiness.
if [[ -f "${target_runtime}/requirements.txt" ]]; then
  chroot "${ROOTFS_DIR}" /usr/bin/python3 -m venv --system-site-packages "${RUNTIME_DIR}/.venv" || true
  chroot "${ROOTFS_DIR}" /bin/bash -lc "${RUNTIME_DIR}/.venv/bin/python -m pip install -r ${RUNTIME_DIR}/requirements.txt" || true
fi

chroot "${ROOTFS_DIR}" chown -R "${TARGET_USER}:${TARGET_USER}" "${RUNTIME_DIR}" || true
