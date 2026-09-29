#!/bin/bash
# usage: build-main.sh <dir> [extra cmake args]   (source: /workspace/WebKit-main)
log() { echo "[$(date +%H:%M:%S)] $*" >> /workspace/wkbuild/build-new.log; }
cd /workspace/wkbuild
dir=$1; shift
if [ ! -f $dir/build.ninja ]; then
  log "configure $dir"
  SRC=/workspace/WebKit-main ./configure.sh $dir "$@" > $dir-configure.log 2>&1; log "configure exit $?"
fi
log "$dir starts ($(git -C /workspace/WebKit-main rev-parse --short HEAD))"
nice -n 5 cmake --build $dir --target jsc -- -j10 > $dir-build.log 2>&1
log "$dir done: exit $? $(ls -la $dir/bin/jsc 2>&1 | cut -c1-120)"
