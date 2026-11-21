#!/usr/bin/env bash
#
# build_rpi3_cross.sh
#
# Cross-compiles yt_tv on this (x86-64) machine for a Raspberry Pi 3B
# running Raspberry Pi OS Lite 64-bit (Debian bookworm, aarch64).
#
#   ./build_rpi3_cross.sh                # build -> dist/yt_tv (stripped)
#   ./build_rpi3_cross.sh --package      # + dist/yt-tv-rpi3-*.tar.gz for scp
#   ./build_rpi3_cross.sh --no-strip     # keep symbols in dist/yt_tv
#   ./build_rpi3_cross.sh --clean        # drop the cached sysroot, rebuild
#   ./build_rpi3_cross.sh --no-deps      # skip the apt toolchain check
#
# How it works: there is no arm64 WebKitGTK on this machine, so the
# script downloads the Debian bookworm arm64 packages needed to compile
# and link (headers, .so stubs, pkg-config files) into a private sysroot
# (.rpi3-cross/sysroot) and points the cross toolchain at it.  Nothing
# is installed system-wide except the cross compiler itself, and the
# resulting binary targets bookworm's glibc, which is exactly what
# Raspberry Pi OS Lite 64-bit ships.
#
# First run downloads ~300 MB; later runs reuse the sysroot.
#
set -euo pipefail
cd "$(dirname "$0")"

DIST_NAME="bookworm"
MIRROR="${MIRROR:-https://deb.debian.org/debian}"

# The sysroot needs real symlinks (libfoo.so -> libfoo.so.1), which a
# FAT/exFAT stick cannot hold, so keep it on a normal filesystem.
default_work() {
    if ln -s .yt-tv-link-test "$PWD/.yt-tv-link-test" 2>/dev/null; then
        rm -f "$PWD/.yt-tv-link-test"
        printf '%s' "$PWD/.rpi3-cross"
    else
        printf '%s' "${XDG_CACHE_HOME:-$HOME/.cache}/yt-tv-rpi3-cross"
    fi
}
WORK="${YT_TV_CROSS_DIR:-$(default_work)}"
SYSROOT="$WORK/sysroot"
DEBDIR="$WORK/debs"
STAMP="$WORK/.sysroot-ok"
OUT="${OUT:-dist}"
CXX="${CXX:-aarch64-linux-gnu-g++}"
SRC="yt_tv.c"

# Packages (arm64) whose headers / link stubs / .pc files we need.
PKGS="
libc6-dev
libstdc++-12-dev
libwebkit2gtk-4.1-dev
libgtk-3-dev
libgstreamer1.0-dev
libgstreamer-plugins-base1.0-dev
libx11-dev
libxinerama-dev
libxrandr-dev
libxtst-dev
"

DO_CLEAN=0
DO_DEPS=1
DO_STRIP=1
DO_PACKAGE=0
for arg in "$@"; do
    case "$arg" in
        --clean)    DO_CLEAN=1 ;;
        --no-deps)  DO_DEPS=0 ;;
        --no-strip) DO_STRIP=0 ;;
        --package)  DO_PACKAGE=1 ;;
        -h|--help)
            awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"
            exit 0 ;;
        *) echo "Unknown option: $arg (try --help)" >&2; exit 2 ;;
    esac
done

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m!!\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- sanity
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
    x86_64|i?86) ;;
    aarch64|arm64)
        die "This machine is already arm64 - just run ./build_rpi3.sh here instead."
        ;;
    *) say "Untested host architecture '$HOST_ARCH'; trying anyway." ;;
esac

[ -f "$SRC" ] || die "$SRC not found (run this from the project folder)."

for t in curl xz python3 dpkg-deb; do
    command -v "$t" >/dev/null || die "Missing tool '$t'. Install it (apt-get install curl xz-utils python3 dpkg)."
done

if [ "$DO_CLEAN" -eq 1 ]; then
    say "Removing cached sysroot"
    rm -rf "$WORK"
fi

# ------------------------------------------------------------ toolchain
if ! command -v "$CXX" >/dev/null; then
    say "Cross compiler '$CXX' is not installed."
    if [ "$DO_DEPS" -eq 0 ]; then
        die "Install it with: sudo apt-get install -y g++-aarch64-linux-gnu"
    fi
    if [ "$(id -u)" -eq 0 ]; then
        apt-get update && apt-get install -y g++-aarch64-linux-gnu
    elif sudo -n true 2>/dev/null; then
        sudo apt-get update && sudo apt-get install -y g++-aarch64-linux-gnu
    else
        die "Run this first, then re-run the script:
    sudo apt-get install -y g++-aarch64-linux-gnu"
    fi
fi

mkdir -p "$WORK" "$DEBDIR" "$OUT"

# --------------------------------------------------------------- sysroot
if [ ! -f "$STAMP" ]; then
    if [ ! -f "$WORK/Packages" ]; then
        say "Downloading Debian $DIST_NAME arm64 package index"
        curl -fsSL -o "$WORK/Packages.xz" \
            "$MIRROR/dists/$DIST_NAME/main/binary-arm64/Packages.xz"
        xz -dc "$WORK/Packages.xz" > "$WORK/Packages"
        rm -f "$WORK/Packages.xz"
    fi

    say "Resolving build dependencies"
    # shellcheck disable=SC2086
    python3 - "$MIRROR" "$DIST_NAME" "$WORK" $PKGS > "$WORK/plan.txt" <<'PYEOF'
import os, re, sys

mirror, dist, work = sys.argv[1], sys.argv[2], sys.argv[3]
roots = sys.argv[4:]

pkgs = {}
prov = {}


def commit(st):
    name = st.get("Package")
    if not name:
        return
    pkgs[name] = st
    for p in st.get("Provides", "").split(","):
        p = re.sub(r"\(.*?\)", "", p).strip().split(":")[0].strip()
        if p:
            prov.setdefault(p, []).append(name)


st = {}
with open(os.path.join(work, "Packages"), encoding="utf-8", errors="replace") as f:
    for line in f:
        if not line.strip():
            if st:
                commit(st)
                st = {}
            continue
        if line[0] in " \t":
            continue
        key, sep, val = line.partition(":")
        if sep:
            st[key.strip()] = val.strip()
if st:
    commit(st)


def clauses(dep):
    for part in dep.split(","):
        alts = []
        for alt in part.split("|"):
            n = re.sub(r"\(.*?\)", "", alt).strip().split(":")[0].strip()
            if n:
                alts.append(n)
        if alts:
            yield alts


# Compiled out of the link anyway, and tens of MB to fetch.
SKIP = {"libgl1-mesa-dri", "mesa-va-drivers", "mesa-vdpau-drivers"}

done = set()
missing = []
queue = list(roots)
while queue:
    n = queue.pop(0)
    if n in done or n in SKIP:
        continue
    real = n if n in pkgs else (prov.get(n) or [None])[0]
    if real is None or real not in pkgs:
        missing.append(n)
        continue
    if real in done:
        continue
    done.add(real)
    p = pkgs[real]
    for field in ("Pre-Depends", "Depends"):
        for alts in clauses(p.get(field, "")):
            for a in alts:
                if a in pkgs:
                    queue.append(a)
                    break
                if prov.get(a):
                    queue.append(prov[a][0])
                    break

total = 0
for name in sorted(done):
    p = pkgs[name]
    total += int(p.get("Size", "0"))
    print(f"{mirror}/{p['Filename']}")

sys.stderr.write(
    "    %d packages, %.0f MB download" % (len(done), total / 1048576.0)
)
if missing:
    sys.stderr.write(
        "\n    (ignored, not in the index: %s)" % ", ".join(sorted(set(missing)))
    )
sys.stderr.write("\n")
PYEOF

    TOTAL=$(wc -l < "$WORK/plan.txt")
    say "Fetching $TOTAL arm64 packages (~300 MB, cached in $DEBDIR)"
    # curl -O keeps the pool basename under --output-dir.
    xargs -P 8 -n 1 curl -fsSL -O --output-dir "$DEBDIR" < "$WORK/plan.txt"

    say "Unpacking sysroot"
    rm -rf "$SYSROOT"
    mkdir -p "$SYSROOT"
    for d in "$DEBDIR"/*.deb; do
        dpkg-deb -x "$d" "$SYSROOT"
    done
    touch "$STAMP"
else
    say "Reusing cached sysroot ($SYSROOT)"
fi

SYSROOT_ABS="$(cd "$SYSROOT" && pwd)"
[ -d "$SYSROOT_ABS/usr/lib/aarch64-linux-gnu/pkgconfig" ] || \
    die "Sysroot looks incomplete (no pkg-config dir). Re-run with --clean."

# -------------------------------------------------------------- pkg-config
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT_ABS"
export PKG_CONFIG_LIBDIR="$SYSROOT_ABS/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT_ABS/usr/lib/pkgconfig:$SYSROOT_ABS/usr/share/pkgconfig"
export PKG_CONFIG_PATH=""

PC_MODULES="webkit2gtk-4.1 gtk+-3.0 gdk-x11-3.0 gstreamer-1.0 gstreamer-video-1.0 x11 xinerama xrandr xtst"
say "Checking pkg-config modules"
if ! pkg-config --exists $PC_MODULES; then
    die "pkg-config could not resolve: $PC_MODULES"
fi

# The host cross compiler's own builtin include path points at *its* arm64
# glibc headers (newer than bookworm's), so put the sysroot's copies ahead
# of them, and link libstdc++ from the sysroot rather than the host's.
SYSINC="-isystem $SYSROOT_ABS/usr/include/aarch64-linux-gnu -isystem $SYSROOT_ABS/usr/include"
GCCLIB="$(ls -d "$SYSROOT_ABS"/usr/lib/gcc/aarch64-linux-gnu/*/ 2>/dev/null | head -1)"
SYSLIB="-L$SYSROOT_ABS/usr/lib/aarch64-linux-gnu"
[ -n "$GCCLIB" ] && SYSLIB="-L${GCCLIB%/} $SYSLIB"
SYSLIB="$SYSLIB -Wl,-rpath-link,$SYSROOT_ABS/usr/lib/aarch64-linux-gnu"

# ------------------------------------------------------------------ build
say "Compiling $SRC for Cortex-A53 (arm64, Debian $DIST_NAME ABI)"
# -mcpu=cortex-a53      : Raspberry Pi 3B / BCM2837
# -mno-outline-atomics  : keep the binary off libgcc's atomic helpers
# shellcheck disable=SC2086
"$CXX" \
    --sysroot="$SYSROOT_ABS" \
    $SYSINC \
    -O2 -pipe -mcpu=cortex-a53 -mno-outline-atomics \
    -fpermissive -std=gnu++17 \
    -o "$OUT/yt_tv" "$SRC" \
    $(pkg-config --cflags $PC_MODULES) \
    $SYSLIB \
    $(pkg-config --libs $PC_MODULES)

chmod 755 "$OUT/yt_tv"

# ------------------------------------------------------------- verify
say "Verifying the binary"
file "$OUT/yt_tv" | sed 's/^/    /'
aarch64-linux-gnu-readelf -l "$OUT/yt_tv" | awk '/interpreter/ {gsub(/\]/,"",$NF); print "    loader: " $NF}'

MAXG=$(aarch64-linux-gnu-readelf -sW "$OUT/yt_tv" 2>/dev/null \
        | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 || true)
MAXC=$(aarch64-linux-gnu-readelf -sW "$OUT/yt_tv" 2>/dev/null \
        | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1 || true)
echo "    needs ${MAXG:-none}, ${MAXC:-none}"

if [ -n "${MAXG:-}" ] && [ "$(printf '%s\nGLIBC_2.36\n' "$MAXG" | sort -V | tail -1)" != "GLIBC_2.36" ]; then
    printf '\033[1;33m!!\033[0m %s is newer than RPi OS bookworm (GLIBC_2.36) - re-run with --clean\n' "$MAXG" >&2
fi

if [ "$DO_STRIP" -eq 1 ] && command -v aarch64-linux-gnu-strip >/dev/null 2>&1; then
    cp -f "$OUT/yt_tv" "$OUT/yt_tv.debug"
    aarch64-linux-gnu-strip --strip-unneeded "$OUT/yt_tv"
fi

SIZE=$(du -h "$OUT/yt_tv" | cut -f1)
echo
say "Built $OUT/yt_tv ($SIZE) for Raspberry Pi 3B / RPi OS Lite 64-bit"

PKG=""
if [ "$DO_PACKAGE" -eq 1 ]; then
    PKG="$OUT/yt-tv-rpi3-$(date +%Y%m%d).tar.gz"
    STAGE="$WORK/pkg/yt-tv"
    rm -rf "$WORK/pkg"
    mkdir -p "$STAGE"
    cp "$OUT/yt_tv" "$STAGE/"
    cp install_rpi3.sh "$STAGE/"
    [ -f README.md ] && cp README.md "$STAGE/"
    tar -czf "$PKG" -C "$WORK/pkg" yt-tv
    say "Packaged $PKG ($(du -h "$PKG" | cut -f1))"
fi

if [ -n "$PKG" ]; then
    B="$(basename "$PKG")"
    cat <<EOF

Copy the tarball to the Pi and run the installer there:

    scp $PKG pi@raspberrypi.local:~/
    ssh pi@raspberrypi.local 'tar xzf $B && cd yt-tv && sudo ./install_rpi3.sh && sudo reboot'

EOF
else
    cat <<EOF

Copy the binary + installer to the Pi:

    scp $OUT/yt_tv install_rpi3.sh pi@raspberrypi.local:~/

Then, on the Pi:

    sudo ./install_rpi3.sh --bin ./yt_tv && sudo reboot

EOF
fi
