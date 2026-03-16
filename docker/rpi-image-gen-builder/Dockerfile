FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
  ca-certificates \
  curl \
  git \
  python3 \
  rsync \
  sudo \
  xz-utils \
  && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/raspberrypi/rpi-image-gen.git /opt/rpi-image-gen \
  && apt-get update \
  && /opt/rpi-image-gen/install_deps.sh \
  && rm -rf /var/lib/apt/lists/*

ENV PATH="/opt/rpi-image-gen:${PATH}"
WORKDIR /workspace
