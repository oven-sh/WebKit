#!/bin/bash
# usage: run-jsc-tests.sh <root with bin/jsc> <log>   (what the WebKit CI test job runs)
cd /workspace/WebKit
export JSCTEST_memoryLimit=4294967296
perl Tools/Scripts/run-javascriptcore-tests --no-build --root="$1" --release --jsc-only \
  --no-testmasm --no-testair --no-testb3 --no-testdfg --no-testapi --no-testwasmdebugger --no-testlibjsctools \
  --no-fail-fast --memory-limited --json-output=/tmp/jsc-results.json > "$2" 2>&1
echo "EXIT=$?" >> "$2"
date -u >> "$2"
