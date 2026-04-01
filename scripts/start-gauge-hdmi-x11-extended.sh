#!/usr/bin/env bash
# xinit must run as root so Xorg can open the console (vt1). Python runs as user pi via x11-extended-xinit-client.sh.
set -euo pipefail
RUNROOT="/opt/el-camino-dash"
CLIENT="${RUNROOT}/scripts/x11-extended-xinit-client.sh"
exec /usr/bin/xinit "${CLIENT}" -- /usr/bin/Xorg :0 -nolisten tcp vt1
