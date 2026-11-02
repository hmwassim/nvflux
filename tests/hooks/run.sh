#!/bin/sh
# Installer tests: install.sh and uninstall.sh against fake root directories
# that look like each init and sleep system. System commands (groupadd,
# systemctl, rc-update, ...) are replaced by stubs that record their calls, so
# the real system is never touched. Usage: tests/hooks/run.sh <nvflux binary>
set -u

src=$(cd "$(dirname "$0")/../.." && pwd)
bin=${1:?usage: tests/hooks/run.sh <nvflux binary>}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if ! unshare -r true 2>/dev/null; then
    echo "skipping installer tests: user namespaces (unshare -r) are not available"
    exit 0
fi

pass=0
fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); printf 'FAIL [%s]: %s\n' "$scenario" "$*"; }

# --- stubs: each logs "name args" to $NVFLUX_ROOT/calls ---------------------------
stubs=$work/stubs
mkdir -p "$stubs"
for c in systemctl rc-update update-rc.d chkconfig chown; do
    printf '#!/bin/sh\necho "%s $*" >> "$NVFLUX_ROOT/calls"\n' "$c" > "$stubs/$c"
done
cat > "$stubs/groupadd" <<'EOF'
#!/bin/sh
echo "groupadd $*" >> "$NVFLUX_ROOT/calls"
echo "nvflux:x:990:" >> "$NVFLUX_ROOT/etc/group"
EOF
cat > "$stubs/usermod" <<'EOF'
#!/bin/sh
echo "usermod $*" >> "$NVFLUX_ROOT/calls"
sed -i "s/^nvflux:\([^:]*\):\([^:]*\):\(.*\)$/nvflux:\1:\2:\3,$3/; s/^nvflux:\([^:]*\):\([^:]*\):,/nvflux:\1:\2:/" "$NVFLUX_ROOT/etc/group"
EOF
cat > "$stubs/groupdel" <<'EOF'
#!/bin/sh
echo "groupdel $*" >> "$NVFLUX_ROOT/calls"
sed -i '/^nvflux:/d' "$NVFLUX_ROOT/etc/group"
EOF
chmod 755 "$stubs"/*

# --- helpers ----------------------------------------------------------------------

# new_root <pid1 name> [dirs...]: a fake system
new_root() {
    root=${work:?}/${scenario:?}
    rm -rf "$root"
    mkdir -p "$root/proc/driver/nvidia" "$root/proc/1" "$root/etc"
    echo "$1" > "$root/proc/1/comm"
    echo "NVRM version: fake 999.99" > "$root/proc/driver/nvidia/version"
    printf 'root:x:0:\nwassim:x:1000:\n' > "$root/etc/group"
    shift
    for d in "$@"; do mkdir -p "$root/$d"; done
    : > "$root/calls"
}

snapshot() { (cd "$root" && find . ! -name calls ! -path './etc/group' | sort; cat etc/group); }

run() { # install.sh or uninstall.sh, with args
    script=$1
    shift
    env NVFLUX_ROOT="$root" NVFLUX_BINARY="$bin" SUDO_USER=wassim PATH="$stubs:$PATH" \
        unshare -r sh "$src/$script" "$@" > "$root.out" 2>&1
}

has_file() { [ -e "$root$1" ] && ok || bad "missing $1"; }
no_file() { [ ! -e "$root$1" ] && ok || bad "unexpected $1"; }
has_line() { grep -qxF -- "$2" "$root$1" 2>/dev/null && ok || bad "'$2' not in $1: $(cat "$root$1" 2>/dev/null)"; }
called() { grep -qF -- "$1" "$root/calls" && ok || bad "'$1' not called: $(cat "$root/calls")"; }
said() { grep -qF -- "$1" "$root.out" && ok || bad "output lacks '$1': $(cat "$root.out")"; }

# install, check, uninstall, check that nothing is left
roundtrip() {
    before=$(snapshot)
    if run install.sh; then ok; else bad "install failed: $(cat "$root.out")"; return 1; fi
    has_file /usr/local/bin/nvflux
    mode=$(stat -c %a "$root/usr/local/bin/nvflux")
    [ "$mode" = 4750 ] && ok || bad "binary mode $mode, expected 4750"
    called "chown root:nvflux $root/usr/local/bin/nvflux"
    called "groupadd -r nvflux"
    grep -q '^nvflux:x:990:wassim$' "$root/etc/group" && ok || bad "wassim not added: $(cat "$root/etc/group")"
    has_line /var/lib/nvflux/installed "group created"
    said "added wassim to the nvflux group"
    ! LC_ALL=C grep -q '[^ -~]' "$root.out" && ok || bad "installer output isn't plain ASCII: $(cat "$root.out")"
    check_install # scenario-specific checks
    if run uninstall.sh; then ok; else bad "uninstall failed: $(cat "$root.out")"; fi
    after=$(snapshot)
    [ "$before" = "$after" ] && ok ||
        bad "uninstall left changes: $(printf '%s\n' "$before" > "$work/b"; printf '%s\n' "$after" > "$work/a"; diff "$work/b" "$work/a")"
}

# --- systemd (Debian, Fedora, Arch, openSUSE, ...) ----------------------------------

scenario=systemd
new_root systemd run/systemd/system
check_install() {
    for u in reload suspend resume; do has_file /usr/local/lib/systemd/system/nvflux-$u.service; done
    has_line /usr/local/lib/systemd/system/nvflux-reload.service "ExecStart=/usr/local/bin/nvflux reload --wait 15"
    has_line /usr/local/lib/systemd/system/nvflux-suspend.service "ExecStart=/usr/local/bin/nvflux off --temporary"
    has_line /usr/local/lib/systemd/system/nvflux-resume.service "ExecStart=/usr/local/bin/nvflux reload --wait 15"
    called "systemctl --root=$root enable --quiet nvflux-reload.service nvflux-suspend.service nvflux-resume.service"
    has_file /usr/local/share/man/man1/nvflux.1
    has_file /usr/local/share/bash-completion/completions/nvflux
    has_file /usr/local/share/zsh/site-functions/_nvflux
    has_file /usr/local/share/fish/vendor_completions.d/nvflux.fish
    has_line /var/lib/nvflux/installed "boot systemd"
    has_line /var/lib/nvflux/installed "sleep systemd"
    no_file /etc/elogind/system-sleep/nvflux
    said "boot hook: systemd"
}
roundtrip
called "systemctl --root=$root disable --quiet nvflux-reload.service nvflux-suspend.service nvflux-resume.service"
called "groupdel nvflux"

# --- OpenRC with elogind (Gentoo, Alpine, Artix) -------------------------------------

scenario=openrc
new_root init run/openrc etc/init.d etc/elogind
check_install() {
    has_file /etc/init.d/nvflux
    [ -x "$root/etc/init.d/nvflux" ] && ok || bad "init script not executable"
    grep -rq '@BINDIR@' "$root/etc" && bad "placeholder left in a hook" || ok
    grep -q '/usr/local/bin/nvflux reload --wait 15' "$root/etc/init.d/nvflux" && ok || bad "openrc command"
    called "rc-update add nvflux default"
    has_file /etc/elogind/system-sleep/nvflux
    has_line /var/lib/nvflux/installed "boot openrc"
    has_line /var/lib/nvflux/installed "sleep elogind"
    no_file /usr/local/lib/systemd/system/nvflux-reload.service
}
roundtrip
called "rc-update del nvflux default"

scenario=openrc-init
new_root openrc-init etc/init.d
check_install() { called "rc-update add nvflux default"; has_line /var/lib/nvflux/installed "boot openrc"; }
roundtrip

# --- runit (Void) with zzz -------------------------------------------------------------

scenario=runit
new_root runit etc/zzz.d etc/runit
printf '#!/bin/sh\n# Default rc.local for void; add your custom commands here.\n' > "$root/etc/rc.local"
printf '#!/bin/sh\n[ -x /etc/rc.local ] && /etc/rc.local\nexec runsvdir\n' > "$root/etc/runit/2"
check_install() {
    has_line /etc/rc.local "/usr/local/bin/nvflux reload --wait 15 &"
    has_line /etc/rc.local "# Default rc.local for void; add your custom commands here."
    has_file /etc/zzz.d/suspend/nvflux
    has_file /etc/zzz.d/resume/nvflux
    has_line /etc/zzz.d/suspend/nvflux "exec /usr/local/bin/nvflux off --temporary"
    has_line /var/lib/nvflux/installed "boot runit"
    has_line /var/lib/nvflux/installed "sleep zzz"
}
roundtrip

# --- runit (Artix): stage 2 runs /etc/local.d/*.start, there's no rc.local ------------

scenario=artix-runit
new_root runit etc/runit etc/elogind
printf '#!/bin/bash\nfor script in /etc/local.d/*.start; do\n    [ -x "$script" ] && "$script"\ndone\n' > "$root/etc/runit/2"
check_install() {
    has_line /etc/local.d/nvflux.start "/usr/local/bin/nvflux reload --wait 15 >/dev/null 2>&1 &"
    [ -x "$root/etc/local.d/nvflux.start" ] && ok || bad "local.d script not executable"
    no_file /etc/rc.local
    has_line /var/lib/nvflux/installed "boot runit"
    has_line /var/lib/nvflux/installed "sleep elogind"
}
roundtrip

scenario=runit-unknown
new_root runit etc/runit
printf '#!/bin/sh\nexec runsvdir\n' > "$root/etc/runit/2"
check_install() { has_line /var/lib/nvflux/installed "boot none"; said "no boot hook for this init system"; }
roundtrip

# --- dinit (Artix, Chimera) -----------------------------------------------------------

scenario=dinit
new_root dinit etc/dinit.d etc/elogind
check_install() {
    has_line /etc/dinit.d/nvflux "command = /usr/local/bin/nvflux reload --wait 15"
    [ "$(readlink "$root/etc/dinit.d/boot.d/nvflux")" = ../nvflux ] && ok || bad "dinit not enabled"
    has_line /var/lib/nvflux/installed "boot dinit"
}
roundtrip

# --- SysV (Devuan) with pm-utils --------------------------------------------------------

scenario=sysv
new_root init etc/init.d etc/pm/sleep.d
check_install() {
    grep -q '^# Provides:          nvflux$' "$root/etc/init.d/nvflux" && ok || bad "LSB header"
    called "update-rc.d nvflux defaults"
    has_file /etc/pm/sleep.d/50nvflux
    has_line /var/lib/nvflux/installed "boot sysv"
    has_line /var/lib/nvflux/installed "sleep pm-utils"
}
roundtrip
called "update-rc.d -f nvflux remove"

# --- Slackware ------------------------------------------------------------------------------

scenario=slackware
new_root init etc/rc.d
echo "Slackware 15.0" > "$root/etc/slackware-version"
printf '#!/bin/sh\n# /etc/rc.d/rc.local\n' > "$root/etc/rc.d/rc.local"
check_install() { has_line /etc/rc.d/rc.local "/usr/local/bin/nvflux reload --wait 15 &"; }
roundtrip

# --- s6 and unknown inits: no automatic hook, clear instructions ------------------------------

scenario=s6
new_root s6-svscan etc/elogind
check_install() {
    has_line /var/lib/nvflux/installed "boot s6"
    said "run '/usr/local/bin/nvflux reload --wait 15' at startup"
    has_file /etc/elogind/system-sleep/nvflux
}
roundtrip

scenario=unknown
new_root someinit
check_install() {
    has_line /var/lib/nvflux/installed "boot none"
    said "no boot hook for this init system"
    said "no sleep hook found"
}
roundtrip

# --- --everyone, upgrades, the old nvflux ----------------------------------------------------

scenario=everyone
new_root systemd run/systemd/system
run install.sh --everyone || bad "install failed"
[ "$(stat -c %a "$root/usr/local/bin/nvflux")" = 4755 ] && ok || bad "--everyone mode"

scenario=reinstall
new_root systemd run/systemd/system
run install.sh || bad "first install failed"
run install.sh || bad "reinstall failed: $(cat "$root.out")"
[ "$(grep -c '^nvflux:' "$root/etc/group")" = 1 ] && ok || bad "group created twice"
[ "$(grep -c 'groupadd' "$root/calls")" = 1 ] && ok || bad "groupadd called twice"
has_line /var/lib/nvflux/installed "group created"   # still owned by nvflux for uninstall
[ "$(grep -c '^usermod' "$root/calls")" = 1 ] && ok || bad "user added twice"
run uninstall.sh || bad "uninstall after reinstall failed"
grep -q '^nvflux:' "$root/etc/group" && bad "group left behind" || ok

scenario=runit-reinstall
new_root runit etc/runit
printf '#!/bin/sh\n' > "$root/etc/rc.local"
printf '[ -x /etc/rc.local ] && /etc/rc.local\n' > "$root/etc/runit/2"
run install.sh && run install.sh || bad "reinstall failed"
[ "$(grep -c 'nvflux reload' "$root/etc/rc.local")" = 1 ] && ok || bad "rc.local block duplicated"

scenario=old-nvflux
new_root systemd run/systemd/system etc/xdg/autostart var/lib/nvflux usr/local/bin
echo "[Desktop Entry]" > "$root/etc/xdg/autostart/nvflux.desktop"
echo "[Desktop Entry]" > "$root/etc/xdg/autostart/nvflux-restore.desktop"
echo "old" > "$root/usr/local/bin/nvflux.bak"
echo "powersave" > "$root/var/lib/nvflux/state"
run install.sh || bad "install over the old nvflux failed"
no_file /etc/xdg/autostart/nvflux.desktop
no_file /etc/xdg/autostart/nvflux-restore.desktop
no_file /usr/local/bin/nvflux.bak
has_line /var/lib/nvflux/state "powersave"   # kept; nvflux converts it when reading

scenario=existing-group
new_root systemd run/systemd/system
printf 'root:x:0:\nwassim:x:1000:\nnvflux:x:500:wassim\n' > "$root/etc/group"
run install.sh || bad "install failed"
grep -q groupadd "$root/calls" && bad "created an existing group" || ok
grep -q usermod "$root/calls" && bad "re-added a member" || ok
grep -q '^group created$' "$root/var/lib/nvflux/installed" && bad "claims a group it didn't create" || ok
run uninstall.sh
grep -q '^nvflux:x:500:wassim$' "$root/etc/group" && ok || bad "removed a group it didn't create"

scenario=no-manifest-uninstall
new_root systemd run/systemd/system usr/local/bin etc/xdg/autostart
echo old > "$root/usr/local/bin/nvflux"
echo "[Desktop Entry]" > "$root/etc/xdg/autostart/nvflux.desktop"
run uninstall.sh || bad "uninstall without manifest failed"
no_file /usr/local/bin/nvflux
no_file /etc/xdg/autostart/nvflux.desktop

scenario=not-root
new_root systemd
env NVFLUX_ROOT="$root" NVFLUX_BINARY="$bin" PATH="$stubs:$PATH" sh "$src/install.sh" > "$root.out" 2>&1 &&
    bad "installed without root" || said "run as root"

echo "installer: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
