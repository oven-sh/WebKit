#!/bin/bash
# Runs after the first JSTests run: the interpreter-only run and the default run on the build of the branch to push.
log() { echo "[$(date +%H:%M:%S)] $*" >> /workspace/wkbuild/after-tests.log; }
log "waiting for the first test run"
while pgrep -f "run-jsc-stress-tests.*test-root-new" > /dev/null; do sleep 20; done
log "first test run done"
while pgrep -f "cmake --build relassert-pin" > /dev/null; do sleep 10; done
if [ ! -x /workspace/wkbuild/relassert-pin/bin/jsc ]; then log "no pin build"; exit 1; fi
mkdir -p /workspace/wkbuild/test-root-pin/bin /workspace/wkbuild/test-root-pin-llint/bin
cp /workspace/wkbuild/relassert-pin/bin/jsc /workspace/wkbuild/test-root-pin/bin/jsc
cp /workspace/wkbuild/relassert-pin/bin/jsc /workspace/wkbuild/test-root-pin-llint/bin/jsc
cd /workspace/WebKit-pin
COMMON="--no-build --release --jsc-only --no-testmasm --no-testair --no-testb3 --no-testdfg --no-testapi --no-testwasmdebugger --no-testlibjsctools --no-fail-fast --memory-limited"
log "interpreter-only run starts"
JSCTEST_memoryLimit=4294967296 nice -n 10 perl Tools/Scripts/run-javascriptcore-tests $COMMON --root=/workspace/wkbuild/test-root-pin-llint \
  --no-jit-stress-tests --env-vars "JSC_useJIT=0" --filter '^(?!wasm)' \
  --json-output=/workspace/wkbuild/jsc-results-pin-llint.json > /workspace/wkbuild/jsc-tests-pin-llint.log 2>&1
log "interpreter-only run done: exit $?"
log "default run starts"
JSCTEST_memoryLimit=4294967296 nice -n 10 perl Tools/Scripts/run-javascriptcore-tests $COMMON --root=/workspace/wkbuild/test-root-pin \
  --filter '^(?!wasm)' \
  --json-output=/workspace/wkbuild/jsc-results-pin.json > /workspace/wkbuild/jsc-tests-pin.log 2>&1
log "default run done: exit $?"
