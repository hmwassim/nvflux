#!/bin/sh
# Safety rule 1: nvflux may only ever load these NVML functions that change
# anything. Overclocking, voltage, power limits, fans and application clocks
# must never even be resolved. Usage: tests/check_symbols.sh src/nvml.c
set -eu

allowed='SetPersistenceMode SetMemoryLockedClocks ResetMemoryLockedClocks SetGpuLockedClocks ResetGpuLockedClocks'

# Every NVML function nvflux resolves appears as SYMBOL(Name, ...).
found=$(sed -n 's/^ *SYMBOL(\([A-Za-z0-9_]*\),.*/\1/p' "$1")
[ -n "$found" ] || { echo "check_symbols: no SYMBOL() entries found in $1"; exit 1; }

bad=0
for name in $found; do
    case $name in
    *Set*|*Reset*|*Clear*|*Enable*|*Disable*|*Register*|*Write*)
        ok=0
        for a in $allowed; do
            [ "$name" = "Device$a" ] && ok=1
        done
        if [ "$ok" -eq 0 ]; then
            echo "check_symbols: nvflux must not load nvml$name"
            bad=1
        fi
        ;;
    esac
done
# Nothing may call dlsym with a name built at runtime either.
if grep -n 'dlsym(' "$1" | grep -v 'symbols\[i\].name' >/dev/null; then
    echo "check_symbols: dlsym() is only allowed through the symbol table"
    bad=1
fi
[ "$bad" -eq 0 ] && echo "symbol allowlist: ok"
exit "$bad"
