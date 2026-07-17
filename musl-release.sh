#!/bin/bash

set -euxo pipefail

export DOCKER_BUILDKIT=1

export BUILDKIT_ARCH=$(uname -m)
export ARCH=${BUILDKIT_ARCH}
export LTO_FLAG="${LTO_FLAG:-""}"
if [ "$BUILDKIT_ARCH" == "amd64" ]; then
    export BUILDKIT_ARCH="amd64"
    export ARCH=x64
fi

if [ "$BUILDKIT_ARCH" == "x86_64" ]; then
    export BUILDKIT_ARCH="amd64"
    export ARCH=x64
fi

if [ "$BUILDKIT_ARCH" == "arm64" ]; then
    export BUILDKIT_ARCH="arm64"
    export ARCH=aarch64
fi

if [ "$BUILDKIT_ARCH" == "aarch64" ]; then
    export BUILDKIT_ARCH="arm64"
    export ARCH=aarch64
fi

if [ "$BUILDKIT_ARCH" == "armv7l" ]; then
    echo "Unsupported platform: $BUILDKIT_ARCH"
    exit 1
fi

export WEBKIT_RELEASE_TYPE=${WEBKIT_RELEASE_TYPE:-"Release"}

# Set default MARCH_FLAG based on architecture if not already set
if [ -z "${MARCH_FLAG:-}" ]; then
    if [ "$BUILDKIT_ARCH" == "arm64" ]; then
        export MARCH_FLAG="-march=armv8-a+crc -mtune=ampere1"
    elif [ "$BUILDKIT_ARCH" == "amd64" ]; then
        export MARCH_FLAG="-march=nehalem"
    fi
fi
export MARCH_FLAG="${MARCH_FLAG:-""}"

export CONTAINER_NAME=bun-webkit-linux-$BUILDKIT_ARCH

if [ "$WEBKIT_RELEASE_TYPE" == "relwithdebuginfo" ]; then
    CONTAINER_NAME=bun-webkit-linux-$BUILDKIT_ARCH-dbg
fi

export USE_MIMALLOC=${USE_MIMALLOC:-"OFF"}
export USE_EXTERNAL_MIMALLOC=${USE_EXTERNAL_MIMALLOC:-"OFF"}

mkdir -p $temp
rm -rf $temp/bun-webkit

docker buildx build -f Dockerfile.musl -t $CONTAINER_NAME --build-arg LTO_FLAG="$LTO_FLAG" --build-arg MARCH_FLAG="$MARCH_FLAG" --build-arg WEBKIT_RELEASE_TYPE=$WEBKIT_RELEASE_TYPE --build-arg USE_MIMALLOC="$USE_MIMALLOC" --build-arg USE_EXTERNAL_MIMALLOC="$USE_EXTERNAL_MIMALLOC" --progress=plain --platform=linux/$BUILDKIT_ARCH --target=artifact --output type=local,dest=$temp/bun-webkit .
