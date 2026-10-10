#!/bin/sh
# Run nvflux against the fake NVML library. Usage: tests/run.sh <test build dir>
#
# Commands that need root run inside "unshare -r", where the current user is
# mapped to root, so no real privileges or GPUs are involved.
set -u

dir=$1
bin=$dir/nvflux
state=$dir/state/state

if ! unshare -r true 2>/dev/null; then
    echo "skipping tests: user namespaces (unshare -r) are not available"
    exit 0
fi

pass=0
fail=0

reset() {
    rm -rf "$dir/state" "$dir/log" "$dir"/mem*
    printf '%s\n' "$@" > "$dir/config"
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

# check_file <description> <file> <expected content>
check_file() {
    got=$(cat "$2" 2>/dev/null)
    if [ "$got" = "$3" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: %s\n  expected "%s", got "%s"\n' "$1" "$3" "$got"
    fi
}

root() { unshare -r "$bin" "$@"; }
user() { "$bin" "$@"; }

# --- profiles -------------------------------------------------------------

reset
check "powersave skips clocks that cap the core" 0 "GPU 0 (Fake GPU 0): memory locked to 810 MHz" root powersave
check_file "powersave is saved" "$state" "powersave"
check_file "driver got the lock" "$dir/mem0" "810"
check "status shows profile" 0 "profile: powersave" root status
check "status shows clocks" 0 "GPU 0 (Fake GPU 0): memory 810 MHz, core 720 MHz" root status

check "balanced takes the middle uncapped clock" 0 "memory locked to 5001 MHz" root balanced
check_file "balanced is saved" "$state" "balanced"

check "performance locks highest clock" 0 "memory locked to 7501 MHz" root performance
check_file "performance is saved" "$state" "performance"

check "lock uses an exact clock" 0 "memory locked to 5001 MHz" root lock 5001
check "lock snaps to nearest clock" 0 "memory locked to 7301 MHz (nearest supported to 7000)" root lock 7000
check_file "lock saves requested clock" "$state" "lock 7000"
check "lock refuses a clock that caps the core" 1 "add --force to lock it anyway" root lock 405
check_file "refused lock touches no GPU" "$dir/mem0" "7301"
check_file "refused lock keeps the saved profile" "$state" "lock 7000"
check "lock --force allows a capped clock" 0 "memory locked to 405 MHz" root lock 405 --force
check "lock --force still warns" 0 "core is limited to 420 MHz (instead of 2100)" root lock --force 405
check_file "--force is saved" "$state" "lock 405 --force"
check "status shows --force" 0 "profile: lock 405 --force" root status
rm -f "$dir"/mem*
check "restore honours saved --force" 0 "memory locked to 405 MHz" root restore
check "--force is harmless on uncapped clocks" 0 "memory locked to 5001 MHz" root lock 5001 --force

check "auto unlocks" 0 "GPU 0 (Fake GPU 0): memory clock unlocked" root auto
check_file "auto is saved" "$state" "auto"
check_file "driver lock cleared" "$dir/mem0" "0"

# --- restore --------------------------------------------------------------

reset
check "restore with nothing saved does nothing" 0 "" root restore
check_file "restore with nothing saved touches no GPU" "$dir/log" ""

reset
root lock 5001 >/dev/null
rm -f "$dir"/mem*
check "restore re-applies saved profile" 0 "memory locked to 5001 MHz" root restore
check_file "restore keeps the state file" "$state" "lock 5001"

root auto >/dev/null
: > "$dir/log"
check "restore of auto does nothing" 0 "" root restore
check_file "restore of auto touches no GPU" "$dir/log" ""

mkdir -p "$dir/state"
for bad in "ultra" "balanced x" "lock" "lock abc" "lock --force" "lock 405 --force --force" "lock 405 810" "powersave extra" ""; do
    printf '%s\n' "$bad" > "$state"
    check "restore rejects saved '$bad'" 1 "is invalid" root restore
done

# --- resume hook ------------------------------------------------------------

hook=$dir/nvflux-resume.sh
reset
root powersave >/dev/null
rm -f "$dir"/mem*
check "resume hook does nothing before sleep" 0 "" unshare -r sh "$hook" pre suspend
check_file "resume hook touches no GPU before sleep" "$dir/mem0" ""
check "resume hook re-applies after sleep (elogind)" 0 "memory locked to 810 MHz" unshare -r sh "$hook" post suspend
rm -f "$dir"/mem*
check "resume hook re-applies without arguments (zzz)" 0 "memory locked to 810 MHz" unshare -r sh "$hook"

# --- multiple GPUs and failures --------------------------------------------

reset "gpus 2"
check "applies to every GPU" 0 "GPU 1 (Fake GPU 1): memory locked to 810 MHz" root powersave

reset "gpus 2" "unsupported 1"
check "reports an unsupported GPU" 1 "GPU 1: cannot lock memory clock to 810 MHz: not supported" root powersave
check_file "still locks the supported GPU" "$dir/mem0" "810"
check_file "saves when at least one GPU took the profile" "$state" "powersave"

reset "unsupported 0"
check "fails when no GPU took the profile" 1 "not supported by this GPU or driver" root performance
check_file "saves nothing when every GPU failed" "$state" ""

reset "nopersist"
check "warns when persistence mode is unavailable" 0 "cannot enable persistence mode" root powersave

reset "clocks 7001 7001 7001"
check "works with a single supported clock" 0 "memory locked to 7001 MHz" root powersave

reset "clocks 5001 3001 810 405" "cap 405 645"
check "powersave on an MX450-like GPU" 0 "memory locked to 810 MHz" root powersave
check "balanced on an MX450-like GPU" 0 "memory locked to 3001 MHz" root balanced

reset "clocks 5001 405"
check "powersave takes the lowest clock when none caps the core" 0 "memory locked to 405 MHz" root powersave

reset "clocks 7501 810 405" "cap 405 420" "cap 810 1200"
check "powersave never caps the core" 0 "memory locked to 7501 MHz" root powersave
check "balanced never caps the core" 0 "memory locked to 7501 MHz" root balanced

reset "nocaps"
check "powersave refuses to guess without core limits" 1 "use 'nvflux lock <MHz>' instead" root powersave
check "balanced refuses to guess without core limits" 1 "use 'nvflux lock <MHz>' instead" root balanced
check "lock still works without core limits" 0 "memory locked to 810 MHz" root lock 810

# --- privileges and usage --------------------------------------------------

reset
chmod g+w "$dir/libnvidia-ml.so.1"
check "refuses a library others can modify" 1 "refusing to load libnvidia-ml.so.1" root powersave
chmod 755 "$dir/libnvidia-ml.so.1"

reset
check "profiles need root" 1 "changing clocks needs root" user powersave
check "restore needs root" 1 "changing clocks needs root" user restore
check_file "nothing saved without root" "$state" ""
check "help works without root" 0 "Usage: nvflux <command>" user help
check "version works without root" 0 "nvflux " user --version
check "no arguments shows status" 0 "profile: auto" root
check "typos get a suggestion" 2 "Did you mean 'powersave'?" user powersav
check "lock needs a clock" 2 "usage: nvflux lock <MHz>" user lock
check "lock rejects junk" 2 "usage: nvflux lock <MHz>" user lock 12abc
check "lock rejects zero" 2 "usage: nvflux lock <MHz>" user lock 0
check "lock --force needs a clock" 2 "usage: nvflux lock <MHz>" user lock --force
check "--force only goes with lock" 2 "'powersave' takes no arguments" user powersave --force
check "profiles take no arguments" 2 "'powersave' takes no arguments" user powersave now
check "status takes no arguments" 2 "'status' takes no arguments" user status x
check "restore takes no arguments" 2 "'restore' takes no arguments" user restore x

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
