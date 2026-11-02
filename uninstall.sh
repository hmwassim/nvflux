#!/bin/sh
# Unlock the clocks and remove everything nvflux installed. Run as root:
#   sudo ./uninstall.sh
#
# install.sh also runs this with --for-upgrade, which keeps the nvflux group
# and the saved mode.
set -eu

ROOT=${NVFLUX_ROOT:-}
PREFIX=${PREFIX:-/usr/local}
BINDIR=$PREFIX/bin
MANIFEST=$ROOT/var/lib/nvflux/installed

say() { printf 'nvflux: %s\n' "$*"; }
die() { printf 'nvflux: error: %s\n' "$*" >&2; exit 1; }

upgrade=0
case ${1:-} in
--for-upgrade) upgrade=1 ;;
"") ;;
*) die "unknown option '$1'" ;;
esac

[ "$(id -u)" -eq 0 ] || die "run as root: sudo $0"

if [ "$upgrade" -eq 0 ] && [ -z "$ROOT" ] && [ -x "$BINDIR/nvflux" ] && [ -e /proc/driver/nvidia/version ]; then
    "$BINDIR/nvflux" off >/dev/null 2>&1 || say "warning: couldn't unlock the clocks; a reboot resets them"
fi

systemctl_() { if [ -n "$ROOT" ]; then systemctl --root="$ROOT" "$@"; else systemctl "$@"; fi; }

if [ -f "$MANIFEST" ]; then
    # Disable services first, then remove files.
    while read -r kind a rest; do
        case $kind in
        enable)
            case $a in
            systemd) systemctl_ disable --quiet $rest 2>/dev/null || true ;;
            openrc) rc-update del nvflux default >/dev/null 2>&1 || true ;;
            sysv)
                if command -v update-rc.d >/dev/null 2>&1; then update-rc.d -f nvflux remove >/dev/null 2>&1 || true
                elif command -v chkconfig >/dev/null 2>&1; then chkconfig --del nvflux 2>/dev/null || true
                fi ;;
            esac ;;
        rclocal)
            [ -f "$ROOT$a" ] && sed -i '/^# nvflux begin$/,/^# nvflux end$/d' "$ROOT$a" ;;
        esac
    done < "$MANIFEST"
    while read -r kind a rest; do
        case $kind in
        file) rm -f "$ROOT$a" ;;
        esac
    done < "$MANIFEST"
    created=$(grep -c '^group created$' "$MANIFEST" || true)
    # Folders nvflux created, deepest first; only empty ones go.
    dirs=$(sed -n 's/^dir //p' "$MANIFEST" | sort -r)
    rm -f "$MANIFEST"
else
    # No manifest: an install from before 1.0, or one that didn't finish.
    created=0
    dirs=
    [ -d "$ROOT/run/systemd/system" ] &&
        systemctl_ disable --quiet nvflux-resume.service nvflux-reload.service nvflux-suspend.service 2>/dev/null || true
    rm -f "$ROOT$BINDIR/nvflux" "$ROOT$BINDIR/nvflux.bak" \
          "$ROOT$PREFIX/lib/systemd/system/nvflux-reload.service" \
          "$ROOT$PREFIX/lib/systemd/system/nvflux-suspend.service" \
          "$ROOT$PREFIX/lib/systemd/system/nvflux-resume.service" \
          "$ROOT/etc/xdg/autostart/nvflux.desktop" "$ROOT/etc/xdg/autostart/nvflux-restore.desktop" \
          "$ROOT/etc/elogind/system-sleep/nvflux" "$ROOT/etc/zzz.d/resume/nvflux" \
          "$ROOT/etc/zzz.d/suspend/nvflux" "$ROOT/etc/pm/sleep.d/50nvflux"
fi

if [ -z "$ROOT" ] && [ -d /run/systemd/system ]; then
    systemctl daemon-reload || true
fi

[ "$upgrade" -eq 1 ] && exit 0

rm -rf "$ROOT/var/lib/nvflux" "$ROOT/run/nvflux"
for d in $dirs; do
    rmdir "$ROOT$d" 2>/dev/null || true
done

if [ "$created" -gt 0 ] && grep -q '^nvflux:' "$ROOT/etc/group" 2>/dev/null; then
    if command -v groupdel >/dev/null 2>&1; then groupdel nvflux
    elif command -v delgroup >/dev/null 2>&1; then delgroup nvflux
    fi
fi
say "removed"
