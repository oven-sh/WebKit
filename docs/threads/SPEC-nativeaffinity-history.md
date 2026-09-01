# SPEC-nativeaffinity — history + BINDING annexes

## PART A / PART B boundary (read this first)

Same structure as the SPEC-objectmodel-history family:

**PART A — BINDING NORMATIVE ANNEXES.** Full text of clauses the spec
body indexes (size-cap overflow). Implementers treat PART A exactly
like the spec; the spec body's index entries cite these by id.

**PART B — non-normative audit trail.** Revision log and review
rounds; new rounds append here ONLY.

---

# PART A — BINDING annexes

## ANNEX NL1 — NativeSerialLock protocol (BINDING; spec §3.2 index)
## (REWRITTEN rev 2 — round-1 findings R1.2/R1.8/R1.9; rev-1 text
## preserved in PART B round 1 for the record)

State: one per VM (the single `m_gilOff` VM, UNGIL-HANDOUT U0b/U0c).

    struct NativeSerialLock {
        // ONE word, WTF LockAlgorithm shape (R1.8): bits 0-15 =
        // owning lite's TID (0 = free), bit 16 = hasParkedBit.
        // Owner TIDs are nonzero for EVERY NL-eligible lite:
        // spawned lites carry TM-allocated TIDs (VMLite.h:211) and
        // GIL-off CARRIER lites carry TM-allocated nonzero carrier
        // TIDs (JSLock.cpp:350 carrier->tid = allocateCarrierTID();
        // :522 RELEASE_ASSERT(lite->tid) — tid 0 never installed
        // GIL-off). No carrier-special encoding is needed.
        Atomic<uint32_t> m_word;        // ALL transitions seq_cst
    };
    static constexpr uint32_t hasParkedBit = 1u << 16;
    // per-lite (L2 append, beside nativeLockEligible):
    //   uint32_t m_nativeLockDepth; // owned-thread-only writes

acquire(lite):

    1. if lite->m_nativeLockDepth { ++depth; return; }   // reentrant
    2. loop:
       a. STOP POLL FIRST (spec §3.2.2 / SPEC-ungil §A.3.2-2b,
          SPEC-ungil.md:216-218), SPLIT BY TRAP CLASS (rev 6, R5.6
          — rev 5's predicate lumped "stop/trap"; a sticky
          termination trap has no resume and never clears until the
          thread's OWN §E.2 close, so the rev-5 (a)->park->(a) loop
          never reached the CAS: a terminated NL waiter hung or
          quantum-livelocked forever, never completing the
          acquisition NA-I12 mandates — join() (SD8) and ~VM
          (EXIT1.9 teardown rule below) wedged. Rev-5 step-(a) text
          of record in PART B round 5):
          - STOP-CLASS (a stop actually IN PROGRESS, validated
            against the stop word per SPEC-ungil §A.3.2b): park on
            the lite's OWN NVS ticket per the standard §J.3 park
            protocol (tokens KEPT; heap access kept at §A.3
            JSThreads stops, but a rule-8 GC stop's NVS park
            performs the F8 MANDATORY revert + gated
            re-acquisition, SPEC-ungil.md:289-298 — legal with NL
            held, spec NA-I13 rev-3 exemption; rev-2's "heap access
            KEPT, as at every §A.3 park" was wrong for GC stops,
            R2.6); on wake, run the §A.3.2b post-wake poll BEFORE
            continuing.
          - TERMINATION-ONLY (SPEC-ungil §A.2.4/TERM1: VM-wide,
            STICKY, no conductor, no resume; delivery at parks is
            D9 10ms-quantum POLLING, SPEC-ungil.md rule 6): do NOT
            park and do NOT abort — proceed to (b)/(c) and COMPLETE
            the acquisition (NA-I12). The termination is delivered
            by the existing post-call exception check at the §4
            bracket / the next JS-level poll after the native frame
            unwinds; acquire() itself is not a JS-level delivery
            point and MUST NOT become one (throwing out of acquire
            would bypass the depth/word bookkeeping).
       b. w = m_word.load(); if (w & ownerMask) == 0 and
          m_word.compareExchangeWeak(w, lite->tid | (w & hasParkedBit)):
          CAS WON. Re-poll the stop bit (it may have been set since
          (a)); if a STOP-CLASS trap is set, park on the NVS ticket
          WHILE HOLDING NL — explicitly legal, no conductor ever
          acquires NL (spec NA-I10) — and run the §A.3.2b post-wake
          poll before proceeding; a TERMINATION-ONLY trap never
          parks here either (rev 6, same split as (a)). Then
          lite->m_nativeLockDepth = 1; return.
          INVARIANT (R1.2): acquire() NEVER returns without a stop
          poll after its final park of either kind; a waiter woken
          by the holder's mid-stop-window release cannot run the
          native body inside the window.
       c. CAS FAILED (held): ensure hasParkedBit is set
          (compareExchangeWeak w -> w | hasParkedBit; if the word
          went free meanwhile, goto a); then
          ParkingLot::parkConditionally(&m_word,
              validation = [w] { return m_word.load() ==
                  (w | hasParkedBit); },   // still held + flagged
              beforeSleep = nop,
              deadline = now + quantum).
          NOTE (R1.9): ParkingLot::compareAndPark takes NO timeout —
          it hard-codes Time::infinity() (wtf/ParkingLot.h:91-102);
          parkConditionally with an explicit deadline is the ONLY
          primitive that supports the bounded quantum, and the
          quantum is LOAD-BEARING (below).
       d. on wake — unpark OR deadline — goto a. (The stop poll in
          (a) therefore runs after EVERY wake, before any CAS.)

release(lite):   (REWRITTEN rev 3, R2.3 — the rev-2 exchange(0) +
                  plain unparkOne lost hasParkedBit for every waiter
                  beyond the first: the woken waiter's acquire CAS
                  re-installed `tid | (w & hasParkedBit)` over w==0,
                  so remaining waiters slept to the deadline — a
                  10ms-stepped convoy falsifying NA-T4's
                  unpark-bounded handoff arm. Rev-2 text of record
                  in PART B round 2.)

    1. ASSERT((m_word & ownerMask) == lite->tid
              && lite->m_nativeLockDepth);
    2. if --lite->m_nativeLockDepth return;
    3. w = m_word.load();
       if !(w & hasParkedBit) and m_word.compareExchangeWeak(w, 0):
           return;                       // fast path: no waiters
       // Slow path — the LockAlgorithm::unlockSlow handoff shape
       // (wtf/LockAlgorithmInlines.h:207-241: unparkOne WITH
       // callback; the callback runs holding the ParkingLot bucket
       // lock, which is what orders the bit republication against
       // concurrent step-(c) parks):
       ParkingLot::unparkOne(&m_word, [&](UnparkResult result) {
           m_word.store(result.mayHaveMoreThreads
                            ? hasParkedBit : 0);   // seq_cst
           return 0; // token unused
       });
       // The plain unparkOne(address) form DISCARDS
       // UnparkResult::mayHaveMoreThreads (wtf/ParkingLot.h:104-119;
       // the :130-135 comment notes WTF::Lock uses the callback
       // form for exactly this) and MUST NOT be used here.

Memory order / lost-wakeup (R1.8): rev 1 kept a SEPARATE relaxed
m_waiters count; on weakly-ordered hardware (ARM64, a shipping Bun
target) the releaser's relaxed load had no happens-before edge to a
waiter's relaxed increment, so a release could legitimately read a
stale 0 and skip unparkOne — a lost wakeup costing up to a full
quantum per miss. The parked indication now lives IN the owner word
and every transition (acquire CAS, parked-bit CAS, release exchange)
is a seq_cst RMW on m_word; ParkingLot's bucket lock orders the
parked-bit set + validation against the releaser's exchange (the
WTF::Lock hasParkedBit protocol — wtf/LockAlgorithm.h shape; the
rev-1 objection to WTF::Lock was ONLY its non-polling park, which
step (a) replaces). The m_word RMW chain is the synchronization edge
between successive critical sections; LIVENESS additionally depends
on (i) the parked bit sharing the released word and (ii) the bounded
deadline existing.

Quantum (R1.9, normative): the park deadline is bounded and MUST be
strictly less than the §A.3 stop-the-world watchdog interval
(`stopTheWorldWatchdogTimeout` = 30s,
bytecode/JSThreadsSafepoint.cpp:379) by a comfortable margin;
default 10ms. It is NOT a tunable whose value is correctness-free:
it bounds (1) lost-progress recovery if an unpark is missed for any
residual reason, and (2) the conductor-visibility window below.

Conductor-side (R1.9 — honest restatement; rev 1 claimed a
(c)-parked waiter "counts as parked at a poll site", but no §A.3.2
conductor mechanism samples foreign ParkingLot buckets): NL appears
in NO conductor code path (spec NA-I10). A waiter parked in (c) is
parked on the NL bucket, NOT on its NVS ticket, and is therefore
CONDUCTOR-INVISIBLE FOR AT MOST ONE QUANTUM: if the NL holder is
itself safepoint-stopped while holding NL (permitted, NA-I10/§3.2.3)
no release ever arrives, the deadline fires, the waiter re-runs (a),
observes the stop bit, and parks compliantly on its NVS ticket.
Worst-case added stop latency per NL waiter = one quantum +
scheduling; with the 10ms default this is invisible against the 30s
watchdog (NA-T4 bounds it).

REV 5 (R4.7) — the two holder shapes this paragraph previously left
unanalyzed (it covered only holder-is-STOPPED):
HOLDER-AS-CONDUCTOR: a Locked native that fires a Class-4
invalidation (haveABadTime, SPEC-ungil §K.5 rule 5) or a synchronous
collection becomes the §A.3/GC conductor WITH NL held — legal per
spec §3.5's rev-5 conductor-HOLD clause (BL1.6); it never ACQUIRES
NL (NA-I10). Liveness: its NL waiters are by definition not
entered-and-running JS — each is either already NVS-parked (step
(a)) or parked on the NL bucket, where the bounded deadline fires
within one quantum, the stop bit is observed at (a), and the waiter
NVS-parks; the conductor's fan-out therefore completes, worst case
+one quantum per waiter, same bound as the stopped-holder case.
HOLDER-AS-LOSER: an NL holder that loses §A.3 arbitration parks on
the §LK.4b pending-job-slot mutex ACCESS-RELEASED for the winner's
whole stop window, WITH NL held. Same waiter math (its NL waiters
NVS-park within one quantum and satisfy the winner's stop); the NL
release arrives after the loser's wake + retry. Both shapes are
pinned by NA-T4's rev-5 conductor-holds-NL arm.

Teardown: ~VM blocks until VM-empty (SPEC-ungil EXIT1.9); an empty VM
has no entered lites, hence m_word == 0 — destructor asserts it. A
thread exiting (EXIT1) with m_nativeLockDepth != 0 is a structural
bug: close (§E.2) asserts depth == 0 (drop scopes are stack-strict,
ANNEX EX1).

## ANNEX EX1 — re-entry drop scope (BINDING; spec §3.3 index)
## (CTOR FORMS REWRITTEN rev 5 — R4.5; rev-4 single-signature text
## of record in PART B round 4)
## ([r10] AMENDED — the NA-I12 verification anchors trimmed from
## spec §3.3 at rev 9 are restored by the ANNEX EX1 AMENDMENT
## (r10) record at the end of this file; they read as part of
## this annex.)

    class NativeLockDropScope {  // also the public embedder type
    public:
        // DEFAULT form — EX1 sites 1-7. REV 5 (R4.5): rev 4's sole
        // signature `NativeLockDropScope(VMLite* lite)` forced
        // every call site to materialize the lite argument BEFORE
        // the ctor body's level-0 gate ran (C++ evaluates argument
        // expressions eagerly), and the only current-lite source on
        // these common paths is the TLS slot t_currentVMLite
        // (runtime/VMLite.cpp:67; L4 accessor block VMLite.h:
        // 333-345) — i.e. a TLS load per JS entry in flag-off/
        // GIL-on processes, contradicting the NA-I1 restatement
        // R3.11 itself wrote ("lite/depth loads never execute").
        // The current-lite resolve now happens INSIDE the ctor,
        // strictly after the level-0 gate.
        ALWAYS_INLINE NativeLockDropScope()
            : m_lite(nullptr), m_savedDepth(0)
        {
            // REV 4 (R3.11): level-0 gate FIRST. The call sites
            // (items 1-6 below) are COMMON-PATH C++ — they execute
            // identically in every configuration. Flag-off/GIL-on
            // processes pay exactly one predictable global-byte
            // branch here; the TLS, lite and depth loads below
            // never execute (NA-I1 as restated rev 5).
            if (!g_jscConfig.gilOffProcess) [[likely]] return;
            // gilOff process only: resolve the current lite via the
            // frozen L4 accessor (VMLite::currentIfExists(), backed
            // by thread_local t_currentVMLite, VMLite.cpp:67).
            initSlow(VMLite::currentIfExists());
        }
        // EXPLICIT-LITE form — EX1 site 8 ONLY (spec NA-I21/BL1.2):
        // the §F.5 nested-entry funnel is keyed on the THREAD's
        // gilOff-VM (VM A) lite, NOT the entered VM's, and at its
        // F8-revert point t_currentVMLite is being swapped to VM
        // B's lite — so the funnel passes the pre-swap lite it
        // already holds (the §6.4.4 VMLite::setCurrent return
        // value, retained in the §F.5 LIFO restore tuple). Same
        // body minus the TLS resolve; level-0 gate still first.
        ALWAYS_INLINE explicit NativeLockDropScope(VMLite* gilOffVMLite)
            : m_lite(nullptr), m_savedDepth(0)
        {
            if (!g_jscConfig.gilOffProcess) [[likely]] return;
            initSlow(gilOffVMLite);
        }
    private:
        ALWAYS_INLINE void initSlow(VMLite* lite)
        {
            // dead-cheap when ineligible (GIL-on VMs' lites
            // coexisting in a gilOff process; rev 2: carriers of
            // the gilOff VM ARE eligible per revised NA-I9): one
            // byte test on already-resident lite state; depth==0
            // (the overwhelmingly common case) costs one more load.
            if (!lite || !lite->nativeLockEligible) [[likely]] return;
            m_lite = lite;
            if (uint32_t d = lite->m_nativeLockDepth) {
                m_savedDepth = d;
                lite->m_nativeLockDepth = 1;     // collapse
                nativeSerialLock(lite).release(lite); // to zero
            }
        }
    public:
        ~NativeLockDropScope()
        {
            if (!m_savedDepth) [[likely]] return;
            // Park-capable reacquire (ANNEX NL1 loop). Runs on EVERY
            // exit path, including exception-pending (spec NA-I12):
            // JSC host exceptions propagate by return value +
            // per-lite m_exception word, never by C++ unwinding
            // through host frames, so this destructor is reached on
            // the throw path too; it MUST NOT inspect or clear the
            // pending exception.
            nativeSerialLock(*m_lite).acquire(*m_lite);
            m_lite->m_nativeLockDepth = m_savedDepth;
        }
    private:
        VMLite* m_lite;
        uint32_t m_savedDepth;
    };

Mandatory instantiation sites (spec NA-I11 funnel; REVISED rev 3,
round-2 finding R2.2 — the rule is CALLEE-defined over the WHOLE
`vmEntryToJavaScript*` family, NORMATIVELY the prefix-matched
symbol set of the llint/LLIntThunks.h:39-52 extern declaration
block (bare + With0..With6Arguments today); rev 2's enumeration of
two exact symbols left With1..6 callers outside the funnel — the
R1.3 bug shape one level down. Lint-enforced by NA-T7 with a
GENERATED symbol list; the list below is the known caller set at
this rev, NOT the definition):
1. `Interpreter::executeCall` / `executeConstruct` / `executeEval` /
   `executeProgram` JS arms (the `vmEntryToJavaScript` callers
   adjacent to interpreter/Interpreter.cpp:1319/:1409's native arms).
2. `Interpreter::executeCachedCall`
   (interpreter/InterpreterInlines.h:100; direct vmEntryToJavaScript
   at :127). MISSED BY rev 1: CachedCall is the re-entry vehicle of
   exactly the natives that ship Locked.
3. `Interpreter::tryCallWithArguments` (InterpreterInlines.h:132-171,
   With0..With6) — ONE scope here covers all seven arity arms.
   MISSED BY rev 2 (it cited only the With0 arm at :158) and
   maximally load-bearing: `CachedCall::callWithArguments`
   (interpreter/CachedCallInlines.h:38-66) routes through it on
   CPU(ARM64)/CPU(X86_64) whenever argumentCountIncludingThis <= 7,
   i.e. the COMMON case on both shipping targets — Array.prototype.
   sort comparators enter via `cachedCall.callWithArguments(
   globalObject, jsUndefined(), left, right)`
   (runtime/ArrayPrototype.cpp:955 -> With2Arguments), NOT via
   executeCachedCall; the TypedArray forEach/map/filter/reduce/sort
   callback family and String.prototype.replace replacers ride the
   same symbols.
4. `MicrotaskCall::tryCallWithArguments`
   (interpreter/MicrotaskCallInlines.h:40-86, With0..With6) and the
   runJSMicrotask direct arms (runtime/JSMicrotask.cpp:159-171
   With0..6 + :198 bare). Also missed by rev 2's two-symbol list.
5. `Interpreter::executeModuleProgram` (interpreter/
   Interpreter.cpp:1662; entry at :1728) — reached from the
   moduleLoaderEvaluate host function and embedder (Bun) natives
   that evaluate modules. Missed by rev 1.
6. The per-lite microtask drain loop entry (SPEC-ungil §E.1/I11) —
   defensive; a native should never pump microtasks while holding NL,
   but the drop scope makes it correct anyway (item 4 covers the
   per-call entries regardless).
7. Embedder/manual: around blocking regions in host bodies (spec
   NA-I13).
8. The §F.5 nested foreign-VM entry funnel, at its F8
   mandatory-revert point, keyed on the THREAD's gilOff-VM lite —
   spec NA-I21 (rev 3, R2.1): inside the nested VM every §3.3 caller
   passes VM B's lites (nativeLockEligible 0 for a GIL-on B, and in
   any case a DIFFERENT lock), so no inner-VM scope can release VM
   A's NL; this is the one callee-defined site keyed on a lite OTHER
   than the entered VM's.
9. THE SECOND CALLEE-DEFINED FAMILY (NEW rev 6, R5.1 — spec
   NA-I26; FULL walk = ANNEX SC2.1): every caller of `vmEntryToWasm`
   (the inline wrapper, llint/LLIntThunks.h:72). The
   `vmEntryToJavaScript*` family closes ONLY the C++->JS boundary;
   wasm is a second native-code-to-JS channel — wasm calls its JS
   imports through the JIT-emitted wasmToJS stub via
   `CallLinkInfo::emitDataICFastPath` (wasm/js/WasmToJS.cpp:350),
   machine code with no vmEntry symbol, structurally invisible to
   the §3.3 funnel and to every NA-T7 token family. Known caller
   set at this rev (the TWELFTH NA-T7 token family DEFINES it):
   `callWebAssemblyFunction` (wasm/js/WebAssemblyFunction.cpp:94 —
   the dominant carrier JS->wasm entry under useJSThreads, per the
   tree's own :70-72 comment: warm IC disabled, this is "the single
   cold JS->wasm entry"; the executable is forced Locked per §2.1,
   so the §4 bracket holds NL on entry — the scope releases it for
   the WHOLE wasm activation and every wasm->JS import inside it),
   `Interpreter::executeCallImpl`'s wasm arm
   (interpreter/Interpreter.cpp:1316 — MISSED by rev 5's §4.5
   enumeration entirely), and the runJSMicrotask wasm arm
   (runtime/JSMicrotask.cpp:203 — rev 5 exempt-cited it as ":204"
   with the rationale "carrier JS-to-Wasm is not a host-native
   call", which was true of that arm but concealed that the
   DOMINANT carrier JS->wasm path IS a host-native call; exempt-cite
   SUPERSEDED, the arm now instantiates the scope — depth 0 there
   in practice, zero work). Default ctor form at all three.

Strictness: scopes are stack-nested (LIFO) by construction (C++
automatic storage); a scope NEVER outlives its native frame. The
collapse-to-1-then-release in the ctor (rather than looping release)
keeps the NL1 owner-word traffic at exactly one CAS per drop
regardless of depth.

Interaction with §A.3 stops: the destructor's reacquire is an NL1
acquire and therefore a compliant park site; a stop arriving between
release (ctor) and reacquire (dtor) sees this thread parked inside JS
machinery as usual — no NL-specific conductor logic exists. rev 2:
the reacquire inherits the rewritten NL1 loop verbatim, including
the R1.2 post-wake/post-CAS stop-poll-before-return invariant (the
rev-1 wake-mid-stop hole applied to this reacquire too).

Interaction with termination (SPEC-ungil §A.2.4, VM-wide v1;
REWRITTEN rev 6, R5.6 — the rev-5 sentence "parks/polls per NL1
step (b)" routed a sticky, resume-less trap into a park with no
completion path, contradicting its own "COMPLETES the reacquire";
text of record in PART B round 5): a termination trap observed
during the dtor's reacquire POLLS and proceeds per the NL1 rev-6
TERMINATION-ONLY arm — it parks ONLY if a stop is concurrently in
progress (stop-class arm) — and COMPLETES the reacquire (spec
NA-I12); the exception then unwinds the native frames above, each
releasing through the §4 brackets to depth 0 before the thread
reaches its §E.2 close (which asserts depth 0, NL1 teardown rule).

## ANNEX PT1 — seed policy table (BINDING; spec §2.2 index)

Grouping = the unit a §5 audit row covers. "Registration" = where
the ConcurrentOk marker lands — one of THREE mechanisms (spec §2.1
as amended rev 6, R5.5; rev 5's two-mechanism header was wrong for
most of this table's own rows): (i) the getHostFunction/
hostFunctionStub parameter (direct registrations), (ii) the static
.lut.h grammar marker, (iii) the JSC_NATIVE_* macro layer's
defaulted NativeConcurrency argument threaded through
putDirectNativeFunction(WithoutTransition) + JSFunction::create
into getHostFunction (FULL plumbing = ANNEX SC2.5) — the mechanism
PT1.A/PT1.B/PT1.C use via the JSC_NATIVE_* macros
(ObjectPrototype.cpp:65-66, ArrayPrototype.cpp:107-124) and
PT1.D/PT1.F use via bare putDirectNativeFunctionWithoutTransition
calls (MathObject.cpp:93ff, AtomicsObject.cpp:94); PT1.E
parse/stringify = (ii) (JSONObject.lut.h); PT1.G = (i)/(iii) at
the SPEC-api natives' registration sites. Every group below still
requires its NA1 rows executed
before useJSThreads ships default-on (spec §5.2); this table is the
APPROVED CANDIDATE LIST, not a waiver of evidence.

PT1.A object/property hot core (runtime/ObjectPrototype.cpp,
runtime/ObjectConstructor.cpp):
  hasOwnProperty, propertyIsEnumerable, isPrototypeOf,
  Object.getPrototypeOf, Object.keys/values/entries fast paths,
  Object.is, Object.create(null|proto) fast path.
  Rationale: bodies are Structure/butterfly walks ruled by
  SPEC-objectmodel; enumeration caches are §K-ruled (K4 rows).

PT1.B array hot core (runtime/ArrayPrototype.cpp):
  push, pop, shift fast paths, indexOf/lastIndexOf/includes, join
  fast path (rope-safe), slice, fill, at, isArray.
  EXCLUDED from seed: sort (comparator JS re-entry + scratch
  buffers), species-creation-heavy paths (concat/splice/flat) until
  their NA1 rows separately argue the species lookup caches.

PT1.C string hot core (runtime/StringPrototype.cpp):
  charCodeAt/codePointAt/charAt/at, indexOf/includes/startsWith/
  endsWith, slice/substring, toLowerCase/toUpperCase ASCII-only
  paths, fromCharCode.
  EXCLUDED: every locale-sensitive variant (NA-I7), replace/match
  family (RegExp side ruled separately via K4 m_regExpGlobalData
  per-lite row AUD1.K2/SD19 — replace/match join the seed ONLY once
  their NA1 rows cite that machinery end-to-end).

PT1.D Math (runtime/MathObject.cpp): all of Math.*; Math.random's
  m_weakRandom is per-lite per K4 AUD1.K4/VIII.10.

PT1.E JSON (runtime/JSONObject.cpp): parse, stringify. Stringify's
  toJSON/replacer JS re-entry is governed by the core machinery
  (concurrent-ok natives may re-enter JS freely; the §3.3 drop rule
  is a LOCKED-path concern).

PT1.F Atomics (runtime/AtomicsObject.cpp): all of Atomics.*; the
  waiter list is process-global and already lock-disciplined.

PT1.G threads API (NEW rev 3, R2.5 — spec NA-I22): the SPEC-api
  Thread/Lock/Condition/ThreadLocal natives, INCLUDING the blocking
  gate set G11 (SPEC-api.md:15: join(), lock.hold(), cond.wait())
  and lock.asyncHold/cond.asyncWait/notify/spawn/postMessage.
  Rationale: their bodies are exactly the lock-disciplined NLS/NCS/
  TS state machine SPEC-api §5.9 (SPEC-api.md:260-275) already
  audited rank-by-rank; if they shipped Locked (the NA-I6 default),
  (a) a joiner would hold NL across an indefinite block while the
  joined thread's Locked natives need NL — mutator deadlock; (b)
  every contended hold/wait would block on NLS::m_lock holding NL —
  the §3.5 forbidden NL > NLS edge; (c) their
  release-access-before-blocking discipline (api 5.9(a1-a3)/(e))
  would trip the NA-I13 assert. NA1 rows (group G) still required
  before useJSThreads default-on, like every seed group.

LOCKED FOR EMPHASIS (not exhaustive — everything unlisted is locked
by NA-I6; rev 3: the threads-API natives are NOT in this bucket —
they are PT1.G above, per NA-I22): ALL Intl* (NA-I7); Date
locale/timezone paths (tz cache);
Function constructor / eval / indirect eval (parser+codegen world);
RegExp compile-heavy paths; console/inspector/debugger natives
(main-only family, SD13/SD14); $vm / test natives; Error.captureStackTrace
and stack-trace materialization; Proxy trap helpers; WeakRef/
FinalizationRegistry natives; Wasm natives (spawned-thread Wasm is
refused v1 anyway, SPEC-ungil §I).

## ANNEX BL1 — blocking, nesting, and the NL liveness scope
## (BINDING; spec §3.4/§3.5 index; NEW rev 3, R2.1/R2.4/R2.5/R2.6)

FULL text behind the spec body's compressed §3.4/§3.5 clauses.

BL1.1 NA-I13 assert scope. The VOLUNTARY access-transition paths
(§A.3.4-gated fresh acquisitions; explicit native-side
release/re-acquire) debug-assert `m_nativeLockDepth == 0`. EXEMPT:
the §J.3 park-site MANDATORY reverts — at a rule-8 GC stop
(SPEC-ungil.md:289-298) the NVS park performs the F8 revert
(§13.5a willPark / per-client m_releasedByGCPark) and the gated
re-acquisition, legally WITH NL held: the GC conductor never
acquires NL (NA-I10), so no deadlock; rev 2's unqualified assert
fired on every GC stop that caught any thread inside a Locked
native body at a poll site (R2.6).

BL1.2 NA-I21 cross-VM nesting (R2.1). Topology licensed by
UNGIL-HANDOUT §F.5 (:2262-2290): a Bun host function in gilOff VM A
on a CARRIER (only — spawned nesting RELEASE_ASSERTs, SPEC-ungil
§F.6(e), SPEC-ungil.md:668-670) enters VM B mid-body; under NA-I6
that host function defaults Locked and holds NL at that point. The
§F.5 funnel mandates the F8 revert of A's access BEFORE installing
B's carrier (:2276-2277) — the exact transition NA-I13 polices —
and inside B every §3.3 drop-scope caller passes B's lites
(`nativeLockEligible` computed from VM B; 0 if B is GIL-on, and in
any case a DIFFERENT VM's lock), so A's NL would stay held across
the ENTIRE nested window: unbounded foreign-VM JS with every Locked
native of every VM-A thread serialized behind it, and a
constructible no-conductor deadlock (B's JS Atomics.waits on a
notify owed by a Locked VM-A native on a spawned thread, which
blocks on NL held by the nesting carrier — NA-I10 is irrelevant to
mutator-vs-mutator cycles, no watchdog fires). THEREFORE: the §F.5
funnel, at its F8-revert point, instantiates `NativeLockDropScope`
keyed on the THREAD's gilOff-VM (VM A) lite — the one
callee-defined drop site keyed on a lite other than the entered
VM's — releasing A's NL for the nested window, reacquiring at the
LIFO restore. Backstop: RELEASE_ASSERT (not debug) that the gilOff
lite's `m_nativeLockDepth == 0` inside the nested window. Gate
§9.6: SUPERSESSION-PENDING with the SPEC-ungil/UNGIL-HANDOUT owner
(§F.5/§F.6(e)/A36C; the §F.6 IU embedder-checklist
"JSContext-inside-host-call" row gains the NL-depth obligation).

BL1.3 NA-I22 engine blocking natives (R2.5). G11 (SPEC-api.md:15:
join(), lock.hold(), cond.wait()) and the rest of the
Lock/Condition/Thread/ThreadLocal native family are seeded
ConcurrentOk as PT1.G. If they shipped Locked (NA-I6 default): (a)
a joiner would hold NL across an indefinite block while the joined
thread — which completes only at §E.2 close (SPEC-ungil U7/SD1) —
needs NL for any Locked native on its exit path: total mutator
deadlock; (b) contended lock.hold would block on NLS::m_lock
holding NL — the BL1.4 forbidden edge; (c) all three release heap
access before blocking (SPEC-api.md:267-271, 5.9(a1-a3)/(e)),
firing the NA-I13 assert on every contended call in debug builds.
Demotion rule: a G11 native may only be Locked with internal
`NativeLockDropScope`s around every blocking region and every
NLS::m_lock acquisition.

BL1.4 The long-hold cycle (R2.4). Forward direction (legal,
structural): NLS::m_lock > NL — lock.hold runs user JS holding
NLS::m_lock (SPEC-ungil §LK long-hold row, SPEC-ungil.md:902-907);
that JS may call Locked natives (acquiring NL), and the ANNEX EX1
dtor reacquires NL inside fn's epilogue while NLS::m_lock is held.
Reverse direction (forbidden, was constructible in rev 2): a
Locked lock.hold body blocking on NLS::m_lock while holding NL.
Cycle witness: T1 in Locked lock.hold holds NL, blocks on
NLS::m_lock; T2 holds NLS::m_lock running fn's user JS, which
calls any Locked native and blocks on NL. No stop in progress —
§3.2 polls never fire; NA-I10 (conductor exclusion) is irrelevant.
Pinned LK.1c addition: NLS::m_lock OUTER to NL; negative edge "no
NL holder blocks on NLS::m_lock or any G11 blocking primitive"
(discharged by PT1.G); SPEC-api §5.9 companion row permitting the
EX1-dtor-inside-fn-epilogue NL reacquire (the legal direction) and
forbidding the reverse — all inside the §9.1 SUPERSESSION-PENDING
scope.

BL1.5 Liveness scope (supersedes rev 2 §3.4's unscoped release-build
"never deadlock — by NA-I10 nobody the conductor needs is behind
NL", which was sound ONLY for conductor liveness). CONDUCTOR
liveness: NA-I10. MUTATOR liveness: NA-I11 (no JS under NL) +
NA-I21 (no nested window under NL) + NA-I22 (no G11 block under
NL) + BL1.4's negative edge (no NLS::m_lock block under NL); each
violation has a constructible deadlock above.

BL1.6 Conductor-hold clause (NEW rev 5, R4.7 — blocker; the NLH1.4
hole one row over). [SCOPE NARROWED rev 8, R7.1 — the
SYNC-COLLECTION leg of this walk ("or a synchronous collection")
was derived against the landed single-window heap §10 conduct and
is SUPERSEDED by ANNEX BL1.8, recorded both sides with SPEC-congc
(CG-I19/CGD6.1/§13.5(4)): a sync-collection requester DROPS NL
(BL1.8), it does not hold it through the bracket. The
haveABadTime/§A.3 walk below STANDS, bounded per the congc CGS2.3
budget ledger.] [r9: the CGS2.3 budget this walk leans on is now
LANDED ungil-side (SPEC-ungil rev 33 §A.3 rule 5 WAIT BOUND;
STRUCTURAL via congc F45/§9.1(2a)/CG-I26) and the §13.5(4) gate
is closed; the bracket walk below otherwise stands unamended.] SPEC-ungil makes the CALLER of a Class-4
invalidation the §A.3 conductor: §K.5 rule 5
(SPEC-ungil.md:768-780) — JSGlobalObject::haveABadTime,
JS-reachable, "whole body under ONE §A.3 stop; conductor = caller"
— and the HBT4 order pin binds ALL §A.3 conductors to
release-access -> arbitration -> GCL, losers parking on the §LK.4b
slot mutex ACCESS-RELEASED (SPEC-ungil.md:240-247, :873-886). Any
un-audited Locked native (NA-I6 default — defineProperty/
setPrototypeOf paths, the $vm/gc() natives PT1 keeps Locked, sort
excluded from PT1.B) can reach haveABadTime or a synchronous
collection mid-body, i.e. WITH the §4 bracket's NL held. Three rev-4
defects composed: (1) NA-I13 forbade exactly this voluntary
transition and debug-asserted — its sole exemption was §J.3
park-site MANDATORY F8 reverts, which a conductor bracket / loser
park is not; a legal, SPEC-ungil-MANDATED path fired the assert
(the R2.6 bug class recurring). (2) The LK.1c row had no analog of
the clause the OTHER long-hold lock needed for precisely this shape
— SPEC-ungil.md:883-885 "a hold(fn) conductor (Class-A fire, §K.5,
OM stops) may HOLD NLS on entry, never ACQUIRES it" (NLH1.4);
NA-I10 addressed only ACQUISITION, and the §LK.4b held-with
enumeration did not license NL held with the slot, leaving the
NL > slot and NL > GCL edges outside both rows' both-sides scope.
(3) ANNEX NL1's conductor-side analysis treated only
holder-is-STOPPED. RESOLUTION: (a) LK.1c conductor-HOLD clause
(spec §3.5): an §A.3/GC conductor MAY hold NL on entry, never
acquires it; acyclicity walk for the new edges NL > §LK.4b-slot >
GCL — sound because slot and GCL holders never acquire NL (NA-I10's
negative edge), so no cycle through NL closes; the loser case (NL
held across an access-released slot park) is live per the NL1 rev-5
conductor paragraph (NL waiters deadline to NVS parks within one
quantum; fan-out completes; release after loser wake). The §LK.4b
held-with amendment ("long-hold NL EXCLUDED", NLS-style wording)
joins the §9.1 SUPERSESSION-PENDING scope — §9.1 as previously
written did not cover §LK.4b. (b) NA-I13's exemption extended to
the HBT4 conductor-bracket transitions (release-access,
arbitration, gated reacquire, loser slot park) when the thread is a
§K.5/heap-rule §A.3 conductor or arbitration loser; VOLUNTARY
native-side transitions remain forbidden and asserted. (c) NL1
conductor paragraph extended (holder-as-conductor, holder-as-loser);
NA-T4 gains the conductor-holds-NL arm: a Locked native triggers
haveABadTime (and a gc()-style sync collection) on T1 while T2
waits on NL and T3 runs JS — stop completes within the watchdog, no
NA-I13 assert, NL still held at T1's resume.

BL1.7 LK.1c row, FULL text (MOVED here rev 7 under the size cap —
content normative, carried VERBATIM from the rev-6 body §3.5, no
semantic change; spec §3.5 is the index).

Proposed insertion in the SPEC-ungil §LK merged process lock table
(SPEC-ungil.md:867-925), as row **LK.1c "NativeSerialLock"**:

- Position: inner to heap rank 1 (entry token/heap access — NL
  acquired only while entered, NA-I9, kept across §A.3 parks);
  OUTER to api ranks 1-3, heap ranks 2-10b, and all leaves (the
  Locked BODY is arbitrary host code).
- LONG-HOLD in the `NLS::m_lock` sense (SPEC-ungil.md:902-907):
  acyclicity by negative edge — NO conductor, heap 2-10b holder
  (range VERBATIM, rev 4 R3.7), or api 1-3 holder ever ACQUIRES NL
  (NA-I10). §E.2 exemption (rev 4, R3.8): NLS-style rank-4
  carve-out LIMITED to §J.3 park-site MANDATORY F8 reverts/
  re-acquisitions; VOLUNTARY transitions forbidden (NA-I13); NL
  NEVER held across parks that run user JS (NA-I11); U20 gains the
  matching exemption. Conductor-HOLD clause (rev 5, R4.7; FULL
  walk = ANNEX BL1.6, BINDING): an §A.3/GC conductor MAY hold NL
  on ENTRY, never ACQUIRES it; NL > §LK.4b-slot > GCL edges
  acyclic; loser case live (NL waiters deadline to NVS parks
  within one quantum, NL1). The §LK.4b held-with amendment
  ("long-hold NL excluded", NLS-style) joins the §9.1 scope.
  Rev 8 (R7.1): the clause covers §A.3 (single-window,
  haveABadTime-class) conductors/losers ONLY — a GC
  sync-collection conduct (SPEC-congc §3 multi-window tenure)
  is NOT licensed to hold NL: BL1.8 drop scope, congc CG-I19
  both sides; the row's wait-bound term cites congc CGS2.3.
- LONG-HOLD vs LONG-HOLD (rev 3, R2.4; cycle walk = BL1.4).
  Pinned: **NLS::m_lock OUTER to NL**. Negative edge: **no NL
  holder ever blocks on NLS::m_lock or any G11 blocking
  primitive** — discharged by NA-I22/PT1.G; a future Locked native
  touching NLS state must use the internal drop scope. Companion
  SPEC-api §5.9 row joins the §9.1 scope.
- BOTH-SIDES RULE: any edge move is a supersession recorded in
  BOTH this spec and SPEC-ungil §LK. Until the SPEC-ungil owner
  lands the cross-cite, this row is SUPERSESSION-PENDING and
  implementation of §3 MUST NOT begin (§9).
- U20 extends to NL edges.

BL1.8 NL drop around GC sync-collection conduct (NEW rev 8,
R7.1 — cross-document finding vs SPEC-congc; the congc-side full
walk and option analysis is SPEC-congc ANNEX CGD6.1, BINDING
there; this annex is the NL-side mechanism. Supersession recorded
BOTH sides: BL1.6's sync-collection leg + NA-T4's rev-5
sync-collection sub-arm superseded here; congc CG-I19/§3.7/
§13.5(4) is the other side). [r9: congc §13.5(4) adoption gate
CLOSED — this row + congc CG-I19 read RECORDED-BOTH-SIDES per
the PART B rev-9 record (back-cites congc ANNEX CGS2.2 edge
removal + CGS2.3 NL terms; ungil-side consumer = SPEC-ungil
rev 33 §LK row 9d / history ANNEX CGS2A.2); item 6's budget cite
is now STRUCTURAL (congc F45/§9.1(2a)/CG-I26), landed in
SPEC-ungil §A.3 rule 5 [r33].]

Problem: BL1.6 licensed "a Locked native can reach ... a sync
collection mid-body WITH NL held" against the landed
SINGLE-window heap §10 conduct (one access release, one GCL
bracket, one re-acquire). SPEC-congc §3 makes a shared collection
a tenure of N stop windows: per-window blocking GCL re-acquires,
per-window GBL barriers, between-window donateAll/
waitForTermination condvar waits, the F28 GCL inter-cycle
handoff, the tail access re-acquire (`Heap.cpp:5031` [r11;
rev-8 anchor :4955]). An
NL-holding mutator-conductor would hold NL across the ENTIRE
cycle — serializing every Locked native, custom accessor (§2.6),
JSClassRef callback (§2.7) and handleHostCall funnel (NA-I28)
process-wide for the cycle's duration. Not a deadlock (NL waiters
are §A.3-compliant park sites that F8-revert at each WND-open,
NL1/BL1.1; marking termination needs nothing from NL waiters) —
a liveness/grounding gap: NA-I13's exemption walked transition
shapes that no longer exist.

MECHANISM (normative):
1. The sync-collection REQUEST funnel — the path from a Locked
   native body into `requestCollectionShared` + election/
   follower-wait/poll-conduct (heap §10.2) — instantiates, on an
   NL-eligible lite with `m_nativeLockDepth != 0`, an
   NL drop bracket of the NA-I11 `NativeLockDropScope` shape:
   depth SAVED, NL FULLY released, BEFORE the first
   arbitration/park/GCL step of the funnel.
2. Reacquire: via the park-capable §3.2/NL1 loop, AFTER the
   funnel's CALLER-SIDE GCL release [r11 RE-PIN, r11 record —
   the rev-8 anchor ("after the conduct's access-reacquire
   tail", `Heap.cpp:4955`, present tree :5031) sat INSIDE the
   caller-held GCL and licensed an NL acquire by a heap-rank-2
   holder, contradicting NA-I10]: election `Heap.cpp:4606`,
   poll tail :4669/:4673, or the F28 successor's final release
   (conductor cases) — or the follower's ticket-served resume
   (follower case), before control returns to the native
   frame. NORMATIVE: the §3.2 reacquire loop runs holding NO
   heap rank >= 2 lock — textually equivalent to NA-I10, not
   merely consistent with it. Exception path
   per NA-I12 (destructor reacquires on exception-pending too);
   a TERMINATION-ONLY trap completes the reacquire (NL1
   trap-class split).
3. Coverage: conduct, follower ticket park, and the F28
   successor arm — the WHOLE funnel is inside the bracket, so NL
   is never held across any ticket park either.
4. The matching conducting-entry assert lives in SPEC-congc
   CG-I19 (`m_nativeLockDepth == 0`; debug, at election win /
   poll grant / `conductSharedCollection` entry) — that spec's
   side of the supersession.
5. Mode gating: same level-0 discipline as EX1/ACQ1 —
   `g_jscConfig.gilOffProcess` first; flag-off/GIL-on dead
   (NA-I1 unchanged; congc CG-I0 unaffected).
6. Liveness consequence: the process-wide Locked-native stall
   during a GC cycle is bounded by the stop WINDOWS (F8) the
   requester's threads see, NOT the cycle length; the BL1.6
   conductor-HOLD license survives only where its bracket is
   bounded (§A.3 single-window conducts), with the total
   conductor wait budget stated ONCE in congc ANNEX CGS2.3
   (cited, not restated, here).
7. [r11] NL-acquire guard (the assert CG-I19 does NOT cover —
   its depth==0 assert fires at conducting ENTRY only, not at
   the reacquire site): the §3.2 acquire path debug-asserts the
   acquiring thread holds NO heap rank >= 2 lock
   (m_gcConductorLock foremost); U20's NL-edge extension
   (§3.5/BL1.7) gains the matching no-*-over-NL-acquire lint
   obligation. gilOffProcess-only, item-5 gating.
Arms: NA-T4 rev-8 multi-window arm (TC1) + congc CGT1.1 F40
sub-arm — composed run required before either spec's flag ships.

## ANNEX SC1 — rev-4 surface closures (BINDING; spec §2.6/§4.4/§6 index)
## (NEW rev 4 — round-3 findings R3.1/R3.3/R3.4/R3.6)

SC1.1 DOMJIT signature dispatch (NA-I16 member d; spec §4.4.1d
index). When a NativeExecutable carries a `DOMJIT::Signature`, the
DFG parser takes `callee.signatureFor(specializationKind)` ->
`handleDOMJITCall` immediately AFTER the handleIntrinsicCall arm
(dfg/DFGByteCodeParser.cpp:2106-2114; function at :5244, emits a
Call node carrying OpInfo(signature)) and the node lowers to a
DIRECT call of `signature->functionWithoutTypeCheck` with no thunk
and no bracket — SpeculativeJIT::compileCallDOM
(dfg/DFGSpeculativeJIT.cpp:11603) and FTL compileCallDOM
(ftl/FTLLowerDFGToB3.cpp:22055). Because CallDOM calls the
WithoutTypeCheck variant, the bypass ALSO skips the type check the
generic path performs. The dispatch keys on the signature, not an
Intrinsic, so rev 3's NA-T6 sets (handleIntrinsicCall intrinsics,
VM.cpp:1283 switch, classInfo list, the three custom-accessor node
kinds) could not see it, and the emitted call is not
HostFunctionPtrTag-tagged so no rev-3 NA-T7 family matched.
Signatures enter through the exact §2.1 registration funnels:
VM::getHostFunction's `const DOMJIT::Signature*` parameter
(runtime/VM.cpp:1429, declared VM.h:1082) and
JITThunks::hostFunctionStub's NativeDOMJITCode arm
(jit/JITThunks.cpp:275-276) — i.e. the embedder API invites it —
and the in-tree $vm natives register two signatures today
(tools/JSDollarVM.cpp:1479 DOMJITFunctionObjectSignature, :1542
DOMJITCheckJSCastObjectSignature), both PT1-LOCKED ("$vm / test
natives"). RULE (mirrors §4.4.1b): in gilOff configs
getHostFunction passes nullptr for the DOMJIT::Signature unless the
executable's policy is ConcurrentOk — this simultaneously kills the
NativeDOMJITCode jit-code arm and starves `signatureFor` so
handleDOMJITCall never fires (locally verifiable, like NA-I23's
parser-side choice). NA-I23's CallDOMGetter disposition does NOT
cover CallDOM. The two compileCallDOM lowering files are
NA-T7-exempt WITH the NA-T6 cross-reference. (Filed twice in round
3; deduplicated.)

SC1.2 Intrinsic GETTER inlining (NA-I16 members e/f; spec §4.4.1e/f
index). Two parallel pipelines execute NativeExecutable-backed
getter semantics with no call: (e) DFG
`ByteCodeParser::handleIntrinsicGetter`
(dfg/DFGByteCodeParser.cpp:5263, dispatched from the GetById path
at :6743 — two hundred lines from the :6647 custom-accessor status
site rev 3 cited) replaces the getter call with plain graph nodes
(DataViewByteLength family, typed-array length family, etc.); (f)
baseline/IC `IntrinsicGetterAccessCase`
(bytecode/IntrinsicGetterAccessCase.cpp:37-48), admitted by
`InlineCacheCompiler::canEmitIntrinsicGetter`
(bytecode/InlineCacheCompiler.cpp:4473, used at
bytecode/Repatch.cpp:692), compiled via
`InlineCacheCompiler::emitIntrinsicGetter` (InlineCacheCompiler.cpp
:3575 dispatch, :4536 definition) inlines the same semantics into
IC stubs. The getters are host functions registered with
intrinsics; none appear in ANNEX PT1, so they are Locked by NA-I6 —
yet their semantics would execute on N threads with no bracket and
(rev 3) no lint row. RULE: each member's inlined semantics must be
OM/heap-ruled member-by-member and the set lint-pinned by NA-T6
(lint source: the handleIntrinsicGetter switch cases + the
canEmitIntrinsicGetter set), OR intrinsic-getter inlining is
disabled in gilOff configs alongside the §4.4.1b suppression.

SC1.3 Raw methodTable/host-hook pointers (NA-I24; spec §6 index).
The `GlobalObjectMethodTable` family
(runtime/GlobalObjectMethodTable.h:58-71) consists of raw function
pointers invoked as direct member-pointer calls from VM internals:
`promiseRejectionTracker` at runtime/VM.cpp:2265 (the §F.1 carrier
drain) and :2304; `reportUncaughtExceptionAtEventLoop` at
runtime/MicrotaskQueue.cpp:66, runtime/DeferredWorkTimer.cpp:284,
runtime/ThreadManager.cpp:933. These calls mint no NativeExecutable
(so "Locked" is not even REPRESENTABLE — no §1.2 byte exists),
reach no §4 emitter (they are direct C++ calls, not
HostFunctionPtrTag/vmEntryToNative/trampoline sites), are not §2.6
accessor funnels, and match no NA-T7 token family. NORMATIVE
consequences: (1) they receive NO NL coverage — their N-mutator
safety rests SOLELY on the SPEC-ungil §E.1b.4 / U-T8e disposition
audit, and §0.4 says so; (2) anti-laundering (inverse of NA-I19): a
U-T8e INLINE disposition for a methodTable hook MUST NOT be granted
or justified on the strength of NL serialization, because none
exists for this family — rev 3's §6 sentences ("an
inline-disposition Locked hook runs under NL"; "a Locked
carrier-queued hook simply runs under NL") asserted serialization
with no mechanism and are superseded (text of record in PART B
round 3); (3) a hook implemented by REGISTERING a
NativeExecutable-backed host function (rather than installing a raw
pointer) is covered by the bit machinery normally; (4) a future
bracket at the `methodTable()->X(...)` call-expression funnels
(an NA-I20-style seventh dispatch surface) is a scope extension,
NA-X5, not v1.

SC1.4 gilOff IC custom-accessor suppression — full grounding
(NA-I20; spec §2.6 index). Tag/symbol enumeration: the C++ dispatch
funnels carry GetValueFuncPtrTag (runtime/PropertySlot.h:97),
PutValueFuncPtrTag (:100), CustomAccessorPtrTag
(runtime/PutPropertySlot.h:37); the IC emission arms additionally
carry, on the JITCage arms, `setupArguments<GetValueFuncWithPtr/
PutValueFuncWithPtr>` (tags GetValueFuncWithPtrPtrTag /
PutValueFuncWithPtrPtrTag, PropertySlot.h:98/:101; tag block
runtime/JSCPtrTag.h:50-58) + `callOperation<OperationPtrTag>
(vmEntryCustomGetter/Setter)` (symbols llint/LLIntThunks.h:41-42,
annotated LLIntThunks.cpp:66-71; call sites
bytecode/InlineCacheCompiler.cpp:3474-3496, :5556, :6051) — all
five tags + both symbols are NA-T7 token families (rev 4, R3.6).
Suppression point: AccessCase CREATION in bytecode/Repatch.cpp —
gilOff mode does not create the four kinds CustomValueGetter /
CustomAccessorGetter (kind chosen at :711-715 inside tryCacheGetBy,
:475) and CustomValueSetter / CustomAccessorSetter (:1251 inside
tryCachePutBy, :1040). Post-suppression IC state: the access joins
the slow-path-only set the same way unsupported kinds already do —
give-up, no repatch retry, hence no regeneration livelock.
InlineCacheCompiler therefore never sees the kinds in gilOff mode;
the existing :3462 gilOff arm (`emitPublishTopCallFrameForHostCall`
with the UNGIL §A.1.3 comment) becomes unreachable in gilOff mode
and is RETAINED byte-identical for GIL-on processes — disposition:
superseded by suppression, not ripped out.

## ANNEX CF1 — creation closure, policy key, trampolines
## (BINDING; spec §1.3/§1.5/§2.1/§2.4 index; NEW rev 5 —
## R4.1/R4.2/R4.3/R4.6)

CF1.1 Creation closure (R4.1, filed 2x). The tree has exactly THREE
`NativeExecutable::create` call sites (grep re-verified this round):
jit/JITThunks.cpp:282 and runtime/VM.cpp:1441 (the two policy
funnels) and wasm/js/WebAssemblyFunction.cpp:101 —
`WebAssemblyFunction::create` first interns a base via
`getHostFunction(callWebAssemblyFunction, ..., WasmFunctionIntrinsic,
...)` at :99, then mints a deliberately NON-interned clone sharing
the same m_function/m_constructor ("Since ClosureCall uses this
executable as an identity for Wasm CallIC thunk, we need to make it
diversified", :100). Rev 4's §2.1 ("every NativeExecutable creation
funnels through exactly two constructors of policy") and NA-I5's
"consulted by EVERY creation on BOTH funnels" were therefore
falsified by the tree; the site had NO policy input (NA-I3's "the
byte is written in the ctor from the policy input at the creation
site" was undefined there), and NO lint saw it — neither NA-T6 nor
rev-4 NA-T7's eight token families grepped NativeExecutable::create
callers, so the closure claim was unpinned and a future direct
creator would have escaped silently (this site is precedent that
they get written). RESOLUTION: (a) `NativeExecutable::create` itself
carries the `NativeConcurrency` parameter with a HARD DEFAULT of
Locked — NA-I6 extended from "every funnel" to "every creation";
un-plumbed direct creators are conservatively safe by construction.
(b) The wasm site is EXEMPT-CITED: forced Locked
(callWebAssemblyFunction is reachable on NL-eligible lites — which
are exactly the gilOff VM's lites, NA-I9 — via .call/.apply/CallIC
slow paths on wasm exports; the §4.5 wasm exempt-cite covers ONLY
the runJSMicrotask vmEntryToWasm CALL arm, not this CREATION
[rev-6 note, R5.1: that §4.5 exempt-cite is itself SUPERSEDED —
the vmEntryToWasm arms now carry the NA-I26 drop scope, SC2.1;
this clause's CREATION disposition is unchanged]; spawned-Wasm
refusal semantics per SPEC-ungil §I trip inside the body). It CONSULTS the NA-I5 table like every creation: the table is
per-PAIR (CF1.2), so the many diversified clones of the
(callWebAssemblyFunction, callHostFunctionAsConstructor) pair are
ONE policy entry — if that pair were ever flipped at the funnel, the
clones follow the table; the two-live-executables-different-bits
divergence the reviewer constructed cannot arise. (c) NA-T7 NINTH
token family: `NativeExecutable::create(` callers, each a funnel or
exempt-cited — a new direct creator is a lint failure.

CF1.2 Policy key (R4.2 + R4.6 — two independent defects, one fix).
`HostFunctionKey` is `std::tuple<TaggedNativeFunction,
TaggedNativeFunction, ImplementationVisibility, String>`
(jit/JITThunks.h:224; equal() at JITThunks.cpp:102). Rev 4 keyed the
strong policy table AND the conflict RELEASE_ASSERT on the full key.
DEFECT 1 — UNBOUNDEDNESS (R4.2): the name leg is a dynamic runtime
String. `JSNativeStdFunction::getHostFunction` registers the SAME
pointer (runStdFunction) under a caller-supplied per-instance name
(runtime/JSNativeStdFunction.cpp:55-58); `JSCustomGetterFunction::
create` keys `String(propertyName.publicName())` per property
(runtime/JSCustomGetterFunction.cpp:66 — reviewer's "get
<propertyName>" is the DISPLAY name minted at :71-72, not the key
name; the substance — per-property dynamic key names — stands); and
spec §2.4 makes getHostFunction the embedder API, so Bun mints
std-function-backed natives with user/runtime-derived names for the
life of the process. Each distinct tuple appended one entry FOREVER
to an append-only strong table, pinning the name String — rev 4's
recorded rationale "entries never removed — size bounded by distinct
host functions" was false (not a re-litigation of R1.4's
weak-vs-strong decision: the decision's boundedness premise was
wrong). DEFECT 2 — ALIAS ESCAPE (R4.6): the same body registered
under different names or visibilities produced DISTINCT keys, so
conflicting NativeConcurrency inputs never hit the assert: one alias
mints ConcurrentOk executables while another mints Locked ones for
the SAME body, whose "serial" guarantee then serializes nothing (the
body runs concurrently via the ConcurrentOk alias against the Locked
alias's holder) — precisely the §0.3 property NA-I5 exists to
protect, arriving via aliases instead of registration order; it also
defeated §5.2 flip discipline (a flip missing one of several
registration sites was silent) and split the enforcement granularity
(key) from the audit granularity (AT1 rows = the body). RESOLUTION:
PolicyKey = the `(m_function, m_constructor)` TaggedNativeFunction
pair; name and visibility are NOT policy inputs (a per-name policy
split for one body is exactly the alias defect); the table is
bounded by distinct code-address pairs — genuinely "distinct host
functions"; the RELEASE_ASSERT compares per-pair, strictly stronger
(order- AND alias-independent). Name-dependent policy, if ever
wanted, must be argued in a spec rev with an honest growth bound
(e.g. store only explicit ConcurrentOk grants). NA-T8 gains the
same-function-different-name and different-visibility
conflicting-registration arms; §5.2 consequence recorded: a flip
must update every registration site of the symbol — the pair-keyed
assert now enforces it. [rev-7 note, R6.1: the "consulted by EVERY
creation" pinning this clause inherited is SUPERSEDED by ANNEX
SC3.1 — consult is per REGISTRATION CALL at funnel entry. The
"pair-keyed assert now enforces it" sentence holds ONLY under the
SC3.1 re-pin: a warm JITThunks cache hit early-returns before
create (JITThunks.cpp:262-267), so the creation-pinned consult
missed a conflicting same-key re-registration whenever the first
executable was still alive — exactly the §5.2 missed-site shape.]

CF1.3 Dispatch trampolines (R4.3). Spec §1.1 grounds the bit on
"function identity"; for JSNativeStdFunction the premise fails:
every instance shares ONE NativeFunction, `runStdFunction`
(runtime/JSNativeStdFunction.cpp:60-65 — downcasts the callee cell
and invokes the std::function stored ON the cell), so the §1.2 byte
— and the CF1.2 per-pair policy — identifies only the TRAMPOLINE
while the semantic body lives in per-cell state the key cannot see.
Failure modes rev 4 permitted: (a) ratchet hole — an NA1 row
flipping the runStdFunction key ConcurrentOk satisfied NA-I18
textually (symbol + registration site + evidence for the lambdas the
campaign exercised) yet blessed EVERY lambda ANY code EVER installs,
including ones written after the audit; the §1.4 obligation ("every
reachable path of the native's body") is undischargeable for an open
set of bodies, and nothing in §5 said so; (b) §2.4's "mark its own
audited natives ConcurrentOk at its call sites" was unimplementable
for std-function natives — per-call-site policy at key granularity
meant two Bun sites installing different lambdas with different
policies RELEASE_ASSERT (process abort), and the rev-4 escape
(distinct names) silently changed the audit unit; under CF1.2's
per-pair key the name escape is gone BY DESIGN, which makes this
clause load-bearing rather than advisory. RESOLUTION (NA-I25): a
NativeExecutable whose function is a dispatch trampoline (body
determined by callee-cell or out-of-band state; runStdFunction named
as the in-tree instance) is PERMANENTLY Locked — NA1 rows for its
pair may only carry Locked-keep; ANNEX AT1 gains the mandatory "body
closed over key?" field (yes/no + argument: the auditor must
establish that the function pointer fully determines the audited
body before ANY flip — the field forces noticing a dispatch
trampoline); §2.4 amended: Bun's ConcurrentOk opt-in exists only for
natives with DEDICATED function pointers. Per-cell affinity (a bit
on the JSNativeStdFunction cell / NativeStdFunction-side policy) is
charted as NA-X6, post-v1. The API/JSCallbackFunction family (same
structural shape) is unaffected in v1: §2.4 forces C-API Locked
unconditionally.

## ANNEX SC2 — rev-6 surface closures (BINDING; spec §2.4/§2.7/
## §3.3/§4.4/§4.5/§4.6/§1.3 index; NEW rev 6 — round-5 findings
## R5.1-R5.5/R5.7)

SC2.1 Wasm channel (R5.1, blocker; spec NA-I26, §3.3/§4.5 index;
EX1 site 9 carries the caller list). Composition rev 5 committed
to without analyzing the DROP side: (1) §2.1/CF1.1 force
callWebAssemblyFunction Locked; (2) wasm execution is carrier-only
but PERMITTED v1 (SPEC-ungil §I; the refusal helper at
wasm/js/WebAssemblyFunction.cpp:69-78 throws only on spawned
threads) and carriers are NL-eligible (NA-I9); (3) under
useJSThreads the warm JS->wasm call IC is DISABLED
(WebAssemblyFunction.cpp:70-72 — callWebAssemblyFunction is "the
single cold JS->wasm entry"), so in exactly the gilOff
configuration EVERY carrier JS->wasm export call lands on the §4
bracket, acquires NL, and ran the ENTIRE wasm activation
(vmEntryToWasm, :94) WITH NL HELD. Consequences rev 5 missed:
(a) NA-I11 funnel escape — wasm calls its JS imports through the
JIT-emitted wasmToJS stub (CallLinkInfo::emitDataICFastPath,
wasm/js/WasmToJS.cpp:350), no vmEntryToJavaScript* symbol on the
path, so user JS executed under NL: the R1.3/R2.2 escape class on
a channel the prefix-matched symbol family is STRUCTURALLY unable
to close. (b) Constructible mutator-vs-mutator deadlock crossing
BL1.4's negative edge with no assert and no conductor: T1
(carrier) wasm export -> §4 bracket holds NL -> JS import runs
lock.hold(cb) on contended L (or Atomics.wait/cond.wait) — T1
blocks on a G11 primitive HOLDING NL; T2 holds L inside
lock.hold(fn2); fn2 calls any Locked native (any Intl call) ->
blocks on NL. AB/BA, no stop in progress, no watchdog. NA-I22/
PT1.G does not help: lock.hold is ConcurrentOk but is entered
while the CALLER's frame holds NL from the wasm bracket. (c) Even
import-free: a long wasm number-crunch serialized every Locked
native of the VM behind it — the §3.3 "GIL regrows" collapse on a
shipping configuration (Bun runs wasm). NOT re-litigation: CF1.1
(rev 5) and the §4.5 exempt-cite addressed the ACQUIRE side only;
the exempt-cite's "carrier JS-to-Wasm is not a host-native call"
was true for the microtask arm it covered and false for the
dominant path. RESOLUTION (NA-I26): vmEntryToWasm callers are a
SECOND callee-defined drop-scope family (EX1 site 9 — three known
callers: WebAssemblyFunction.cpp:94, Interpreter.cpp:1316 (missed
entirely by rev 5), JSMicrotask.cpp:203 (exempt-cite superseded));
each instantiates the default-ctor NativeLockDropScope around the
vmEntryToWasm call, releasing NL for the whole activation and
every wasm->JS import inside it, reacquiring at scope exit per
NA-I12. NA-T7 gains the TWELFTH token family (`vmEntryToWasm`
call-expression callers; the LLIntThunks.h:72 inline wrapper is
the definitional symbol). §3.3 now states explicitly that the
vmEntryToJavaScript* family closes only the C++->JS boundary and
any OTHER native-code-to-JS channel needs its own named drop
family. Charters: NA-T3 wasm arm; NA-T11(d) deadlock witness.
NOTE for the SPEC-ungil owner (recorded, not actioned here): the
Interpreter.cpp:1316 and JSMicrotask.cpp:203 arms reach
vmEntryToWasm WITHOUT passing callWebAssemblyFunction's
spawned-thread refusal helper — a possible §I enforcement gap
OUTSIDE this spec's scope.

SC2.2 JSClassRef / JSCallbackObject embedder callbacks (R5.2,
major; spec NA-I27, §2.7 index). The C-API callback-object family
is squarely inside the §0.1 threat ("embedder native ... arbitrary
C++") and fell through every closure: JSCallbackObject installs
ClassInfo-methodTable overrides (API/JSCallbackObject.h:211-225 —
getOwnPropertySlot, put, putByIndex, deleteProperty,
getCallData/getConstructData, getOwnSpecialPropertyNames,
customHasInstance) whose bodies invoke raw JSClassRef embedder
callbacks (API/JSCallbackObjectFunctions.h: hasProperty :164,
getProperty :175, convertToType :250, setProperty :282-306/:349-373,
deleteProperty :415, initialize :139, static-table entry->
getProperty/setProperty arms, getPropertyNames family :619-675).
These (a) mint no NativeExecutable; (b) reach no §4 emitter, §2.6
funnel, or §4.4 surface; (c) match no rev-5 NA-T7 family; (d) are
NOT GlobalObjectMethodTable hooks, so NA-I24/SC1.3 (scoped to
GlobalObjectMethodTable.h:58-71) does not reach them; UNGIL-HANDOUT
R30 covers the callback DATA maps, not the BODIES. Hook invocation
needs no C-API entry: a carrier JSObjectMake's a callback object
into the shared heap; any spawned thread's plain `obj.x` dispatches
the methodTable and runs the embedder's C callback concurrently
with the carrier — no serializer, in a spec whose purpose is
"un-audited native bodies are correct-but-serial". §2.4's sole
deflection ("SPEC-api keeps C-API entry carrier-bound anyway")
carried no cite and is FALSE: SPEC-api rev 14 is the
Thread/Lock/Condition JS-API spec; grep finds no C-API/JSClassRef
clause (verified this round). Inconsistency: the FUNCTION face
(JSObjectMakeFunctionWithCallback -> JSCallbackFunction,
InternalFunction-backed) is NA-I8-serialized while the OBJECT face
of the same API was fully concurrent. RESOLUTION (option (a) —
bracket; the hooks are ordinary C++ funnels, NA-I20's shape):
NA-I27 — on NL-eligible lites, every JSClassRef-callback INVOCATION
EXPRESSION in JSCallbackObjectFunctions.h (and JSCallbackConstructor
/APICallbackFunction construct/call impls where not already funneled
through NA-I28's bracketed paths) is NL-bracketed via the §4.1
helper, unconditionally ("Locked" unrepresentable — no byte
exists, the §2.6 custom-accessor rationale verbatim). JS re-entry
from inside a callback (C-API calls back in) reaches the EX1
funnels and drops NL normally. EXEMPT-CITED, NOT bracketed:
JSObjectFinalizeCallback invocations (JSCallbackObjectFunctions.h
:87) — finalizers run in GC/sweep context where NL acquisition is
forbidden (NA-I10); their safety story is the C-API finalize
contract (no JSC re-entry permitted from finalize) + heap sweep
discipline, named here so the exclusion is recorded, not silent.
NA-T7 gains the ELEVENTH token family: invocation expressions
through `JSObject*Callback`/`JSClass*Callback`-typed locals in
API/** (greppable from the typedef set, JSObjectRef.h), each
bracketed or exempt-cited. NA-I24/SC1.3 amended BY THIS CLAUSE:
the "raw methodTable hooks excluded" sentence covers
GlobalObjectMethodTable hooks ONLY; the ClassInfo-methodTable
embedder-callback family is NA-I27-bracketed, not excluded.
Post-v1 refusal alternative (publication refusal / hook-time
spawned-thread throw, the JSWebAssemblyHelpers.h:61 shape) charted
as NA-X7.

SC2.3 CallData-native-without-NativeExecutable + the handleHostCall
funnels (R5.3, blocker; spec NA-I28, §4.5/§4.6 index). Two in-tree
host-call dispatch funnels invoke the host body as a direct C++
call of the CallData function pointer: llint/LLIntSlowPaths.cpp
handleHostCall (`callData.native.function(...)` :2222, construct
arm :2243) and bytecode/RepatchInlines.h handleHostCall (:96,
:117; reached from linkFor/virtualFor). Neither was in NA-I17's
"COMPLETE" set, and NA-T7 could not see them: the
HostFunctionPtrTag is hidden inside TaggedNativeFunction
(runtime/NativeFunction.h:40), so the call expression matches no
family. VERIFIED dispatch structure: both funnels are reached ONLY
for callables that are neither JSFunction nor InternalFunction
(LLIntSlowPaths.cpp:2263-2270; RepatchInlines.h:138-167 —
JSFunction callees take the executable's covered entrypoint,
InternalFunctions take the NA-I8 trampoline), i.e. EXACTLY the
no-NativeExecutable family: ProxyObject (performProxyCall
installed at runtime/ProxyObject.cpp:644, performProxyConstruct
:703 — engine code), JSCallbackObject callAsFunction/
callAsConstructor (API/JSCallbackObjectFunctions.h getCallData
:545ff/getConstructData :461ff — arbitrary client C), and
JSCallbackConstructor (API/JSCallbackConstructor.cpp:75). For this
family "Locked" is unrepresentable — the NA-I24-analog clause rev
5 never wrote. Two adjacent clauses were also wrong: (1) §2.4(b)
"the public C API mints Locked executables UNCONDITIONALLY" —
JSObjectMakeFunctionWithCallback creates JSCallbackFunction,
`final : public InternalFunction` (API/JSCallbackFunction.h:37),
which mints NO NativeExecutable; its serialization is NA-I8's
InternalFunction arm; an implementer sent to find the minting site
would find none. CORRECTED in §2.4. (2) §4.5's bracket recipe
"executable from the CallData/callee" was unimplementable at
Interpreter::executeCallImpl's native arm (Interpreter.cpp:1320)
for executable-less callees. RESOLUTION (NA-I28, the NA-I8/NA-I20
disposition — "no executable byte exists" => unconditional):
on NL-eligible lites, (a) BOTH handleHostCall funnels NL-bracket
the `native.function(...)` invocation unconditionally (release
before their exception checks per NA-I14); (b) the §4.5 C++ helper
is SPECIFIED for the executable-less case: callee is a JSFunction
=> read the NativeExecutable byte (existing recipe); otherwise =>
treat as Locked unconditionally (InternalFunction => NA-I8;
everything else => this clause). Applies to executeCall/
executeConstruct native arms and the runJSMicrotask native arm
alike. NA-T7 gains the TENTH token family: `native.function(`
call-expression sites (both funnels + the Interpreter/JSMicrotask
arms' vmEntryToNative dispatch is already family 3), each
bracketed or exempt-cited. Re-entry inside these bodies
(performProxyCall runs the trap JS via the §3.3 funnels) drops NL
normally — proxies stay correct-but-serial at the apply face, as
§0.3 intends for un-audited paths; an engine-side NA1 audit may
later flip the funnels to byte-keyed treatment for specific
callables (NA-X8, post-v1).

SC2.4 Intrinsic admission gating (R5.4, major; spec NA-I29,
§4.4.1 index; SUPERSEDES the adequacy implication of the
§4.4.1b/d nullptr-generator RULEs and SC1.2's "alongside the
§4.4.1b suppression" wording — both texts of record stand as
mechanism for surfaces (b)/(d) ONLY). VERIFIED: the Intrinsic does
NOT travel with the thunk generator — JITThunks::hostFunctionStub
constructs ALL THREE forCall JITCode arms with `intrinsic`
regardless of generator (jit/JITThunks.cpp:271-279: DirectJITCode,
NativeDOMJITCode, AND the plain NativeJITCode arm), the no-JIT arm
bakes it via jitCodeForCallTrampoline(intrinsic)
(runtime/VM.cpp:1441), and handleIntrinsicCall keys on
callee.intrinsicFor -> ExecutableBase::intrinsic() ->
NativeExecutable::intrinsic() -> generatedJITCodeFor(CodeForCall)
->intrinsic() (dfg/DFGByteCodeParser.cpp:2095-2098,
runtime/NativeExecutable.cpp:90-92, runtime/
ExecutableBaseInlines.h:43-54). Passing a nullptr generator
suppresses only the specialized thunk; DFG still inlines the
native's semantics with no bracket. LIVE for natives the spec
ships Locked: ArrayUnshiftIntrinsic (DFGByteCodeParser.cpp:2747),
ArraySpliceIntrinsic (:2855) — both excluded from PT1.B;
ObjectAssignIntrinsic (:3749) — not in PT1.A;
StringPrototypeReplaceIntrinsic (:3851) — excluded from PT1.C.
Surfaces (e)/(f) likewise key on the executable's intrinsic
(IntrinsicGetterAccessCase.cpp:50; canEmitIntrinsicGetter takes
the getter JSFunction), so "alongside b" named a mechanism that
does not reach them. RESOLUTION (NA-I29, NA-I23's locally-
verifiable parser/admission-side style — chosen over stripping
the Intrinsic at gilOff registration, whose collateral kills
profiling identity, the §4.3 fallback and getter intrinsics
wholesale): in gilOff configs, handleIntrinsicCall (guard at
DFGByteCodeParser.cpp:2095-2098), handleIntrinsicGetter
(:5263/:6743) and canEmitIntrinsicGetter
(InlineCacheCompiler.cpp:4473) BAIL unless the callee
NativeExecutable's m_concurrentOk byte is set (constant-foldable,
NA-I3; lint-pinned by NA-T6, which now checks the GUARDS exist
rather than set-emptiness alone). The b/d nullptr suppressions
REMAIN (they close the thunk/CallDOM arms); NA-I29 closes the
parser/IC admission the generator never controlled. [rev-7 note,
R6.2 (filed twice in round 6): the rev-6 predicate "BAIL unless
the callee NativeExecutable's m_concurrentOk byte is set" is
RE-TYPED. This clause's own dispatch trace cited only the
NativeExecutable arm; ExecutableBase::intrinsic() has TWO —
`isHostFunction() ? uncheckedDowncast<NativeExecutable> :
uncheckedDowncast<ScriptExecutable>`
(runtime/ExecutableBaseInlines.h:43-48) — and handleIntrinsicCall
is reached with ScriptExecutable callees BY DESIGN: the tree
comments the case at the guard site itself ("We might still try
to inline the Intrinsic because it might be a builtin JS
function", dfg/DFGByteCodeParser.cpp:2103-2104). A literal
implementation either (a) downcasts a ScriptExecutable to read a
garbage byte at the m_concurrentOk offset — type confusion, and a
nonzero garbage byte PASSES, so no test fails — or (b) bails ALL
builtin-JS intrinsics in gilOff configs, a parser-wide deopt of
bodies that are not native at all. CORRECTED predicate: bail IFF
the callee executable isHostFunction() AND its NativeExecutable's
m_concurrentOk is clear. ScriptExecutable-carried intrinsics are
OUT OF SCOPE — their inlined semantics are JS bytecode, governed
by core SPEC-ungil/SPEC-jit machinery, not this spec's §0.1
native-body audit — and remain admitted. The getter arms
(handleIntrinsicGetter/canEmitIntrinsicGetter) key on host-
function getters today (IntrinsicGetterAccessCase.cpp:50;
canEmitIntrinsicGetter takes the getter JSFunction), but the
guard is WRITTEN against the same isHostFunction() discriminator
at all three sites. NA-T6 gains the complementary
builtin-still-inlines control arm (TC1); gate §9.2(e) wording
updated.]

SC2.5 Macro-layer policy plumbing (R5.5, major; spec §2.1 third
mechanism; NA-I30). Rev 5 named two landing sites for the
ConcurrentOk marker, but nearly the whole PT1 seed registers
through neither: JSC_NATIVE_INTRINSIC_FUNCTION_WITHOUT_TRANSITION
sites (runtime/ObjectPrototype.cpp:65-66, runtime/
ArrayPrototype.cpp:107-124) expand to putDirectNativeFunction/
putDirectNativeFunctionWithoutTransition (declared
runtime/JSObject.h:1007-1009; macro block :2212-2228 — CITATION
CORRECTION: reviewer wrote ":2173-2189"; substance stands), which
reach getHostFunction only via JSFunction::create layers inside
the engine; Math/Atomics register via bare
putDirectNativeFunctionWithoutTransition calls
(MathObject.cpp:93ff, AtomicsObject.cpp:94). NA-I6's hard default
kept every un-plumbed layer safely Locked, so correctness was
never at risk — but v1 SHIPS the seed ConcurrentOk (§2.2), so the
missing plumbing was on the v1 critical path with no normative
route, and PT1's "Registration =" header was wrong for most of
its own rows (fixed in PT1 this rev). RESOLUTION (NA-I30): a
defaulted `NativeConcurrency` parameter (default Locked, NA-I6)
threaded through putDirectNativeFunction/
putDirectNativeFunctionWithoutTransition (JSObject.h:1007-1009)
and JSFunction::create into getHostFunction, plus trailing
defaulted arguments on the JSC_NATIVE_*_FUNCTION macro family —
the opt-in is greppable at the prototype finishCreation call
site, preserving §2.1's "greppable, diffable, audit-citable"
property. The NA-T7 ninth family (NativeExecutable::create
callers) is unaffected — these sites all funnel; they previously
could not EXPRESS the opt-in.

SC2.6 Policy side-table lock row (R5.7, major; spec §1.3/§9.7
index; finding received TRUNCATED mid-sentence at its third item
— the legible defects are dispositioned; the truncated tail
(allocation adjacency) is subsumed by the holding discipline
below). The §1.3 side-table lock is a NEW process lock consulted
at every NativeExecutable creation in EVERY configuration, yet
rev 5 gave it (1) no §LK row, no §9 gate and no U20 coverage — a
unilaterally asserted "leaf-rank" never entering SPEC-ungil §LK's
canonical set (SPEC-ungil.md:867-925; §LK is "canonical for U20
(r22 list)"), exactly the shape U20 exists to catch, while this
spec's own §3.5 demonstrates the required diligence for NL; (2)
no ordering against JITThunks::m_lock although both live on the
SAME registration path — hostFunctionStub takes m_lock TWICE
(lookup Locker jit/JITThunks.cpp:260, insert Locker :284, both
under AssertNoGC) around the NativeExecutable::create call
(:282), and nothing pinned whether the consult runs under m_lock,
between the critical sections, or inside the ctor; (3) no holding
discipline although the consult sits adjacent to GC allocation
(NativeExecutable::create allocates). RESOLUTION: (a) PINNED
ordering — the policy-table lock is a TRUE LEAF: the consult/
insert runs OUTSIDE JITThunks::m_lock and OUTSIDE the ctor,
BEFORE the create call's allocation, at each §2.1 funnel/
exempt-cited site (the NativeConcurrency value then flows into
create as a plain argument; NA-I3's "written in the ctor from the
policy input" is the BYTE STORE, not the table consult); m_lock >
policy-lock never occurs because the consult is outside both
m_lock sections; (b) holding discipline — NOTHING is acquired and
NOTHING allocates under the policy lock (HashMap rehash uses
fastMalloc, which the §LK.6 inner-set precedent treats as
lock-rank-inert; the RELEASE_ASSERT message formatting happens
after release); never held across NativeExecutable::create, GC,
or any park; (c) ADOPTION GATE §9.7 — SPEC-ungil owner lands the
§LK.7 leaf-row addition ("NA policy table lock", leaf, U20-linted)
both-sides; implementation of §1.3's enforcement structure MUST
NOT begin before the row lands. Until then the row is
SUPERSESSION-PENDING like LK.1c. [rev-7 note, R6.1: item (a)'s
"BEFORE the create call's allocation" pinning is SUPERSEDED by
ANNEX SC3.1 — the consult moves to funnel ENTRY, before the
JITThunks.cpp:260 lookup Locker, hit or miss. Items (b)/(c) — the
leaf rank, holding discipline and §9.7 gate — are unchanged and
apply verbatim to the entry-pinned consult: it still runs outside
both m_lock sections and outside the ctor, and the m_lock >
policy-lock ordering question still cannot arise.]

## ANNEX SC3 — rev-7 closures (BINDING; spec §1.3/§4.4 index;
## NEW rev 7 — round-6 findings R6.1/R6.4/R6.6)

SC3.1 Registration-call consult (R6.1, major; spec §1.3/NA-I5
index; SUPERSEDES rev 6's creation-pinned consult — CF1.2 and
SC2.6(a) carry the matching both-sides notes). DEFECT: §1.3 rev 6
tied the policy-table consult to CREATION ("consulted by EVERY
creation"; SC2.6(a) pinned it "BEFORE the create call's
allocation"), but the JIT funnel does not reach create on a cache
hit — JITThunks::hostFunctionStub (jit/JITThunks.cpp:253) looks up
the full HostFunctionKey under m_lock and EARLY-RETURNS the live
cached executable (:262-267); NativeExecutable::create (:282) is
never reached. Constructible escape: site A registers (fn, ctor)
with Locked; site B later registers the SAME pair with
ConcurrentOk. First executable alive (the warm-process common
case) => B hits the weak map, the conflicting NativeConcurrency
input is silently dropped, the NA-I5 RELEASE_ASSERT never fires.
First executable collected => the same call misses, consults, and
ABORTS. Enforcement was therefore GC-timing- and cache-state-
dependent — the exact non-determinism NA-I5's rev-2 rationale
disqualified the weak map over — and mode-dependent (the no-JIT
arm, VM.cpp:1440-1442, has no cache: consults per call, always
asserts). It also falsified CF1.2's recorded §5.2 consequence
("the pair-keyed assert now enforces" flip completeness): a
Locked->ConcurrentOk flip missing one registration site of the
same symbol is exactly a conflicting re-registration against a
warm cache — silent. NA-T8 could not see it: all four chartered
arms (JITThunks, no-JIT, collected-then-re-registered, rev-5
alias arms) go through a cache MISS and hence through create.
NOTE: the executables themselves cannot diverge on the hit path
(the hit returns the first-policy executable), so §0.3
serialization was NOT breached — the failures are the assert
mechanism, NA-I5's determinism clause, and §5.2 flip-completeness
detection. RESOLUTION: (a) the consult+conflict-assert runs per
REGISTRATION CALL, at funnel ENTRY of VM::getHostFunction
(VM.cpp:1429 — covers the no-JIT arm once, replacing its
per-creation consult) / JITThunks::hostFunctionStub
(JITThunks.cpp:253, BEFORE the :260 lookup Locker — hit or miss)
and at each exempt-cited direct site (WebAssemblyFunction.cpp:101
consults at its own entry as before); the returned/validated
NativeConcurrency flows into create as a plain argument on the
miss path only. (b) NA-I5's determinism sentence gains the two
axes: registration-order-, alias-, GC-timing- AND
cache-state-independent. (c) NA-I1's cost row restates the
consult as one leaf-lock acquisition per registration CALL —
still cold-path (registration sites only; a hit pays one more
leaf-lock acquisition than rev 6, on a path that already takes
m_lock). (d) SC2.6's leaf-rank and holding discipline carry over
verbatim: the entry consult is outside both m_lock sections,
outside the ctor, acquires/allocates/parks nothing. (e) NA-T8
gains the WARM-HIT arm: register Locked, hold the executable
STRONGLY, re-register the same full HostFunctionKey with
ConcurrentOk — the RELEASE_ASSERT MUST fire with no intervening
GC.

SC3.2 isDirect constant-path emitter (R6.6, major; spec §4.4
intro + §4.4.2 index). DEFECT: §4.4's opening sentence ("calls
land on the §4.3 thunk — covered EXCEPT the NA-I16 union") was
FALSE for the isDirect path: for a known host-function callee
with intrinsic() == NoIntrinsic, DFG emits a DIRECT call of the
TaggedNativeFunction with no thunk —
dfg/DFGSpeculativeJIT64.cpp:999-1062 (JITCage arm
`callOperation<OperationPtrTag>(vmEntryHostFunction)` :1031;
non-cage arm `callOperation<HostFunctionPtrTag>(nativeFunction)`
:1033) — and FTL mirrors it at ftl/FTLLowerDFGToB3.cpp:
14749-14796 (:14787/:14789). The path is in NO NA-I16 union
member (requires NoIntrinsic, no DOMJIT signature, not
InternalFunction lowering). NA-I17 listed "DFG/FTL constant-path"
among the four emitters, but the only bracket language was
§4.4.2's permissive "the compiler MAY fold ... and either omit
the bracket (bit=1) or emit unconditional acquire/release calls
(bit=0)" — constant-folding phrasing with no file:line, in a
family whose convention is normative clauses with file:line.
Worse, TC1's NA-T7 exempt-of-record "`vmEntryHostFunction` is
INSIDE the §4.3 bracket" is true ONLY for the
ThunkGenerators.cpp:510/:518 callers; it is FALSE for
DFGSpeculativeJIT64.cpp:1031 / FTLLowerDFGToB3.cpp:14787, which
sit inside the constant-path emitter outside any thunk — so an
unbracketed JITCage isDirect arm PASSED NA-T7 via the wrong
blanket exemption (the R3.3 "no new emitter" bug shape recurring
on the remaining direct-call arm). RESOLUTION: (a) §4.4 intro
rewritten — covered EXCEPT the NA-I16 union AND the isDirect
constant-path emitter (both arms cited). (b) §4.4.2 made
NORMATIVE: the constant-path emitter MUST emit the §4.1 bracket —
callee known, bit immutable (NA-I3): bit=1 => omit; bit=0 =>
unconditional acquire/release around the direct call, release
BEFORE the loadException check (DFGSpeculativeJIT64.cpp:1041-1043
/ FTLLowerDFGToB3.cpp:14794-14796) per NA-I14. (c) TC1's
vmEntryHostFunction exempt is SPLIT PER CALLER and the symbol
joins the NA-T7 generated check — a future caller is a lint
failure, never silently exempt. (d) Gate §9.2(a)'s by-name list
gains the two isDirect lowering sites.

## ANNEX ACQ1 — acquire-side C++ bracket helper (BINDING;
## spec §4.1 NA-I31 / §2.6 / §2.7 / §4.5 index; NEW rev 7, R6.3 —
## the R4.5/EX1 lesson applied to the ACQUIRE side)

DEFECT CLOSED (R6.3): four normative surfaces instantiate "§4.1
as an inline C++ helper" — §2.6/NA-I20 (PropertySlot.cpp:36-48 +
JSObject.cpp:1449-1493 put-side), §2.7/NA-I27 (JSCallbackObject
invocation expressions), §4.5 (vmEntryToNative arms), NA-I28
(both handleHostCall funnels) — but only the DROP side had a
pinned form (EX1, rewritten rev 5 after R4.5 established that an
unpinned ctor form produced an eager TLS load per JS entry in
flag-off/GIL-on builds). §4.1's pseudocode BEGINS with the lite
byte — correct for EMITTERS (asm already on the gilOff
mode-split path), but a C++ funnel transcribing it must resolve
the lite first, and the only current-lite source is the TLS slot
t_currentVMLite (runtime/VMLite.cpp:67; accessor VMLite.h:
333-345): a TLS load per custom-getter / callback-object access /
handleHostCall in EVERY build — the R4.5 defect class on hotter
paths, violating NA-I1 with no oracle watching (NA-T9(d) was
scoped to the EX1 funnels; closed by R6.4). Nothing pinned where
the release lands relative to the funnel's exception handling
(NA-I14 is stated for asm emitters whose exception branch is
explicit). No round decided to leave the acquire form open —
R4.5's accepted fix covered only NativeLockDropScope. PINNED
FORM:

    class NativeLockBracket {
    public:
        // DEFAULT form — funnels with no executable in hand
        // (§2.6 custom accessors, §2.7 JSClassRef callbacks,
        // NA-I28 handleHostCall, §4.5 executable-less arms):
        // unconditional Locked treatment on eligible lites.
        ALWAYS_INLINE NativeLockBracket() : m_lite(nullptr)
        {
            // Level-0 gate FIRST (EX1/R4.5 discipline verbatim):
            // flag-off/GIL-on processes pay exactly one
            // predictable global-byte branch; the TLS, lite and
            // executable-byte loads below never execute (NA-I1).
            if (!g_jscConfig.gilOffProcess) [[likely]] return;
            initSlow(VMLite::currentIfExists(), nullptr);
        }
        // BYTE-KEYED form — §4.5 helper-policy arm where the
        // callee IS a JSFunction host function: skips
        // acquisition when m_concurrentOk is set.
        ALWAYS_INLINE explicit NativeLockBracket(
                NativeExecutable* executable) : m_lite(nullptr)
        {
            if (!g_jscConfig.gilOffProcess) [[likely]] return;
            initSlow(VMLite::currentIfExists(), executable);
        }
    private:
        ALWAYS_INLINE void initSlow(VMLite* lite,
                                    NativeExecutable* executable)
        {
            if (!lite || !lite->nativeLockEligible) [[likely]]
                return;            // GIL-on VM's lite in a gilOff
                                   // process: byte test only
            if (executable && executable->concurrentOk()) return;
            m_lite = lite;
            nativeSerialLock(*lite).acquire(*lite); // NL1 loop
        }
    public:
        ~NativeLockBracket()
        {
            if (!m_lite) [[likely]] return;
            nativeSerialLock(*m_lite).release(*m_lite);
        }
    private:
        VMLite* m_lite;
    };

NA-I14 AT C++ FUNNELS, BY CONSTRUCTION: the bracket wraps the
INVOCATION EXPRESSION in its own scope (plus result marshal where
inseparable) and NOTHING else; RAII destruction at that scope's
closing brace is the release point, which therefore precedes the
funnel's pending-exception check / return-to-caller branch. Host
exceptions propagate by return value + per-lite state, never C++
unwinding through host frames (NA-I12), so the destructor runs on
the throw path too and MUST NOT inspect or clear the pending
exception (EX1 discipline verbatim). The acquire is the NL1
park-capable loop — a compliant park site; the rev-6
TERMINATION-ONLY arm applies (acquisition COMPLETES, delivery at
the funnel's post-call check). Cited from spec §2.6, §2.7, §4.5,
NA-I28 and NA-I31; §4.1's lite-byte-first ordering is hereby
scoped to EMITTERS only.

## ANNEX EM1 — emitter shapes, FULL text (BINDING; spec §4.2/§4.3
## index; MOVED here rev 5 under the size cap — content normative,
## carried VERBATIM from the rev-4 body, no semantic change)

EM1.1 LLInt. `nativeCallTrampoline`
(llint/LowLevelInterpreter64.asm:3161-3219): the gilOff arm already
exists (`branchIfGilOffGroup3ToT3` at the pre-call topCallFrame
store, :3175-3182, lite base in t3). Extension: on the
`.liteStoreTopCallFrame` arm, additionally `loadb
VMLite::nativeLockEligible[t3]`; if set, `loadb
NativeExecutable::m_concurrentOk[a2]` (a2 = executable, loaded at
:3165-3168); if clear, cCall the acquire operation around the host
call. Register notes are implementation detail BUT the constraint
is normative: the pre-call slow call must preserve a0-a2 (the host
call's argument set, see the :3171-3176 liveness comment) — spill
per the trampoline's existing conventions. The post-call
release+exception sequencing follows NA-I14: release happens before
the `.checkLiteException` test (:3211).
`internalFunctionCallTrampoline` (:3220) takes the NA-I8
unconditional arm (lite byte test only, no executable byte).

EM1.2 Baseline/JIT thunk. `nativeForGenerator`
(jit/ThunkGenerators.cpp:455-576): the thunk is generated per-VM
and cached, so the level-0 split is the C++ `vm.gilOff()` branch
already used twice in this function (:481-491 topCallFrame, :551
exception arm) — GIL-on VMs get an unchanged thunk. In the gilOff
thunk: after the executable lands in `argumentGPR2` (:502-512
JSFunction arm), emit `load8` of the lite byte (lite via
`loadVMLite`, the :481-491 pattern) and of `offsetOfConcurrentOk()`;
locked path does the acquire `callOperation` (operation calls are
already in this thunk's vocabulary:
`operationDebuggerWillCallNativeExecutable` :495,
`vmEntryHostFunction` under JITCage :510/:518). Release before the
`loadException` check (:535-536) per NA-I14, on BOTH the JSFunction
and InternalFunction arms (:514-521, per NA-I8). The same shape
applies to the construct-kind thunk (same generator, :460
`executableOffsetToFunction` switch).

## ANNEX TC1 — test charters, FULL text (BINDING; spec §8 index)
## (MOVED here rev 4 under the size cap; content normative,
## carried forward from rev 3 + rev-4 amendments)

- NA-T1 Serialization witness. A Locked test native (test-only
  registration, e.g. via the jsc shell's `$vm`/test natives)
  increments-checks-decrements an unsynchronized global; N spawned
  threads hammer it. Bit=0: zero observed overlap (NL serializes).
  Same body registered bit=1 under the amplifier: overlap observed
  (proves the bracket, not luck, provides the serialization).
  TSAN arm: bit=0 body is TSAN-clean BECAUSE of NL. Carrier arm
  (rev 2, NA-I9): the MAIN thread hammers the Locked witness while
  spawned threads do — zero overlap (carrier-vs-spawned
  serialization).
- NA-T2 Tier coverage matrix. The NA-T1 witness driven through each
  entry: LLInt-only (`--useJIT=false`), thunk (baseline), DFG/FTL
  hot loop (forced tier-up), C++ entry (API call from a spawned
  thread, §4.5), microtask job with a native callee
  (queueMicrotask(nativeFn), §4.5 rev 4, R3.2), InternalFunction
  `new` (NA-I8), and the construct kind via `new` over a host
  function (NA-I4). Each cell must serialize.
- NA-T3 Re-entry drop. A Locked native calls `toString` on an
  argument whose `toString` (a) blocks on a condition variable
  released only after a SECOND thread completes a different Locked
  native, and (b) throws after resume. (a) passes only if NL was
  dropped (NA-I11 — else deadlock-by-timeout); (b) verifies
  exception-path reacquisition (NA-I12: the outer native observes
  the pending exception with NL held; instrumented assert).
  CachedCall arm (rev 2, R1.3): the same shape driven through a
  Locked sort comparator (`Interpreter::executeCachedCall`,
  InterpreterInlines.h:100) and through module evaluation
  (`executeModuleProgram`, Interpreter.cpp:1662) — the rev-1 funnel
  missed both. Small-arity arm (rev 3, R2.2): the comparator arm
  runs on x86_64/ARM64 so `callWithArguments` ->
  `vmEntryToJavaScriptWith2Arguments` (NOT executeCachedCall) is
  the exercised path; plus a microtask-runner arm (With0..6).
  Wasm arm (rev 6, R5.1/NA-I26): carrier JS -> Locked wasm export
  -> JS import that blocks on a condition released only by a
  SECOND thread completing a Locked native — passes only if NL was
  dropped across the activation; instrumented assert that
  m_nativeLockDepth == 0 while wasm runs and the saved depth is
  restored at the export's return.
- NA-T4 Conductor liveness. While one thread is parked WAITING on NL
  and another HOLDS NL inside a spinning host body, a third forces
  GC and an §A.3 stop (jettison path): both complete within the
  watchdog (NA-I10 + §3.2.2 compliant parking; arms the SPEC-ungil
  U4 litmus family against NL). Wake-mid-stop arm (rev 2, R1.2): the
  conductor initiates a stop while T2 is parked on the NL bucket and
  T1 holds NL; T1 releases inside the window (host-call return); T2
  MUST NOT execute the native body inside the window — instrumented
  assert that acquire() ran a stop poll after its final park.
  Lost-wakeup arm (rev 2, R1.8): release-vs-park race hammered on
  ARM64; NL handoff latency bounded by unpark, never by the park
  deadline expiring. Multi-waiter arm (rev 3, R2.3): THREE
  contenders; every handoff unpark-bounded — the third waiter wakes
  via unpark, not deadline (exercises NL1's callback-form release).
  GC-stop-with-NL arm (rev 3, R2.6): rule-8 GC stop lands while a
  thread holds NL at a poll site — no NA-I13 assert; F8 revert,
  re-acquisition and GC complete; the native resumes with NL held.
  Conductor-holds-NL arm (rev 5, R4.7; RE-ARMED rev 8, R7.1): a
  Locked native on T1 triggers JSGlobalObject::haveABadTime while
  T2 waits on NL and T3 runs JS — the stop completes within the
  watchdog (rev 8: measured against the congc CGS2.3 budget), no
  NA-I13 assert fires, and NL is still held at T1's resume; a
  loser-variant forces T1 to lose arbitration so the
  NL-held-across-slot-park path (BL1.6) is exercised. The rev-5
  gc()-style sync-collection sub-arm is SUPERSEDED (BL1.8, both
  sides): it now expects the DROP, not the hold — see the rev-8
  multi-window arm. Rev-8 multi-window arm (composes SPEC-congc
  CGT1.1 F40): a Locked native on T1 calls a gc()-style sync
  collection that conducts a forced multi-window concurrent
  cycle — the BL1.8 bracket releases NL before the request
  funnel (congc CG-I19 depth==0 assert silent), T2's Locked
  native PROGRESSES between windows of the cycle (witnessed),
  T1 reacquires depth-restored after the conduct tail before its
  native body resumes; follower variant: T1 loses the election
  and parks on its ticket inside the bracket; control build with
  the bracket disabled shows the cycle-length stall (latency
  witness, no deadlock).
  Termination-vs-NL-waiter arm (rev 6, R5.6): VM-wide termination
  fires while T2 waits on NL held by a long Locked body on T1 —
  T2 COMPLETES the acquire (no NVS park-loop, NL1 rev-6
  TERMINATION-ONLY arm), unwinds through the §4 brackets to depth
  0, reaches its §E.2 close; join() returns the sticky-termination
  result (SD8/TERM1.3 fresh Error); ~VM completes within the
  harness timeout (EXIT1.9).
- NA-T11 Nesting + G11 liveness (rev 3, R2.1/R2.5). (a) Carrier in
  a Locked VM-A native enters VM B (§F.5); B's JS Atomics.waits on
  a notify owed by a Locked VM-A native on a spawned thread: must
  complete (NA-I21); assert depth 0 across the nested window.
  (b) `t.join()` while the joined thread's exit path runs Locked
  natives: completes (NA-I22). (c) Contended `lock.hold`/
  `cond.wait` in debug: no NA-I13 assert. (d) Wasm-import deadlock
  witness (rev 6, R5.1): T1 carrier calls a wasm export whose JS
  import does `lock.hold(cb)` on contended L while T2 holds L and
  calls an Intl (Locked) native — completes (NA-I26 dropped NL);
  control build with the drop scope disabled deadlocks-by-timeout.
- NA-T5 Flip campaign template. Per NA1 row: amplifier-scheduled
  N-thread corpus hitting the candidate native from all tier cells
  (NA-T2 matrix), TSAN no-JIT + ASAN JIT configs, plus a thread-fuzz
  session whose profile includes the candidate; evidence ids land in
  the row (NA-I18).
- NA-T6 Inline-bypass lint (rev 2). Build-time/test-time
  cross-check over the §4.4.1 UNION: Intrinsics reachable from
  `handleIntrinsicCall`, cases of the `thunkGeneratorForIntrinsic`
  switch (VM.cpp:1283), and the DFGByteCodeParser constant-
  InternalFunction classInfo list — minus gilOff-disabled minus
  concurrent-ok == empty (NA-I16). Includes the
  BoundFunctionCallIntrinsic disposition. rev 3 (R2.8): verifies
  the gilOff parser builds NO CallCustomAccessorGetter/Setter/
  CallDOMGetter nodes (NA-I23). rev 4 (R3.3/R3.4): + the
  signature-bearing-executable set (greppable: DOMJIT::Signature
  constructions + getHostFunction calls with non-null signature) —
  asserts gilOff builds NO Call-with-signature/CallDOM nodes — and
  the intrinsic-getter sets (handleIntrinsicGetter switch +
  canEmitIntrinsicGetter), each member ruled or disabled. rev 6
  (R5.4/NA-I29): additionally verifies the THREE gilOff
  m_concurrentOk admission guards EXIST and fire
  (handleIntrinsicCall DFGByteCodeParser.cpp:2095-2098;
  handleIntrinsicGetter :5263/:6743; canEmitIntrinsicGetter
  InlineCacheCompiler.cpp:4473): a Locked intrinsic-bearing native
  (ArrayUnshiftIntrinsic witness) tier-ups WITHOUT intrinsic
  inlining in a gilOff config and serializes per NA-T1. rev 7
  (R6.2): complementary CONTROL arm — a ScriptExecutable-carried
  (builtin-JS) intrinsic callee STILL INLINES in a gilOff config:
  the corrected NA-I29 predicate keys on isHostFunction()
  (ExecutableBaseInlines.h:43-48) and must not over-bail; an
  uncheckedDowncast-style mis-implementation is caught by the
  same arm under ASAN/UBSAN (the garbage-byte read).
- NA-T7 Surface-closure lint (rev 4; rev 5 R4.1; rev 6 R5.1/R5.2/
  R5.3). Source scan: every site in the TWELVE token families of
  §4.6 (HostFunctionPtrTag, cloopCallNative, vmEntryToNative
  callers, the three accessor tags, the accessor call-expression
  tokens, the vmEntryCustomGetter/Setter symbol callers, the two
  WithPtr tags, `NativeExecutable::create(` callers — rev 5 — and,
  rev 6: TENTH `native.function(` call-expression sites (SC2.3),
  ELEVENTH `JSObject*Callback`/`JSClass*Callback`-typed invocation
  expressions in API/** (SC2.2; finalize sites exempt-cited),
  TWELFTH `vmEntryToWasm` callers (SC2.1; symbol of record
  LLIntThunks.h:72)) is bracketed/exempt-cited (NA-I17, NA-I20,
  NA-I23, NA-I26/I27/I28; §2.1 for the ninth family). Exempt set of record (moved from the rev-4 §4.6
  parenthetical; vmEntryHostFunction entry SPLIT PER CALLER rev 7,
  R6.6/SC3.2 — the rev-4 blanket sentence was false for two of the
  four callers): `vmEntryHostFunction` callers at
  jit/ThunkGenerators.cpp:510/:518 are INSIDE the §4.3 bracket;
  the callers at dfg/DFGSpeculativeJIT64.cpp:1031 and
  ftl/FTLLowerDFGToB3.cpp:14787 are INSIDE the §4.4.2
  constant-path bracket (NOT the thunk's); `vmEntryHostFunction`
  joins the generated symbol check — a future caller FAILS the
  lint rather than inheriting a blanket exemption;
  `callHostFunctionAsConstructor` runs
  UNDER its caller's construct-kind bracket; the §4.4.1 bypass
  surfaces and the §4.4.3 + compileCallDOM lowering files are
  exempt WITH the NA-T6 cross-reference (their fast paths make no
  tagged host call, which is why a call-token grep alone cannot see
  them); wasm/js/WebAssemblyFunction.cpp:101 is the §2.1
  exempt-cited direct creation site (forced Locked). Every caller
  of the `vmEntryToJavaScript*` family instantiates the NA-I11 drop
  scope. SELF-TEST (R2.2): the JS-entry symbol list is generated
  from the LLIntThunks.h:39-52 block at lint runtime; a symbol
  absent from the previous snapshot FAILS the lint without a spec
  rev (a new With7 forces a conscious update, never silent escape).
- NA-T8 Policy-conflict assert (rev 2; rev 5 R4.6). Registering one
  function pointer twice with conflicting NativeConcurrency
  RELEASE_ASSERTs (NA-I5) via the strong side table; covers the
  JITThunks path, the no-JIT path, the collected-then-re-registered
  sequence (force GC of the first executable between registrations
  — the assert must still fire), AND (rev 5) the alias arms: the
  SAME (function, constructor) pair registered under a DIFFERENT
  name, and under a different ImplementationVisibility, with
  conflicting policy — the pair-keyed assert must fire in both.
  rev 7 (R6.1/SC3.1) WARM-HIT arm: register Locked, hold the
  minted executable STRONGLY, re-register the SAME full
  HostFunctionKey with ConcurrentOk — the RELEASE_ASSERT must
  fire with NO intervening GC (all prior arms went through a
  cache miss; this one exercises the funnel-entry consult on the
  JITThunks.cpp:262-267 early-return path).
- NA-T9 Mode-cost oracle (RECHARTERED rev 4 R3.10, rev 5 R4.8 —
  rev 4's (c) "non-GILOFF builds byte-identical outright" was
  unsatisfiable on the C++ axis: gilOffProcess is an unconditional
  JSCConfig field and the EX1/§4.5/§2.6/§1.3 surfaces compile in
  every build). (a) Byte-compare the GIL-on-generated per-VM thunk
  against a pre-change build; (b) arm-level LLInt diff: new
  trampoline bytes reachable ONLY via branchIfGilOffGroup3* arms
  (jit R1.e executed-path discipline, UNGIL-HANDOUT:172-179);
  (c) ASM-artifact identity ONLY: the offlineasm OUTPUT for
  non-GILOFF_TLS configurations is identical to a pre-change build
  (whole-binary byte identity dropped); (d) C++-side oracle (EXTENDED rev 7, R6.4 — rev 6 added two
  always-compiled acquire-side bracket surfaces, §2.7/NA-I27 and
  NA-I28, without restating this oracle, so an eager TLS load at
  them was an NA-I1 violation no test observed): an instrumented
  (or counter-asserting) build verifies the FULL acquire- and
  drop-side C++ funnel set — the EX1 funnels AND the ACQ1
  brackets (§2.6, §2.7, §4.5 incl. NA-I28) — executes exactly one
  gilOffProcess branch and ZERO TLS/lite/depth/executable-byte
  loads per entry in flag-off/GIL-on runs, and that no
  NL/side-table symbol is reached outside registration paths
  (perf-neutrality bound acceptable as the release-build
  proxy).
- NA-T10 U-T8e non-interference (rev 2). The carrier-queued
  promiseRejectionTracker corpus (SPEC-ungil §E.1b.4) runs unchanged
  with the bit machinery active; carrier NL acquisitions occur ONLY
  for Locked NativeExecutable-backed bodies and NEVER for the
  raw-pointer hook invocations themselves (rev 4, NA-I24 — rev 3
  implied the queued tracker runs under NL; superseded), and
  ordering/identity of the queued hooks is unchanged. (rev 1
  asserted ZERO carrier NL acquisitions; superseded with NA-I9.)

## ANNEX AT1 — NA1 audit row template (BINDING; spec §5.1 index)

File: docs/threads/SPEC-nativeaffinity-audit-NA1.md, style of
SPEC-ungil-audit-K4.md (status header stating the tree/branch the
audit executed against; classification key; rows consumed verbatim;
row ids NA1.<group>.<n>, groups mirror PT1 letters + E for embedder).

Row fields (all REQUIRED for a Locked->ConcurrentOk flip; "n/a" must
be argued, not bare):

| field | content |
|---|---|
| natives | symbol(s) + registration site file:line (one row may cover a PT1 sub-group that shares a body pattern) |
| kinds | call / construct / both (spec NA-I4: construct column mandatory even when it is callHostFunctionAsConstructor — say so) |
| shared state | every VM-/global-/process-resident datum touched -> ruling id (K4.x.y / N7-xx / NA1 row / OM-heap § cite); "none" allowed only with the grep recipe used to establish it |
| cell state | §N/OM rulings for cell-internal touches |
| JS re-entry | every re-entry site in the body (coercions, callbacks); for ConcurrentOk these need no NL note, but the column forces the auditor to FIND them |
| body closed over key? | MANDATORY (rev 5, NA-I25/CF1.3): yes/no + argument — does the (function, constructor) pair fully determine the audited body? "no" (dispatch trampoline: callee-cell or out-of-band state selects the body, runStdFunction shape) => disposition may ONLY be Locked-keep; the field exists to force the auditor to notice trampolines before any flip |
| ICU | Intl gate (spec NA-I7): every ICU API touched + the per-API thread-safety argument; non-Intl rows: "none" |
| TSAN | run id, config (TSAN no-JIT target per docs/threads/TSAN.md), corpus, amplifier schedule, result (zero races attributable) |
| fuzzer | campaign id, profile, wall hours, coverage proof that the native executed on spawned threads, result |
| disposition | ConcurrentOk \| Locked-keep \| Locked-blocked-on <row/spec §> |
| revocations | initially empty; a ConcurrentOk->Locked regression flip appends here with date + incident/bug cite (spec §5.2) |

---

# PART B — non-normative audit trail

## Revision log

### rev 1 (2026-06-07) — initial draft

Drafted per the thread-specs2 charter: (1) per-NativeExecutable
concurrent-ok bit (function identity, not PropertyAttribute); (2)
park-capable, safepoint-polling native serial lock taken on spawned
threads only, released around JS re-entry; (3) load8+branch check
shape per tier following the gilOff()/group3Primitives() mode-split
pattern; (4) TSAN+fuzzer-evidenced ungating audit extending the
K4/N7 style; (5) U-T8e complement rule; (6) test charter NA-T1..T10.

Grounding pass executed against the live tree:
runtime/NativeExecutable.{h,cpp}, runtime/JSFunction.cpp,
runtime/VM.cpp getHostFunction funnel, jit/JITThunks.cpp
hostFunctionStub + HostFunctionKey weak map,
jit/ThunkGenerators.cpp nativeForGenerator (including its existing
gilOff arms), llint/LowLevelInterpreter64.asm nativeCallTrampoline /
internalFunctionCallTrampoline (existing AB-1 gilOff arms),
llint/LowLevelInterpreter.asm two-level discriminator macros,
runtime/VMLite.h L2 append region + gilOff byte,
interpreter/Interpreter.cpp vmEntryToNative arms,
dfg/DFGByteCodeParser.cpp handleIntrinsicCall;
SPEC-ungil §A.3/§LK/§E.1b.4/§K/§N + audits K4/N7 + UNGIL-HANDOUT
rev 32 §A.1.3/U0b/U0c.

Design decisions of record (rationale not in the spec body):

- One lock per VM, not per native or per group. Per-native locks
  would let two un-audited natives that share hidden state (the
  whole reason they are locked) race each other; the single lock is
  the GIL's safety argument scoped to locked-native bodies on
  spawned threads. Granularity (lock striping by audit group) is a
  post-v1 optimization that must NOT precede group-level audits —
  striping IS an audit claim ("group X and group Y share no state").
- The bit defaults Locked even for natives the K4/N7 audits already
  touch: those audits ruled specific DATA, not whole BODIES; §0.4.
- Owner word uses TID (16-bit, VMLite.h:211) rather than a lite
  pointer: avoids any question of lite-pointer caching across
  samples (EXIT1 forbids caching lite pointers; a TID is a value,
  and NL ownership cannot survive thread exit by the EX1/NL1
  depth-0-at-close assert).
- ParkingLot directly rather than WTF::Lock: WTF::Lock's slow path
  parks WITHOUT polling the lite stop bit; NL1 step (b) is the whole
  point. (WTF::Lock under the hood is the same compareAndPark shape,
  so cost is identical.)
- The InternalFunction conservative rule (NA-I8) was chosen over
  mirroring the byte because InternalFunction lacks the single
  interning funnel NativeExecutable has (JITThunks weak map);
  policy-consistency (NA-I5) would need a second mechanism. Small
  set, cold path, v1 simplicity wins.

Known open items carried into review (also spec §9):
- §LK row LK.1c both-sides supersession (SPEC-ungil owner).
- SPEC-jit R1.e-family extension note for the new emitted branches.
- SPEC-api one-liner: C API mints Locked.
- vmstate L2 append ratification for nativeLockEligible +
  m_nativeLockDepth.
- Whether the §4.5 C++-entry bracket should ALSO cover
  `JSObjectCallAsFunction` C-API paths on spawned threads, or
  whether SPEC-api's carrier-binding of the C API makes that
  unreachable (believed unreachable; needs an api-owner confirm).

Adversarial review rounds: append below.

## Review round 1 (2026-06-07) -> rev 2

Twelve reviewer findings received; deduplicated to nine items (the
carrier-exemption blocker, the NL1 wake-ordering blocker, and the
specialized-thunk bypass were each filed twice/thrice). Every
file:line citation was re-verified against the live tree before
acceptance. Dispositions:

R1.1 ACCEPTED (blocker; filed 2x): carrier-side calls to Locked
natives were unserialized. Rev-1 NA-I9 set nativeLockEligible only
on spawned lites, justified by "carrier semantics are the GIL-on
semantics already shipping" — unsound, because under SPEC-ungil
GIL-off carriers run JS in parallel with spawned threads
(SPEC-ungil.md:272 "GIL-off EVERY thread uses a real carrier lite";
verified). An un-audited Locked body (any ICU native) could run
concurrently on main + spawned, falsifying §0.3 and voiding NA-I7's
Intl serialization in the normal Bun topology. FIX: NA-I9 revised to
`nativeLockEligible = vm.m_gilOff` (all lites of the gilOff VM);
§0.3, NA-I1/NA-I15 cost claims, §6 carrier-queued bullet, NA-T1
(carrier arm) and NA-T10 (carrier NL acquisitions now legal)
amended. Rev-1 NA-I9 text of record: "nativeLockEligible =
vm.m_gilOff && (lite is a SPAWNED thread's lite ...); main/embedder
CARRIER lites emit 0. Main-thread/carrier calls to locked natives
take NOTHING."
  SUB-CLAIM REFUTED: the reviewer asserted "carrier tid is 0,
  VMLite.h:211 — owner encoding must distinguish 'free' from 'main
  thread holds', e.g. tid+1". FALSE for the configuration where NL
  exists: GIL-off carrier lites receive TM-allocated NONZERO carrier
  TIDs — runtime/JSLock.cpp:350 (`carrier->tid =
  allocateCarrierTID(); // unique nonzero TM allocation`) and :522
  (`RELEASE_ASSERT(lite->tid); // tid 0 never installed GIL-off`).
  VMLite.h:211's "0 = main thread" describes the GIL-on default,
  which never reaches NL (level-0 discriminator). No encoding change
  made; NL1 owner-word comment now cites these lines.

R1.2 ACCEPTED (blocker; filed 3x incl. the EX1-destructor variant):
rev-1 ANNEX NL1 ordered the contended loop CAS-first ((a) CAS,
(b) stop poll, (c) park, (d) goto a). A waiter parked in (c) and
woken by the holder's release INSIDE a stop window would CAS, win,
and RETURN into the host body with no post-wake stop poll —
violating SPEC-ungil §A.3.2b(ii) (SPEC-ungil.md:216-218, verified)
and contradicting the spec body §3.2.2, which was poll-first. FIX:
NL1 rewritten poll-first with two invariants: every wake (unpark or
deadline) loops to the stop poll before any CAS, and a winning CAS
re-polls — if the stop bit is set, the thread parks on its NVS
ticket WHILE HOLDING NL (legal per NA-I10) and completes the
post-wake poll before returning. Body §3.2.2 and annex now state the
identical order; EX1 destructor inherits it; NA-T4 gains the
wake-mid-stop arm. Rev-1 NL1 acquire text of record: "2. loop: a. if
m_owner.compareExchangeWeak(0, lite->tid) { depth=1; return; }
b. STOP POLL ... c. m_waiters++; ParkingLot::compareAndPark(&m_owner,
snapshot, timeout = bounded quantum); m_waiters--. d. goto a."

R1.3 ACCEPTED (blocker): the NA-I11 four-function re-entry funnel
was falsified by the tree. Verified: `Interpreter::executeCachedCall`
(interpreter/InterpreterInlines.h:100) calls vmEntryToJavaScript
directly at :127 and serves exactly the Locked natives' callbacks
(sort comparator ArrayPrototype.cpp:950, replace replacer
StringPrototype.cpp:399-400); `Interpreter::executeModuleProgram`
(Interpreter.cpp:1662, entry :1728) likewise bypasses the four.
FIX: funnel restated callee-defined (every vmEntryToJavaScript /
vmEntryToJavaScriptWith0Arguments caller), EX1 site list extended
(items 2-4), NA-T7 greps the caller set, NA-T3 gains the
CachedCall/module arms.

R1.4 ACCEPTED (major; merged with the no-JIT-leg finding): NA-I5's
RELEASE_ASSERT was anchored to the JITThunks weak map — verified
weak (`Weak<NativeExecutable>` entries with dead-entry override,
jit/JITThunks.cpp:262-268/:285-300), so a collected Locked entry
followed by a ConcurrentOk re-registration of the same key would
assert against nothing (GC-timing-dependent policy, transient
Locked/ConcurrentOk twins); and the no-JIT arm
(runtime/VM.cpp:1440-1442) creates fresh executables every call with
no interning, so the chartered assert had no mechanism there at all.
FIX: §1.3 now specifies a per-VM STRONG append-only
HashMap<HostFunctionKey, NativeConcurrency> side table (leaf-rank
lock) consulted by BOTH funnels; the weak map is explicitly NOT the
enforcement structure; NA-T8 gains the collected-then-re-registered
sequence.

R1.5 ACCEPTED (part of the closure finding): DFG lowers
constant-callee InternalFunction constructions to plain graph nodes
with no call — verified at dfg/DFGByteCodeParser.cpp:6080-6140
(SymbolConstructor -> NewSymbol, ObjectConstructor ->
NewObject/CallObjectConstructor) — contradicting rev-1 NA-I8's
"EVERY InternalFunction native call takes NL". FIX: §2.5 caveat
scoping NA-I8 to actual calls; the inline set joins the NA-T6 lint
(§4.4.1c).

R1.6 ACCEPTED (blocker): custom accessors were in the declared scope
(§0.1 "every static-table getter thunk") but had no bit carrier, no
bracket, and were invisible to NA-T7 (CustomAccessorPtrTag sites in
bytecode/InlineCacheCompiler.cpp, GetterSetterAccessCase.cpp,
Repatch.cpp, GetByVariant.cpp, PutByVariant.cpp — verified; 105
JSC_DEFINE_CUSTOM_GETTER sites in runtime/*.cpp — verified). FIX:
new §2.6 + NA-I20 (unconditionally Locked v1; gilOff IC compilation
slow-paths custom-accessor cases through an NL-bracketed operation);
NA-T7 greps CustomAccessorPtrTag; NA-X4 charts the post-v1 marker.

R1.7 ACCEPTED (major; filed 2x): specialized intrinsic thunks bypass
the §4.3 bracket. Verified: VM::getHostFunction passes
thunkGeneratorForIntrinsic (runtime/VM.cpp:1283 switch, used at
:1435); hostFunctionStub installs the generator's code as the
executable's CALL code (jit/JITThunks.cpp:273-275 DirectJITCode);
only the SpecializedThunkJIT::finalize fallback
(jit/SpecializedThunkJIT.h:173) reaches the bracketed generic thunk;
BoundFunctionCallIntrinsic (VM.cpp:1444-1458) is not PT1-seeded.
FIX: NA-I16/NA-T6 extended to the three-surface UNION; gilOff
getHostFunction suppresses the generator for Locked executables;
NA-I17/NA-T7 name the bypass surfaces as lint-governed exempts;
BoundFunctionCallIntrinsic explicitly flagged.

R1.8 ACCEPTED (major): the rev-1 split owner-word/relaxed-m_waiters
design had a lost-wakeup window on weakly-ordered hardware (no
happens-before from the waiter's relaxed increment to the releaser's
relaxed load; the annex's own "sole synchronization edge" clause
guaranteed the absence of the needed edge). FIX: NL1 rewritten to a
single word with hasParkedBit (WTF LockAlgorithm shape), all
transitions seq_cst RMW on m_word; the memory-order clause now
states what liveness depends on; NA-T4 gains the lost-wakeup arm.

R1.9 ACCEPTED (major): rev-1 NL1 step (c) cited
ParkingLot::compareAndPark with a timeout — verified that
compareAndPark takes none and hard-codes Time::infinity()
(wtf/ParkingLot.h:91-102) — and claimed (c)-parked waiters "count as
parked at a poll site" with no conductor mechanism behind the claim,
while calling the quantum correctness-free. FIX: NL1 specifies
parkConditionally with an explicit bounded deadline, normatively <
stopTheWorldWatchdogTimeout (30s,
bytecode/JSThreadsSafepoint.cpp:379), default 10ms; the
conductor-side paragraph now states honestly that an NL waiter is
conductor-invisible for at most one quantum and records the
worst-case stop-latency bound.

Supersessions recorded this side: rev-1 NA-I9/NA-I15/NA-T10 carrier
exemption (R1.1), rev-1 NL1 loop order + m_waiters + compareAndPark
+ conductor claim (R1.2/R1.8/R1.9), rev-1 NA-I11 funnel list (R1.3),
rev-1 NA-I5 weak-map enforcement (R1.4), rev-1 NA-I8 universal-call
claim (R1.5), rev-1 NA-I16/NA-I17/NA-T6/NA-T7 closure set
(R1.6/R1.7). No counterparty spec text changed: the §9 adoption
gates (SPEC-ungil §LK row, SPEC-jit note, SPEC-api note, vmstate L2
ratification) remain PENDING and unmodified; R1.1 does not alter the
proposed LK.1c edge set (NL stays inner to heap rank 1, outer to api
1-3 / heap 2-10b; only WHO acquires it widened to carrier lites,
which adds no conductor edge — NA-I10 unchanged).

## Review round 2 (2026-06-07) -> rev 3

Ten reviewer findings received; deduplicated to eight items (the
NL1 hasParkedBit release loss was filed 3x; the vmEntryToJavaScript
With1..6 funnel escape 2x). Every file:line citation re-verified
against the live tree and the counterparty specs before acceptance.
No finding refuted this round. Dispositions:

R2.1 ACCEPTED (blocker): NL held across licensed carrier cross-VM
nesting. Verified: UNGIL-HANDOUT §F.5 (UNGIL-HANDOUT.md:2262-2290)
licenses the carrier-side "Bun JSContext-inside-host-call" pattern
(:2270) and mandates "lock() on VM B while holding any other VM A's
token FIRST releases A's client heap access (F8 mandatory-revert)"
(:2276-2277); SPEC-ungil §F.6(e) (SPEC-ungil.md:668-670) forbids
nesting only for SPAWNED threads. A Locked VM-A native on the
carrier entering VM B therefore (1) performed the exact transition
rev-2 NA-I13 forbade, with only a debug assert behind it; (2) kept
NL across the whole nested window — every §3.3 drop-scope caller
inside B passes B's lites, which cannot release A's NL; (3) made a
mutual-wait deadlock constructible (B's JS Atomics.waits on a
notify owed by a Locked VM-A native on a spawned thread that blocks
on NL) with no conductor involved, falsifying rev-2 §3.4's
unscoped release-build "never deadlock" sentence (which was sound
only for conductor liveness). FIX: NA-I21 — the §F.5 funnel
instantiates NativeLockDropScope keyed on the thread's gilOff-VM
lite at its F8-revert point, RELEASE_ASSERT depth==0 backstop;
EX1 site 8; §3.4 liveness-scope paragraph rewritten (conductor vs
mutator liveness split); adoption gate §9.6 (SUPERSESSION-PENDING
with the SPEC-ungil/UNGIL-HANDOUT owner: §F.5/§F.6(e)/A36C + the
§F.6 IU embedder-checklist NL-depth row); NA-T11(a).

R2.2 ACCEPTED (blocker; filed 2x): the rev-2 NA-I11 funnel named
two exact symbols while the tree's JS-entry family is
vmEntryToJavaScript + With0..With6Arguments
(llint/LLIntThunks.h:39, :46-52; asm at
llint/LowLevelInterpreter.asm:2194-2329). Verified escaped callers:
Interpreter::tryCallWithArguments (interpreter/
InterpreterInlines.h:132-171, all seven arms), reached from
CachedCall::callWithArguments (interpreter/CachedCallInlines.h:38-66)
on CPU(ARM64)/CPU(X86_64) whenever argumentCountIncludingThis <= 7
— so the spec's own poster child, the Array.prototype.sort
comparator (runtime/ArrayPrototype.cpp:955, two args ->
With2Arguments), bypassed the funnel on both shipping targets (rev
2's :950/executeCachedCall story covered only the >7-arg fallback);
MicrotaskCall::tryCallWithArguments
(interpreter/MicrotaskCallInlines.h:40-86); runJSMicrotask arms
(runtime/JSMicrotask.cpp:159-171, :198). The R1.3 bug shape
recurring one level down, as filed. FIX: funnel restated as the
prefix-matched family defined by the LLIntThunks.h:39-52
declaration block (a future With7 cannot silently escape); EX1
sites 3-4 added and item 3's cite corrected from the With0 arm to
the whole wrapper; NA-T7 symbol list generated from LLIntThunks.h +
self-test; NA-T3 small-arity comparator arm pinned to
x86_64/ARM64 so tryCallWithArguments (not executeCachedCall) is
exercised.

R2.3 ACCEPTED (major; filed 3x): ANNEX NL1 release lost
hasParkedBit on multi-waiter handoff. Verified: the rev-2 release
(`old = m_word.exchange(0); if (old & hasParkedBit) unparkOne`)
cleared the bit for ALL waiters while waking ONE; the woken
waiter's acquire CAS preserved only `w & hasParkedBit` with w == 0,
so remaining waiters slept to the bounded deadline — one full
quantum (default 10ms) per subsequent handoff under contention
depth >= 2, directly failing NA-T4's own "handoff latency bounded
by unpark" arm; the plain unparkOne(address) form discards
UnparkResult::mayHaveMoreThreads (wtf/ParkingLot.h:104-119, :130);
WTF::LockAlgorithm::unlockSlow uses the CALLBACK form and
republishes isParkedBit iff mayHaveMoreThreads
(wtf/LockAlgorithmInlines.h:207-241) — the one load-bearing piece
of the "LockAlgorithm shape" rev 2 omitted. FIX: NL1 release step 3
rewritten to the callback-form handoff (bucket-locked store of
hasParkedBit iff mayHaveMoreThreads, else 0; fast path stays a bare
CAS to 0 when the bit is clear); memory-order note updated (the
bucket-locked callback store replaces the bare exchange as the
release-side transition); NA-T4 multi-waiter handoff arm added
(third waiter wakes via unpark). Rev-2 release text of record:
"3. old = m_word.exchange(0); // seq_cst, same word / if (old &
hasParkedBit) ParkingLot::unparkOne(&m_word)."

R2.4 ACCEPTED (blocker): no order existed between the two
long-hold locks (NL and NLS::m_lock), and a mutator-vs-mutator
AB/BA cycle was constructible from normative text: SPEC-ungil §LK
long-hold row (SPEC-ungil.md:902-907) — lock.hold runs user JS
holding NLS::m_lock, that JS may call a Locked native (NLS > NL,
also via the EX1 dtor reacquire in fn's epilogue); while a Locked
lock.hold body (host native, absent from rev-2 PT1, hence Locked
by NA-I6) blocks on NLS::m_lock holding NL (SPEC-api.md:57-59
contended hold blocks) — NL > NLS. §3.5's acyclicity argument was
silent on long-hold-vs-long-hold. FIX: LK.1c gains the pinned edge
NLS::m_lock OUTER to NL plus the negative edge "no NL holder
blocks on NLS::m_lock or any G11 blocking primitive" (discharged
operationally by R2.5's PT1.G seeding); SPEC-api §5.9 companion
row (the EX1-dtor-inside-fn-epilogue case is the LEGAL direction)
added to the §9.1 SUPERSESSION-PENDING scope.

R2.5 ACCEPTED (blocker): the G11 blocking natives (SPEC-api.md:15:
join(), lock.hold(), cond.wait()) defaulted Locked and would block
holding NL: joiner-vs-joined-thread total deadlock (the joined
thread completes only at §E.2 close and its Locked natives need
NL); and all three release heap access before blocking
(SPEC-api.md:267-271, 5.9(a1-a3)/(e)), tripping rev-2 NA-I13's
assert on every contended call. The engine's own threads API — the
project's deliverable — was unaddressed by §3.4's embedder-only
remedy. FIX: NA-I22 + ANNEX PT1.G (threads-API natives seeded
ConcurrentOk; their bodies are the api-§5.9-audited lock
discipline; NA1 rows still required); demotion rule (a G11 native
may only be Locked with internal drop scopes); PT1
LOCKED-FOR-EMPHASIS amended; gate §9.3(b); NA-T11(b)/(c).

R2.6 ACCEPTED (major): rev-2 NA-I13's unqualified
release-path assert fired on a legal, spec-mandated path — rule-8
GC stops fan through the per-lite trap bit and their NVS park
performs the F8 MANDATORY access revert (§13.5a willPark /
per-client m_releasedByGCPark; SPEC-ungil.md:289-298 "the GC stop
DOES set client-visible stop state"), so any thread holding NL at
an NL1 stop poll during a GC stop would assert in debug builds;
ANNEX NL1's "tokens + heap access KEPT, as at every §A.3 park"
parenthetical was true only for §A.3 JSThreads stops. FIX: NA-I13
scoped to VOLUNTARY transitions with an explicit exemption for
§J.3 park-site mandatory reverts; §3.2.4 and the NL1 step-(a)
parenthetical corrected (kept at §A.3 stops, F8-reverted at rule-8
GC stops); NA-T4 GC-stop-with-NL arm added.

R2.7 ACCEPTED (major): NA-I20's bracket was ungrounded — the C++
dispatch carries GetValueFuncPtrTag (runtime/PropertySlot.h:97) /
PutValueFuncPtrTag (PropertySlot.h:100) / CustomAccessorPtrTag
(runtime/PutPropertySlot.h:37), three distinct tags
(runtime/JSCPtrTag.h:53-56), and the invocation sites are untagged
call expressions (PropertySlot::customGetter's
m_data.custom.getValue, runtime/PropertySlot.cpp:36-48; put-side
customSetter invocations, runtime/JSObject.cpp:1449-1493) that a
CustomAccessorPtrTag grep cannot find; and "the slow-path
operation gains the bracket" was ambiguous-to-unimplementable
(custom-ness is unknown until after lookup). FIX: NA-I20
regrounded — the bracket wraps the FunctionPtr INVOCATION at the
named dispatch funnels; NA-T7 token families extended to all three
tags + the call-expression tokens.

R2.8 ACCEPTED (major): DFG/FTL direct custom-accessor calls had no
disposition in NA-I17's closure. Verified: CallCustomAccessorGetter
/Setter and CallDOMGetter lower to direct calls of the retagged
accessor pointer (dfg/DFGSpeculativeJIT.cpp:11689/:11813/:11840;
ftl/FTLLowerDFGToB3.cpp:15961-15989, :22171-22187
"bypassedFunction"), built from GetByStatus/PutByStatus variants at
dfg/DFGByteCodeParser.cpp:6647/:7076/:5559 — reaching neither the
§2.6 IC mechanism nor any C++ funnel. FIX: NA-I23 (§4.4.3) — gilOff
byte-code parser does not build the three node kinds (parser-side
suppression chosen over the unstated starvation argument because
it is locally verifiable); NA-T6 extended; the lowering files are
NA-T7 exempt with the NA-T6 cross-reference.

Supersessions recorded this side: rev-2 NA-I11 two-symbol funnel +
EX1 item 3 (R2.2), rev-2 NL1 release step 3 + "heap access KEPT"
parenthetical (R2.3/R2.6), rev-2 NA-I13 unqualified assert +
unscoped "never deadlock" sentence (R2.1/R2.5/R2.6), rev-2 §3.5
edge set silence on NLS::m_lock (R2.4), rev-2 NA-I20
slow-path-operation wording + single-tag lint (R2.7), rev-2
NA-I17 surface set (R2.8). Counterparty-spec obligations are
gates, not edits made here: §9.1 (extended LK.1c + api §5.9 row),
§9.3(b) (PT1.G api-owner ack), §9.6 (§F.5/§F.6 NL-drop
obligation) — all SUPERSESSION-PENDING; no text outside
SPEC-nativeaffinity{,-history}.md was modified.

## Review round 3 (2026-06-07) -> rev 4

Eleven reviewer findings received; deduplicated to ten items (the
DOMJIT/CallDOM bypass was filed twice). Every file:line citation
re-verified against the live tree before acceptance (one path
correction: "Repatch.cpp" lives at bytecode/Repatch.cpp in this
tree, not jit/ — kinds at :711-715/:1251, tryCacheGetBy :475,
tryCachePutBy :1040). No finding refuted this round; one refined
(R3.10, GILOFF-build scope). Numbering: the two DOMJIT filings
took R3.3 and R3.5 on intake; R3.5 merged into R3.3, id retired —
the R3.5 gap below is deliberate. Size-cap action: spec body was at
49,966/50,000 pre-round; the §8 charter full text moved to ANNEX
TC1 and §1.3/§3.1/§3.2.2 rationale prose compressed against
existing history records to absorb the rev-4 additions.
Dispositions:

R3.1 ACCEPTED (blocker): §6 asserted NL serialization for
globalObjectMethodTable hooks with no mechanism. Verified: the
hooks are raw member pointers (GlobalObjectMethodTable.h:58-71)
invoked directly — promiseRejectionTracker at runtime/VM.cpp:2265
(the §F.1 drain; rev 3's own carrier-queued example) and :2304;
reportUncaughtExceptionAtEventLoop at runtime/MicrotaskQueue.cpp:66,
runtime/DeferredWorkTimer.cpp:284, runtime/ThreadManager.cpp:933.
No NativeExecutable is minted ("Locked" unrepresentable), no §4
emitter or §2.6 funnel is reached, no NA-T7 family matches — rev
3's "an inline-disposition Locked hook runs under NL" and "a Locked
carrier-queued hook simply runs under NL" claimed serialization the
design does not provide (R1.1 severity class), inviting U-T8e
dispositions laundered ON the nonexistent NL — the inverse of
NA-I19. FIX (reviewer option (a), the smaller honest fix): NA-I24 +
ANNEX SC1.3 — §6 scoped to NativeExecutable-backed natives; raw
hooks get NO NL coverage, safety rests SOLELY on the U-T8e audit;
anti-laundering clause (no INLINE disposition on the strength of
NL); §0.3/§0.4 cross-notes; NA-T10 stops implying the queued
tracker is NL-serialized. Option (b) (a methodTable bracket) is
charted as NA-X5 in SC1.3, not v1. Rev-3 §6 bullets text of record:
"An inline-disposition hook that is Locked is legal — it runs on
the spawned thread under NL"; "under revised NA-I9 the carrier IS
NL-eligible, so a Locked carrier-queued hook simply runs under NL
like any other Locked native — correct, ordered".

R3.2 ACCEPTED (major): §4.5's "both sites" falsified — verified
THIRD vmEntryToNative caller at runtime/JSMicrotask.cpp:206 (the
CallData::Type::Native arm, reachable from plain JS via
queueMicrotask(nativeFn) on a spawned thread); rev 3 cited this
function's JS arms (:159-171, :198) for the NA-I11 DROP funnel
while missing the ACQUIRE-side native arm below them — the
R1.3/R2.2 bug shape a third level down. FIX: §4.5 enumerates three
sites and DEFINES the set by the NA-T7 vmEntryToNative token
family; wasm arm (:204) exempt-cited (spawned Wasm refused,
SPEC-ungil §I); NA-T2 gains the microtask-native-callee cell;
NA-I17 names the family as definitional.

R3.3 ACCEPTED (major; filed 2x): DOMJIT signature dispatch is a
fourth inline-bypass surface — verified end-to-end
(DFGByteCodeParser.cpp:2106-2114 signatureFor -> handleDOMJITCall
:5244; CallDOM direct call of functionWithoutTypeCheck,
DFGSpeculativeJIT.cpp:11603, FTLLowerDFGToB3.cpp:22055; funnels
VM.cpp:1429 + JITThunks.cpp:275-276 NativeDOMJITCode; $vm
signatures JSDollarVM.cpp:1479/:1542, PT1-LOCKED), invisible to
rev-3 NA-T6/NA-T7 and additionally type-check-skipping. R1.7/R2.8
class, recurring on the signature channel. FIX: NA-I16 member (d) +
ANNEX SC1.1 — gilOff getHostFunction passes nullptr signature
unless ConcurrentOk (kills the jit-code arm AND starves
signatureFor); NA-T6 gains the signature-bearing-executable set;
§4.4's "no new emitter" sentence corrected; compileCallDOM files
NA-T7-exempt with NA-T6 cross-ref; recorded that NA-I23's
CallDOMGetter row does not cover CallDOM.

R3.4 ACCEPTED (major): intrinsic GETTER inlining (DFG
handleIntrinsicGetter, DFGByteCodeParser.cpp:5263/:6743; IC
IntrinsicGetterAccessCase + emitIntrinsicGetter,
IntrinsicGetterAccessCase.cpp:37-48, InlineCacheCompiler.cpp
:3575/:4536, admission canEmitIntrinsicGetter :4473 used at
bytecode/Repatch.cpp:692) executes Locked-by-default getter
semantics outside every bracket and outside the rev-3 NA-T6 sets.
FIX: NA-I16 members (e)/(f) + ANNEX SC1.2, §2.5-style treatment —
each member OM/heap-ruled and lint-pinned, or gilOff-disabled.

R3.6 ACCEPTED (major; reviewer's two-part IC finding): (1) NA-I20's
gilOff IC suppression named no mechanism while the live tree's IC
emission already carries a gilOff arm (InlineCacheCompiler.cpp:3462
emitPublishTopCallFrameForHostCall). FIX: suppression GROUNDED at
AccessCase creation in bytecode/Repatch.cpp (four kinds named;
slow-path-only give-up state — no regeneration livelock; the :3462
arm's disposition recorded: unreachable gilOff, retained GIL-on).
(2) The JITCage emission arm calls via vmEntryCustomGetter/Setter
(LLIntThunks.h:41-42) under GetValueFuncWithPtrPtrTag/
PutValueFuncWithPtrPtrTag (PropertySlot.h:98/:101) — none in rev
3's six NA-T7 families, and NA-I20's "three tags" undercounted.
FIX: NA-T7 extended to EIGHT families; §2.6 enumerates five tags +
two symbols; full grounding = ANNEX SC1.4.

R3.7 ACCEPTED (major): negative-edge rank range inconsistent inside
the proposed LK.1c row — §3.2.3/NA-I10 said heap 2-10b, §3.5's
acyclicity bullet said 2-9b (copied from the narrower NLS row,
SPEC-ungil.md:903-906 "no conductor or heap-2..9b holder ACQUIRES
it"). Since NL is declared outer to 2-10b and SPEC-heap L4's
allocation back-edge shows 10a/10b sections are not lock-terminal,
the exclusion must be stated. FIX: 2-10b pinned VERBATIM in §3.5,
matching NA-I10; the §9.1 gate scope records the range verbatim.

R3.8 ACCEPTED (major): §3.5's "needs no §E.2 rank-4 exemption" was
stale after R2.6 — BL1.1/NL1 license holding NL across the F8
MANDATORY revert + gated re-acquisition at rule-8 GC stops, exactly
the shape §E.2's park-site clause polices (SPEC-ungil.md:489-495,
"release BEFORE, re-acquire AFTER (ditto §J.3 park sites)") and for
which NLS::m_lock carries the recorded RANK-4 EXEMPTION; U20 lints
the order. Recording/composition defect, not unsoundness
(deadlock-freedom argued by NA-I10/BL1.1). FIX: §3.5 grants NL an
NLS-style carve-out LIMITED to §J.3 park-site MANDATORY F8
reverts/re-acquisitions; voluntary transitions remain forbidden
(NA-I13); the carve-out joins the LK.1c row text and §9.1 scope.

R3.9 ACCEPTED (major): three counterparty obligations lacked gates.
(1) §9.4 ratified only nativeLockEligible while ANNEX NL1 appends
m_nativeLockDepth to L2 (the rev-1 open-item list named both). FIX:
§9.4 names both fields. (2) NL1's teardown asserts edit
SPEC-ungil-owned lifecycle text (§E.2 close depth==0; ~VM m_word==0
ordered against EXIT1.9) with no supersession row. FIX: added to
the §9.1 SUPERSESSION-PENDING scope. (3) §2.6 cited "gate §9.2"
whose stated scope ("codegen note for the §4.3/§4.4 arms") did not
bind the IC suppression. FIX: §9.2 reworded to name, explicitly,
the bracket arms + the §2.6 Repatch suppression + the NA-I23
parser suppression + the §4.4.1b/d/e/f suppressions.

R3.10 ACCEPTED (major, REFINED): NA-I1's "byte-for-byte" and
NA-T9's whole-binary byte-compare are unsatisfiable for
GILOFF-enabled builds — §4.2 adds instructions inside
nativeCallTrampoline's `.liteStoreTopCallFrame` arm
(LowLevelInterpreter64.asm:3161-3219) and LLInt is assembled once
at build time. REFINEMENT (verified): the arm lives inside the
`if GILOFF_TLS` assembler conditional, so non-GILOFF builds DO
remain byte-identical; the claim fails only for GILOFF-enabled
binaries running flag-off — which is the shipping Bun
configuration, so the finding stands. The family convention is
executed-path identity (jit R1.e, UNGIL-HANDOUT:172-179), not
binary identity; SPEC-ungil §J does not rescue the literal charter.
FIX: NA-I1 restated (executed sequences unchanged; new LLInt bytes
confined to branchIfGilOffGroup3*-guarded arms; GIL-on per-VM
thunks byte-identical; non-GILOFF builds byte-identical outright);
NA-T9 rechartered to thunk byte-compare + arm-level LLInt diff.

R3.11 ACCEPTED (major): body §3.3 and BINDING ANNEX EX1
contradicted each other on the drop scope's mode gating — §3.3
claimed a §4.1-style mode test plus mode-split call sites; the EX1
ctor had only the lite test, and the mandatory sites
(InterpreterInlines.h:100-171, JSMicrotask.cpp, etc.) are
common-path code executing identically in every configuration. Both
texts normative => conflicting instructions, and the "dead code
GIL-on" cost story was wrong. FIX (shape chosen: add the gate, keep
the cost claim honest): EX1 ctor gains the level-0
`g_jscConfig.gilOffProcess` test FIRST; §3.3 rewritten — flag-off/
GIL-on processes pay one predictable global-byte branch per
JS-entry funnel, the lite/depth loads never execute; the false
"call sites are themselves on gilOff-mode-split paths" sentence
superseded (text of record: "the C++ check is one TLS-adjacent
load, and it is additionally gated on the same mode test as §4.1,
making it dead code GIL-on").

Supersessions recorded this side: rev-3 §6 hook-NL bullets + NA-T10
implication (R3.1, NA-I24), rev-3 §4.5 two-site enumeration (R3.2),
rev-3 NA-I16 three-surface union + §4.4 "no new emitter" + NA-I17
surface set + NA-T6/NA-T7 lint sets (R3.3/R3.4/R3.6), rev-3 NA-I20
"three tags" + ungrounded IC suppression (R3.6), rev-3 §3.5 2-9b
range + "needs no §E.2 rank-4 exemption" (R3.7/R3.8), rev-3
§9.2/§9.4 gate wording (R3.9), rev-3 NA-I1/NA-I15/NA-T9
"byte-for-byte" (R3.10), rev-3 §3.3 gating sentence + EX1 ctor
(R3.11). Size-cap supersessions (content unchanged, location
moved): §8 full charters -> ANNEX TC1; §1.3/§3.1/§3.2.2 rationale
prose -> already-recorded history rounds. Counterparty-spec
obligations remain gates: §9.1 (rev-4-extended LK.1c scope), §9.2
(reworded), §9.3, §9.4 (both fields), §9.6 — all
SUPERSESSION-PENDING; no text outside
SPEC-nativeaffinity{,-history}.md was modified.

## Review round 4 (2026-06-07) -> rev 5

Eight reviewer findings received; deduplicated to seven items (the
WebAssemblyFunction direct-creation finding was filed twice — the
second filing took R4.4 on intake, merged into R4.1, id retired;
the R4.4 gap below is deliberate). Every file:line citation was
re-verified against the live tree before acceptance. No finding
refuted this round; two citation corrections recorded under their
items. Size-cap action: the spec body was at 49,990/50,000
pre-round; §4.2/§4.3 full emitter text moved to NEW ANNEX EM1, the
§5.1 row-template listing reduced to its AT1 pointer, the §4.6
exempt-set parenthetical moved into TC1 NA-T7 (content unchanged,
location moved), and §0/§1/§2/§3/§6/§9 prose compressed against
existing history records to absorb the rev-5 additions; post-round
body 49,933 bytes. Dispositions:

R4.1 ACCEPTED (major; filed 2x): §2.1's "exactly two constructors
of policy" falsified by the tree — grep over Source/JavaScriptCore
confirms exactly three NativeExecutable::create call sites
(jit/JITThunks.cpp:282, runtime/VM.cpp:1441,
wasm/js/WebAssemblyFunction.cpp:101), the third a deliberately
non-interned CallIC-identity clone with no policy input, no NA-I5
participation, and no lint coverage (the R1.3/R2.2/R3.2 bug class —
a normative enumeration falsified by the tree — recurring in §2.1,
the one enumeration no prior round ground-checked). FIX (reviewer's
shape adopted whole): NativeConcurrency parameter moved onto
NativeExecutable::create itself, hard default Locked (NA-I6 "every
creation"); §2.1 restated funnels-plus-exempt-cited-direct-sites;
wasm site exempt-cited forced-Locked + per-pair table consult;
NA-T7 ninth token family. FULL text ANNEX CF1.1.

R4.2 ACCEPTED (major): NA-I5's strong side table was unbounded —
HostFunctionKey (jit/JITThunks.h:224) embeds a dynamic name String;
JSNativeStdFunction (runtime/JSNativeStdFunction.cpp:55-58) and
JSCustomGetterFunction (runtime/JSCustomGetterFunction.cpp:66)
mint per-instance/per-property names over fixed pointers, so the
append-only strong table grew per registration forever, pinning
embedder-controlled Strings; rev 4's recorded "size bounded by
distinct host functions" rationale was false (the R1.4
weak-vs-strong decision itself is NOT re-litigated — its
boundedness premise was). CITATION CORRECTION: the reviewer's
"mints 'get <propertyName>' names" describes the DISPLAY name
(JSCustomGetterFunction.cpp:71-72); the KEY name is
String(propertyName.publicName()) at :66 — still per-property and
dynamic, substance unaffected. FIX: policy table REKEYED to the
(function, constructor) TaggedNativeFunction pair (one fix shared
with R4.6); bounded by distinct code-address pairs; §1.3 restated.
FULL text ANNEX CF1.2.

R4.3 ACCEPTED (major): the §1.1 function-identity premise breaks
for dispatch trampolines — runStdFunction's semantic body lives on
the callee cell (JSNativeStdFunction.cpp:60-65), so a single
ConcurrentOk flip would bless every lambda ever installed,
including post-audit ones; §5 had no clause forcing the auditor to
notice, and §2.4's per-call-site opt-in was unimplementable for
std-function natives (conflicting policies on one key
RELEASE_ASSERT). The spec never mentioned JSNativeStdFunction —
missed-state, as filed. FIX: NA-I25 + new §1.5 (trampolines
PERMANENTLY Locked; Locked-keep-only NA1 rows); AT1 mandatory "body
closed over key?" field; §2.4 dedicated-pointer-only opt-in; NA-X6
charts per-cell affinity. FULL text ANNEX CF1.3.

R4.5 ACCEPTED (major): ANNEX EX1's ctor signature
`NativeLockDropScope(VMLite*)` contradicted the restated NA-I1 —
C++ evaluates ctor arguments eagerly, so every common-path JS-entry
site had to materialize the lite (the only common-path source being
TLS t_currentVMLite, runtime/VMLite.cpp:67; L4 accessors
VMLite.h:333-345) BEFORE the level-0 gate: a TLS load per JS entry
in flag-off/GIL-on processes, the R3.11 defect class one level
down (two binding texts, conflicting instructions); and the spec
never said how site 8 — keyed on the THREAD's gilOff-VM lite, not
the entered VM's — obtains a non-default lite. FIX: EX1 rewritten
with TWO forms — default no-arg ctor resolving
VMLite::currentIfExists() INSIDE the ctor after the gilOffProcess
gate (sites 1-7), and an explicit-lite form for site 8 only,
passing the pre-swap lite the §F.5 funnel already holds (the
setCurrent return value / LIFO restore tuple); §3.3 restated to
the cost actually paid; the L4 accessor cited both places.

R4.6 ACCEPTED (major): the NA-I5 conflict RELEASE_ASSERT, keyed on
the full HostFunctionKey, let the same body carry conflicting
policy under different names/visibilities silently — voiding the
Locked guarantee for that body (the Locked alias's NL serializes
nothing against the ConcurrentOk alias) and silently defeating
§5.2 flip discipline; rev-4 NA-T8 (same key registered twice)
never exercised the alias path. FIX: shared with R4.2 — per-pair
PolicyKey makes the assert alias-independent and strictly
stronger; NA-T8 alias arms (different name, different visibility);
§5.2 every-registration-site consequence recorded. FULL text
ANNEX CF1.2.

R4.7 ACCEPTED (blocker): a Locked native firing an §A.3 conductor
(Class-4 haveABadTime, sync GC, or losing arbitration) was
normatively forbidden by NA-I13 and unlicensed by LK.1c — the
NLH1.4 hole one row over. Verified: §K.5 rule 5 makes the caller
the conductor (SPEC-ungil.md:768-780 — CITATION CORRECTION: the
reviewer's :775-783 range; rule text actually spans :768-780,
same clause); HBT4 binds ALL conductors to release-access ->
arbitration -> GCL with losers parking access-released on the
§LK.4b slot (SPEC-ungil.md:240-247, :873-886); NLH1.4
(:883-885) licenses NLS-held-on-entry but nothing licensed NL;
NA-I13's sole exemption did not cover conductor brackets; NL1's
conductor analysis covered only holder-is-stopped. The R2.6 bug
class (legal mandated path fires the assert) recurring, plus a
lock-table scope hole. FIX (reviewer's three-part shape adopted):
LK.1c conductor-HOLD clause + acyclicity walk + §LK.4b held-with
amendment into the §9.1 gate scope (spec §3.5); NA-I13 exemption
(b) for HBT4 conductor-bracket/loser-park transitions (spec §3.4);
NL1 conductor paragraph extended (holder-as-conductor,
holder-as-loser); NA-T4 conductor-holds-NL arm incl. loser
variant. FULL text ANNEX BL1.6.

R4.8 ACCEPTED (major): NA-I1's "non-GILOFF builds byte-identical
outright" and NA-T9(c) were falsified by rev 4's own C++ surfaces
— verified: GILOFF_TLS exists only in
llint/LLIntOfflineAsmConfig.h:187-191; g_jscConfig.gilOffProcess
is an UNCONDITIONAL field (runtime/JSCConfig.h:177; common-C++
latch :79-96), so the EX1 drop scope, §4.5/§2.6 brackets and §1.3
side table compile and execute in every build, and the §1.3
registration-path consult (deliberately not mode-gated — gating it
would void NA-I5/NA-T8 in GIL-on processes and miss
pre-gilOff-latch registrations) contradicted "EXECUTED sequences
unchanged" as an absolute. Rev 4 was internally inconsistent
(claimed whole-build byte identity while carving out the EX1
branch). The R3.10 defect recurring on the C++ axis, as filed.
FIX: NA-I1 restated as a per-surface cost contract (LLInt asm /
GIL-on thunks / C++ branch-per-funnel + declared cold
registration consult); §1.3 gains the explicit MODE DISPOSITION
sentence (table active in every configuration by design); NA-T9
rechartered — (c) scoped to the offlineasm ASM artifact, new (d)
C++ branch/TLS-load-count oracle at the EX1 funnels.

Supersessions recorded this side: rev-4 §2.1 two-funnel enumeration
+ NA-I6 funnel scope (R4.1), rev-4 NA-I5 HostFunctionKey keying +
"size bounded by distinct host functions" rationale + NA-T8 charter
(R4.2/R4.6), rev-4 §1.1 unqualified function-identity rationale +
§2.4 unconditional opt-in + AT1 field set (R4.3), rev-4 EX1
single-signature ctor + §3.3 gating text (R4.5), rev-4 NA-I13
single-exemption scope + §3.5 row text + NL1 conductor paragraph
(R4.7), rev-4 NA-I1 "non-GILOFF byte-identical" + NA-T9(c) (R4.8).
Size-cap supersessions (content unchanged, location moved): §4.2/
§4.3 emitter text -> ANNEX EM1; §5.1 field listing -> ANNEX AT1;
§4.6 exempt-set parenthetical -> ANNEX TC1 NA-T7; §0.1/§0.3/§1.1/
§1.2/§2.3/§2.5/§2.6/§3.1/§3.2/§6 prose compressed against existing
history records. Counterparty-spec obligations remain gates, not
edits made here: §9.1 (rev-5-extended: conductor-HOLD clause +
§LK.4b held-with amendment), §9.2-§9.4, §9.6 — all
SUPERSESSION-PENDING; no text outside
SPEC-nativeaffinity{,-history}.md was modified.

## Review round 5 (2026-06-07) -> rev 6

Seven reviewer findings received (one — R5.7 — arrived TRUNCATED
mid-sentence inside its third numbered item; the legible defects
were dispositioned, the truncation recorded in SC2.6). Every
file:line citation re-verified against the live tree and the
counterparty specs before acceptance. No finding refuted this
round; three citation corrections recorded under their items.
Size-cap action: spec body was at 49,968/50,000 pre-round; rev-6
full texts land in NEW ANNEX SC2 + amendments to NL1/EX1/PT1/TC1;
body prose compressed against existing BINDING annex/history
records (content unchanged, location/derivation cited): §0.1-§0.4,
§1.1-§1.3, §1.5, §2.1, §2.3-§2.4, §2.6, §3.1-§3.5, §4.1,
§4.4.1(b,d)/NA-I23, §5.2, §6, §7 NA-I1, §9.1, §10, reading order.
Post-round body 49,935 bytes. Dispositions:

R5.1 ACCEPTED (blocker): NL held across carrier wasm execution and
across wasm->JS import re-entry; the NA-I11 funnel family
structurally cannot see the wasm channel. Verified: refusal helper
throws only on spawned threads + ":70-72" warm-IC-disabled comment
(wasm/js/WebAssemblyFunction.cpp:69-78, :70-72 — reviewer wrote
:62-78/:71-72, same clauses, minor drift); vmEntryToWasm at :94;
wasmToJS import call via CallLinkInfo::emitDataICFastPath
(wasm/js/WasmToJS.cpp:350; reviewer :348-349, drift); no
vmEntryToJavaScript* symbol on the path. ADDITIONALLY verified
beyond the filing: a THIRD vmEntryToWasm caller exists that rev 5
never mentioned at all — Interpreter::executeCallImpl's wasm arm,
interpreter/Interpreter.cpp:1316 — and the microtask wasm arm
lives at JSMicrotask.cpp:203 (spec said :204). The BL1.4-edge
deadlock and the GIL-regrowth composition check out as filed; not
re-litigation (CF1.1/§4.5 addressed only the ACQUIRE side; the
exempt-cite's "not a host-native call" sentence concealed the
dominant path). FIX (reviewer's shape adopted whole): NA-I26 —
vmEntryToWasm callers are the SECOND callee-defined drop-scope
family; EX1 site 9 (three callers); §4.5 exempt-cite superseded;
NA-T7 twelfth family; NA-T3 wasm arm; NA-T11(d) witness; §3.3 now
states the vmEntryToJavaScript* family closes only the C++->JS
boundary. FULL text ANNEX SC2.1, which also records (not actions)
a possible SPEC-ungil §I refusal gap at the two non-host-function
vmEntryToWasm arms.

R5.2 ACCEPTED (major): JSCallbackObject/JSClassRef embedder
callbacks were a coverage hole. Verified: methodTable overrides at
API/JSCallbackObject.h:211-225; raw JSClassRef callback
invocations in JSCallbackObjectFunctions.h (getProperty :175,
setProperty :282-306, deleteProperty :415, hasProperty :164,
initialize :139, convertToType :250, static-table arms); NA-I24/
SC1.3 scoped to GlobalObjectMethodTable.h:58-71 only; UNGIL-HANDOUT
R30 covers data maps only; grep over SPEC-api.md rev 14 confirms NO
C-API/JSClassRef clause — §2.4's "SPEC-api keeps C-API entry
carrier-bound anyway" was uncited and false, and hook invocation
needs no API entry anyway (shared-heap publication + spawned-thread
property access). The function-face/object-face inconsistency
confirmed (JSCallbackFunction is InternalFunction-backed, NA-I8).
FIX (reviewer option (a)): NA-I27 — NL-bracket the JSClassRef
callback invocation expressions (finalize EXEMPT-CITED: GC context,
NA-I10); NA-T7 eleventh family; NA-I24/SC1.3 amended to name the
family; §2.4 sentence deleted and §2.7 added. Option (b) (refusal)
charted NA-X7. FULL text ANNEX SC2.2.

R5.3 ACCEPTED (blocker): the handleHostCall C++ funnels and the
CallData-native-without-NativeExecutable callable family were
outside every coverage mechanism. Verified: direct
`callData.native.function(...)` calls at llint/LLIntSlowPaths.cpp
:2222/:2243 and bytecode/RepatchInlines.h:96/:117; tag hidden in
TaggedNativeFunction (NativeFunction.h:40) so no NA-T7 family
matched; family members ProxyObject.cpp:644/:703,
JSCallbackObjectFunctions.h getCallData/getConstructData,
JSCallbackConstructor.cpp:75; §2.4(b) wrong (JSCallbackFunction
`final : public InternalFunction`, API/JSCallbackFunction.h:37 —
NO NativeExecutable minted); §4.5 helper unimplementable for
executable-less callees (Interpreter.cpp:1320 native arm; reviewer
wrote :1319, drift). ADDITIONALLY verified: both funnels are
reached ONLY by non-JSFunction, non-InternalFunction callables
(LLIntSlowPaths.cpp:2263-2270, RepatchInlines.h:138-167), which
makes the unconditional disposition exact, not conservative
overreach. FIX (reviewer suggestion (b)-(e) adopted; (a)'s
enumeration recorded): NA-I28 — unconditional NL bracket at both
funnels + the specified executable-less helper arm; §2.4(b)
corrected to NA-I8 + JSCallbackObject named; NA-T7 tenth family.
FULL text ANNEX SC2.3.

R5.4 ACCEPTED (major): the §4.4.1b/d nullptr-generator RULEs
provably do not disable the intrinsic-call/getter bypasses, and
SC1.2's "alongside b" inherited the hole. Verified: intrinsic
passed to all three forCall arms regardless of generator
(JITThunks.cpp:271-279; reviewer :270-280, drift), no-JIT arm
VM.cpp:1441, admission chain DFGByteCodeParser.cpp:2095-2098 ->
ExecutableBaseInlines.h:43-54 -> NativeExecutable.cpp:90-92;
Locked intrinsics live at :2747/:2855/:3749/:3851 with
unshift/splice/assign/replace all outside the PT1 seed;
getter admission keys on the executable's intrinsic
(IntrinsicGetterAccessCase.cpp:50, InlineCacheCompiler.cpp:4473).
The spec as written affirmatively misled (a reader implementing
b/d would believe surface (a) closed). FIX (reviewer's mechanism
adopted): NA-I29 — gilOff m_concurrentOk admission guards at the
three sites, NA-I23 style; SC1.2 wording superseded; NA-T6
recharter (guards exist + witness). FULL text ANNEX SC2.4.

R5.5 ACCEPTED (major): the JSC_NATIVE_*/putDirectNativeFunction
macro layer — the PT1 seed's ACTUAL registration surface — had no
named policy-input mechanism; PT1's "Registration =" header was
wrong for most of its own rows. Verified: ObjectPrototype.cpp:
65-66, ArrayPrototype.cpp:107-124, JSObject.h:1007-1009; CITATION
CORRECTION: the macro block lives at JSObject.h:2212-2228, not
:2173-2189 (substance stands); Math/Atomics additionally register
via BARE putDirectNativeFunctionWithoutTransition calls
(MathObject.cpp:93ff, AtomicsObject.cpp:94) — slightly WIDER than
filed, folded into the same fix. Correctness was never at risk
(NA-I6 default) but the v1-critical opt-in route was unspecified.
FIX: NA-I30 — defaulted NativeConcurrency threaded through the
putDirect layer + JSFunction::create + macro variants; §2.1 gains
the third policy surface; PT1 header rewritten per-group. FULL
text ANNEX SC2.5.

R5.6 ACCEPTED (blocker): NL1 step (a) routed termination-class
traps into an NVS park with no completion path. Verified:
termination is VM-wide + sticky-until-own-§E.2-close with
quantum-poll delivery and NO conductor/resume (SPEC-ungil §A.2.4/
TERM1, SPEC-ungil.md:151-157 rule 4, :160-171 rule 6, :552-556;
ANNEX TERM1.2/1.3); NL1 step (a)'s "stop/trap" predicate +
step-(d) loop (history :43-52 pre-rev-6) therefore never reached
the CAS under either park semantics — hang or quantum livelock —
while NA-I12 (spec) and EX1's own termination paragraph mandated
COMPLETION: two BINDING texts in contradiction, the R2.6/R4.7 bug
class, and no charter covered terminate-vs-NL-waiter. FIX
(reviewer's split adopted verbatim): NL1 step (a) and the
step-(b) win-CAS re-poll split by trap class (STOP-CLASS parks,
validated against the stop word; TERMINATION-ONLY proceeds and
completes the acquisition, delivery at the §4 bracket's post-call
check); EX1 termination paragraph rewritten ("polls; parks only
if a stop is concurrently in progress"); NA-T4
termination-vs-NL-waiter arm. Rev-5 texts of record: NL1 (a) "if
this lite's stop/trap bit is set, park on the lite's OWN NVS
ticket per the standard §J.3 park protocol ...; on wake, run the
§A.3.2b post-wake poll BEFORE continuing"; EX1 "a termination
trap observed during the dtor's reacquire parks/polls per NL1
step (b) and then COMPLETES the reacquire".

R5.7 ACCEPTED (major; truncated as received): the NA-I5 policy
side-table lock had no §LK row, no U20 coverage, no pinned order
against JITThunks::m_lock, no holding discipline, no §9 gate.
Verified: §LK is the canonical U20-linted one-order table
(SPEC-ungil.md:867-925, "§LK canonical for U20 (r22 list)");
hostFunctionStub takes m_lock twice around NativeExecutable::
create (Lockers at jit/JITThunks.cpp:260/:284, create :282, both
sections under AssertNoGC; reviewer wrote ":259/:284", drift);
the consult's position among them was genuinely unpinned (NA-I3's
ctor-store sentence invited reading the consult INTO the ctor,
i.e. between the locked sections, creating an unexamined
leaf-under-leaf edge). FIX: SC2.6 pins the consult OUTSIDE
m_lock and OUTSIDE the ctor (true leaf; no allocation, no
acquisition, no park under it; never held across create/GC);
§1.3 gains the lock-row sentence; NEW gate §9.7 (SPEC-ungil §LK.7
leaf row, both-sides, SUPERSESSION-PENDING — blocks §1.3
enforcement-structure implementation).

Supersessions recorded this side: rev-5 §4.5 wasm exempt-cite +
"carrier JS-to-Wasm is not a host-native call" sentence + §3.3
single-family funnel claim (R5.1, NA-I26); rev-5 §2.4 "SPEC-api
keeps C-API entry carrier-bound anyway" sentence + NA-I24/SC1.3
silent absorption of the ClassInfo-methodTable family (R5.2,
NA-I27); rev-5 §2.4(b) "mints Locked executables UNCONDITIONALLY"
+ §4.5 "executable from the CallData/callee" recipe + NA-I17
"COMPLETE" surface set (R5.3, NA-I28); rev-5 §4.4.1b/d adequacy
implication + SC1.2 "alongside the §4.4.1b suppression" wording
(R5.4, NA-I29); rev-5 §2.1 two-mechanism policy-surface list +
PT1 "Registration =" header (R5.5, NA-I30); rev-5 NL1 step (a)
predicate + step (b) re-poll + EX1 termination paragraph (R5.6);
rev-5 §1.3 bare "leaf-rank lock" parenthetical (R5.7, SC2.6).
Counterparty-spec obligations remain gates, not edits made here:
§9.1-§9.4, §9.6 unchanged; NEW §9.7 (policy-lock §LK.7 leaf row);
the SC2.1 §I-refusal observation is recorded for the SPEC-ungil
owner, not actioned. No text outside
SPEC-nativeaffinity{,-history}.md was modified.

## Review round 6 (2026-06-07) -> rev 7

Seven findings received; two (the NA-I29 mis-typing) are duplicate
filings of one defect. Net: five distinct defects, ALL ACCEPTED
after verification against the tree; no refutations this round.
Full texts land in NEW ANNEX SC3 + NEW ANNEX ACQ1 + amendments to
CF1.2/SC2.4/SC2.6/TC1. Body re-measured under the 50000-byte cap
after edits.

R6.1 ACCEPTED (major): the NA-I5 conflict RELEASE_ASSERT silently
escaped on warm JITThunks cache hits. Verified:
JITThunks::hostFunctionStub (jit/JITThunks.cpp:253) early-returns
the live cached executable under m_lock (:262-267) without
reaching NativeExecutable::create (:282), so rev 6's
creation-pinned consult ("consulted by EVERY creation"; SC2.6(a)
"BEFORE the create call's allocation") never ran on the hit path;
same-pair conflicting re-registration asserted only after a GC —
GC-timing-, cache-state- and mode-dependent enforcement (no-JIT
arm VM.cpp:1440-1442 consults per call), the exact
non-determinism NA-I5's own rev-2 rationale excludes; CF1.2's
§5.2 flip-completeness consequence falsified; NA-T8's four arms
all went through create. Reviewer's no-§0.3-breach note confirmed
(the hit returns the first-policy executable). FIX (reviewer's
re-pin adopted verbatim): consult per REGISTRATION CALL at funnel
entry, hit or miss; determinism sentence gains GC-timing- and
cache-state-independence; NA-I1 cost row restated per
registration CALL; NA-T8 warm-hit arm. FULL text ANNEX SC3.1;
supersession notes in CF1.2 and SC2.6 (both sides within this
spec's own annexes). Rev-6 texts of record: §1.3 "written on
first registration, consulted by EVERY creation — all §2.1
funnels AND exempt-cited direct sites"; SC2.6(a) "the consult/
insert runs OUTSIDE JITThunks::m_lock and OUTSIDE the ctor,
BEFORE the create call's allocation".

R6.2 ACCEPTED (major; filed 2x): NA-I29's admission-guard
predicate was defined over a field that does not exist for the
ScriptExecutable half of handleIntrinsicCall's callee population.
Verified: ExecutableBase::intrinsic() dispatches
`isHostFunction() ? uncheckedDowncast<NativeExecutable> :
uncheckedDowncast<ScriptExecutable>`
(runtime/ExecutableBaseInlines.h:43-48); the builtin-JS
fall-through is commented at the guard site itself
(dfg/DFGByteCodeParser.cpp:2103-2104); SC2.4's dispatch trace
cited only the NativeExecutable arm. Both natural literal
implementations defective (garbage-byte downcast = UB that PASSES;
defensive bail = parser-wide builtin-intrinsic deopt in exactly
the gilOff milestone config), and no charter saw either (NA-T6
checked only the does-bail direction). Builtin intrinsics need no
NA bit — their inlined semantics are JS, governed by core
SPEC-ungil/SPEC-jit; §0.1 scopes this spec to native BODIES. NOT
re-litigation of R5.4: the admission-side mechanism stands; its
predicate was under-typed. FIX: NA-I29 predicate re-typed — bail
IFF isHostFunction() AND byte clear; ScriptExecutable-carried
intrinsics explicitly OUT OF SCOPE and admitted; getter arms keep
their wording with the host-only-population note, guard written
against the same discriminator; NA-T6 builtin-still-inlines
control arm; gate §9.2(e) wording updated. FULL text = SC2.4
rev-7 note. Rev-6 text of record: "BAIL unless the callee
NativeExecutable's m_concurrentOk byte is set".

R6.3 ACCEPTED (major): the acquire side of NL had no pinned C++
helper form — the R4.5 eager-TLS-load defect class recurred
across four surfaces (§2.6, §2.7, §4.5, NA-I28), since §4.1's
pseudocode begins with the lite byte and the only current-lite
source at a C++ funnel is the TLS slot t_currentVMLite
(runtime/VMLite.cpp:67; accessor VMLite.h:333-345). NA-I1 stated
the cost constraint with no annex saying HOW, and nothing pinned
the release point relative to C++ exception handling. Verified
not re-litigation: no round decided to leave the acquire form
open; R4.5's fix covered only NativeLockDropScope. FIX: NEW
BINDING ANNEX ACQ1 — RAII NativeLockBracket (default +
byte-keyed ctor forms; level-0 gilOffProcess gate FIRST; lite
resolve only on the gated path; dtor releases at the invocation
expression's scope close, BEFORE the funnel's exception branch —
NA-I14 by construction); NEW spec clause NA-I31 cites it from
§2.6/§2.7/§4.5/NA-I28; §4.1's lite-first ordering scoped to
EMITTERS only.

R6.4 ACCEPTED (major): NA-I1's funnel enumeration and NA-T9(d)'s
oracle were not restated when rev 6 added the §2.7/NA-I27 and
NA-I28 always-compiled bracket surfaces — a cost regression at
those funnels violated NA-I1 with no observing test, and the
normative index did not say whether the NA-I27 brackets were
bound by NA-I1 at all. FIX: NA-I1 restated "per
EX1/§2.6/§2.7/§4.5(incl. NA-I28) funnel" (one predictable
gilOffProcess branch, zero TLS/lite/depth loads flag-off/GIL-on);
NA-T9(d) extended to the full acquire-side funnel set (TC1
amendment carries the rationale). Supersession recorded both
sides (spec NA-I1 row + TC1 NA-T9(d)). Rev-5 text of record:
"one predictable gilOffProcess branch per EX1/§4.5/§2.6 funnel";
"a C++ branch-count/TLS-load oracle at the EX1 funnels".

R6.5 DUPLICATE of R6.2 (the same NA-I29 mis-typing filed a second
time, with the ExecutableBaseInlines.h:43-48 and
DFGByteCodeParser.cpp:2103-2104 cites; consolidated into the R6.2
disposition — its getter-arm same-discriminator suggestion is
adopted there).

R6.6 ACCEPTED (major): §4.4's coverage premise was false for the
DFG/FTL isDirect direct-native-call emitter; §4.4.2's bracket
language was permissive ("MAY"), ungrounded; and TC1's
vmEntryHostFunction exempt-of-record was false for two of its
four callers. Verified in-tree: DFGSpeculativeJIT64.cpp:999-1062
emits `callOperation<HostFunctionPtrTag>(nativeFunction)` (:1033)
/ `callOperation<OperationPtrTag>(vmEntryHostFunction)` (:1031,
JITCage) for known host callees with NoIntrinsic, no thunk, with
loadException at :1041-1043; FTL mirror
FTLLowerDFGToB3.cpp:14749-14796 (:14787/:14789, check
:14794-14796); the path is in no NA-I16 union member. The R3.3
bug shape recurring on the remaining direct-call arm. FIX: §4.4
intro rewritten (covered EXCEPT NA-I16 union AND the isDirect
emitter, both arms cited); §4.4.2 made NORMATIVE MUST with NA-I14
placement; TC1 exempt split per caller + vmEntryHostFunction
joins the generated symbol check; gate §9.2(a) gains the two
sites. FULL walk ANNEX SC3.2. Rev-6 texts of record: §4.4 intro
"DFG/FTL calls to host functions land on the NativeExecutable's
call thunk (§4.3) — covered EXCEPT the NA-I16 union below";
§4.4.2 "the compiler MAY fold the §4.1 byte tests and either omit
the bracket (bit=1) or emit unconditional acquire/release calls
around the direct native call (bit=0)"; TC1 "`vmEntryHostFunction`
is INSIDE the §4.3 bracket".

R6.7 ACCEPTED (major): gate §9.3(a) still asked the SPEC-api
owner to land "C API mints Locked executables (§2.4)" — the exact
claim rev 6's own §2.4/SC2.3 correction superseded as false
(JSObjectMakeFunctionWithCallback -> JSCallbackFunction, `final :
public InternalFunction`, API/JSCallbackFunction.h:37 — no
NativeExecutable minted). Executing §9.3 as written would have
recorded a superseded-false statement both-sides in the
counterparty spec, and left the rev-5->rev-6 §2.4 supersession
with no corrected landing target on the SPEC-api side. FIX:
§9.3(a) rewritten to the rev-6 §2.4 content (no NativeExecutable
minted; NA-I8/NA-I27/NA-I28 serialization; finalize exempt-cited;
no C-API opt-in, NA-X3 post-v1). §9.3(b) unchanged. Rev-6 text of
record: "SPEC-api notes: (a) C API mints Locked executables
(§2.4)".

Supersessions recorded this side: rev-6 §1.3 creation-pinned
consult + SC2.6(a) before-create pinning + CF1.2 flip-completeness
sentence (R6.1, SC3.1); rev-6 NA-I29 predicate (R6.2, SC2.4
rev-7 note); §4.1's implied universal lite-first ordering (R6.3,
ACQ1/NA-I31); rev-5 NA-I1 funnel enumeration + NA-T9(d) EX1-only
scope (R6.4); rev-6 §4.4 intro coverage sentence + §4.4.2 "MAY"
+ TC1 vmEntryHostFunction blanket exempt (R6.6, SC3.2); rev-6
gate §9.3(a) text (R6.7). Counterparty-spec obligations remain
gates: §9.1-§9.4, §9.6, §9.7 unchanged in kind; §9.2(a)/(e) and
§9.3(a) wording updated to the corrected clauses. No text outside
SPEC-nativeaffinity{,-history}.md was modified.

Size-cap actions this round (content normative, no semantic
change): §3.5's full LK.1c row text MOVED to ANNEX BL1.7 (body
keeps the index); several body rationale parentheticals compressed
to annex pointers (their full texts already lived in
CF1.2/SC1.4/SC2.1/SC2.2/SC2.3/NL1/EX1/PT1/TC1). Body measured at
49994/50000 bytes post-edit.

## Review round 7 (2026-06-07) -> rev 8 — SPEC-congc cross-document pass

Source: the cross-document review of the two thread-specs2 drafts
(this spec + SPEC-congc) against each other and the frozen family.
One finding lands here (R7.1); the other two findings of that pass
(the §LK lock-table fork and the frozen-§A.3 recording defect) are
SPEC-congc-side defects, fixed in congc rev 8 (its F41/F42) and
cross-cited from §3.5/§9.1 here.

R7.1 (major — falsified grounding, ACCEPTED): NA-I13's exemption
(b) / ANNEX BL1.6 let a Locked native reach a SYNC COLLECTION
mid-body WITH NL held, on a walk derived against the landed
single-window heap §10 conduct. SPEC-congc §3 replaces that
conduct with an N-window tenure (per-window blocking GCL
re-acquires, GBL barriers, between-window condvar waits, the F28
handoff, the `:4955` tail) — shapes BL1.6 never walked. An
NL-holding mutator-conductor would serialize every Locked native,
custom accessor, JSClassRef callback and handleHostCall funnel
process-wide for a whole concurrent cycle. Verified (reviewer +
re-walk): NOT a deadlock — NL waiters are §A.3-compliant park
sites, F8-reverting at each WND-open (NL1/BL1.1); marking
termination needs nothing from NL waiters — so a liveness/
grounding gap. Also: ZERO cross-references existed between the
two new specs; congc CG-I19 was silent on NL.

DISPOSITION (option (a) of the finding's two — forbid; option
(b), re-deriving BL1.6 for the windowed protocol, REJECTED in
congc CGD6.1: cycle-length stall stands even if sound, walk
re-derives on every window-model change, CG-I19's closed loop
already excludes every other foreign obligation): NA-I13
exemption (b) NARROWED to §A.3 (single-window) conductors/losers;
NEW ANNEX BL1.8 — NL drop bracket around the sync-collection
request funnel (NA-I11-style depth save/full release, park-capable
reacquire after conduct tail/follower resume, NA-I12 exception
discipline, level-0 mode gate); BL1.6 head note + LK.1c (BL1.7)
conductor-HOLD bullet narrowed; NA-T4 rev-5 sync-collection
sub-arm superseded, rev-8 multi-window arm added (composes congc
CGT1.1 F40); §9.1 gate text updated (NARROWED clause; watchdog
budget stated once, congc CGS2.3).

Supersessions recorded this side: BL1.6 sync-collection leg ->
BL1.8 (both sides: congc CG-I19/§3.7/CGD6.1/§13.5(4)); NA-T4
rev-5 sync-collection sub-arm -> the rev-8 arm (TC1). Recorded
counterparty-side (congc rev 8): the §LK LK.9c/9d rows + U20
extension (their F41 — closes the lock-table fork this spec's
§3.5/§9.7 convention exposed; LK.1c's "OUTER to ... all leaves"
re-grounded against the new congc leaves, congc CGS2.2); the
frozen-§A.3/HBT4/U32 pending rows (their F42, congc CGS2.3-4).
Counterparty obligations remain gates: §9.1-§9.7 unchanged in
kind; §9.1 gains the rev-8 NARROWED clause + coordination note.
No text outside the four SPEC-{congc,nativeaffinity}{,-history}
files was modified.

Size-cap actions this round (content normative, no semantic
change): §3.2(2), NA-I22, §1.3 SC3.1/CF1.2 parentheticals, §2.6
intro, NA-I29 parenthetical, NA-T6/NA-T9 index entries and the
§3.3 funnel parentheticals compressed to annex pointers (full
texts already in NL1/BL1.3/SC3.1/CF1.2/SC1.4/SC2.4/TC1/EX1).
Body measured at 49997/50000 bytes post-edit.

## Rev 9 record (2026-06-10) — SPEC-congc §13.5 adoption-gate
## closure (gate (4) this side; gates (1)-(3) = SPEC-ungil
## rev 33). NOT a review round; no findings; solo amender.

AUTHORITY: CONGC-HANDOUT.md §0 gates (1)-(4) +
SPEC-congc-history.md ANNEX CGS2 (rows CGS2.1-CGS2.4,
SUPERSESSION-PENDING at congc rev 12). Landed as ONE coherent
change with SPEC-ungil rev 33 (its history REV 33 record + ANNEX
CGS2A carry the adopted row texts and the cite-anchor refresh
ledger). The congc files are NOT edited from here; per the §9 /
congc §13.5(5) convention the rows read RECORDED-BOTH-SIDES
through these two records' explicit back-cites.

What this rev records, per gate:

- Gate (4) (congc §13.5(4) — BLOCKS C1 in gilOff configs):
  BL1.8's NL DROP around the sync-collection request funnel +
  congc CG-I19's `m_nativeLockDepth == 0` conducting-entry assert
  (election win / poll grant / conductSharedCollection entry;
  debug) now read RECORDED-BOTH-SIDES: this side = ANNEX BL1.8
  [r9] note + spec §3.4/NA-I13(b) [r9] notes; congc side =
  CG-I19/CGD6.1/§13.5(4) (pre-existing); ungil side consumes the
  GC-CONDUCT NL>GCL edge removal in §LK row 9d (CGS2A.2
  back-cite). F40 closed in kind.
- Gate (2) shared story (congc §13.5(2), CGS2.3 — BL1.6/BL1.8
  adjacent): the conductor wait BUDGET stays stated ONCE in congc
  ANNEX CGS2.3; it is now LANDED ungil-side (SPEC-ungil §A.3
  rule 5 WAIT BOUND, [r33]) and is STRUCTURAL per congc F45
  (§9.1(2a) foreign-waiter fairness, CG-I26), no longer
  probabilistic. BL1.6's bounded-bracket license and NA-T4's
  watchdog arm re-point at the landed, structural form (spec §8
  [r9] note: the arm asserts the bound, not samples it). NA-I10's
  conductor-exclusion negative edge is unchanged and is what
  keeps the nativeaffinity NL term of the budget ZERO.
- Gates (1)/(3) (congc §13.5(1)/(3)): no nativeaffinity-side
  obligation; recorded here for the one-story stamp — SPEC-ungil
  rev 33 lands §LK rows 9c/9d + the U20 extension (CGS2.1-2; the
  CGS2.2 chain walk cites BL1.6/NA-I10/BL1.8 exactly as this
  spec states them) and the HBT4 window-re-entry extension
  (CGS2.4(b)). §3.5/BL1.7's LK.1c row, the §LK.4b held-with
  amendment and the SPEC-api §5.9 companion row are NOT landed by
  this — §9.1 REMAINS OPEN (spec §9.1 [r9] note); the row 9d
  text cites LK.1c as a pending nativeaffinity row.

Spec-body deltas (rev 9): header restamp; §3.4 [r9] gate-closed
note (BL1.8 paragraph); §9.1 [r9] status note (congc §13.5(1)-(3)
closed; LK.1c et al. still open); NA-I13 INV row [r9] note; NA-T4
[r9] structural-budget note. Cite hygiene: the two drifted
SPEC-ungil.md line cites re-anchored via the ungil r33 ledger
(§A.3.8 F8 revert :289-298 -> :311-321; §LK table :867-925 ->
:834-915); all other SPEC-ungil.md:NNN cites in body+annexes
re-read through that ledger (SPEC-ungil-history.md REV 33,
"Cite-anchor refresh ledger").

Size-cap actions this rev (no semantic change; full texts in the
named BINDING annexes): §3.1 rationale (round-1 R1.1 text), §3.2
intro (NL1), §3.3 NA-I26/NA-I12 cite parentheticals (EX1/SC2.1),
§2.1 macro/wasm bullets (SC2.5/CF1.1), §2.6 NA-I20 (SC1.4), §2.7
NA-I27 (SC2.2), §4.4.1 d/e-f + NA-I29 (SC1.1/SC1.2/SC2.4).
Body measured at 49775/50000 bytes post-edit.

No SDs; no INV renumbering (NA-I* IDs frozen); NA-X* unchanged.
Mode gating: every [r9]-referenced behavior is flag-gated
useConcurrentSharedGCMarking on the congc side and gilOffProcess
on this side; flag-off/GIL-on unchanged (NA-I1; congc CG-I0).

## Rev 10 record (2026-06-10) — ANNEX EX1 cite restore (external
## review finding F-C, REAL; solo amender, no review round) +
## SPEC-ungil r34 coordination note

F-C (major, REAL — verified before ruling): the rev-9 size-cap
action list claimed "§3.3 NA-I26/NA-I12 cite parentheticals
(EX1/SC2.1)" — i.e. the trimmed NA-I12 full text lives in ANNEX
EX1 — but EX1 only paraphrases the mechanism ("return value +
per-lite m_exception word, never by C++ unwinding") and carried
NEITHER file:line anchor NOR the loadException symbol; the two
anchors appeared in none of the four
SPEC-{ungil,nativeaffinity}{,-history}.md files. Under the
frozen-spec convention (normative clauses cite file:line; trimmed
full text must stay in the cited annex), the verification anchors
were lost and the body's "(cites: ANNEX EX1)" pointer dangled.
FIX = the EX1 amendment below (history form chosen — the body
keeps its pointer, which now resolves; the rev-9 size-cap action
list stands as written; no body byte change beyond the header
restamp).

## ANNEX EX1 AMENDMENT (r10; BINDING; evidence note under the
## destructor's NA-I12 paragraph)

The NA-I12 verification anchors trimmed from spec §3.3 at rev 9,
restored verbatim (rev-8-baseline anchors, internally consistent
with the body's §4.2/§4.3 anchors :3161-3219 / :535-536):

  per-lite GIL-off m_exception: LLInt pending-exception check
  LowLevelInterpreter64.asm:3199-3217; thunk loadException
  ThunkGenerators.cpp:524-536.

Present-tree drift note (2026-06-10 tree; for the next
cite-refresh ledger, congc CGD7.4 pattern — anchors re-verified
against the tree this rev): nativeCallTrampoline now
LowLevelInterpreter64.asm:3240-3298 with the per-lite
`.checkLiteException` arm at :3278-3296 (the
internalFunctionCallTrampoline mirror at :3331-3344); the thunk
loadException comment+load now ThunkGenerators.cpp:547-556.

## SPEC-ungil r34 coordination note (no nativeaffinity
## obligation; recorded for the one-story stamp)

SPEC-ungil rev 34 (its REV 34 record + ANNEX CGS2A [r34]
markers) splits the conductor wait story: the congc CGS2.3
windowed sum bounds ONE WINNER's GCL leg (structural per
§9.1(2a)/CG-I26), while the LANDED 30s watchdog budget is
per-REQUESTER end-to-end and adds an explicit queue term
(k earlier winners x (GCL leg + full stop window), k <=
supported fan-in; congc-counterpart pending for the CGS2.3
ledger + CG-T8 storm arm). BL1.6's bounded-bracket license, the
§3.4/NA-I13 rows and NA-T4's watchdog arm all cite the
per-WINNER GCL leg and are UNAFFECTED; NA-I10's
conductor-exclusion negative edge still zeroes the NL term in
BOTH quantities (per-winner sum AND queue term). The ungil r34
WATCHDOG COVERAGE obligations (timed sampled pause,
blocking-ctor requestStart, VM-threaded watchdog ctor) name no
nativeaffinity surface — NL never participates in the stop-scope
bracket (NA-I10; BL1.8 drop).

Spec-body deltas (rev 10): header restamp only. Body measured
49844/50000 bytes post-edit. No SDs; NA-I*/NA-T*/NA-X*
unchanged; §9 gate table unchanged (§9.1 REMAINS OPEN as at r9).

## Rev 11 record (2026-06-10) — BL1.8 reacquire re-pin (external
## composition-review finding, REAL — the ungil REV 35 round's
## G-C; solo amender, no review round) + SPEC-ungil r35
## coordination note

FINDING (verified against the tree before ruling): ANNEX BL1.8
item 2 anchored the park-capable NL reacquire "AFTER the
conduct's access-reacquire tail (`Heap.cpp:4955`, conductor
case)". In the present tree that tail is
`conductorClient.acquireHeapAccess()` at `Heap.cpp:5031`, which
executes INSIDE `conductSharedCollection` while the CALLER still
holds GCL — the §10.2 election releases GCL only afterwards
(`Heap.cpp:4606`; poll tail :4669/:4673), and under congc F23
the FINAL WND-close likewise leaves GCL held through the tail
(SPEC-congc §3.2). The literal anchor therefore licensed an
implementation that re-acquires NL in the window
[post-:5031, pre-:4606] — a heap-rank-2 (GCL) holder ACQUIRING
NL, directly contradicting NA-I10 (the negative edge that IS the
acyclicity proof of congc CGS2.2 / ungil CGS2A.2) and closing
the ONE constructible cycle in the rebuilt merged table:
  T1 = a BL1.6 §A.3 conductor holding NL, blocked in its
       HBT4-bracket GCL acquire (`Heap.cpp:5568-5590` tryLock
       loop) on T2's GCL;
  T2 = a BL1.8 sync requester holding GCL post-final-close,
       parked in the NL reacquire behind T1.
Outcome: deterministic 30s watchdogAssertStopProgress fail-stop
(`JSThreadsSafepoint.cpp:512`) — loud, not silent, but a real
deadlock from BINDING text whose congc §13.5(4) adoption gate
CLOSED at rev 9, and no assert covered the bad reading (congc
CG-I19's `m_nativeLockDepth == 0` fires at conducting ENTRY
only, never at the reacquire site).

RULING (= ANNEX BL1.8 in-place [r11] edits, BINDING):
- Item 2 RE-PINNED: reacquire AFTER the funnel's CALLER-SIDE GCL
  release (election `Heap.cpp:4606`; poll tail :4669/:4673; the
  F28 successor's final release), with the NORMATIVE sentence
  "the §3.2 reacquire loop runs holding NO heap rank >= 2 lock"
  — textually equivalent to NA-I10 instead of in tension with
  it. The follower-resume arm is unchanged (followers never
  touch GCL, HBT4.3).
- NEW item 7: NL-acquire-side debug assert (acquiring thread
  holds no heap rank >= 2 lock, m_gcConductorLock foremost) +
  the U20 NL-edge lint obligation; this is the guard CG-I19
  structurally cannot provide.
- Stale anchor refreshed: `Heap.cpp:4955` -> `:5031` at both
  BL1.8 sites (preamble tail cite; item 2, kept as the
  bracketed rev-8 provenance).
- BOTH-SIDES: recorded HERE with explicit back-cites to congc
  CGD6.1 / CG-I19 / ANNEX CGS2.2 (the reversed-direction
  convention — the congc history is not edited from this
  workflow; its CGS2.2 acyclicity walk is RE-GROUNDED by this
  re-pin, as is ungil §LK row 9d's "GC-conduct NL>GCL edge
  REMOVED" clause; ungil-side consumer note = SPEC-ungil-history
  REV 35 G-C). No congc-side text change is REQUIRED — CG-I19
  and CGD6.1 stand as written; the defect was this annex's
  anchor, not their assert.
- Spec body §3.4 (BL1.8 index paragraph) re-pinned to match;
  NA-T4's TC1 multi-window arm inherits the re-pin (the arm
  exercises the funnel; no charter text change).

SPEC-ungil r35 coordination note: the r10 coordination note's
"k <= supported fan-in" clause re-reads per ungil REV 35 G-B —
the queue term is a COUNT bound, 'supported fan-in' RETIRED
(never defined), the per-requester 30s fail-stop is the sole
TIME bound, and the CG-T8 storm arm is attribution-only. This
changes no nativeaffinity quantity: NA-I10 still zeroes the NL
term in both the per-winner sum and the queue term. Cite
hygiene: SPEC-ungil.md anchors re-read through the r35
cite-anchor refresh ledger (SPEC-ungil-history REV 35; covers
the r34+r35 moves — r34 shipped no ledger): the two body cites
re-anchored at r9 re-resolve :311-321 -> :314-323 (§A.3.8) and
:834-915 -> :837-917 (§LK table); both updated in the body this
rev at zero net byte cost.

Spec-body deltas (rev 11): header restamp; §3.4 BL1.8 index
paragraph re-pinned ([r11]); the two r9-re-anchored SPEC-ungil
cites refreshed to the r35 ledger values. Body measured
49979/50000 bytes post-edit. No SDs; NA-I*/
NA-T*/NA-X* IDs unchanged; §9 gate table unchanged (§9.1 REMAINS
OPEN as at r9; the congc §13.5(4) closure STANDS — this rev
amends the closed record's mechanism text both-sides-style, it
does not reopen the gate). Mode gating unchanged (gilOffProcess
/ useConcurrentSharedGCMarking; flag-off/GIL-on dead, NA-I1 /
congc CG-I0).
