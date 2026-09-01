# TP2 round-2 experimenter working notes (scratch)

Binary lineage this round:
- base: Debug jsc (tree as of 2026-06-09, pristine except scratch header edit)
- ARM A  (built 05:29): KeyAtomStringCacheInlines.h instrumented — verified
  snapshot + 2000-iter spin on every cache hit GIL-off + reload-vs-verified
  detector; returns the RELOAD (faithful to original `return slot;` at -O0).
- ARM A' (built ~05:32): spin widened to 30000 iters, gated to 2-3 char keys
  starting with 'p'. Same reload-return.
- ARM B  (next): same as A' but `return verified;` (snapshot fix shape).

Static facts established:
- Debug build is -O0 (CMAKE_CXX_FLAGS_DEBUG = -g): every `slot` mention in
  make() is a separate load; `return slot;` is a reload after equal().
- KEYATOM-IDX in vivo: hash("p8")=15955301 (0xF37565), hash("p17")=14316901
  (0xDA7565); both % 512 = 357. Unique colliding p-pair (no other pair logged
  and no other pair ever appeared in race lines).
- SK2 chain: ThreadAtomics.cpp:594/613/653/691 gilOff dispatch ->
  probeOwnPropertyForAtomicsConcurrent -> JSObject::atomicSlotReadModifyWrite
  (ConcurrentButterfly.cpp:2907) -> atomicSlotLockFreeLoop seq_cst load + CAS
  (ConcurrentButterfly.cpp:2855/2897) — rendezvous establishes HB from
  buildOne writes to all foreign reads.

ARM A results (2000-iter spin, ~12 completed runs, solo, no external load):
- 1x CORRUPT: /tmp/tp2-armA-d/CORRUPT-r0-s12000.log — detector line
  `KEYATOM-RACE idx=357 requested=p17 verified=p17 reloaded=p8` AND
  `named property corrupt: got 2000008 want 2000017` in the SAME run,
  foreign-read frame (test line 86), same-object {p8,p17} signature —
  byte-identical shape to the two historical failures.
- 1x benign trip: requested=p16 verified=p16 reloaded=p16 (different
  JSString, same atom; idx=472) — window alive, content-equal case.

ARM A' results (30000-iter p-key spin): second corruption
  /tmp/tp2-armA2/CORRUPT-r1-s21001.log — TWO trips
  `requested=p17 verified=p17 reloaded=p8`, corruption got 23008 want 23017
  (tid=0 serial=23). (counts to be finalized)

GR2 arm: repro.js loop under ARM A' binary — waiting for a MISMATCH dump
  (expect: siblings intact, butterfly intact, HEALED on re-read).

Preserved logs: Tools/threads/bughunt/logs/TP2-armA-CORRUPT-s12000.log
Patch: Tools/threads/bughunt/tp2/armA-instrumentation.patch
