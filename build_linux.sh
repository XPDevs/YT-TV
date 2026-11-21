#!/usr/bin/env bash
# Build yt_tv for regular 64-bit Linux (Debian / Ubuntu / Mint)
set -euo pipefail

cd "$(dirname "$0")"

if [ "$(uname -m)" != "x86_64" ]; then
    echo "This script targets 64-bit Linux, but this machine is $(uname -m)." >&2
    exit 1
fi

# Use sudo unless we're already root
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

echo "==> Installing runtime GStreamer codecs + CEC for TV remote control..."
$SUDO apt-get install -y --no-install-recommends \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-x \
    cec-utils

echo "==> Compiling..."
g++ -O2 -fpermissive -std=gnu++17 \
    -o yt_tv yt_tv.c \
    $(pkg-config --cflags --libs \
        webkit2gtk-4.1 gtk+-3.0 gdk-x11-3.0 \
        gstreamer-1.0 gstreamer-video-1.0 \
        x11 xinerama xrandr xtst)

echo "==> Done: ./yt_tv"
