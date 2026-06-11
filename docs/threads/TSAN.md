# TSAN no-JIT build

ThreadSanitizer configuration for the shared-memory Thread work (see
`THREAD.md`, "adversarial race-hunting pass with TSAN (no-JIT config)").
This is the race-detection oracle: every VM/runtime/WTF memory access is
compiler-instrumented, so data races introduced by the threads workstreams
show up as TSAN reports instead of heisenbugs.

## Why no JIT

TSAN only sees memory accesses in code the compiler instrumented. JIT-emitted
machine code and the hand-written LLInt assembly are invisible to it, which
produces both false negatives (races reached from jitted frames are missed)
and false positives (TSAN cannot model synchronization performed in
uninstrumented frames). So this configuration builds the CLoop — the C++
interpreter — and compiles out every JIT tier:

- `ENABLE_C_LOOP=ON`, `ENABLE_JIT=OFF`, `ENABLE_DFG_JIT=OFF`,
  `ENABLE_FTL_JIT=OFF`
- `ENABLE_WEBASSEMBLY=OFF` (+ BBQ/OMG tiers), `ENABLE_SAMPLING_PROFILER=OFF`
  — both hard-conflict with the CLoop
  (`Source/cmake/WebKitFeatures.cmake:312-315`)

Per THREAD.md, each JIT tier is independently verifiable via `--jitTier`
caps in normal builds; this build is the tier-0 ground truth those runs are
compared against.

## How the configuration works

- `-DENABLE_SANITIZERS=thread` is the in-tree mechanism:
  `Source/cmake/WebKitCompilerFlags.cmake:436-438` adds `-fsanitize=thread`
  to all compile and link flags (clang/gcc only, not MSVC).
- `TSAN_ENABLED` is auto-detected from the sanitizer flag in
  `Source/WTF/wtf/Compiler.h` (`__SANITIZE_THREAD__` /
  `__has_feature(thread_sanitizer)`); no extra define is needed, and
  `SUPPRESS_TSAN` becomes available in C++.
- bmalloc self-disables under TSAN (`Source/bmalloc/bmalloc/BPlatform.h:387-390`
  sets `BUSE_SYSTEM_MALLOC` when `BTSAN_ENABLED`); the script additionally
  passes `-DUSE_SYSTEM_MALLOC=ON` so WTF FastMalloc also routes through the
  system allocator, which TSAN's runtime intercepts and understands.
- `CMAKE_BUILD_TYPE=RelWithDebInfo` plus `-g -fno-omit-frame-pointer`:
  TSAN's 5-15x slowdown makes Debug builds impractical, and frame pointers +
  debug info keep race reports readable.

## Building

```bash
bash tsan.sh                  # configure + build jsc into WebKitBuild/TSan
bash tsan.sh --configure-only # just (re)configure
```

Output is `WebKitBuild/TSan/bin/jsc`. The directory is distinct from
`WebKitBuild/{Debug,Release,ReleaseLTO}` used by `build.ts`, so the
configurations never poison each other's CMake caches.

Requirements: clang (the script prefers `clang-21`, falling back to `clang`,
same as `build.ts`), cmake, ninja. `ccache` is used automatically if present.

## Running

Always pass the suppressions file and keep enough history for second stacks:

```bash
TSAN_OPTIONS="suppressions=$PWD/Tools/tsan/suppressions.txt history_size=7 second_deadlock_stack=1" \
  WebKitBuild/TSan/bin/jsc test.js
```

Useful additions:

- `halt_on_error=1` — stop at the first report (good for bisection).
- `exitcode=66` — distinguish TSAN failures from normal test failures in
  harnesses.
- `log_path=/tmp/tsan` — write reports to `/tmp/tsan.<pid>` instead of stderr.
- `TSAN_OPTIONS="...:report_thread_leaks=0"` — quiets noise while Thread
  teardown is still being built out (remove once join paths are complete).

Running the threads corpus:

```bash
for t in JSTests/threads/*.js; do
  TSAN_OPTIONS="suppressions=$PWD/Tools/tsan/suppressions.txt halt_on_error=1" \
    WebKitBuild/TSan/bin/jsc "$t" || echo "TSAN/FAIL: $t"
done
```

## Suppressions policy

`Tools/tsan/suppressions.txt` starts empty and may only ever contain
known-benign, pre-existing races (e.g. intentionally racy profiling counters
that concurrent compiler threads already read in upstream JSC), each with a
one-line justification. Races in code touched by the threads workstreams —
object model, heap block handout, atom table, watchpoints, VM-lite state —
are bugs and must be fixed, never suppressed. Prefer `SUPPRESS_TSAN` (with a
comment) in source over a suppressions entry when an access is intentionally
racy by design, since it survives refactors and is visible in review.

## Interpreting reports

- A report names two stacks (write/read or write/write), the shadow memory
  state, and the threads involved. With `history_size=7` the second stack is
  almost always available; if it shows `[failed to restore the stack]`,
  re-run with a larger `history_size` (max 7) or `flush_memory_ms=2000`.
- Races inside `__tsan_*` or allocator frames usually mean a missing
  `-fsanitize=thread` on some object file — check that no target overrides
  `CMAKE_CXX_FLAGS`.
- TSAN understands pthread primitives, `std::atomic`, and WTF's `Atomics`
  (they lower to compiler atomics). It does NOT understand synchronization
  performed via custom futex paths unless annotated; if a legitimate
  happens-before edge is reported as a race, annotate the primitive
  (e.g. `WTF::Lock` already lowers to atomics and is understood).

## Known limitations

- No JIT means JIT-only race classes (IC publication, code patching,
  CodeBlock reclamation) are out of scope here; those are covered by the
  race amplifier runs on normal builds (`docs/threads/` specs).
- **Shared CLoopStack vs. parked threads (phase-1 GIL stub):** the CLoop
  keeps interpreter frames on the VM's single `CLoopStack`, shared by every
  JS thread under the GIL. A thread that parks (join, cond.wait,
  Atomics.wait, blocked lock.hold) leaves its CLoop frames in that shared
  stack; if another thread then runs and its interpreter SP walks below the
  parked thread's frames, they get clobbered, and the parked thread crashes
  on resume (observed: intermittent SEGV in `CLoop::execute` reloading
  `Callee[cfr]` after a host call returns — `smoke.js`,
  `condition-wait-notify.js`, roughly 1-in-3 under TSAN timing). These are
  NOT data races (no TSAN race report fires); they are the shared
  interpreter stack, which is CLoop-only — JIT/LLInt-asm builds use the
  per-thread native stack, and the whole corpus is stable there (see the
  Verify phase's debug-build stress runs). Goes away when the VM-lite
  workstream gives each thread its own CLoop stack. Until then treat
  multi-thread corpus runs under this build as best-effort: a clean run is
  meaningful for race detection; an intermittent `CLoop::execute` SEGV with
  this signature is this limitation, not a finding.
- TSAN and ASAN cannot be combined in one build; the existing
  `ENABLE_SANITIZERS=address` debug default in `build.ts` is replaced, not
  augmented, by this configuration.
- MSVC/Windows is unsupported (`WebKitCompilerFlags.cmake` only wires
  `thread` for non-MSVC); use Linux or macOS.
