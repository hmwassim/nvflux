#!/bin/sh
# Install nvflux. Run as root: sudo ./install.sh [--everyone]
#
#   --everyone   let every user change clocks (mode 4755) instead of only
#                members of the nvflux group (mode 4750)
#
# For tests and packagers: NVFLUX_ROOT installs into another root directory,
# NVFLUX_BINARY uses an already built binary, PREFIX defaults to /usr/local.
set -eu
cd "$(dirname "$0")"

ROOT=${NVFLUX_ROOT:-}
PREFIX=${PREFIX:-/usr/local}
BINDIR=$PREFIX/bin
UNITDIR=$PREFIX/lib/systemd/system
STATEDIR=/var/lib/nvflux
MANIFEST=$STATEDIR/installed

say() { printf 'nvflux: %s\n' "$*"; }
die() { printf 'nvflux: error: %s\n' "$*" >&2; exit 1; }

mode=4750
for arg in "$@"; do
    case $arg in
    --everyone) mode=4755 ;;
    -h|--help) sed -n '2,9s/^# \{0,1\}//p' "$0"; exit 0 ;;
    *) die "unknown option '$arg'" ;;
    esac
done

[ "$(id -u)" -eq 0 ] || die "run as root: sudo $0"
[ -e "$ROOT/proc/driver/nvidia/version" ] ||
    say "warning: the NVIDIA driver isn't loaded; nvflux works once it is"

# --- build -------------------------------------------------------------------

if [ -n "${NVFLUX_BINARY:-}" ]; then
    binary=$NVFLUX_BINARY
else
    if ! command -v cc >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
        say "installing a C compiler and make"
        if   command -v apt-get      >/dev/null 2>&1; then apt-get update -qq && apt-get install -y -qq gcc make libc6-dev
        elif command -v dnf          >/dev/null 2>&1; then dnf install -y gcc make
        elif command -v yum          >/dev/null 2>&1; then yum install -y gcc make
        elif command -v pacman       >/dev/null 2>&1; then pacman -S --needed --noconfirm gcc make
        elif command -v zypper       >/dev/null 2>&1; then zypper --non-interactive install gcc make
        elif command -v xbps-install >/dev/null 2>&1; then xbps-install -Sy gcc make
        elif command -v apk          >/dev/null 2>&1; then apk add --no-cache gcc make musl-dev
        elif command -v eopkg        >/dev/null 2>&1; then eopkg install -y gcc make
        elif command -v emerge       >/dev/null 2>&1; then emerge --noreplace sys-devel/gcc dev-build/make
        else die "install a C compiler and make, then run this again"
        fi
    fi
    # Build outside the source tree so it isn't left full of root-owned files.
    build=$(mktemp -d)
    say "building"
    make -s BUILDDIR="$build" PREFIX="$PREFIX" STATEDIR="$STATEDIR" "$build/nvflux"
    binary=$build/nvflux
fi

# --- undo an earlier install (upgrade) ------------------------------------------

record=$(mktemp)
trap 'rm -rf "${build:-}" "$record" "$record.hook"' EXIT
rec() { printf '%s\n' "$*" >> "$record"; }

if [ -f "$ROOT$MANIFEST" ]; then
    # Keep what the earlier install created (folders, the group), so a later
    # uninstall still removes them.
    grep -E '^(dir|group) ' "$ROOT$MANIFEST" > "$record" || true
    NVFLUX_ROOT=$ROOT PREFIX=$PREFIX sh ./uninstall.sh --for-upgrade
fi

# Leftovers from the old nvflux (before 1.0). Its state file is kept: nvflux
# reads and converts it.
rm -f "$ROOT/etc/xdg/autostart/nvflux.desktop" "$ROOT/etc/xdg/autostart/nvflux-restore.desktop" \
      "$ROOT$BINDIR/nvflux.bak"

# --- helpers -------------------------------------------------------------------

# mkdirs <dir>: create a folder and any missing parents, recording each one
# so uninstall removes exactly the folders nvflux created
mkdirs() {
    missing=
    d=$1
    while [ ! -d "$ROOT$d" ]; do
        missing="$d $missing"
        d=$(dirname "$d")
    done
    for d in $missing; do
        mkdir "$ROOT$d"
        rec "dir $d"
    done
}

# put <mode> <source> <destination>: install a file, atomically, and record it
put() {
    mkdirs "$(dirname "$3")"
    cp "$2" "$ROOT$3.new"
    chmod "$1" "$ROOT$3.new"
    mv -f "$ROOT$3.new" "$ROOT$3"
    rec "file $3"
}

# hook <template> <mode> <destination>
hook() {
    sed "s|@BINDIR@|$BINDIR|g" "hooks/$1.in" > "$record.hook"
    put "$2" "$record.hook" "$3"
}

# rclocal <file>: add the reload line to an rc.local-style script
rclocal() {
    f=$ROOT$1
    [ -f "$f" ] || { printf '#!/bin/sh\n' > "$f"; chmod 755 "$f"; }
    sed -i '/^# nvflux begin$/,/^# nvflux end$/d' "$f"
    sed "s|@BINDIR@|$BINDIR|g" hooks/rc.local.in >> "$f"
    rec "rclocal $1"
}

in_group() {
    sed -n 's/^nvflux:[^:]*:[^:]*://p' "$ROOT/etc/group" 2>/dev/null | tr ',' '\n' | grep -qx "$1"
}

# --- group and program -------------------------------------------------------------

if ! grep -q '^nvflux:' "$ROOT/etc/group" 2>/dev/null; then
    if command -v groupadd >/dev/null 2>&1; then groupadd -r nvflux
    elif command -v addgroup >/dev/null 2>&1; then addgroup -S nvflux
    else die "can't create the nvflux group (no groupadd or addgroup)"
    fi
    rec "group created"
fi

put 755 "$binary" "$BINDIR/nvflux"
chown root:nvflux "$ROOT$BINDIR/nvflux"
chmod "$mode" "$ROOT$BINDIR/nvflux" # after chown, which clears the setuid bit

put 644 man/nvflux.1 "$PREFIX/share/man/man1/nvflux.1"
put 644 completions/nvflux.bash "$PREFIX/share/bash-completion/completions/nvflux"
put 644 completions/_nvflux "$PREFIX/share/zsh/site-functions/_nvflux"
put 644 completions/nvflux.fish "$PREFIX/share/fish/vendor_completions.d/nvflux.fish"

user=${SUDO_USER:-${DOAS_USER:-}}
added=
if [ -n "$user" ] && [ "$user" != root ] && ! in_group "$user"; then
    if command -v usermod >/dev/null 2>&1; then usermod -aG nvflux "$user"
    else adduser "$user" nvflux
    fi
    added=$user
fi

# --- boot hook: re-apply the saved mode at boot ---------------------------------------

init=$(cat "$ROOT/proc/1/comm" 2>/dev/null || echo unknown)
case $init in
systemd) boot=systemd ;;
runit|runit-init) boot=runit ;;
dinit) boot=dinit ;;
s6-svscan) boot=s6 ;;
openrc-init) boot=openrc ;;
init)
    if [ -d "$ROOT/run/openrc" ]; then boot=openrc
    elif [ -f "$ROOT/etc/slackware-version" ]; then boot=slackware
    else boot=sysv
    fi ;;
*) boot=none ;;
esac

systemctl_() { if [ -n "$ROOT" ]; then systemctl --root="$ROOT" "$@"; else systemctl "$@"; fi; }

case $boot in
systemd)
    hook nvflux-reload.service 644 "$UNITDIR/nvflux-reload.service"
    hook nvflux-suspend.service 644 "$UNITDIR/nvflux-suspend.service"
    hook nvflux-resume.service 644 "$UNITDIR/nvflux-resume.service"
    [ -n "$ROOT" ] || systemctl daemon-reload
    systemctl_ enable --quiet nvflux-reload.service nvflux-suspend.service nvflux-resume.service
    rec "enable systemd nvflux-reload.service nvflux-suspend.service nvflux-resume.service"
    rec "sleep systemd" ;;
openrc)
    hook openrc 755 /etc/init.d/nvflux
    rc-update add nvflux default >/dev/null
    rec "enable openrc nvflux" ;;
dinit)
    hook dinit 644 /etc/dinit.d/nvflux
    mkdirs /etc/dinit.d/boot.d
    ln -sf ../nvflux "$ROOT/etc/dinit.d/boot.d/nvflux"
    rec "file /etc/dinit.d/boot.d/nvflux" ;;
runit)
    # runit has no service for one-shot jobs; use whatever its stage 2 runs:
    # /etc/rc.local on Void, /etc/local.d/*.start on Artix.
    if grep -q '/etc/rc.local' "$ROOT/etc/runit/2" 2>/dev/null; then
        rclocal /etc/rc.local
    elif grep -q '/etc/local.d' "$ROOT/etc/runit/2" 2>/dev/null; then
        hook local.d 755 /etc/local.d/nvflux.start
    else
        boot=none
    fi ;;
slackware)
    rclocal /etc/rc.d/rc.local ;;
sysv)
    hook sysv 755 /etc/init.d/nvflux
    if command -v update-rc.d >/dev/null 2>&1; then update-rc.d nvflux defaults >/dev/null
    elif command -v chkconfig >/dev/null 2>&1; then chkconfig --add nvflux
    fi
    rec "enable sysv nvflux" ;;
esac
rec "boot $boot"

# --- sleep hooks: unlock before sleep, re-apply after ---------------------------------

if [ "$boot" != systemd ]; then
    if [ -d "$ROOT/etc/elogind" ] || [ -d "$ROOT/usr/lib/elogind" ]; then
        hook elogind 755 /etc/elogind/system-sleep/nvflux
        rec "sleep elogind"
    fi
    if [ -d "$ROOT/etc/zzz.d" ]; then
        hook zzz-suspend 755 /etc/zzz.d/suspend/nvflux
        hook zzz-resume 755 /etc/zzz.d/resume/nvflux
        rec "sleep zzz"
    fi
    if [ -d "$ROOT/etc/pm/sleep.d" ]; then
        hook pm-utils 755 /etc/pm/sleep.d/50nvflux
        rec "sleep pm-utils"
    fi
fi

mkdirs "$STATEDIR"
cp "$record" "$ROOT$MANIFEST"
chmod 644 "$ROOT$MANIFEST"

# --- done -----------------------------------------------------------------------------

say "installed $BINDIR/nvflux"
case $boot in
none|s6) say "no boot hook for this init system; run '$BINDIR/nvflux reload --wait 15' at startup" ;;
*) say "boot hook: $boot" ;;
esac
grep -q '^sleep ' "$ROOT$MANIFEST" ||
    say "no sleep hook found; run 'nvflux reload' after resuming"
if [ -n "$added" ]; then
    say "added $added to the nvflux group; log out and back in to use nvflux without sudo"
elif [ "$mode" = 4750 ]; then
    say "to use nvflux without sudo: usermod -aG nvflux <user>, then log out and back in"
fi

if [ -z "$ROOT" ]; then
    "$BINDIR/nvflux" reload --wait 3 >/dev/null 2>&1 || true # apply a saved mode (e.g. from the old nvflux)
    echo
    "$BINDIR/nvflux" doctor || true
fi
