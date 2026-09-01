#!/usr/bin/env bash
# GR1 experimenter campaign: load6x on the butterfly stress test under
# moderate co-tenant load (stress-ng CPU workers), 06-08 shape (SEED_BASE=1000,
# Debug, period=64). Usage: campaign.sh <outdir> <runs_per_worker> [test]
set -u
OUT=${1:?outdir}
RPW=${2:?runs_per_worker}
TEST=${3:-/root/WebKit/JSTests/threads/jit/spawned-thread-butterfly-stress.js}
CPUS=${CPUS:-96}

stress-ng --cpu "$CPUS" --timeout 0 --quiet &
SNG=$!
trap 'kill $SNG 2>/dev/null; wait $SNG 2>/dev/null' EXIT

OUT="$OUT" RUNS_PER_WORKER="$RPW" SEED_BASE=1000 TIMEOUT_S=240 \
    /root/WebKit/Tools/threads/bughunt/gr1/gr1-load6.sh "$TEST"
