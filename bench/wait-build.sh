#!/bin/bash
# Waits for the preview release of a WebKit PR commit, then builds the arm. usage: wait-build.sh <arm> <tag>
arm=$1; tag=$2
url=https://github.com/oven-sh/WebKit/releases/download/$tag/bun-webkit-linux-amd64-lto.tar.gz
for i in $(seq 1 200); do
  code=$(curl -sIL -o /dev/null -w '%{http_code}' "$url")
  [ "$code" = 200 ] && break
  sleep 30
done
echo "asset http $code after $i tries at $(date -u)"
[ "$code" = 200 ] || exit 1
# the debug-asan tarball for the jsc probes
mkdir -p /workspace/dl/wk-$arm-asan && curl -sL --retry 40 --retry-delay 30 --retry-all-errors -o /workspace/dl/$arm-asan.tgz https://github.com/oven-sh/WebKit/releases/download/$tag/bun-webkit-linux-amd64-debug-asan.tar.gz && tar -xzf /workspace/dl/$arm-asan.tgz -C /workspace/dl/wk-$arm-asan && echo "asan unpacked $(date -u)" &
/tmp/repro/build-arm.sh $arm $tag > /tmp/repro/build-$arm.log 2>&1
tail -3 /tmp/repro/build-$arm.log
wait
echo "WAITBUILD DONE $(date -u)"
