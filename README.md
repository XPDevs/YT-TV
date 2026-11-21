# yt_tv — YouTube TV for the Raspberry Pi

A small fullscreen kiosk that boots straight into the [YouTube TV](https://www.youtube.com/tv)
web interface on a Raspberry Pi. One C++ binary, no browser, no desktop
environment, no account sharing — it drives the TV UI with a keyboard, a USB
gamepad, or the TV's own remote over HDMI-CEC.

Written for a **Raspberry Pi 3B with 1 GB of RAM running Raspberry Pi OS Lite
64-bit (bookworm)**, and it also runs on a Pi 4/5 or on any x86-64 Linux with
WebKitGTK 4.1.

---

## Disclaimer — not affiliated with Google or YouTube

This project is **not affiliated with, endorsed by, sponsored by, or in any way
officially connected to Google LLC, YouTube, or any of their subsidiaries or
affiliates.** "YouTube", "YouTube TV" and the YouTube logo are trademarks or
registered trademarks of Google LLC. All other trademarks are the property of
their respective owners.

`yt_tv` is an unofficial browser window pointed at YouTube's public TV web
interface. It:

- does not download, record, re-encode, cache or redistribute any content;
- does not bypass DRM, ads, regional restrictions, or any access control;
- is not a replacement for a YouTube subscription or a YouTube TV subscription;
- requires its own network connection and your own ability to sign in to
  whatever Google account you already use.

You are responsible for using it in line with the [YouTube Terms of
Service](https://www.youtube.com/t/terms) and any local law. Use at your own
risk; no warranty of any kind is provided. If Google or a rights holder objects
to this project, it should be taken down.

This project contains no Google code or assets. It links only against
LGPL/BSD-style system libraries (WebKitGTK, GTK3, GStreamer, Xlib).

---

## Features

- **Straight into the app.** No splash, no boot animation, no desktop: the app
  starts on tty1 and loads the TV UI immediately.
- **Built-in HDMI-CEC.** Talks to the kernel CEC adapter (`/dev/cec0`) that the
  VC4/KMS driver exposes, registers itself as a *Playback Device*, and maps the
  TV remote's keys onto the UI. No `libcec`, no `cec-client`, no daemon. Set
  `YT_TV_NO_CEC=1` to disable.
- **Keyboard, gamepad and remote.** USB keyboards, Xbox/PlayStation-style
  gamepads (hotplug via inotify) and CEC remotes all work at the same time.
- **Low-memory mode.** On boards with less than 1.5 GiB of RAM the WebKit
  compositor, WebGL, accelerated canvas and the disk page cache are switched
  off — WebKit on a Pi 3B has no usable GL path anyway.
- **H.264 only on ARM.** VP9/AV1 are hidden from `MediaSource.isTypeSupported`
  so YouTube serves H.264, which the Pi's V4L2 hardware decoder can play. This
  is the difference between smooth 720p and a slideshow.
- **Multi-display aware.** Picks a screen at startup (or remembers the last
  one) and blacks out the others; `Ctrl+Shift+D` opens the picker again.

## Requirements

| | |
|---|---|
| Board | Raspberry Pi 3B / 3B+ (also 4, 5, Zero 2 W), or any x86-64 Linux |
| OS | Raspberry Pi OS **Lite 64-bit, bookworm** (aarch64) |
| Display | HDMI TV or monitor; CEC needs a CEC-capable TV |
| Libraries | WebKitGTK **4.1**, GTK **3**, GStreamer **1.20+**, Xlib, XTest |
| RAM | 1 GB works (see [performance](#performance-on-1-gib)); 2 GB+ is comfortable |

Bullseye and older do not ship WebKitGTK 4.1 and will not work.

## Quick start (recommended: cross-compile, then install on the Pi)

**1. On your Linux PC** (any x86-64 Debian/Ubuntu/Mint; needs
`g++-aarch64-linux-gnu`):

```bash
git clone <your-repo-url> yt_tv
cd yt_tv
./build_rpi3_cross.sh --package
```

The first run downloads the arm64 build dependencies into a private sysroot in
`~/.cache/yt-tv-rpi3-cross` (~300 MB, reused afterwards), then produces:

```
dist/yt_tv                    # aarch64 binary, dynamically linked to bookworm libs
dist/yt-tv-rpi3-YYYYMMDD.tar.gz   # binary + installer + this README
```

**2. Copy it to the Pi and install:**

```bash
scp dist/yt-tv-rpi3-*.tar.gz pi@raspberrypi.local:~/
ssh pi@raspberrypi.local 'tar xzf yt-tv-rpi3-*.tar.gz && cd yt-tv && sudo ./install_rpi3.sh --reboot'
```

That is the whole setup. After the reboot the Pi comes up on the TV, in
YouTube TV.

## Build on the Pi itself (alternative)

Slower (WebKit headers on a Pi 3B take a while) but needs no cross toolchain:

```bash
cd yt_tv
./build_rpi3.sh              # installs build deps, compiles ./yt_tv natively
sudo ./install_rpi3.sh --bin ./yt_tv --reboot
```

## Desktop build

```bash
./build_linux.sh             # x86-64 Debian/Ubuntu/Mint, same libraries
./yt_tv                      # or: xinit ./yt_tv -- :0
```

## Installer

`install_rpi3.sh` is idempotent — re-running it is safe.

```
sudo ./install_rpi3.sh [options]

  --bin PATH     yt_tv binary to install (default: ./yt_tv, ./dist/yt_tv)
  --user NAME    user that owns the X session (default: the sudo user)
  --no-config    do not touch /boot/firmware/config.txt or cmdline.txt
  --no-memory    do not touch zram / swap / sysctl settings
  --dry-run      print what would change, change nothing
  --uninstall    stop, disable and remove the kiosk
  --status       show installed state + hardware diagnostics
  --reboot       reboot when finished
  -v, --verbose  echo every command that runs
  -h, --help     usage
```

What it does:

1. **Preflight** — checks architecture, OS release, RAM, free disk, HDMI state
   and whether `/dev/cec*` exists.
2. **Packages** — X server (with `xserver-xorg-legacy` so a non-root user can
   start X on tty1), `libwebkit2gtk-4.1-0`, GTK3, GStreamer (base/good/bad/lav/x
   **/alsa**), `alsa-utils`, `zram-tools`, `v4l-utils`, fonts, CA certs.
3. **Boot config** — enables `dtoverlay=vc4-kms-v3d` (needed for X, `/dev/dri`
   *and* `/dev/cec0`), `dtparam=audio=on`, `hdmi_force_hotplug=1`,
   `gpu_mem=64`, `disable_overscan`, `disable_splash`, and `consoleblank=0`.
   The original `config.txt` is backed up to `config.txt.yt-tv.bak`.
4. **Memory** — zram swap (LZ4, 100 % of RAM), `vm.swappiness=100`,
   `vm.page-cluster=0`, and a 512 MB swapfile.
5. **X** — `/etc/X11/Xwrapper.config`, and adds the user to
   `input,video,audio,render,tty` (input = gamepads and remote keys, video = `/dev/cec0`).
6. **App** — `/opt/yt-tv/yt_tv`, `/opt/yt-tv/start.sh`, `/etc/default/yt-tv`.
7. **Service** — `yt-tv.service` runs
   `xinit /opt/yt-tv/start.sh -- :0 vt1 -nocursor` as that user on tty1,
   restarts on failure, and disables `getty@tty1`.

Everything is logged to `/var/log/yt-tv-install.log`.

## Controls

| Action | Keyboard | Gamepad | CEC remote |
|---|---|---|---|
| Navigate | Arrows | D-pad / left stick | Arrows |
| Select | Enter | A | OK / Select |
| Back | Esc | B / Select | Back |
| Play / pause | Space, K | Start | Play / Pause |
| Seek ±10 s | J / L | X / Y | Rewind / Fast-forward |
| Volume | F9 / F10, + / - | LB / RB | Vol + / - |
| Mute | M | LT | Mute |
| Like | R | L3 | — |
| Guide | G | — | Channel up |
| YouTube home | Y | Guide | Channel down |
| Numbers | 0-9 | — | 0-9 |
| Reload | F5 | — | — |
| Machine picker | Ctrl+Shift+D | — | — |
| Quit | hold Esc 1 s, or Q | — | — |

## Configuration

Runtime settings live in `/etc/default/yt-tv` (plain `KEY=value`, applied by
`systemctl restart yt-tv`):

| Variable | Default | Meaning |
|---|---|---|
| `WEBKIT_DISABLE_COMPOSITING_MODE` | `1` | skip WebKit's accelerated compositor |
| `WEBKIT_DISABLE_DMABUF_RENDERER` | `1` | avoid the DMABUF renderer (broken on VC4) |
| `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS` | `1` | no bubblewrap helper processes (saves RAM) |
| `MALLOC_ARENA_MAX` | `2` | fewer glibc arenas → lower RSS |
| `GST_PLUGIN_FEATURE_RANK` | `v4l2h264dec:MAX,…` | prefer the hardware decoder |
| `YT_TV_ALLOW_VP9` | unset | let YouTube serve VP9/AV1 (software; very slow) |
| `YT_TV_NO_CEC` | unset | disable the built-in CEC handling |
| `JSC_useJIT` | unset | `0` trades CPU for a lot less JS memory |

Command line: `./yt_tv --debug` (`-d`) dumps every raw input event from
gamepads/remotes, which is the fastest way to find out what a remote sends.

## How it works

```
systemd (yt-tv.service) → xinit → Xorg on tty1 (no WM)
                                  └─ start.sh → yt_tv
                                                 ├─ GTK3 window, fullscreen, overlay stack
                                                 ├─ WebKitGTK webview (TV user-agent) → youtube.com/tv
                                                 ├─ XTest: synthesises the key events the TV UI expects
                                                 ├─ evdev + inotify: gamepads, hotplug
                                                 └─ /dev/cec0: kernel CEC API, remote → UI keys
```

YouTube's TV interface only responds to keyboard/remote input, so the app turns
gamepad buttons and CEC messages into X key events with `XTestFakeKeyEvent`,
and layers a few direct JS calls on top (`video.play()`, volume) for actions the
page does not expose as keys.

## Performance on 1 GiB

A Pi 3B has ~800 MB free after boot and WebKit wants a lot of it. What the
installer and the code do about it:

- zram (compressed swap) + early swapping;
- no WebKit compositor, no WebGL, no accelerated canvas, no page cache;
- no bubblewrap sandbox processes;
- H.264 instead of VP9 so the hardware decoder does the work.

Realistic expectations: the UI is usable but not snappy (~3–6 s to load the
guide), and **playback should stay at 480p or 720p**. 1080p H.264 decodes in
hardware but leaves little headroom for the UI. If it still gets tight:

```bash
# more compressed swap
sudo nano /etc/default/zramswap     # PERCENT=200, then: sudo systemctl restart zramswap
# or trade JavaScript speed for memory
echo 'JSC_useJIT=0' | sudo tee -a /etc/default/yt-tv
sudo systemctl restart yt-tv
```

A Pi 4 (2 GB+) removes all of these trade-offs; the binary is built for
Cortex-A53 but runs unchanged on A72/A76.

## Troubleshooting

**Black screen / back to a console**

```bash
journalctl -u yt-tv -b --no-pager | tail -50
```

- `no screens detected` → X started without a display: check that
  `dtoverlay=vc4-kms-v3d` is active (`vcgencmd get_config dtoverlay`) and reboot.
- X failing to start at all → `cat /var/log/Xorg.0.log`, and make sure
  `xserver-xorg-legacy` is installed with `needs_root_rights=yes`.

**No audio.** `alsamixer` → unmute `PCM` and raise it; `speaker-test -c2 -twav`.
HDMI audio needs `dtparam=audio=on` and `hdmi_force_hotplug=1` (both set by the
installer) plus a reboot.

**No CEC.** `/dev/cec0` must exist:

```bash
ls -l /dev/cec*
cec-ctl -d /dev/cec0 --playback --osd-name yt-tv   # from v4l-utils
```

If the device is missing, you are not on `vc4-kms-v3d` (FKMS and the legacy
firmware CEC path do not expose it), or the cable/TV does not carry CEC. The
app logs `[cec] ... playback device, logical address 0x..` on success and
`[cec] no adapter ...` otherwise.

**Out of memory** (`dmesg | grep -i oom`). Lower the playback quality, raise
zram, or try `JSC_useJIT=0`.

**Sign-in prompts.** YouTube TV asks for a Google account sign-in; that is
Google's own flow, done inside the webview. `yt_tv` never sees your password.

## Uninstall

```bash
sudo ./install_rpi3.sh --uninstall
sudo reboot
```

Removes the service, `/opt/yt-tv`, the defaults file, the sysctl and Xwrapper
files, and restores `config.txt` from its backup. The apt packages and the
swapfile size are intentionally left alone; `sudo apt-get autoremove --purge`
if you want them gone.

## Repository layout

```
yt_tv.c                  the whole application
build_rpi3_cross.sh      cross-compile for aarch64 (bookworm) from x86-64
build_rpi3.sh            native build on the Pi
build_linux.sh           native build on x86-64 Linux
install_rpi3.sh          installer / status / uninstall for the Pi
.github/workflows/       CI: cross-build + artefacts on every push
```

## Third-party libraries

WebKitGTK (LGPL-2.1+ / BSD), GTK3 (LGPL-2.1+), GLib (LGPL-2.1+), GStreamer
(LGPL-2.0+), libX11/libXinerama/libXtst (MIT/X11), and the Linux kernel CEC
UAPI headers (GPL-2.0 WITH Linux-syscall-note — used only as a userspace ABI
definition).

## License

No license has been chosen for this repository yet. Add a `LICENSE` file (MIT
and GPL-3.0 are the usual choices) before publishing if you want other people
to be able to reuse it.
