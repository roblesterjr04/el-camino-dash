#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo bash scripts/setup_boot_image.sh" >&2
  exit 1
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_IMAGE="${PROJECT_DIR}/assets/bootscreen/boot2.png"

if [[ ! -f "${SOURCE_IMAGE}" ]]; then
  echo "Boot image not found: ${SOURCE_IMAGE}" >&2
  exit 1
fi

if [[ -f /boot/firmware/config.txt ]]; then
  BOOT_DIR="/boot/firmware"
elif [[ -f /boot/config.txt ]]; then
  BOOT_DIR="/boot"
else
  echo "Could not locate Raspberry Pi boot config directory." >&2
  exit 1
fi

CONFIG_TXT="${BOOT_DIR}/config.txt"
CMDLINE_TXT="${BOOT_DIR}/cmdline.txt"
THEME_DIR="/usr/share/plymouth/themes/el-camino"
PLYMOUTH_DEF="/usr/share/plymouth/themes/el-camino/el-camino.plymouth"
PLYMOUTH_SCRIPT="/usr/share/plymouth/themes/el-camino/el-camino.script"
DEFAULT_THEME_LINK="/usr/share/plymouth/themes/default.plymouth"
THEME_IMAGE="${THEME_DIR}/boot_hdmi.png"
TARGET_W=800
TARGET_H=480
PLYMOUTH_CONF="/etc/plymouth/plymouthd.conf"
BOOT_CHANGED=0

append_line_if_missing() {
  local file="$1"
  local line="$2"
  if grep -Fqx "${line}" "${file}"; then
    return 0
  fi
  printf "\n%s\n" "${line}" >> "${file}"
  BOOT_CHANGED=1
}

set_cmdline() {
  local file="$1"
  shift
  local cmdline
  cmdline="$(<"${file}")"
  for token in "$@"; do
    if [[ "${cmdline}" != *"${token}"* ]]; then
      cmdline="${cmdline} ${token}"
      BOOT_CHANGED=1
    fi
  done
  printf "%s\n" "${cmdline}" > "${file}"
}

remove_cmdline_prefix() {
  local file="$1"
  local prefix="$2"
  local cmdline
  cmdline="$(<"${file}")"
  local rebuilt=""
  local changed=0
  for token in ${cmdline}; do
    if [[ "${token}" == "${prefix}"* ]]; then
      changed=1
      continue
    fi
    rebuilt="${rebuilt} ${token}"
  done
  rebuilt="${rebuilt# }"
  if [[ "${changed}" -eq 1 ]]; then
    printf "%s\n" "${rebuilt}" > "${file}"
    BOOT_CHANGED=1
  fi
}

echo "[1/6] Installing Plymouth..."
apt-get update
apt-get install -y plymouth plymouth-themes

echo "[2/6] Installing custom Plymouth theme..."
mkdir -p "${THEME_DIR}"
if command -v python3 >/dev/null 2>&1; then
  # Normalize to a full-frame HDMI splash image so Plymouth does not render
  # a tiny centered logo on a black screen.
  python3 - "${SOURCE_IMAGE}" "${THEME_IMAGE}" "${TARGET_W}" "${TARGET_H}" <<'PY'
import sys
from PIL import Image
from PIL import ImageDraw

src = sys.argv[1]
dst = sys.argv[2]
target_w = int(sys.argv[3])
target_h = int(sys.argv[4])

img = Image.open(src).convert("RGB")
iw, ih = img.size
if iw <= 0 or ih <= 0:
    raise SystemExit("invalid source image size")

# Aspect-preserving "contain" into target frame.
scale = min(target_w / iw, target_h / ih)
new_w = max(1, int(iw * scale))
new_h = max(1, int(ih * scale))
resized = img.resize((new_w, new_h), Image.Resampling.LANCZOS)

canvas = Image.new("RGB", (target_w, target_h), (64, 64, 64))
paste_x = (target_w - new_w) // 2
paste_y = (target_h - new_h) // 2
canvas.paste(resized, (paste_x, paste_y))

# Add a bright frame so the splash is clearly visible on very dark panels.
draw = ImageDraw.Draw(canvas)
frame_pad = 3
draw.rectangle(
    (
        max(0, paste_x - frame_pad),
        max(0, paste_y - frame_pad),
        min(target_w - 1, paste_x + new_w + frame_pad),
        min(target_h - 1, paste_y + new_h + frame_pad),
    ),
    outline=(230, 230, 230),
    width=3,
)
canvas.save(dst, format="PNG")
PY
else
  cp "${SOURCE_IMAGE}" "${THEME_IMAGE}"
fi

cat > "${PLYMOUTH_DEF}" <<'EOF'
[Plymouth Theme]
Name=El Camino Boot Splash
Description=Static El Camino boot image
ModuleName=script

[script]
ImageDir=/usr/share/plymouth/themes/el-camino
ScriptFile=/usr/share/plymouth/themes/el-camino/el-camino.script
EOF

cat > "${PLYMOUTH_SCRIPT}" <<'EOF'
img = Image("boot_hdmi.png");
sprite = Sprite(img);
sprite.SetZ(10000);

sprite.SetX(0);
sprite.SetY(0);

Plymouth.SetBootProgressFunction(function () {});
Plymouth.SetRefreshFunction(function () {});
EOF

echo "[3/6] Setting default splash theme..."
# Register and select the custom theme in update-alternatives explicitly.
# On some Raspberry Pi OS images the alternative exists but is unresolved.
if command -v update-alternatives >/dev/null 2>&1; then
  update-alternatives --install "${DEFAULT_THEME_LINK}" default.plymouth "${PLYMOUTH_DEF}" 200 >/dev/null 2>&1 || true
  update-alternatives --set default.plymouth "${PLYMOUTH_DEF}" >/dev/null 2>&1 || true
fi
if command -v plymouth-set-default-theme >/dev/null 2>&1; then
  plymouth-set-default-theme -R el-camino
elif command -v update-alternatives >/dev/null 2>&1; then
  update-alternatives --set default.plymouth "${PLYMOUTH_DEF}" >/dev/null 2>&1 || true
  if command -v update-initramfs >/dev/null 2>&1; then
    update-initramfs -u
  fi
else
  echo "Neither plymouth-set-default-theme nor update-alternatives found." >&2
  exit 1
fi

ACTIVE_PLYMOUTH="$(readlink -f /etc/alternatives/default.plymouth 2>/dev/null || true)"
if [[ "${ACTIVE_PLYMOUTH}" != "${PLYMOUTH_DEF}" ]]; then
  echo "Warning: active plymouth theme is not el-camino (${ACTIVE_PLYMOUTH}). Forcing selection..."
  if command -v update-alternatives >/dev/null 2>&1; then
    update-alternatives --install "${DEFAULT_THEME_LINK}" default.plymouth "${PLYMOUTH_DEF}" 200 >/dev/null 2>&1 || true
    update-alternatives --set default.plymouth "${PLYMOUTH_DEF}" >/dev/null 2>&1 || true
    if command -v update-initramfs >/dev/null 2>&1; then
      update-initramfs -u
    fi
  fi
fi

# Force distro fallback path as well; some builds use this link directly.
ln -sf "${PLYMOUTH_DEF}" "${DEFAULT_THEME_LINK}"

ACTIVE_DEFAULT_LINK="$(readlink -f "${DEFAULT_THEME_LINK}" 2>/dev/null || true)"
if [[ "${ACTIVE_DEFAULT_LINK}" != "${PLYMOUTH_DEF}" ]]; then
  echo "Failed to activate el-camino as default plymouth theme." >&2
  echo "Expected: ${PLYMOUTH_DEF}" >&2
  echo "Actual:   ${ACTIVE_DEFAULT_LINK}" >&2
  exit 1
fi

echo "[4/6] Updating boot config..."
append_line_if_missing "${CONFIG_TXT}" "disable_splash=1"
append_line_if_missing "${CONFIG_TXT}" "avoid_warnings=1"

echo "[5/6] Updating kernel cmdline for quiet splash..."
remove_cmdline_prefix "${CMDLINE_TXT}" "console=tty1"
set_cmdline "${CMDLINE_TXT}" \
  "quiet" \
  "splash" \
  "plymouth.ignore-serial-consoles" \
  "vt.global_cursor_default=0" \
  "loglevel=0" \
  "logo.nologo" \
  "rd.systemd.show_status=false" \
  "systemd.show_status=false" \
  "udev.log_priority=0"

echo "[6/6] Configuring Plymouth daemon + TTY visibility..."
cat > "${PLYMOUTH_CONF}" <<'EOF'
[Daemon]
ShowDelay=0
UseSimpledrm=1
DeviceScale=1
EOF
systemctl disable getty@tty1.service >/dev/null 2>&1 || true
systemctl mask getty@tty1.service >/dev/null 2>&1 || true

if command -v update-initramfs >/dev/null 2>&1; then
  update-initramfs -u
fi

echo
echo "Boot splash setup complete."
echo "Boot image: ${SOURCE_IMAGE}"
echo "Theme: el-camino"
if [[ "${BOOT_CHANGED}" -eq 1 ]]; then
  echo "Boot configuration changed. Reboot required:"
  echo "  sudo reboot"
else
  echo "No boot config changes detected."
  echo "Reboot to verify splash behavior:"
  echo "  sudo reboot"
fi
