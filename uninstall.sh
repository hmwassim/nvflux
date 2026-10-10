#!/bin/sh
# Unlock clocks and remove nvflux. Run as root.
set -eu

say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

bin=/usr/local/bin/nvflux

[ "$(id -u)" -eq 0 ] || die "run as root: sudo $0"

if [ -x "$bin" ]; then
    say "Unlocking memory clocks"
    "$bin" auto || say "Could not unlock; a reboot will reset the clocks"
fi

say "Removing nvflux"
unit=/usr/local/lib/systemd/system/nvflux-resume.service
if [ -f "$unit" ] && [ -d /run/systemd/system ]; then
    systemctl disable --quiet nvflux-resume.service || true
fi
rm -f "$unit" /etc/elogind/system-sleep/nvflux /etc/zzz.d/resume/nvflux
[ -d /run/systemd/system ] && systemctl daemon-reload
rm -f "$bin" /etc/xdg/autostart/nvflux.desktop
rm -rf /var/lib/nvflux
# Leftovers from nvflux 1.x.
rm -f /usr/local/bin/nvflux.bak /etc/xdg/autostart/nvflux-restore.desktop

say "Done"
