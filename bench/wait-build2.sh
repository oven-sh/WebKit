#!/bin/bash
# Waits for the preview release of a WebKit PR commit and for a build that runs in /workspace/bun-ab2, then builds the arm with bun main.
# usage: wait-build2.sh <arm> <tag> <log of the build to wait for>
arm=$1; tag=$2; prev=$3
url=https://github.com/oven-sh/WebKit/releases/download/$tag/bun-webkit-linux-amd64-lto.tar.gz
for i in $(seq 1 400); do
  # A HEAD request gets 401 from the asset host, so ask for the first bytes.
  code=$(curl -sL -r 0-99 -o /dev/null -w '%{http_code}' "$url"); [ "$code" = 206 ] && code=200
  [ "$code" = 200 ] && break
  sleep 30
done
echo "asset http $code after $i tries at $(date -u)"
[ "$code" = 200 ] || exit 1
( mkdir -p /workspace/dl/wk-$arm-asan && curl -sL --retry 40 --retry-delay 30 --retry-all-errors -o /workspace/dl/$arm-asan.tgz https://github.com/oven-sh/WebKit/releases/download/$tag/bun-webkit-linux-amd64-debug-asan.tar.gz && tar -xzf /workspace/dl/$arm-asan.tgz -C /workspace/dl/wk-$arm-asan && echo "asan unpacked $(date -u)" ) &
while [ -n "$prev" ] && ! grep -q "^EXIT=" "$prev" 2>/dev/null; do sleep 15; done
/tmp/repro/build-arm2.sh $arm $tag > /tmp/repro/build-$arm.log 2>&1
tail -3 /tmp/repro/build-$arm.log
wait
echo "WAITBUILD DONE $(date -u)"
