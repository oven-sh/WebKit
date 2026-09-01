# LIMP MODE — log-and-survive recon build (worktree /root/WebKit-limp, branch jarred/threads @ 5c0e51c2b543)

Date: 2026-06-11. Purpose: RECONNAISSANCE ONLY — paper over the known GIL-off
crash mechanisms with log-and-survive guards so long campaigns produce
information instead of dying. NOTHING here is intended to land.

## Build

- Build dir: `/root/WebKit-limp/WebKitBuild/Limp` (Debug + ASAN, same cmake line
  as the main-tree Debug build per build.ts, clang-21/ld.lld-21/ninja/ccache).
- DELIBERATE deviation from the main Debug configure:
  `-DCMAKE_C_FLAGS=-fsanitize-recover=address -DCMAKE_CXX_FLAGS=-fsanitize-recover=address`
  so residual ASAN findings can be run with `ASAN_OPTIONS=halt_on_error=0`
  (full report logged, process continues) instead of aborting.
- Run env used everywhere: `ASAN_OPTIONS="halt_on_error=0:detect_leaks=0"`
  (`detect_leaks=0` because the libpas limp path leaks BY DESIGN; LSAN exit
  reports would be pure noise).
- Binary: `/root/WebKit-limp/WebKitBuild/Limp/bin/jsc`.

## Guards (grep for `LIMP` in the worktree)

All guards emit ONE dataLog/pas_log line with a stable tag + key values, then
take a bounded fallback. Tags:

1. `LIMP: varargs-scratch-race` —
   `Source/JavaScriptCore/llint/LLIntSlowPaths.cpp`, `varargsSetup`.
   Before copying arguments through the per-lite scratch pair
   (`newCallFrameReturnValue`/`varargsLength`), recompute the expected callee
   frame from THIS op's bytecode (`calleeFrameForVarargs(callFrame,
   -m_firstFree.offset(), length+1)`) and require exact equality plus
   `length <= maxArguments` plus non-null. Any mismatch (the W>=16 evidence-pack
   family: null/wild calleeFrame, foreign-thread stack pointer, bogus length)
   logs and bails to a thrown RangeError instead of memcpy'ing JSValues through
   a foreign stack pointer (the "wild copyToArguments dest" face is the same
   pointer one call deeper, so the guard covers it).
2. `LIMP: forward-args-length-race` — same function, forward-arguments arm:
   requires `length == callFrame->argumentCount()` before the frame memcpy.
3. `LIMP: varargs-constant-oob` — both `slow_path_size_frame_for_varargs`
   (`site=size_frame`) and `varargsSetup` (`site=varargsSetup`): any constant
   operand whose `toConstantIndex()` is outside
   `codeBlock->constants().size()` logs and bails to a RangeError instead of
   indexing `m_constantRegisters` OOB.
4. `LIMP: pas-dealloc-fail` — libpas deallocation failures converted to
   log + LEAK (skip the deallocation, return) at:
   - `pas_segregated_page_inlines.h` `pas_segregated_page_deallocate_with_page`
     check_deallocation path — the x86 `btrl+jc -> noreturn` asm replaced with a
     portable C check that logs and returns ("Alloc bit not set ...");
   - same function, terminal double-free check ("word already empty");
   - `pas_segregated_page_inlines.h` logging-mode check ("Page bit not set");
   - `pas_deallocate.c` large-heap miss ("Large heap did not find object") —
     unlock + report success;
   - `pas_try_shrink.h` ("Object not allocated") — keep old allocation.
   Helper: `pas_limp_deallocation_did_fail` in `pas_utils.{h,c}`.
5. `LIMP: stw-watchdog-timeout` —
   `Source/JavaScriptCore/bytecode/JSThreadsSafepoint.cpp`,
   `watchdogAssertStopProgress`. Added DURING verification: with 3 concurrent
   full-size W=16 ASAN runs on the (shared) 64-core box, one run tripped the
   30s STW watchdog RELEASE_ASSERT with a nil Class-A context + "OM transition
   stop" pending (overload latency, not a wedge — the same binary completes
   W=16 alone). Limp conversion: keep the full diagnostic participant dump
   (information preserved), rate-limit it to once per 30s per thread, and KEEP
   WAITING instead of aborting (both the registry-lock-unavailable arm and the
   end-of-dump arm). A real wedge now presents as repeated
   `LIMP: stw-watchdog-timeout` dumps + a run that never completes.
6. markListSet serialization (no tag — structural, not an error event) —
   `runtime/MarkedVector.{h,cpp}` + new static
   `MarkedVectorBase::s_limpMarkListSetLock`. Found DURING verification, full
   W=16 run1: SEGV (zero-page read, freed-table iteration, rdx=0xf5f5...) in
   `HashTable::removeIterator`/`add` on `vm.heap.markListSet()` reached from
   `MarkedVector::fill` <- `sortImpl` <- `arrayProtoFuncSort` on spawned
   Thread T5. Under `--useSharedGCHeap=1` GIL-off the per-Heap
   `m_markListSet` is ONE shared HashSet mutated by every Thread's
   MarkedVector/MarkedArgumentBuffer spill path with no synchronization.
   Limp conversion: one global Lock around every add/remove (addMarkSet x2,
   removeFromMarkSetAndDeallocateBuffer, adopt, fill, fillWith) and around the
   GC's markLists iteration. This is a REAL GIL-off bug worth a proper
   per-heap-sharded fix on the main branch; the evidence line is this section.

## NOT papered (deliberately left crashing — unboundable mid-operation bail)

- The bitfit family in `pas_bitfit_page_inlines.h` / `pas_bitfit_page.c`
  ("attempt to free bitfit page header", "attempt to shrink to a larger size",
  `pas_bitfit_page_deallocation_did_fail`): failure is detected deep inside the
  free/end bitvector walk with the ownership lock held and partial bit-state
  derivations in flight; a safe leak-and-return needs restructuring of the
  whole `_impl` switch. Evidence pack points at the segregated path for the
  observed 133s, so this is accepted partial-limp coverage.
- `loadVarargs` itself has no dest-bounds guard: DFG `operationVarargs` and
  checkpoint handlers legitimately pass non-stack (heap scratch / MarkedArgumentBuffer)
  dests, so a stack-bounds check there cannot discriminate. The LLInt-side
  equality guard (tag 1) covers the only writer that consumes the racy scratch
  slots.

## Verification (pinned GIL-off flags, W=16)

Flags: `--useJSThreads=1 --useThreadGIL=0 --useVMLite=1
--useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1`

- smoke (`-- 16 smoke`) x4 total (1 pre-gate + 3 banked while the box was
  killing long processes): 4/4 exit 0, identical correct checksums
  (checksumA `f9f349a1fc92a0d0`), 0 LIMP lines.
- full (`-- 16`) x3 SEQUENTIAL: **3/3 exit 0**, ~2.2-2.5 h wall each (ASAN
  debug), checksums IDENTICAL across all three runs
  (checksumA `b3e65a6855b9bdeb`, postings 4158957, checksumA2
  `39c33392b2a4c5b2`, checksumB `c4bdd580f85ee058`, checksumC
  `af028188d7a56a96`).
  - run1: 0 LIMP lines.
  - run2: 0 LIMP lines.
  - run3: 3x `LIMP: stw-watchdog-timeout` (elapsed 30/60/90s on one stop
    request, then the stop converged and the run COMPLETED with correct
    checksums) — exactly the log-and-survive behavior the guard was built
    for; under the old fail-stop this run would have been a SIGTRAP.
- minimal varargs repro (`bughunt/repro.js`, no-JIT, W=8, 30k iters): exit 0,
  0 mismatches, 0 LIMP lines (the U-T1 per-lite scratch routing already in
  this tree removed the root race; the varargs guards remain as tripwires).

## Verification-time incidents (campaign infrastructure, not engine)

- 3 CONCURRENT full W=16 ASAN runs overload the box and trip the (formerly
  fatal) STW watchdog — run sequentially.
- An external process on this shared box SIGTERMs long-running jsc processes
  (two campaign attempts killed mid-run, exit 143, plus one campaign shell);
  the final campaign retried on 143/137 and got through.
- /tmp (124G tmpfs) filled to 0 during verification: ANOTHER workflow was
  crash-looping and dumping 14 GB cores into /tmp/cores. Freed by deleting
  cores older than ~3 h (kept the 2 freshest).

## Bottom line

Limp gate GREEN: full-size `bench.js -- 16` completes 3/3 under the pinned
GIL-off flags, deterministic correct output, no crashes. Coverage is partial
by design: bitfit libpas faces left crashing (documented above). The
markListSet race (guard 6) is a REAL engine bug found by this build and is
the first thing the main branch should fix properly.
