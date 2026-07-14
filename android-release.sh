#!/bin/bash

set -euxo pipefail

export DOCKER_BUILDKIT=1

# Android is always cross-compiled (NDK only ships linux-x86_64 prebuilts),
# so the target arch comes from ANDROID_ARCH, not the host. Run this on an
# x64 host regardless of target.
export ANDROID_ARCH="${ANDROID_ARCH:-aarch64}"
export ANDROID_API="${ANDROID_API:-28}"
export WEBKIT_RELEASE_TYPE="${WEBKIT_RELEASE_TYPE:-Release}"
export LTO_FLAG="${LTO_FLAG:-}"
export USE_MIMALLOC="${USE_MIMALLOC:-OFF}"
export USE_EXTERNAL_MIMALLOC="${USE_EXTERNAL_MIMALLOC:-OFF}"

case "$ANDROID_ARCH" in
    aarch64) : "${MARCH_FLAG:="-march=armv8-a+crc -mtune=cortex-a78"}" ;;
    x86_64)  : "${MARCH_FLAG:="-march=x86-64-v2"}" ;;
    *) echo "error: ANDROID_ARCH must be aarch64 or x86_64, got '$ANDROID_ARCH'" >&2; exit 1 ;;
esac
export MARCH_FLAG

export CONTAINER_NAME="bun-webkit-linux-android-${ANDROID_ARCH}"
if [ "$WEBKIT_RELEASE_TYPE" == "Debug" ]; then
    CONTAINER_NAME="${CONTAINER_NAME}-debug"
fi

temp="${temp:-${TMPDIR:-/tmp}}"
mkdir -p "$temp"
rm -rf "$temp/bun-webkit"

docker buildx build -f Dockerfile.android -t "$CONTAINER_NAME" \
    --build-arg ANDROID_ARCH="$ANDROID_ARCH" \
    --build-arg ANDROID_API="$ANDROID_API" \
    --build-arg LTO_FLAG="$LTO_FLAG" \
    --build-arg MARCH_FLAG="$MARCH_FLAG" \
    --build-arg WEBKIT_RELEASE_TYPE="$WEBKIT_RELEASE_TYPE" \
    --build-arg USE_MIMALLOC="$USE_MIMALLOC" \
    --build-arg USE_EXTERNAL_MIMALLOC="$USE_EXTERNAL_MIMALLOC" \
    --progress=plain \
    --platform=linux/amd64 \
    --target=artifact \
    --output type=local,dest="$temp/bun-webkit" .
