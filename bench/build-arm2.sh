#!/bin/bash
# usage: build-arm2.sh <arm name> <webkit version or tag> [bun commit, default origin/main]
# LTO release build of bun main (worktree /workspace/bun-ab2) against that WebKit; copies to /workspace/bins/<arm>/{bun,bun-profile}.
arm=$1; wk=$2; commit=${3:-origin/main}
if [ ! -d /workspace/bun-ab2 ]; then (cd /workspace/bun && git worktree prune && git worktree add -q --detach /workspace/bun-ab2 $commit) || exit 1; fi
cd /workspace/bun-ab2
git log --oneline -1 | cat
bun scripts/build.ts --profile=release --build-dir=build/release-ab --webkit-version=$wk
code=$?
echo "EXIT=$code"
if [ $code = 0 ]; then mkdir -p /workspace/bins/$arm && cp build/release-ab/bun /workspace/bins/$arm/bun && cp build/release-ab/bun-profile /workspace/bins/$arm/bun-profile && echo "COPIED $arm $(/workspace/bins/$arm/bun --revision)"; fi
date -u
