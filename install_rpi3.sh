#!/usr/bin/env bash
#
# install_rpi3.sh - set up (or remove) the yt_tv kiosk on a Raspberry Pi.
#
# Installs: X server with the VC4/KMS graphics driver, ALSA audio, the
# GStreamer stack (incl. V4L2 hardware H.264 decode), the WebKitGTK
# runtime, zram + swap tuning for 1 GiB boards, and a systemd service
# that starts yt_tv fullscreen on tty1 at boot.
#
#   sudo ./install_rpi3.sh                 install (idempotent - safe to re-run)
#   sudo ./install_rpi3.sh --status        show what is installed + diagnostics
#   sudo ./install_rpi3.sh --uninstall     remove everything this script added
#   sudo ./install_rpi3.sh --dry-run       show what would change, change nothing
#   sudo ./install_rpi3.sh --bin ./yt_tv   use a specific binary
#   sudo ./install_rpi3.sh --reboot        reboot when finished
#
# Options:
#   --bin PATH     yt_tv binary to install (default: ./yt_tv, ./dist/yt_tv)
#   --user NAME    user that owns the X session (default: the sudo user)
#   --no-config    do not touch /boot/firmware/config.txt or cmdline.txt
#   --no-memory    do not touch zram / swap / sysctl settings
#   --dry-run      print commands instead of running them
#   --uninstall    stop, disable and remove the kiosk
#   --status       print current state and hardware diagnostics
#   --reboot       reboot at the end
#   -v, --verbose  echo every command that runs
#   -h, --help     this text
#
# Everything it does is logged to /var/log/yt-tv-install.log.
#
set -euo pipefail

APP_NAME="yt-tv"
APP_DIR="/opt/yt-tv"
DEFAULTS="/etc/default/yt-tv"
UNIT="/etc/systemd/system/${APP_NAME}.service"
START_SH="${APP_DIR}/start.sh"
SYSCTL_FILE="/etc/sysctl.d/99-yt-tv.conf"
LOG_FILE="/var/log/yt-tv-install.log"

BIN=""
RUN_USER=""
EDIT_CONFIG=1
EDIT_MEMORY=1
DRY_RUN=0
VERBOSE=0
DO_UNINSTALL=0
DO_STATUS=0
DO_REBOOT=0

while [ $# -gt 0 ]; do
    case "$1" in
        --bin)        BIN="${2:-}"; shift 2 ;;
        --user)       RUN_USER="${2:-}"; shift 2 ;;
        --no-config)  EDIT_CONFIG=0; shift ;;
        --no-memory)  EDIT_MEMORY=0; shift ;;
        --dry-run)    DRY_RUN=1; VERBOSE=1; shift ;;
        --uninstall)  DO_UNINSTALL=1; shift ;;
        --status)     DO_STATUS=1; shift ;;
        --reboot)     DO_REBOOT=1; shift ;;
        -v|--verbose) VERBOSE=1; shift ;;
        -h|--help)    awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit 0 ;;
        *) echo "Unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
done

# ----------------------------------------------------------------- colours
if [ -t 1 ]; then
    C_OK=$'\033[1;32m'; C_INFO=$'\033[1;34m'; C_WARN=$'\033[1;33m'
    C_ERR=$'\033[1;31m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
else
    C_OK=""; C_INFO=""; C_WARN=""; C_ERR=""; C_DIM=""; C_OFF=""
fi

STEP=0
STEPS=0
say()  { printf '%s==>%s %s\n' "$C_INFO" "$C_OFF" "$*"; log "INFO  $*"; }
step() { STEP=$((STEP + 1)); printf '%s[%d/%d]%s %s\n' "$C_INFO" "$STEP" "$STEPS" "$C_OFF" "$*"; log "STEP  $*"; }
ok()   { printf '    %sok%s   %s\n' "$C_OK" "$C_OFF" "$*"; }
warn() { printf '    %swarn%s %s\n' "$C_WARN" "$C_OFF" "$*" >&2; log "WARN  $*"; }
die()  { printf '%s!!%s %s\n' "$C_ERR" "$C_OFF" "$*" >&2; log "FATAL $*"; exit 1; }
log()  {
    if [ "${LOGGING:-0}" = "1" ]; then
        printf '%s %s\n' "$(date '+%F %T')" "$*" >>"$LOG_FILE" 2>/dev/null || true
    fi
}

run() {
    if [ "$VERBOSE" -eq 1 ]; then
        printf '    %s$ %s%s\n' "$C_DIM" "$*" "$C_OFF"
    fi
    log "RUN   $*"
    if [ "$DRY_RUN" -eq 1 ]; then return 0; fi
    "$@"
}

# write_stdin PATH MODE   (file content on stdin)
write_stdin() {
    local path="$1" mode="${2:-644}"
    log "WRITE $path"
    if [ "$DRY_RUN" -eq 1 ]; then
        cat >/dev/null
        printf '    %swould write%s %s\n' "$C_DIM" "$C_OFF" "$path"
        return 0
    fi
    cat >"$path"
    chmod "$mode" "$path"
}

# -------------------------------------------------------------------- root
if [ "$(id -u)" -ne 0 ] && [ "$DRY_RUN" -eq 0 ]; then
    exec sudo -E bash "$0" "$@"
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$DRY_RUN" -eq 0 ]; then
    if touch "$LOG_FILE" 2>/dev/null; then
        LOGGING=1
        log "===== $APP_NAME installer: $* ====="
    else
        LOG_FILE="/tmp/yt-tv-install.log"
        LOGGING=1
    fi
else
    LOGGING=0
fi

# ================================================================ helpers
model_name() {
    local m=""
    if [ -r /proc/device-tree/model ]; then
        m="$(tr -d '\0' </proc/device-tree/model)"
    fi
    printf '%s' "${m:-unknown}"
}

total_ram_mb() {
    awk '/^MemTotal:/ {print int($2/1024); exit}' /proc/meminfo 2>/dev/null || echo 0
}

boot_dir() {
    local d
    for d in /boot/firmware /boot; do
        if [ -f "$d/config.txt" ]; then printf '%s' "$d"; return 0; fi
    done
    return 1
}

cec_devices() { ls /dev/cec* 2>/dev/null || true; }

hdmi_state() {
    local f state="" found=0
    for f in /sys/class/drm/card*-HDMI-A-*/status; do
        [ -r "$f" ] || continue
        found=1
        state="$(cat "$f")"
        if [ "$state" = "connected" ]; then printf 'connected'; return 0; fi
    done
    [ "$found" -eq 1 ] && printf 'disconnected' || printf 'unknown'
}

have() { command -v "$1" >/dev/null 2>&1; }

# =============================================================== preflight
preflight() {
    local fail=0 ram model disk cec hdmi

    [ "$(uname -m)" = "aarch64" ] || {
        printf '%s!!%s This is not 64-bit Raspberry Pi OS (uname -m: %s).\n' \
            "$C_ERR" "$C_OFF" "$(uname -m)"
        fail=1
    }

    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        case "${VERSION_CODENAME:-}" in
            bookworm) ;;
            bullseye|buster)
                printf '%s!!%s %s is too old: yt_tv needs WebKitGTK 4.1 (bookworm).\n' \
                    "$C_ERR" "$C_OFF" "Raspberry Pi OS ${VERSION_CODENAME}"
                fail=1 ;;
            *) warn "untested OS release '${VERSION_CODENAME:-unknown}'" ;;
        esac
    fi

    ram="$(total_ram_mb)"
    model="$(model_name)"
    cec="$(cec_devices | tr '\n' ' ')"
    hdmi="$(hdmi_state)"

    printf '\n  %shardware%s\n' "$C_INFO" "$C_OFF"
    printf '    model      : %s\n' "$model"
    printf '    os         : %s\n' "$(. /etc/os-release 2>/dev/null; printf '%s %s' "${PRETTY_NAME:-?}" 2>/dev/null)"
    printf '    kernel     : %s\n' "$(uname -r)"
    printf '    memory     : %s MiB\n' "$ram"
    printf '    hdmi       : %s\n' "$hdmi"
    printf '    cec device : %s\n' "${cec:-none found}"

    if [ "$ram" -gt 0 ] && [ "$ram" -lt 1536 ]; then
        printf '    %snote%s       : 1 GiB board - zram + 480/720p playback recommended\n' \
            "$C_WARN" "$C_OFF"
    fi

    disk="$(df -Pm / | awk 'NR==2 {print $4}')"
    if [ -n "$disk" ] && [ "$disk" -lt 400 ]; then
        warn "only ${disk} MiB free on / - the packages need ~400 MiB"
    fi

    if [ -z "$cec" ]; then
        warn "no /dev/cec* yet - HDMI-CEC appears after enabling vc4-kms-v3d and rebooting"
    fi
    if [ "$hdmi" = "disconnected" ]; then
        warn "no HDMI display detected right now (fine if the TV is off)"
    fi

    if [ "$fail" -ne 0 ]; then
        die "preflight failed - fix the items above and re-run"
    fi
    printf '\n'
}

# ================================================================ packages
PKGS="
xinit xserver-xorg xserver-xorg-legacy x11-xserver-utils
xserver-xorg-video-fbdev xserver-xorg-input-libinput xserver-xorg-input-evdev
libwebkit2gtk-4.1-0 libgtk-3-0
libgstreamer1.0-0 libgstreamer-plugins-base1.0-0
libx11-6 libxinerama1 libxrandr2 libxtst6
gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
gstreamer1.0-plugins-bad gstreamer1.0-libav gstreamer1.0-x gstreamer1.0-alsa
alsa-utils fonts-dejavu-core ca-certificates zram-tools v4l-utils
"

install_packages() {
    export DEBIAN_FRONTEND=noninteractive

    if ! apt-cache show libwebkit2gtk-4.1-0 >/dev/null 2>&1; then
        die "libwebkit2gtk-4.1-0 is not in this image's apt sources.
   yt_tv links against WebKitGTK 4.1, which shipped with bookworm.
   Flash the 64-bit 'Raspberry Pi OS Lite (bookworm)' image."
    fi

    run apt-get update
    # shellcheck disable=SC2086
    run apt-get install -y --no-install-recommends $PKGS

    local missing=""
    have xinit || missing="$missing xinit"
    if [ ! -e /usr/lib/xorg/Xorg ] && ! have Xorg; then missing="$missing Xorg"; fi
    [ -e /usr/lib/aarch64-linux-gnu/libwebkit2gtk-4.1.so.0 ] || missing="$missing libwebkit2gtk-4.1"
    [ -e /usr/lib/aarch64-linux-gnu/libgtk-3.so.0 ] || missing="$missing libgtk-3"
    if [ -n "$missing" ]; then die "packages installed but these are missing:$missing"; fi
    ok "X server, WebKitGTK 4.1, GTK3, GStreamer, ALSA present"

    if have gst-inspect-1.0; then
        local e
        for e in alsasink ximagesink avdec_h264 v4l2h264dec v4l2slh264dec; do
            if gst-inspect-1.0 "$e" >/dev/null 2>&1; then
                ok "gstreamer element $e"
            else
                printf '    %s--   %s not available%s\n' "$C_DIM" "$e" "$C_OFF"
            fi
        done
    fi
}

# ============================================================ boot config
configure_boot() {
    local dir cfg cmdline
    dir="$(boot_dir)" || { warn "no config.txt found - skipping boot config"; return 0; }
    cfg="$dir/config.txt"
    cmdline="$dir/cmdline.txt"

    if [ ! -f "$cfg.yt-tv.bak" ]; then
        run cp -a "$cfg" "$cfg.yt-tv.bak"
        ok "backed up $cfg -> $cfg.yt-tv.bak"
    fi

    # VC4 full-KMS: gives us /dev/dri, the modesetting X driver and /dev/cec0.
    if grep -qE '^[[:space:]]*dtoverlay=vc4-kms-v3d' "$cfg"; then
        ok "vc4-kms-v3d already enabled"
    elif grep -qE '^[[:space:]]*#dtoverlay=vc4-kms-v3d' "$cfg"; then
        run sed -i 's|^[[:space:]]*#dtoverlay=vc4-kms-v3d.*|dtoverlay=vc4-kms-v3d|' "$cfg"
        ok "enabled dtoverlay=vc4-kms-v3d"
    else
        if [ "$DRY_RUN" -eq 0 ]; then
            printf '\n[all]\ndtoverlay=vc4-kms-v3d\n' >>"$cfg"
        fi
        ok "added dtoverlay=vc4-kms-v3d"
    fi

    # Analog jack + HDMI audio from the SoC (bcm2835 ALSA card).
    if grep -qE '^[[:space:]]*dtparam=audio=on' "$cfg"; then
        ok "audio already enabled"
    elif grep -qE '^[[:space:]]*#dtparam=audio=on' "$cfg"; then
        run sed -i 's|^[[:space:]]*#dtparam=audio=on.*|dtparam=audio=on|' "$cfg"
        ok "enabled dtparam=audio=on"
    else
        if [ "$DRY_RUN" -eq 0 ]; then printf 'dtparam=audio=on\n' >>"$cfg"; fi
        ok "added dtparam=audio=on"
    fi

    add_cfg() {
        if ! grep -qE "^[[:space:]]*$1=" "$cfg"; then
            if [ "$DRY_RUN" -eq 0 ]; then printf '%s=%s\n' "$1" "$2" >>"$cfg"; fi
            ok "set $1=$2"
        fi
    }
    add_cfg hdmi_force_hotplug 1     # TV box: keep HDMI driven even if the TV is off
    add_cfg gpu_mem 64               # KMS needs little; RAM is precious
    add_cfg disable_overscan 1
    add_cfg disable_splash 1         # no rainbow screen

    if [ -f "$cmdline" ] && ! grep -q 'consoleblank=0' "$cmdline"; then
        run sed -i '1 s/$/ consoleblank=0/' "$cmdline"
        ok "disabled console blanking in cmdline.txt"
    fi
}

# ================================================================ memory
configure_memory() {
    if [ -f /etc/default/zramswap ] && [ ! -f /etc/default/zramswap.yt-tv.bak ]; then
        run cp -a /etc/default/zramswap /etc/default/zramswap.yt-tv.bak
    fi
    write_stdin /etc/default/zramswap 644 <<'EOF'
# zram-tools: compressed swap in RAM. yt_tv on a 1 GiB Pi needs this to
# keep WebKit's helper processes alive while the TV UI is loaded.
ALGO=lz4
PERCENT=100
PRIORITY=100
EOF
    run systemctl enable zramswap.service || warn "zramswap.service not found"
    run systemctl restart zramswap.service || warn "zramswap did not start (retried at boot)"

    write_stdin "$SYSCTL_FILE" 644 <<'EOF'
# yt_tv / Raspberry Pi: swap early, and keep zram's compressed chunks
# small so one page fault does not decompress a whole cluster.
vm.swappiness=100
vm.page-cluster=0
vm.vfs_cache_pressure=50
EOF
    run sysctl -q -p "$SYSCTL_FILE"

    # Bigger fallback swapfile than the RPi OS default of 100 MB.
    if [ -f /etc/dphys-swapfile ]; then
        local cur
        cur="$(sed -n 's/^CONF_SWAPSIZE=\([0-9]*\).*/\1/p' /etc/dphys-swapfile | tail -1)"
        if [ -z "$cur" ] || [ "$cur" -lt 512 ]; then
            if grep -q '^CONF_SWAPSIZE=' /etc/dphys-swapfile; then
                run sed -i 's/^CONF_SWAPSIZE=.*/CONF_SWAPSIZE=512/' /etc/dphys-swapfile
            else
                run sh -c "echo CONF_SWAPSIZE=512 >>/etc/dphys-swapfile"
            fi
            run systemctl restart dphys-swapfile.service || warn "swapfile resize applies at boot"
            ok "swapfile raised to 512 MB"
        fi
    fi
}

# ============================================================== X + groups
configure_x() {
    run mkdir -p /etc/X11
    write_stdin /etc/X11/Xwrapper.config 644 <<'EOF'
# yt_tv kiosk: X is started by yt-tv.service on tty1, not by startx.
allowed_users=anybody
needs_root_rights=yes
EOF
    local g
    for g in input video audio render tty; do
        if getent group "$g" >/dev/null 2>&1; then
            run usermod -a -G "$g" "$RUN_USER"
        fi
    done
    ok "X wrapper + groups (input, video, audio, render) for ${RUN_USER}"
}

# ================================================================== the app
install_app() {
    run install -d -m 755 "$APP_DIR"
    run install -m 755 "$BIN" "$APP_DIR/yt_tv"
    # The boot animation is gone: the app goes straight to the TV UI.
    run rm -f "$APP_DIR/yt_tv_startup.mp4"

    if [ ! -f "$DEFAULTS" ]; then
        write_stdin "$DEFAULTS" 644 <<'EOF'
# Runtime environment for yt-tv.  Edit, then: sudo systemctl restart yt-tv
#
# WebKitGTK on the Pi 3B: no GL compositor, no DMABUF renderer (both are
# broken/slow on VC4), and no bubblewrap sandbox processes (saves ~3 x 20 MB).
WEBKIT_DISABLE_COMPOSITING_MODE=1
WEBKIT_DISABLE_DMABUF_RENDERER=1
WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1

# Fewer glibc malloc arenas -> measurably less RSS in WebKit's threads.
MALLOC_ARENA_MAX=2

# Prefer the V4L2 hardware H.264 decoder over the software one.
GST_PLUGIN_FEATURE_RANK=v4l2h264dec:MAX,v4l2slh264dec:MAX,v4l2convert:MAX

# Uncomment to let YouTube serve VP9/AV1 (software decode: far too slow).
#YT_TV_ALLOW_VP9=1

# Uncomment to disable the built-in HDMI-CEC remote support.
#YT_TV_NO_CEC=1
EOF
    fi

    write_stdin "$START_SH" 755 <<'EOF'
#!/bin/bash
# X client for yt-tv: environment, audio level, then the app.
set -u
if [ -r /etc/default/yt-tv ]; then . /etc/default/yt-tv; fi

export WEBKIT_DISABLE_COMPOSITING_MODE="${WEBKIT_DISABLE_COMPOSITING_MODE:-1}"
export WEBKIT_DISABLE_DMABUF_RENDERER="${WEBKIT_DISABLE_DMABUF_RENDERER:-1}"
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS="${WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS:-1}"
export MALLOC_ARENA_MAX="${MALLOC_ARENA_MAX:-2}"
export GST_PLUGIN_FEATURE_RANK="${GST_PLUGIN_FEATURE_RANK:-v4l2h264dec:MAX,v4l2slh264dec:MAX}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

# bcm2835 exposes 'PCM'; HDMI/USB audio devices usually expose 'Master'.
if command -v amixer >/dev/null 2>&1; then
    amixer -q sset PCM 85% unmute >/dev/null 2>&1 || true
    amixer -q sset Master 85% unmute >/dev/null 2>&1 || true
fi
if command -v xset >/dev/null 2>&1; then
    xset s off >/dev/null 2>&1 || true
    xset -dpms >/dev/null 2>&1 || true
    xset s noblank >/dev/null 2>&1 || true
fi

cd /opt/yt-tv || exit 1
exec ./yt_tv "$@"
EOF
    ok "app installed in $APP_DIR"
}

install_service() {
    write_stdin "$UNIT" 644 <<EOF
[Unit]
Description=YouTube TV kiosk (yt_tv)
After=systemd-user-sessions.service sound.target network-online.target
Wants=network-online.target sound.target
Conflicts=getty@tty1.service

[Service]
Type=simple
User=${RUN_USER}
PAMName=login
TTYPath=/dev/tty1
StandardInput=tty
StandardOutput=journal
StandardError=journal
EnvironmentFile=-${DEFAULTS}
Environment=XDG_RUNTIME_DIR=/run/user/${RUN_UID}
Environment=HOME=/home/${RUN_USER}
ExecStart=/usr/bin/xinit ${START_SH} -- :0 vt1 -nolisten tcp -nocursor
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

    if [ ! -d "/run/user/${RUN_UID}" ] && [ "$DRY_RUN" -eq 0 ]; then
        mkdir -p "/run/user/${RUN_UID}"
        chown "${RUN_USER}:${RUN_USER}" "/run/user/${RUN_UID}" 2>/dev/null || true
        chmod 700 "/run/user/${RUN_UID}" 2>/dev/null || true
    fi

    run systemctl daemon-reload
    run systemctl enable "$APP_NAME.service"
    # getty on tty1 would fight X for the console.
    run systemctl disable getty@tty1.service
    ok "systemd unit installed and enabled"
}

report() {
    cat <<EOF

${C_OK}────────────────────────────────────────────────────────────${C_OFF}
 yt_tv is installed and enabled for user '${RUN_USER}'.

   sudo reboot        # or --reboot next time

 It comes up straight into YouTube TV on tty1: HDMI video, audio on the
 3.5 mm jack or HDMI, and the TV remote via built-in HDMI-CEC.

   logs      journalctl -u yt-tv -f
   restart   sudo systemctl restart yt-tv
   stop      sudo systemctl stop yt-tv
   tune      sudo nano /etc/default/yt-tv
   uninstall sudo $HERE/install_rpi3.sh --uninstall

 Controls: arrows + Enter, Space/K play-pause, J/L seek, F9/F10 volume,
 M mute, hold Esc or Q quit. USB keyboards, gamepads and CEC remotes
 all work.

 On a 1 GiB Pi 3B keep playback at 480p/720p (YouTube TV settings).
 1080p H.264 uses the hardware decoder; VP9/AV1 is blocked on purpose
 because this CPU cannot decode it in real time.
${C_OK}────────────────────────────────────────────────────────────${C_OFF}

EOF
}

# ================================================================= install
do_install() {
    STEP=0
    if [ "$EDIT_CONFIG" -eq 1 ] && [ "$EDIT_MEMORY" -eq 1 ]; then STEPS=7
    elif [ "$EDIT_CONFIG" -eq 1 ] || [ "$EDIT_MEMORY" -eq 1 ]; then STEPS=6
    else STEPS=5; fi

    preflight

    BIN_RESOLVED="$BIN"
    if [ -z "$BIN_RESOLVED" ]; then
        local c
        for c in "$HERE/yt_tv" "$HERE/dist/yt_tv" "$PWD/yt_tv"; do
            if [ -f "$c" ]; then BIN_RESOLVED="$c"; break; fi
        done
    fi
    if [ -z "$BIN_RESOLVED" ] || [ ! -f "$BIN_RESOLVED" ]; then
        die "yt_tv binary not found. Build it with ./build_rpi3_cross.sh, copy
   dist/yt_tv next to this script, or pass --bin PATH."
    fi
    file "$BIN_RESOLVED" | grep -q 'ARM aarch64' || die \
        "'$BIN_RESOLVED' is not an aarch64 binary - build it with ./build_rpi3_cross.sh."
    BIN="$BIN_RESOLVED"
    ok "binary: $BIN"

    step "Installing packages"
    install_packages

    if [ "$EDIT_CONFIG" -eq 1 ]; then
        step "Configuring GPU / HDMI / audio (boot config)"
        configure_boot
    else
        warn "skipping boot config (--no-config)"
    fi

    if [ "$EDIT_MEMORY" -eq 1 ]; then
        step "Configuring zram + swap for 1 GiB RAM"
        configure_memory
    else
        warn "skipping memory tuning (--no-memory)"
    fi

    step "Configuring X (kiosk, no display manager)"
    configure_x

    step "Installing the application"
    install_app

    step "Installing the systemd service"
    install_service

    step "Finishing"
    ok "done"
    report

    if [ "$DO_REBOOT" -eq 1 ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
            say "would reboot now"
        else
            say "rebooting"
            systemctl reboot
        fi
    fi
}

# =================================================================== status
do_status() {
    local ram
    printf '\n%syt_tv status%s\n' "$C_INFO" "$C_OFF"
    printf '  service   : '
    if systemctl is-active --quiet "$APP_NAME.service" 2>/dev/null; then
        printf '%srunning%s\n' "$C_OK" "$C_OFF"
    else
        printf '%sstopped%s\n' "$C_WARN" "$C_OFF"
    fi
    printf '  enabled   : %s\n' "$(systemctl is-enabled "$APP_NAME.service" 2>/dev/null || echo no)"
    printf '  binary    : %s\n' "$([ -x "$APP_DIR/yt_tv" ] && echo "$APP_DIR/yt_tv" || echo 'not installed')"
    printf '  user      : %s\n' "$(awk -F= '/^User=/{print $2}' "$UNIT" 2>/dev/null || echo '?')"
    printf '  cec       : %s\n' "$(cec_devices | tr '\n' ' ' | sed 's/ $//;s/^$/none (needs vc4-kms-v3d + reboot)/')"
    printf '  hdmi      : %s\n' "$(hdmi_state)"
    ram="$(total_ram_mb)"
    printf '  memory    : %s MiB total, %s MiB available\n' "$ram" \
        "$(awk '/^MemAvailable:/ {print int($2/1024); exit}' /proc/meminfo)"
    printf '  swap      : %s\n' "$(free -m | awk '/^Swap:/ {print $2" MiB"}')"
    printf '  zram      : %s\n' "$(ls /dev/zram0 >/dev/null 2>&1 && echo present || echo absent)"
    printf '  config.txt: %s\n' "$(boot_dir 2>/dev/null || echo 'not found')"
    if have gst-inspect-1.0; then
        printf '  h264 hw   : %s\n' \
            "$(gst-inspect-1.0 v4l2h264dec >/dev/null 2>&1 && echo 'v4l2h264dec available' || echo 'software decode only')"
    fi
    printf '\n  last log lines:\n'
    journalctl -u "$APP_NAME.service" -n 12 --no-pager 2>/dev/null | sed 's/^/    /' || true
    printf '\n'
}

# ================================================================ uninstall
do_uninstall() {
    say "Removing yt_tv"
    STEP=0
    STEPS=6

    step "Stopping the service"
    run systemctl stop "$APP_NAME.service" 2>/dev/null || true
    run systemctl disable "$APP_NAME.service" 2>/dev/null || true

    step "Removing unit and app"
    run rm -f "$UNIT"
    run rm -rf "$APP_DIR"
    run rm -f "$DEFAULTS"
    run systemctl daemon-reload

    step "Restoring getty on tty1"
    run systemctl enable getty@tty1.service 2>/dev/null || true

    step "Reverting memory tuning"
    run rm -f "$SYSCTL_FILE"
    if [ -f /etc/default/zramswap.yt-tv.bak ]; then
        run mv -f /etc/default/zramswap.yt-tv.bak /etc/default/zramswap
    fi
    run sysctl -q --system 2>/dev/null || true

    step "Reverting X wrapper config"
    run rm -f /etc/X11/Xwrapper.config

    step "Restoring boot config"
    local dir
    if dir="$(boot_dir)" && [ -f "$dir/config.txt.yt-tv.bak" ]; then
        run cp -a "$dir/config.txt.yt-tv.bak" "$dir/config.txt"
        run rm -f "$dir/config.txt.yt-tv.bak"
        ok "config.txt restored (reboot to apply)"
    else
        ok "no boot config backup found - nothing to restore"
    fi

    cat <<EOF

${C_OK}yt_tv removed.${C_OFF}
  Kept on purpose: the apt packages (X, WebKitGTK, GStreamer, ALSA) and
  the swapfile size. Remove packages with:
      sudo apt-get autoremove --purge
  and reboot to get your console back on tty1.

EOF
}

# ==================================================================== main
case "$RUN_USER" in
    "") if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
            RUN_USER="$SUDO_USER"
        elif id pi >/dev/null 2>&1; then
            RUN_USER="pi"
        else
            RUN_USER="$(awk -F: '$3 >= 1000 && $3 < 60000 {print $1; exit}' /etc/passwd)"
        fi ;;
esac
id "$RUN_USER" >/dev/null 2>&1 || die "user '$RUN_USER' does not exist (use --user NAME)."
RUN_UID="$(id -u "$RUN_USER")"

if [ "$DO_STATUS" -eq 1 ]; then
    do_status
elif [ "$DO_UNINSTALL" -eq 1 ]; then
    do_uninstall
else
    do_install
fi
