#!/bin/bash
cd /workspace/wkbuild/results/jst-llint-on
FILTER='llint|get-by-id|proto|unset|length|dictionar|inline-cache|poly|instanceof|iterator|for-of|put-by-id|tier|osr|string|array|global|missing|undefined|property|structure|watchpoint|jit|deferral|baseline'
perl /workspace/WebKit-new/Tools/Scripts/run-javascriptcore-tests --no-build --root=/workspace/wkbuild/relassert-new --release --jsc-only \
  --no-testmasm --no-testair --no-testb3 --no-testdfg --no-testapi --no-testwasmdebugger --no-testlibjsctools \
  --no-fail-fast --memory-limited --child-processes=8 \
  --env-vars="JSC_useJIT=0 JSC_useLLIntUnsetCaching=1 JSC_useLLIntPrototypeCacheRearming=1" \
  --filter="$FILTER" --json-output=/workspace/wkbuild/results/jst-llint-on/results.json > /workspace/wkbuild/results/jst-llint-on/log.txt 2>&1
echo "exit $? $(date -u +%H:%M:%S)" > /workspace/wkbuild/results/jst-llint-on/done.txt
