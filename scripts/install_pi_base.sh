#!/usr/bin/env bash
set -euo pipefail

cat <<'EOF'
scripts/install_pi_base.sh is archived.

Base package and boot provisioning now belong to the image build flow:
  bash scripts/build_image.sh

After provisioning a device from the built image, use:
  bash scripts/deploy_to_pi.sh <user@host>
EOF

exit 1
