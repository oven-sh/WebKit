#!/bin/bash

set -euxo pipefail

export DOCKER_BUILDKIT=1

# FreeBSD is cross-compiled from a Linux host via clang --target + base.txz
# sysroot, so target arch is selected here independent of the host.
export FREEBSD_ARCH="${FREEBSD_ARCH:-x86_64}"
export FREEBSD_VERSION="${FREEBSD_VERSION:-14.3}"
export WEBKIT_RELEASE_TYPE="${WEBKIT_RELEASE_TYPE:-Release}"
export LTO_FLAG="${LTO_FLAG:-}"
export USE_MIMALLOC="${USE_MIMALLOC:-OFF}"
export USE_EXTERNAL_MIMALLOC="${USE_EXTERNAL_MIMALLOC:-OFF}"

case "$FREEBSD_ARCH" in
    x86_64)  : "${MARCH_FLAG:="-march=haswell"}" ;;
    aarch64) : "${MARCH_FLAG:="-march=armv8-a+crc -mtune=ampere1"}" ;;
    *) echo "error: FREEBSD_ARCH must be x86_64 or aarch64, got '$FREEBSD_ARCH'" >&2; exit 1 ;;
esac
export MARCH_FLAG

export CONTAINER_NAME="bun-webkit-freebsd-${FREEBSD_ARCH}"
if [ "$WEBKIT_RELEASE_TYPE" == "Debug" ]; then
    CONTAINER_NAME="${CONTAINER_NAME}-debug"
fi

temp="${temp:-${TMPDIR:-/tmp}}"
mkdir -p "$temp"
rm -rf "$temp/bun-webkit"

docker buildx build -f Dockerfile.freebsd -t "$CONTAINER_NAME" \
    --build-arg FREEBSD_ARCH="$FREEBSD_ARCH" \
    --build-arg FREEBSD_VERSION="$FREEBSD_VERSION" \
    --build-arg LTO_FLAG="$LTO_FLAG" \
    --build-arg MARCH_FLAG="$MARCH_FLAG" \
    --build-arg WEBKIT_RELEASE_TYPE="$WEBKIT_RELEASE_TYPE" \
    --build-arg USE_MIMALLOC="$USE_MIMALLOC" \
    --build-arg USE_EXTERNAL_MIMALLOC="$USE_EXTERNAL_MIMALLOC" \
    --progress=plain \
    --platform=linux/amd64 \
    --target=artifact \
    --output type=local,dest="$temp/bun-webkit" .
