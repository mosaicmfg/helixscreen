# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen - VNC Runtime Image
#
# Build:   docker build -t helixscreen-vnc .
# Run:     docker run -p 8080:8080 -p 5900:5900 helixscreen-vnc
# Access:  http://localhost:8080/vnc.html
#
# Target: x86_64 Linux — native SDL build with headless VNC access.
#
# Strategy: Multi-stage. Stage 1 builds helixscreen with system SDL2.
# Stage 2 packages the binary with Xvfb + x11vnc + noVNC for browser access.
# Resolution is configurable via env vars (RESOLUTION, HELIX_SCREEN_SIZE).
#
# Ports:
#   8080/tcp — noVNC web UI (HTML5 VNC in browser)
#   5900/tcp — Direct VNC (native VNC clients)
#
# CI note: The source checkout MUST include submodules:
#   git submodule update --init --recursive

FROM debian:bookworm-slim AS builder

LABEL stage="helixscreen-builder"

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    git \
    pkg-config \
    cmake \
    python3 \
    make \
    file \
    libsdl2-dev \
    libssl-dev \
    libasound2-dev \
    libnl-3-dev \
    libnl-genl-3-dev \
    libusb-1.0-0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY . .

RUN git -C lib/lvgl init && git -C lib/lvgl add -A && \
    git -C lib/libhv init && git -C lib/libhv add -A

RUN make PLATFORM_TARGET=native SKIP_OPTIONAL_DEPS=1 -j$(nproc)

FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libsdl2-2.0-0 \
    libssl3 \
    libasound2 \
    ca-certificates \
    xvfb \
    x11vnc \
    novnc \
    libnl-3-200 \
    libnl-genl-3-200 \
    libusb-1.0-0 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/build/bin/helix-screen /usr/local/bin/

ENV DISPLAY=:99
ENV RESOLUTION=1280x720x24
ENV HELIX_SCREEN_SIZE=1280x720

RUN cat > /docker-entrypoint.sh << 'EOF'
#!/bin/bash
set -e

cleanup() {
    echo "Shutting down..."
    kill $XVFB_PID $X11VNC_PID $NOVNC_PID 2>/dev/null || true
    wait
}

trap cleanup EXIT INT TERM

Xvfb :99 -screen 0 $RESOLUTION &
XVFB_PID=$!
sleep 1

x11vnc -display :99 -forever -nopw -quiet &
X11VNC_PID=$!
sleep 1

/usr/share/novnc/utils/novnc_proxy --vnc localhost:5900 --listen 8080 &
NOVNC_PID=$!
sleep 1

/usr/local/bin/helix-screen "$@"
EOF

RUN chmod +x /docker-entrypoint.sh

EXPOSE 8080
EXPOSE 5900

ENTRYPOINT ["/docker-entrypoint.sh"]
CMD ["--test", "-vv"]
