#!/bin/bash
# usage: build-arm.sh <arm name> <webkit version or tag>
# LTO release build of bun 590035462 (worktree /workspace/bun-ab) against that WebKit; copies to /workspace/bins/<arm>/{bun,bun-profile}.
arm=$1; wk=$2
[ -d /workspace/bun-ab ] || (cd /workspace/bun && git worktree prune && git worktree add -q --detach /workspace/bun-ab 5900354623)
cd /workspace/bun-ab
bun scripts/build.ts --profile=release --build-dir=build/release-ab --webkit-version=$wk
code=$?
echo "EXIT=$code"
if [ $code = 0 ]; then mkdir -p /workspace/bins/$arm && cp build/release-ab/bun /workspace/bins/$arm/bun && cp build/release-ab/bun-profile /workspace/bins/$arm/bun-profile && echo "COPIED $arm $(/workspace/bins/$arm/bun --revision)"; fi
date -u
