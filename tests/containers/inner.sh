#!/bin/sh
# Runs inside a fresh distro container (as root), from tests/containers/run.sh.
#   1. A real install with install.sh: installs a compiler if needed, builds,
#      creates the group, installs the setuid binary. Then a real uninstall.
#   2. The full test suite as an unprivileged user.
# There's no GPU or NVIDIA driver in the container; nvflux must say so cleanly.
set -eu

step() { printf '\n=== %s\n' "$*"; }
fail() { printf 'CONTAINER FAIL: %s\n' "$*"; exit 1; }

. /etc/os-release
step "distro: $PRETTY_NAME"

# A real system has a synced package database; fresh images may not.
if command -v pacman >/dev/null 2>&1; then pacman -Sy --noconfirm >/dev/null; fi
if command -v xbps-install >/dev/null 2>&1; then xbps-install -Syu -y xbps >/dev/null 2>&1 || true; xbps-install -Sy >/dev/null; fi

cp -r /src /tmp/nvflux
cd /tmp/nvflux
rm -rf build

step "install.sh"
sh install.sh
bin=/usr/local/bin/nvflux
[ "$(stat -c '%a %U %G' $bin)" = "4750 root nvflux" ] || fail "binary is $(stat -c '%a %U %G' $bin)"
grep -q '^nvflux:' /etc/group || fail "no nvflux group"
[ -f /var/lib/nvflux/installed ] || fail "no manifest"
[ -f /usr/local/share/man/man1/nvflux.1 ] || fail "no man page"
$bin version
$bin help >/dev/null
if $bin status >/tmp/out 2>&1; then fail "status succeeded without a driver"; fi
cat /tmp/out
grep -q '^nvflux: ' /tmp/out || fail "status error not in the nvflux: form"
$bin doctor || true

step "uninstall.sh"
sh uninstall.sh
[ ! -e $bin ] || fail "binary left behind"
grep -q '^nvflux:' /etc/group && fail "group left behind"
[ ! -e /var/lib/nvflux ] || fail "state left behind"

step "test dependencies"
if   command -v apt-get >/dev/null 2>&1; then apt-get install -y -qq util-linux diffutils findutils python3 >/dev/null
elif command -v dnf >/dev/null 2>&1; then dnf install -y -q util-linux diffutils findutils python3 libasan libubsan shadow-utils >/dev/null
elif command -v pacman >/dev/null 2>&1; then pacman -S --needed --noconfirm util-linux diffutils findutils python >/dev/null
elif command -v zypper >/dev/null 2>&1; then zypper --non-interactive install util-linux diffutils findutils python3 shadow >/dev/null
elif command -v apk >/dev/null 2>&1; then apk add --no-cache util-linux diffutils findutils python3 >/dev/null
elif command -v xbps-install >/dev/null 2>&1; then xbps-install -y util-linux diffutils findutils python3 shadow >/dev/null 2>&1 || true
fi

# Sanitizers aren't available everywhere (e.g. AddressSanitizer on musl).
echo 'int main(void){return 0;}' > /tmp/t.c
if cc -fsanitize=address,undefined -o /tmp/t /tmp/t.c 2>/dev/null && /tmp/t; then
    sanitize="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
elif cc -fsanitize=undefined -o /tmp/t /tmp/t.c 2>/dev/null && /tmp/t; then
    sanitize="-O1 -g -fsanitize=undefined -fno-sanitize-recover=all"
else
    sanitize="-O1 -g"
fi
echo "sanitizers: $sanitize"

step "make test (unprivileged user)"
if command -v useradd >/dev/null 2>&1; then useradd -m tester; else adduser -D tester; fi
cp -r /src /home/tester/nvflux
rm -rf /home/tester/nvflux/build
chown -R tester /home/tester/nvflux
su -s /bin/sh tester -c "cd /home/tester/nvflux && make -s test SANITIZE='$sanitize'"
step "PASS"
