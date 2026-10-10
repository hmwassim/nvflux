#!/bin/sh
# Build nvflux and install it system-wide. Run as root.
set -eu
cd "$(dirname "$0")"

say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root: sudo $0"
[ -e /proc/driver/nvidia/version ] ||
    die "the NVIDIA driver is not loaded; install it (and reboot) first"

if ! command -v cc >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
    say "Installing a C compiler and make"
    if   command -v apt-get      >/dev/null 2>&1; then apt-get update -qq && apt-get install -y -qq gcc make libc6-dev
    elif command -v dnf          >/dev/null 2>&1; then dnf install -y gcc make
    elif command -v yum          >/dev/null 2>&1; then yum install -y gcc make
    elif command -v pacman       >/dev/null 2>&1; then pacman -S --needed --noconfirm gcc make
    elif command -v zypper       >/dev/null 2>&1; then zypper --non-interactive install gcc make
    elif command -v xbps-install >/dev/null 2>&1; then xbps-install -y gcc make
    elif command -v apk          >/dev/null 2>&1; then apk add gcc make musl-dev
    elif command -v eopkg        >/dev/null 2>&1; then eopkg install -y gcc make
    else die "install a C compiler and make, then run this again"
    fi
fi

# Build outside the source tree so it isn't left full of root-owned files.
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT

say "Building"
make -s BUILDDIR="$build"

say "Installing"
make -s BUILDDIR="$build" install

# The driver can drop the lock across suspend, so also re-apply on resume.
resume=
if [ -d /run/systemd/system ]; then
    make -s BUILDDIR="$build" install-systemd
    systemctl daemon-reload
    systemctl enable --quiet nvflux-resume.service
    resume=systemd
fi
if [ -z "$resume" ] && [ -d /etc/elogind ]; then
    make -s BUILDDIR="$build" install-elogind
    resume=elogind
fi
if [ -d /etc/zzz.d ]; then
    make -s BUILDDIR="$build" install-zzz
    resume="${resume:+$resume and }zzz"
fi
if [ -n "$resume" ]; then
    say "Profile will also be re-applied after resume ($resume)"
else
    say "No supported suspend system found; run 'nvflux restore' after resuming"
fi

# Leftovers from nvflux 1.x: its backup binary, its autostart entry, and
# the "ultra" profile, which no longer exists.
rm -f /usr/local/bin/nvflux.bak /etc/xdg/autostart/nvflux-restore.desktop
case $(cat /var/lib/nvflux/state 2>/dev/null || true) in
ultra)
    rm -f /var/lib/nvflux/state
    say "Your saved nvflux 1.x profile no longer exists; pick a new one" ;;
esac

say "Installed $(/usr/local/bin/nvflux version)"
echo
/usr/local/bin/nvflux status || true
cat <<'MSG'

Next:
  nvflux powersave     lock memory to its lowest clock (fixes audio dropouts)
  nvflux help          all commands

The profile you choose is re-applied at every login and after resume.
MSG
