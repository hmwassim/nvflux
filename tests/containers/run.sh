#!/bin/sh
# Builds, installs and tests nvflux in fresh containers of several distros.
# Needs docker. Usage: tests/containers/run.sh [image...]
# Logs go to build/containers/<image>.log.
set -u

src=$(cd "$(dirname "$0")/../.." && pwd)
logs=$src/build/containers
mkdir -p "$logs"

[ $# -gt 0 ] || set -- \
    debian:13 \
    fedora:42 \
    archlinux:latest \
    opensuse/tumbleweed:latest \
    alpine:3.22 \
    ghcr.io/void-linux/void-glibc-full:latest \
    gentoo/stage3:latest

pass=0
fail=0
for image in "$@"; do
    name=$(printf '%s' "$image" | tr '/:' '__')
    printf '%-45s ' "$image"
    # --privileged: the integration tests create user namespaces (unshare -r).
    if docker run --rm --privileged -v "$src":/src:ro "$image" sh /src/tests/containers/inner.sh \
        > "$logs/$name.log" 2>&1; then
        pass=$((pass + 1))
        echo "pass"
    else
        fail=$((fail + 1))
        echo "FAIL (see build/containers/$name.log)"
    fi
done
echo "containers: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
