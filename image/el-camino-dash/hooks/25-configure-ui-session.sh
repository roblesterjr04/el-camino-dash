#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_rootfs

install -d "$(in_rootfs /etc/lightdm/lightdm.conf.d)"
cat > "$(in_rootfs /etc/lightdm/lightdm.conf.d/99-gauge-autologin.conf)" <<EOF
[Seat:*]
autologin-user=${TARGET_USER}
autologin-user-timeout=0
user-session=lightdm-xsession
autologin-session=lightdm-xsession
EOF

target_home="$(in_rootfs /home/${TARGET_USER})"
mkdir -p "${target_home}"
cat > "${target_home}/.xsession" <<'EOF'
#!/usr/bin/env sh
set -eu

if command -v openbox-session >/dev/null 2>&1; then
  exec openbox-session
fi
if command -v openbox >/dev/null 2>&1; then
  exec openbox
fi
if command -v x-session-manager >/dev/null 2>&1; then
  exec x-session-manager
fi
if command -v x-window-manager >/dev/null 2>&1; then
  exec x-window-manager
fi
exec x-terminal-emulator
EOF

chmod 755 "${target_home}/.xsession"
