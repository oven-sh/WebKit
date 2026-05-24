#!/bin/bash
# Cross-compile JavaScriptCore for macOS on a Linux host via Dockerfile.macos.
# macOS targets are always cross-compiled here (there is no Linux runner with
# Apple's toolchain), so the target arch comes from MACOS_ARCH, not the host.
set -euxo pipefail

export DOCKER_BUILDKIT=1

export MACOS_ARCH="${MACOS_ARCH:-arm64}"
export MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-13.0}"
export WEBKIT_RELEASE_TYPE="${WEBKIT_RELEASE_TYPE:-Release}"
export LTO_FLAG="${LTO_FLAG:-}"
export CPP_FLAGS="${CPP_FLAGS:-}"
export ENABLE_SANITIZERS="${ENABLE_SANITIZERS:-}"

# Match the native macOS lanes: apple-m1 on the arm64 runners (`cpu: native`
# on macos-14-xlarge), haswell on the amd64 ones.
case "$MACOS_ARCH" in
    arm64)  : "${MARCH_FLAG:="-mcpu=apple-m1"}" ;;
    x86_64) : "${MARCH_FLAG:="-march=haswell"}" ;;
    *) echo "error: MACOS_ARCH must be arm64 or x86_64, got '$MACOS_ARCH'" >&2; exit 1 ;;
esac
export MARCH_FLAG

# Debug builds default ENABLE_MALLOC_HEAP_BREAKDOWN=ON, same as
# mac-release.bash.
DEFAULT_MALLOC_HEAP_BREAKDOWN="OFF"
if [ "$WEBKIT_RELEASE_TYPE" == "Debug" ]; then
    DEFAULT_MALLOC_HEAP_BREAKDOWN="ON"
fi
export ENABLE_MALLOC_HEAP_BREAKDOWN="${ENABLE_MALLOC_HEAP_BREAKDOWN:-$DEFAULT_MALLOC_HEAP_BREAKDOWN}"

export CONTAINER_NAME="bun-webkit-macos-cross-${MACOS_ARCH}"
if [ "$WEBKIT_RELEASE_TYPE" == "Debug" ]; then
    CONTAINER_NAME="${CONTAINER_NAME}-debug"
fi

temp="${temp:-${TMPDIR:-/tmp}}"
mkdir -p "$temp"
rm -rf "$temp/bun-webkit"

docker buildx build -f Dockerfile.macos -t "$CONTAINER_NAME" \
    --build-arg MACOS_ARCH="$MACOS_ARCH" \
    --build-arg MACOS_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
    --build-arg LTO_FLAG="$LTO_FLAG" \
    --build-arg CPP_FLAGS="$CPP_FLAGS" \
    --build-arg MARCH_FLAG="$MARCH_FLAG" \
    --build-arg WEBKIT_RELEASE_TYPE="$WEBKIT_RELEASE_TYPE" \
    --build-arg ENABLE_MALLOC_HEAP_BREAKDOWN="$ENABLE_MALLOC_HEAP_BREAKDOWN" \
    --build-arg ENABLE_SANITIZERS="$ENABLE_SANITIZERS" \
    --progress=plain \
    --platform=linux/amd64 \
    --target=artifact \
    --output type=local,dest="$temp/bun-webkit" .

echo "Successfully built $CONTAINER_NAME to $temp/bun-webkit"
