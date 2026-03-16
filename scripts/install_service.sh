#!/usr/bin/env bash
set -euo pipefail

cat <<'EOF'
scripts/install_service.sh is archived.

Systemd unit files are now tracked under:
  deploy/systemd/

To install/update them on a device:
  sudo bash scripts/install_runtime_services.sh

For normal iterative development:
  bash scripts/deploy_to_pi.sh <user@host>
EOF

exit 1
