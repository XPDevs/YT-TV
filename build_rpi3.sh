#!/usr/bin/env bash
# Build yt_tv for a Raspberry Pi 3 running Raspberry Pi OS Lite (Bookworm).
# Run it from the TV with:  xinit ./yt_tv -- :0
set -euo pipefail

cd "$(dirname "$0")"

if ! [ -f /etc/os-release ] || ! grep -qi "raspbian\|raspberry" /etc/os-release; then
    echo "This script expects Raspberry Pi OS (raspbian/raspberry in /etc/os-release)." >&2
    read -r -p "Continue anyway? [y/N] " ans || true
    [[ "$ans" =~ ^[Yy]$ ]] || exit 1
fi

# Use sudo unless we're already root (RPi OS Lite images run as 'pi' by default)
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

echo "==> Installing build dependencies..."
$SUDO apt-get update
$SUDO apt-get install -y --no-install-recommends \
    build-essential pkg-config \
    libwebkit2gtk-4.1-dev \
    libgtk-3-dev \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libx11-dev libxinerama-dev libxrandr-dev libxtst-dev

echo "==> Installing runtime deps (X server + GStreamer codecs + CEC diagnostics)..."
# HDMI-CEC is handled inside yt_tv via /dev/cec0 (kernel CEC API), so no
# libcec/cec-client is needed; v4l-utils only provides cec-ctl for testing.
$SUDO apt-get install -y --no-install-recommends \
    xinit xserver-xorg \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-x \
    gstreamer1.0-alsa \
    v4l-utils \
    unclutter

echo "==> Compiling..."
g++ -O2 -fpermissive -std=gnu++17 \
    -o yt_tv yt_tv.c \
    $(pkg-config --cflags --libs \
        webkit2gtk-4.1 gtk+-3.0 gdk-x11-3.0 \
        gstreamer-1.0 gstreamer-video-1.0 \
        x11 xinerama xrandr xtst)

echo "==> Granting input/video group access (needed for /dev/input remote keys)"
USER_ME="${SUDO_USER:-$(id -un)}"
$SUDO usermod -a -G input,video "$USER_ME"

echo
echo "==> Done: ./yt_tv"
echo "    Run it from the TV with:  xinit ./yt_tv -- :0"
echo "    Re-login (or reboot) once for the input/video group change to apply."
