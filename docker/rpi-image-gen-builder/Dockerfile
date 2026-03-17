FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
  build-essential \
  ca-certificates \
  curl \
  file \
  git \
  gpg \
  gpg-agent \
  kmod \
  pigz \
  python3 \
  quilt \
  rsync \
  slirp4netns \
  sudo \
  xxd \
  xz-utils \
  && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL https://archive.raspberrypi.com/debian/raspberrypi.gpg.key \
  | gpg --dearmor > /usr/share/keyrings/raspberrypi-archive-keyring.gpg

ARG RPIIG_GIT_SHA=
RUN if [ -n "${RPIIG_GIT_SHA}" ]; then \
    git clone --no-checkout https://github.com/raspberrypi/rpi-image-gen.git /opt/rpi-image-gen \
    && cd /opt/rpi-image-gen \
    && git checkout "${RPIIG_GIT_SHA}"; \
  else \
    git clone --depth 1 https://github.com/raspberrypi/rpi-image-gen.git /opt/rpi-image-gen; \
  fi

ARG TARGETARCH
RUN /bin/bash -lc '\
  arch="${TARGETARCH:-}"; \
  if [ -z "${arch}" ]; then \
    host="$(dpkg --print-architecture)"; \
    case "${host}" in \
      amd64|arm64) arch="${host}" ;; \
      *) echo "Unsupported host architecture: ${host}" >&2; exit 1 ;; \
    esac; \
  fi; \
  echo "Installing dependencies for architecture: ${arch}"; \
  case "${arch}" in \
    arm64) \
      apt-get update && /opt/rpi-image-gen/install_deps.sh ;; \
    amd64) \
      sed -i "s|\"\${binfmt_misc_required}\" == \"1\"|! -z \"\"|g" /opt/rpi-image-gen/scripts/dependencies_check; \
      if ! grep -q binfmt_misc /proc/filesystems; then \
        echo "binfmt_misc is not supported. Install binfmt-support on host." >&2; \
        exit 1; \
      fi; \
      apt-get update && apt-get install --no-install-recommends -y \
        qemu-user-static \
        dirmngr \
        parted \
        debootstrap \
        zerofree \
        libcap2-bin \
        libarchive-tools \
        bc \
        arch-test \
      && /opt/rpi-image-gen/install_deps.sh ;; \
    *) \
      echo "Architecture ${arch} is not supported (arm64/amd64)." >&2; \
      exit 1 ;; \
  esac \
  && rm -rf /var/lib/apt/lists/*'

ENV PATH="/opt/rpi-image-gen:${PATH}"
WORKDIR /workspace
