# SPEC-heap-annex — FROZEN NORMATIVE ANNEX (rev 11)

Normative annex of `SPEC-heap.md`: the §13 manifest item-5 `runtime/VMManager.cpp` hunks,
moved verbatim for the size cap. Integrator writes these into `INTEGRATE-heap.md`/applies
them exactly as written. Read-only; the spec's §13.5 pointer is the index.

---

## §A5. `runtime/VMManager.cpp` hunks (manifest item 5)

5. **`runtime/VMManager.cpp`** - inert when callbacks null / GC bit never set:
   a. **Park hooks** in `notifyVMStop`: call `g_jscConfig.gcWillParkInStopTheWorld(vm)` if non-null (`[[unlikely]]`) after the counter-increment block (ends `:371`; no `m_worldLock`); same for `gcDidResumeFromStopTheWorld` after the final decrement block (`:525`). Impls (heap-owned, idempotent, no VMM lock held - L6): **willPark** - iff ISS∧GSP∧this VM's client holds access: RHA+set the client's `m_releasedByGCPark` (§10A); else no-op. **didResume** - iff `m_releasedByGCPark`: AHA (F8-blocking if a NEW stop pends) then clear; else no-op (F8 step 0 backstops).
   b. **Keep-parked** - new FIRST condition in `shouldStop()` (`:413-430`): `if (singleton().m_pendingStopRequestBits.loadRelaxed() & static_cast<StopRequestBits>(StopReason::GC)) return true;` - parked until `requestResumeAll(GC)`.
   c. **Latch exclusion**: `fetchTopPriorityStopReason` (`:391-399`) skips the GC bit; `case StopReason::GC: RELEASE_ASSERT_NOT_REACHED();` (`:462-463`) **stays**.
   d. Implement `setGCParkCallbacks` mirroring the debugger setters.
   e. **GC resume notify** - `requestResumeAllInternal` (`:305-316`): `reason == GC`=>after clearing the bit, ALWAYS (under `m_worldLock`) `m_worldConditionVariable.notifyAll()` - even with other bits pending (`:312-313`) or the RunOne early-return (`:328-329`); else (b)-parked VMs never wake.
   f. **Re-latch** - replace the pre-loop latch (`:404-407`)+`while (shouldStop()) m_worldConditionVariable.wait(m_worldLock);` (`:432-433`) with EXACTLY: `for (;;) { if (m_currentStopReason == StopReason::None) m_currentStopReason = fetchTopPriorityStopReason(); if (!shouldStop()) break; m_worldConditionVariable.wait(m_worldLock); }` (post-loop code unchanged; fetch precedes the FIRST `shouldStop()` AND re-runs after every wake).
   g. **GC re-check while parked** (VMs parked pre-GSP hold access; entry hook won't re-fire; §10C(a)/(c)/(d)): (i) wait loop (post-f), GC bit pending=>before each wait: drop `m_worldLock`, call `gcWillParkInStopTheWorld(vm)` (idempotent per (a)), re-take, re-evaluate; (ii) `requestStopAllInternal` (`:223-233`): `reason == GC`∧`m_worldMode >= Stopping`=>before the early return (lock held): `vm.requestStop()` on entered VMs (traps a RunOne targetVM)+`m_worldConditionVariable.notifyAll()`.
   No GC extension of `dispatchStopHandler`. **Coordination (jit M4, same file; jit's "disjoint" claim superseded):** jit R1.c edits the same `:391-460` region. Merge order (normative): heap b/c/f/g(i) first; then jit R1.c on the post-heap shape - conductor-pin (`m_targetVM = m_jsThreadsConductor`) where `m_currentStopReason` just latched `JSThreads` (wait loop, after f), all active VMs stopped; b's GC-bit check stays FIRST in `shouldStop()`. Resume tail: M4's fence, then (a)'s didResume; integrator re-checks both specs.
