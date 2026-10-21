#!/bin/bash
# After a container reset:
#   cd /workspace/WebKit && git fetch origin robobun/c4a0b0aa/scratch && git worktree add /workspace/scratch FETCH_HEAD && /workspace/scratch/bench/setup.sh
mkdir -p /tmp/repro && cp -r /workspace/scratch/bench/. /tmp/repro/ && echo "scripts in /tmp/repro"
