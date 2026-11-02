#!/bin/sh
# Integration tests: the real nvflux binary against the fake NVIDIA library,
# a fake /sys and a fake /proc/cmdline. Usage: tests/run.sh <test build dir>
#
# Commands that need root run inside "unshare -r", where the current user is
# mapped to root, so no real privileges or GPUs are involved.
set -u

dir=${1:?usage: tests/run.sh <test build dir>}
bin=$dir/nvflux
state=$dir/var/state
temp=$dir/run/current

if ! unshare -r true 2>/dev/null; then
    echo "skipping integration tests: user namespaces (unshare -r) are not available"
    exit 0
fi

pass=0
fail=0

# reset [config lines...]: fresh fake driver, sysfs and state
reset() {
    rm -rf "${dir:?}/var" "${dir:?}/run" "${dir:?}/sys" "${dir:?}/proc" "${dir:?}/log" "${dir:?}/initlog" \
        "${dir:?}"/mem* "${dir:?}"/gfx*
    printf '%s\n' "$@" > "$dir/config"
    gpus=$(sed -n 's/^gpus //p' "$dir/config")
    i=1
    while [ "$i" -le "${gpus:-1}" ]; do
        d=$dir/sys/bus/pci/devices/0000:0$i:00.0
        mkdir -p "$d/power"
        echo 0x10de > "$d/vendor"
        echo 0x030000 > "$d/class"
        echo active > "$d/power/runtime_status"
        echo on > "$d/power/control"
        i=$((i + 1))
    done
    # the card's HDMI audio function must not be mistaken for a GPU
    a=$dir/sys/bus/pci/devices/0000:01:00.1
    mkdir -p "$a/power"
    echo 0x10de > "$a/vendor"
    echo 0x040300 > "$a/class"
    echo suspended > "$a/power/runtime_status"
    mkdir -p "$dir/proc/driver/nvidia"
    echo "BOOT_IMAGE=/vmlinuz root=/dev/sda1 quiet" > "$dir/proc/cmdline"
    echo "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  999.99  Release Build  (fake)" \
        > "$dir/proc/driver/nvidia/version"
}

# check <description> <expected exit code> <text expected in output> <command...>
check() {
    desc=$1 want_rc=$2 want=$3
    shift 3
    out=$("$@" 2>&1)
    rc=$?
    if [ -z "$want" ]; then
        matched=$([ -z "$out" ] && echo y)
    else
        matched=$(printf '%s' "$out" | grep -qF -- "$want" && echo y)
    fi
    if [ "$rc" -eq "$want_rc" ] && [ "$matched" = y ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: %s\n  expected rc=%s and "%s"\n  got rc=%s:\n%s\n' \
            "$desc" "$want_rc" "$want" "$rc" "$(printf '%s' "$out" | sed 's/^/    /')"
    fi
}

# check_not <description> <text that must not appear> <command...>
check_not() {
    desc=$1 bad=$2
    shift 2
    out=$("$@" 2>&1)
    if printf '%s' "$out" | grep -qF -- "$bad"; then
        fail=$((fail + 1))
        printf 'FAIL: %s\n  output contains "%s":\n%s\n' "$desc" "$bad" "$(printf '%s' "$out" | sed 's/^/    /')"
    else
        pass=$((pass + 1))
    fi
}

# check_file <description> <file> <expected content> (missing file = "")
check_file() {
    got=$(cat "$2" 2>/dev/null)
    if [ "$got" = "$3" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: %s\n  expected "%s", got "%s"\n' "$1" "$3" "$got"
    fi
}

# check_has <description> <file> <line that must be in it>
check_has() {
    if grep -qxF -- "$3" "$2" 2>/dev/null; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: %s\n  "%s" not in %s:\n%s\n' "$1" "$3" "$2" "$(sed 's/^/    /' "$2" 2>/dev/null)"
    fi
}

root() { unshare -r "$bin" "$@"; }
user() { "$bin" "$@"; }
UUID1=GPU-fake0000-0000-0000-0000-000000000001

# --- modes ------------------------------------------------------------------

reset
check "balanced" 0 "GPU 0: balanced: memory 810 MHz, graphics 945-2100 MHz" root balanced
check_file "balanced locks memory at the lowest uncapped clock" "$dir/mem0" "810"
check_file "balanced sets a numeric graphics floor" "$dir/gfx0" "945 2100"
check_has "balanced is saved" "$state" "mode=balanced"
check "a change prints one line per GPU and nothing else" 0 "$(printf 'GPU 0: balanced: memory 810 MHz, graphics 945-2100 MHz')" sh -c "unshare -r \"$bin\" balanced | wc -l | grep -qx 1 && unshare -r \"$bin\" balanced"
check "status shows the mode" 0 "mode: balanced" user status
check "status shows the memory target" 0 "memory      810 MHz  (locked 810)" user status
check "status shows the graphics target" 0 "graphics    945 MHz  (locked 945-2100)" user

: > "$dir/log"
check "powersave" 0 "GPU 0: powersave: memory 810 MHz, graphics up to base clock" root powersave
check "switching modes clears the old graphics lock first" 0 "order: resetgfx lockgfx" \
    sh -c "printf 'order: '; grep gfx '$dir/log' | cut -d' ' -f1 | tr '\n' ' '"
check_file "powersave uses the symbolic base-clock cap" "$dir/gfx0" "unl tdp"
check_has "powersave is saved" "$state" "mode=powersave"

check "max" 0 "GPU 0: max: memory 7501 MHz" root max
check_file "max locks the highest memory clock" "$dir/mem0" "7501"
check_file "max leaves graphics to the driver" "$dir/gfx0" "0"

check "off" 0 "GPU 0: off" root off
check_file "off resets memory" "$dir/mem0" "0"
check_file "off resets graphics" "$dir/gfx0" "0"
check_has "off is saved" "$state" "mode=off"
check "status when off" 0 "mode: off" user status

reset nosymbolic
check "powersave falls back to a numeric cap on old drivers" 0 "graphics 405-1320 MHz" root powersave
check_file "fallback cap is 63% of max" "$dir/gfx0" "405 1320"

check "aliases work and teach the real name" 0 "nvflux: warning: 'performance' is now 'max'" root performance
check_file "alias applied" "$dir/mem0" "7501"

# --- expert locks -------------------------------------------------------------

reset
check "lock snaps to the nearest supported clock" 0 "GPU 0: locked: memory 810 MHz (nearest to 1000)" root lock memory 1000
check_file "never sends an unsupported clock (the driver would round 1000 up to 5001)" "$dir/mem0" "810"
check_has "lock is saved as custom" "$state" "mode=custom"
check_has "lock saves the requested clock" "$state" "memory=1000"
check "lock graphics keeps the memory lock" 0 "memory 810 MHz (nearest to 1000), graphics 900-1800 MHz" root lock graphics 900-1800
check_has "both locks are saved" "$state" "memory=1000"
check_has "both locks are saved (graphics)" "$state" "graphics=900-1800"
check_file "graphics range applied" "$dir/gfx0" "900 1800"
check "status shows custom locks" 0 "mode: locked" user status

check "a memory clock that caps the core is refused" 1 "nvflux: GPU 0: 405 MHz memory limits graphics to 420 MHz; add --force to allow" root lock memory 405
check_file "refused lock touches nothing" "$dir/mem0" "810"
check_has "refused lock keeps the saved setting" "$state" "memory=1000"
check "--force allows it, with a warning" 0 "nvflux: warning: GPU 0: 405 MHz memory limits graphics to 420 MHz" root lock memory 405 --force
check_has "--force is saved" "$state" "memory_force=1"

reset
check "a low graphics cap is refused" 1 "nvflux: GPU 0: graphics cap below 945 MHz; add --force to allow" root lock graphics 405-600
check_file "refused cap touches nothing" "$dir/gfx0" ""
check "--force allows a low cap" 0 "graphics 405-600 MHz" root lock graphics 405-600 --force
check "a floor never needs --force" 0 "graphics 2010-2100 MHz" root lock graphics 2010-2100
check "graphics values snap and say so" 0 "graphics 1995-2100 MHz (nearest to 2000-2100)" root lock graphics 2000-2100
check "a single graphics clock" 0 "graphics 1500 MHz" root lock graphics 1500
check "an inverted range is a usage error with the fix" 2 "use 'nvflux lock graphics 900-1800'" root lock graphics 1800-900

# --- temporary, reload and sleep ------------------------------------------------

reset
root balanced >/dev/null
check "--temporary applies now" 0 "(temporary, saved: balanced)" root max --temporary
check_file "temporary max is applied" "$dir/mem0" "7501"
check_has "the saved mode is untouched" "$state" "mode=balanced"
check_has "the temporary mode is recorded in /run" "$temp" "mode=max"
check "status shows both" 0 "mode: max (temporary, saved: balanced)" user status
check "reload brings the saved mode back" 0 "GPU 0: balanced: memory 810 MHz" root reload
check_file "reload re-applied balanced" "$dir/mem0" "810"
check_file "reload removes the temporary state" "$temp" ""

check "before sleep: off --temporary" 0 "GPU 0: off" root off --temporary
check_file "locks are dropped before sleep" "$dir/mem0" "0"
check_has "the saved mode survives sleep" "$state" "mode=balanced"
check "after resume: reload" 0 "balanced" root reload --wait 3
check_file "locks are back after resume" "$dir/mem0" "810"
check_file "graphics floor is back after resume" "$dir/gfx0" "945 2100"

reset
root off >/dev/null
root max --temporary >/dev/null
check "reload undoes a temporary mode when the saved mode is off" 0 "off" root reload
check_file "temporary max was undone" "$dir/mem0" "0"

reset
root balanced >/dev/null
root max --temporary >/dev/null
check "a saved mode replaces a temporary one" 0 "powersave" root powersave
check_file "the temporary state is gone" "$temp" ""

reset
check "reload with nothing saved does nothing" 0 "" root reload
check_file "reload with nothing saved touches no GPU" "$dir/log" ""

reset
root balanced >/dev/null
rm -f "$dir"/mem* "$dir"/gfx* "$dir/log"
check "reload re-applies after a reboot" 0 "balanced" root reload
check_file "reboot: memory re-locked" "$dir/mem0" "810"

reset
root balanced >/dev/null
rm -f "$dir"/mem* "$dir/log"
echo "BOOT_IMAGE=/vmlinuz nvflux=off quiet" > "$dir/proc/cmdline"
check "nvflux=off on the kernel command line disables reload" 0 "disabled by nvflux=off" root reload
check_file "nvflux=off: nothing applied" "$dir/log" ""
echo "BOOT_IMAGE=/vmlinuz nvflux=offline quiet" > "$dir/proc/cmdline"
check "only the exact word nvflux=off counts" 0 "balanced" root reload

reset
mkdir -p "$dir/var"
echo "mode=custom" > "$state"
check "reload rejects a damaged state file" 1 "is damaged; choose a mode again" root reload
check "a damaged state file is replaced by a new choice" 0 "balanced" root balanced
check_has "replaced" "$state" "mode=balanced"

# --- the old nvflux ----------------------------------------------------------------

for old in powersave balanced; do
    reset
    mkdir -p "$dir/var"
    echo "$old" > "$state"
    check "old '$old' becomes balanced" 0 "GPU 0: balanced: memory 810 MHz" root reload
done
reset
mkdir -p "$dir/var"
echo performance > "$state"
check "old 'performance' becomes max" 0 "GPU 0: max: memory 7501 MHz" root reload
reset
mkdir -p "$dir/var"
echo "lock 5001" > "$state"
check "old 'lock 5001' becomes a custom memory lock" 0 "GPU 0: locked: memory 5001 MHz" root reload
reset
mkdir -p "$dir/var"
echo "lock 405 --force" > "$state"
check "old forced locks stay forced" 0 "GPU 0: locked: memory 405 MHz" root reload

reset
check "the old 'lock <MHz>' still works and teaches the new form" 0 "nvflux: warning: 'lock 5001' is now 'lock memory 5001'" root lock 5001
check_file "old lock syntax applied" "$dir/mem0" "5001"
check "the old 'restore' still works and teaches 'reload'" 0 "nvflux: warning: 'restore' is now 'reload'" root restore
check_file "restore re-applied" "$dir/mem0" "5001"

# --- sleeping laptop GPUs ------------------------------------------------------------

reset
root balanced >/dev/null
echo suspended > "$dir/sys/bus/pci/devices/0000:01:00.0/power/runtime_status"
rm -f "$dir/initlog"
check "status doesn't wake a sleeping GPU" 0 "GPU 0000:01:00.0: asleep (not checked)" user status
check "info doesn't wake a sleeping GPU" 0 "asleep" user info
check "reload doesn't wake a sleeping GPU" 0 "GPU asleep; not waking it" root reload
check_file "the driver was never touched" "$dir/initlog" ""
check "status --json reports it asleep" 0 '"asleep":true' user status --json

reset
echo auto > "$dir/sys/bus/pci/devices/0000:01:00.0/power/control"
root balanced >/dev/null
check_not "no persistence mode on GPUs that power themselves off" "persistence" cat "$dir/log"
reset
root balanced >/dev/null
check_has "persistence mode on desktop GPUs" "$dir/log" "persistence 0 1"
reset nopersist
check "a persistence failure is only a warning" 0 "nvflux: warning: GPU 0: persistence mode failed" root balanced

# --- several GPUs ------------------------------------------------------------------------

reset "gpus 2"
check "modes apply to every GPU" 0 "GPU 1: balanced" root balanced
check_file "GPU 0 locked" "$dir/mem0" "810"
check_file "GPU 1 locked" "$dir/mem1" "810"

check "--gpu applies to one GPU" 0 "GPU 1: max" root max --gpu 1
check_file "--gpu: GPU 1 changed" "$dir/mem1" "7501"
check_file "--gpu: GPU 0 untouched" "$dir/mem0" "810"
check_has "--gpu is saved by UUID" "$state" "$UUID1.mode=max"
check_has "--gpu keeps the others' mode" "$state" "mode=balanced"
check "status shows the per-GPU mode" 0 "GPU 1: Fake GPU 1 (mode: max)" user status
rm -f "$dir"/mem* "$dir"/gfx*
check "reload applies per-GPU modes" 0 "max" root reload
check_file "reload: GPU 0 balanced" "$dir/mem0" "810"
check_file "reload: GPU 1 max" "$dir/mem1" "7501"
check "a mode for all GPUs clears per-GPU modes" 0 "powersave" root powersave
check_not "per-GPU mode cleared" "$UUID1" cat "$state"
check "--gpu with a missing GPU" 2 "nvflux: no GPU 5; see 'nvflux status'" root balanced --gpu 5

reset "gpus 2" "arch 1 6"
check "Turing: graphics part only, explained" 0 "nvflux: warning: GPU 1: memory clock can't be locked (needs RTX 30 or newer)" root balanced
check_file "Turing: graphics floor applied" "$dir/gfx1" "945 2100"
check_file "Turing: memory never touched" "$dir/mem1" ""
check "Turing: expert memory lock refused" 5 "nvflux: GPU 1: memory clock can't be locked" root lock memory 5001

reset "gpus 2" "arch 1 4"
check "Pascal next to Ampere: partly applied" 5 "nvflux: GPU 1: clocks can't be locked" root balanced
check_file "Pascal: the Ampere GPU still locked" "$dir/mem0" "810"
check_has "saved when at least one GPU took it" "$state" "mode=balanced"
reset "arch 0 4"
check "Pascal alone: unsupported" 4 "nvflux: GPU 0: clocks can't be locked" root balanced
check_file "Pascal alone: nothing saved" "$state" ""

reset "gpus 2" "nomem 1"
check "driver refusing memory locks is handled per GPU" 0 "GPU 1: memory clock can't be locked" root balanced
check_file "nomem: GPU 0 still locked" "$dir/mem0" "810"

# --- failures ---------------------------------------------------------------------------

reset "failgfx 0"
check "a graphics failure is reported" 1 "nvflux: GPU 0: failed to set graphics clock: Fake Error; nothing changed" root balanced
check_file "all-or-nothing: the memory lock was rolled back" "$dir/mem0" "0"
check_file "nothing saved after a failure" "$state" ""

reset "memshown 0 7501"
check "displays keeping memory higher are reported" 0 "nvflux: warning: GPU 0: memory runs at 7501 MHz, not 810" root balanced

reset nocaps
check "unknown tables: modes refuse to guess" 1 "nvflux lock memory <MHz>" root balanced
check "unknown tables: expert memory lock works" 0 "GPU 0: locked: memory 810 MHz" root lock memory 810

reset bigtable
check "graphics tables over 256 entries" 0 "GPU 0: balanced" root balanced

reset "clocks 7001"
check "a GPU with a single memory clock" 0 "memory 7001 MHz" root balanced

reset "gpus 0"
check "no GPU" 1 "nvflux: no NVIDIA GPU found" root balanced

# --- privileges ---------------------------------------------------------------------------

reset
check "status needs no root" 0 "mode: off" user status
check "info needs no root" 0 "lock memory: yes, lock graphics: yes" user info
check "help needs no root" 0 "usage: nvflux" user help
check "version" 0 "nvflux 1.0.0" user --version
check "without root: exit 3, with the fix" 3 "nvflux: changing clocks needs root; use sudo or join the nvflux group" user balanced
check_file "nothing changed without permission" "$dir/mem0" ""
check "reload needs root too" 3 "needs root" user reload

reset
chmod g+w "$dir/libnvidia-ml.so.1"
check "refuses a library others can modify" 1 "refusing to load libnvidia-ml.so.1" root balanced
chmod 755 "$dir/libnvidia-ml.so.1"

# --- usage and machine output ---------------------------------------------------------------

check "typos get a suggestion" 2 "nvflux: unknown command 'balance'; did you mean 'balanced'?" user balance
check "usage errors say what to do" 2 "see 'nvflux info'" user lock memory abc
reset
root balanced >/dev/null
if command -v python3 >/dev/null 2>&1; then
    check "status --json is valid JSON" 0 '"schema": 1' sh -c "\"$bin\" status --json | python3 -m json.tool"
    check "info --json is valid JSON" 0 '"memory_clocks"' sh -c "\"$bin\" info --json | python3 -m json.tool"
fi
check "status --json has the targets" 0 '"target":{"min":810,"max":810}' user status --json

# --- doctor -------------------------------------------------------------------------------

reset
check "doctor runs without root" 0 "ok    driver: 999.99" user doctor
check "doctor: no hooks installed" 0 "warn  hooks: not installed; the mode won't come back after reboot; run install.sh" user doctor
check "doctor: GPU recognised" 0 "ok    GPU 0: Fake GPU 0 (Ampere)" user doctor
check "doctor: build not setuid" 0 "is not setuid root; changing clocks needs sudo" user doctor
check "doctor: persistence off on a desktop GPU" 0 "warn  GPU 0: persistence mode off; the lock may be lost when idle" user doctor

root balanced >/dev/null
check "doctor: mode applied" 0 "ok    GPU 0: balanced applied" user doctor
rm -f "$dir"/mem* "$dir"/gfx*
check "doctor: mode saved but not applied" 1 "FAIL  GPU 0: balanced not applied (memory 405 MHz, expected 810); run 'sudo nvflux reload'" user doctor

reset
mkdir -p "$dir/var"
printf 'file %s\nboot systemd\nsleep systemd\n' "$dir/nvflux" > "$dir/var/installed"
check "doctor: boot hook" 0 "ok    boot hook: systemd" user doctor
check "doctor: sleep hook" 0 "ok    sleep hook: systemd" user doctor
printf 'file %s/missing.service\nboot systemd\n' "$dir" > "$dir/var/installed"
check "doctor: a missing hook file fails" 1 "is missing; run install.sh again" user doctor
check "doctor: no sleep hook" 1 "warn  sleep hook: none; locks stay on during sleep" user doctor

reset
mkdir -p "$dir/var"
echo "mode=custom" > "$dir/var/state"
check "doctor: damaged state" 1 "is damaged; choose a mode again" user doctor

reset
echo "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  998.10  Release Build" > "$dir/proc/driver/nvidia/version"
check "doctor: driver updated without reboot" 1 "FAIL  driver: updated without reboot (kernel 998.10, library 999.99); reboot" user doctor
rm -f "$dir/proc/driver/nvidia/version"
check "doctor: kernel module not loaded" 1 "FAIL  driver: NVIDIA kernel module not loaded" user doctor

reset
echo "BOOT_IMAGE=/vmlinuz nvflux=off" > "$dir/proc/cmdline"
check "doctor: nvflux=off" 0 "warn  boot: nvflux=off on the kernel command line" user doctor

reset "memshown 0 7501"
root balanced >/dev/null 2>&1
check "doctor: displays keeping memory higher" 0 "warn  GPU 0: memory runs at 7501 MHz, not 810" user doctor

reset "arch 0 4"
check "doctor: GPU too old" 1 "FAIL  GPU 0: Fake GPU 0 (Pascal) can't lock clocks" user doctor
reset "arch 0 6"
check "doctor: Turing" 0 "warn  GPU 0: Fake GPU 0 (Turing) can't lock memory; balanced won't stop audio crackle" user doctor

reset
echo suspended > "$dir/sys/bus/pci/devices/0000:01:00.0/power/runtime_status"
echo auto > "$dir/sys/bus/pci/devices/0000:01:00.0/power/control"
rm -f "$dir/initlog"
check "doctor: sleeping GPU left asleep" 0 "ok    GPU 0000:01:00.0: asleep; GPU checks skipped so it stays asleep" user doctor
check_file "doctor never woke it" "$dir/initlog" ""

# --- output for people ------------------------------------------------------------

reset
check "help lists the modes with their uses" 0 "balanced    steady memory, graphics floor   (audio crackle, flicker, lag)" user help
check "help explains --temporary" 0 "--temporary   apply without saving" user --help
check "help says how to get permission" 0 "use sudo or join the nvflux group" user help
check "temperature in plain ASCII" 0 "42 C" user status
check_not "no colour codes when not on a terminal" "$(printf '\033')" user status
root max >/dev/null
check "status shows max" 0 "memory     7501 MHz  (locked 7501)" user status
check "info shows what each mode would set" 0 "balanced   memory locked 810, graphics locked 945-2100" user info
check "info flags clocks that limit graphics" 0 "405-420  (limits graphics)" user info

# Every message, in every situation above and here, is plain ASCII.
reset "gpus 2" "arch 1 6" "memshown 0 7501"
all=$( { root balanced; root powersave --gpu 1; root lock memory 405; root lock graphics 405-600 --force;
         root max --temporary; user status; user info; user help; user balance; user lock memory abc;
         root reload; user status --json; user info --json; user balanced; user doctor; } 2>&1)
if printf '%s' "$all" | LC_ALL=C grep -q '[^ -~	]'; then
    fail=$((fail + 1)); echo "FAIL: output contains non-ASCII characters or control codes:"
    printf '%s\n' "$all" | LC_ALL=C grep -n '[^ -~	]' | sed 's/^/    /'
else
    pass=$((pass + 1))
fi

echo "integration: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
