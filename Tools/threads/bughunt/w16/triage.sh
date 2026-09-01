#!/bin/bash
# Extract a dedupe key per failed run log: signal kind + top frames of each reported stack.
for f in "$@"; do
  echo "===== $f ====="
  grep -m1 "RC=" "$f"
  # ASAN error line
  grep -m1 "ERROR: AddressSanitizer" "$f"
  grep -m1 "SUMMARY: AddressSanitizer" "$f"
  # libpas
  grep -m1 "pas_deallocation_did_fail\|deallocation did fail\|pas_panic" "$f"
  # JSC assertion
  grep -m1 "ASSERTION FAILED\|RELEASE_ASSERT\|FATAL:" "$f"
  # First 4 frames of first stack
  awk '/^    #0/{c=1} c&&/^    #/{print; n++} n>=5{exit}' "$f"
  echo
done
