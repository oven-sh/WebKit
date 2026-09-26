#!/bin/bash
# Builds the measurement binaries one after the other.
log() { echo "[$(date +%H:%M:%S)] $*" >> /workspace/wkbuild/build-queue.log; }
cd /workspace/wkbuild
while pgrep -f "cmake --build relassert-pin-base" > /dev/null; do sleep 5; done
log "relassert-pin-base done: $(ls -la relassert-pin-base/bin/jsc 2>&1 | cut -c1-120)"
for dir in release-instr-pin release-pin release-pin-base; do
  log "$dir starts"
  nice -n 5 cmake --build $dir --target jsc -- -j16 > $dir-build.log 2>&1
  log "$dir done: exit $? $(ls -la $dir/bin/jsc 2>&1 | cut -c1-120)"
done
