# map-MC-SPEC — Speculative side channel enabled by shared memory

Status: surface map, 2026-06-07; line citations re-verified 2026-06-15;
B15 embedder-obligation disposition LANDED 2026-06-18 (INTEGRATE-api.md 9.2-11). Defensive audit artifact for `--useJSThreads`
(jarred/threads). Class definition: docs/threads/CVE-AUDIT.md "MC-SPEC" (merged from
JS-SP; exemplars Spectre v1/v2, CVE-2017-5753 / CVE-2017-5715; industry response = SAB
disabled Jan 2018, re-enabled only under cross-origin isolation).

Mechanism, restated for our engine: speculative execution bypasses an architectural
check (bounds compare, type check, branch), transiently reads memory the architectural
path would never return, and encodes the secret into microarchitectural state (cache
lines). The *enabling capability* is a high-resolution, no-permission clock to read
that state back — and a second thread spinning on a shared counter IS that clock,
with resolution finer than any performance.now()-style API. This class is therefore
STRUCTURAL: it is not a bug in any one check, it is a property of running attacker
JS in the same address space as secrets while handing it a timer. CVE-AUDIT's framing
is exact: "`--useJSThreads` is a capability grant — it hands a shared-memory clock to
all code in the process."

Two consequences frame every verdict below:

1. **No invariant in any frozen SPEC addresses misspeculation.** The SPECs reason
   exhaustively about *architectural* interleavings (seq_cst orders, acquire pairs,
   re-validation loops). All of that machinery executes — architecturally. A
   transient execution window retires nothing, fires no fences' security intent
   (fences ARE speculation barriers on some cores, but none of ours are placed for
   that purpose), and rolls back every "check" while leaving the cache state behind.
   "Immune-by-construction" via a protocol citation is therefore NOT AVAILABLE for
   any data-reachability surface in this class — only for capability-gating claims.
2. **Severity is relative to the embedder baseline.** Bun is a server runtime: code
   in the process already has `process.hrtime`, Workers, and (embedder-flagged) SAB.
   Browsers' 2018 SAB lockdown defended a boundary (cross-origin JS in one process)
   that does not exist by design here — the engine does not claim to confine
   in-process JS from in-process memory. Per CVE-AUDIT cross-cutting rule 1 this caps
   severity, it does not dismiss the class: the audit's job is to make the grant
   EXPLICIT so no embedder ships `--useJSThreads` to semi-trusted code (plugins,
   user-supplied lambdas, multi-tenant sandboxes) believing the JS sandbox holds.

Verdict key: **immune-by-construction** (protocol cited, adversarial argument given) /
**needs-test** (test written under JSTests/threads/cve/, executed post-ungil) /
**susceptible-suspected** (precise suspected hole).

---

## S1 — The timer grant: Atomics-on-properties + Thread spawn

**Where:** `Source/JavaScriptCore/runtime/AtomicsObject.cpp:239` (RMW/load family
routes non-view objects to the property-atomics path under `useJSThreadsEnabled()`),
`:438` (store), `:558/:625/:679/:770` (wait/waitAsync/notify/isLockFree entry points);
`Source/JavaScriptCore/runtime/ThreadAtomics.cpp:403-404/:793-795` (dispatch into the
OM §9.5 lock-free slot accessors, `ConcurrentButterfly.cpp:3063` `atomicSlotLockFreeLoop`);
`Source/JavaScriptCore/runtime/JSGlobalObject.cpp:2142` (`Thread`/`Lock` globals
under `Options::useJSThreads()`, `OptionsList.h:691`).

**Governing spec:** SPEC-api §4.5 (Atomics-on-props semantics) and §2 (gating).
Neither section — nor any other SPEC — records that §4.5 constitutes a timing
capability. SPEC-objectmodel §9.5 makes the property-atomic fast path deliberately
lock-free, i.e. deliberately FAST: a cross-thread counter increments at near
cache-coherency rate (~tens of ns per seq_cst RMW), giving a clock 4-5 orders of
magnitude finer than a 100µs-coarsened browser timer.

**Adversarial walk:** disable `--useSharedArrayBuffer` (the lever browsers pulled in
2018) and the channel is UNCHANGED: a spawned `Thread` captures any plain shared-heap
object, spins `Atomics.add(o, "c", 1)`, and the observer reads `Atomics.load(o, "c")`.
No SAB, no typed arrays, no permissions. The 2018-era mitigation lever does not exist
for this engine: the timer is inherent to the Thread API's reason for existing.
Coarsening or jittering property-atomics would break SPEC-api §4.5 semantics (and be
defeated by counting anyway — a free-running counter thread is the one clock jitter
cannot blunt, which is WHY SAB was the thing browsers removed).

**Verdict: needs-test** — not for a hole, for the grant itself.
`JSTests/threads/cve/mc-spec-timer-capability.js` is a capability WITNESS, not a
failure detector: it (a) deterministically asserts the gating shape of S2 below
(`--useJSThreads=1 --useSharedArrayBuffer=0` ⇒ `Thread` present, `SharedArrayBuffer`
absent), and (b) demonstrates and quantifies the counter-thread clock (asserts it
ticks between back-to-back property-atomic loads; reports ticks/ms vs `Date.now`).
If a future change ever coarsens or gates this path, the witness fails and forces the
SPEC conversation. The structural susceptibility itself is acknowledged here, not
"fixed": the only true mitigation is the embedder rule in the closing section.

---

## S2 — Capability gating: `useJSThreads` vs `useSharedArrayBuffer`

**Where:** `Source/JavaScriptCore/runtime/OptionsList.h:691` (`useJSThreads`,
default false) vs `:712` (`useSharedArrayBuffer`, default false);
`JSGlobalObject.cpp:2139` (SAB constructor exposed ONLY under
`Options::useSharedArrayBuffer()`) vs `:2142` (Thread API exposed ONLY under
`Options::useJSThreads()`). Caveat site: `Source/JavaScriptCore/jsc.cpp:4147` — the
SHELL force-enables `useSharedArrayBuffer` in its `Options::initialize` default
block, so every shell/test run has SAB unless a later `--useSharedArrayBuffer=0`
overrides it (runtime flags parse after the default block, so the override works —
the S1 test relies on this).

**Governing spec:** SPEC-api §2 (scope/gate split, rev 14).

**Verdict: immune-by-construction** — for the NARROW claim only: enabling
`--useJSThreads` does not implicitly flip `useSharedArrayBuffer`. The two options are
independent bools with independent read sites and no cross-assignment anywhere in
`Source/**` (the only assignment to `useSharedArrayBuffer` outside option parsing is
the shell default at `jsc.cpp:4147`, which is embedder code, not engine code — Bun
sets its own policy). Adversarial check: `JSGlobalObject` init reads the two flags in
separate, adjacent, unconditional `if`s; no test configuration can make one imply the
other. BUT — be adversarial with ourselves — this immunity is security theater if
read as a mitigation: per S1, hiding SAB removes nothing. Its real value is API
hygiene (an embedder reasoning about "did I grant SAB semantics?" gets a straight
answer) and keeping the S1 witness test deterministic.

---

## S3 — Spectre v1: speculative bounds bypass on shared-heap loads

**Where (representative, not exhaustive — that is the point of a structural class):**
- DFG bounds checks: `Source/JavaScriptCore/dfg/DFGSpeculativeJIT.cpp:2667/:2757/:2870`
  (`speculationCheck(OutOfBounds, ...)` = conditional branch to OSR exit, then the
  load; no index masking between branch and load).
- FTL: `Source/JavaScriptCore/ftl/FTLLowerDFGToB3.cpp:6919-6927` (`compileCheckInBounds` —
  `speculate(OutOfBounds, ..., m_out.aboveOrEqual(index, bound))`; the comment "users
  of this node just need to maintain that we dominate them" is an *architectural*
  dominance claim; transient execution does not respect dominance).
- C++/runtime segmented-butterfly clamp: `runtime/ConcurrentButterfly.cpp:2720/:2750`
  (`bound = std::min(publicLength, vectorLength)` — the `std::min` is data-flow, fine;
  the loop compare against `bound` is a predictable branch) and `:3410`
  (`if (index >= vectorLength || index >= publicLength)`);
  flat fast paths `runtime/JSObject.h:420` (`canGetIndexQuickly`).
- Engine-wide mitigation inventory: **empty.** `grep -ri spectre Source/` finds
  nothing; upstream WebKit removed JSValue poisoning and butterfly index masking
  years ago, and this fork inherits that posture. There is no
  `usePoisoning`/index-masking option to turn on.

**Governing spec:** SPEC-objectmodel C4/I33 is the closest thing — and the
adversarial honesty required here is that I33 is an *architectural race-correctness*
bound (its own rationale: "[vectorLength, publicLength) reads as holes (SAB
staleness)"). It bounds what a racing reader RETIRES, not what it transiently
touches. Same for SPEC-jit's R5/§Write fast-path tag checks: hoistable compare/branch
sequences, i.e. ideal v1 gadget shapes (attacker trains the in-bounds/owner-TID
branch, supplies the out-of-range index, transient load proceeds).

**What `--useJSThreads` changes vs stock JSC:** stock JSC post-2020 accepts v1
in-process and defends the PROCESS boundary (site isolation in browsers; one trust
domain in servers). Two things move under our flag: (1) the S1 clock makes the
readback leg practical in-process with no embedder-visible API use; (2) the shared
heap server (SPEC-heap §2 — N mutators over ONE `JSC::Heap`) means a transient OOB
from thread A's butterfly lands in a heap that *by design* also holds every other
thread's objects — there is no "my worker's heap vs your heap" distance. Per
CVE-AUDIT rule 4, sharing concentrates the blast radius.

**Verdict: susceptible-suspected** — structural, inherited, and not fixable by a
patch this audit could demand; the precise statement of the "hole": every
speculatively-reachable bounds/type check on the shared heap is branch-only, the
tree contains zero speculation-hardening primitives, and the threads flag supplies
the missing attacker capability (clock) within the process. NOT mapped to a JS test:
a deterministic test would be a working Spectre PoC against our own engine —
exploit tooling, out of scope for a defensive artifact, and hardware/μarch-dependent
besides. Disposition instead: (a) the embedder rule below; (b) flagged for the
thread-scanners pass to inventory speculation-reachable load sites; (c) cheap
hardening recommendation: where the I33/C4 clamps already compute `std::min`, prefer
branchless clamp-and-load (cmov/`umin` on the INDEX, not just a guarded branch) on
the runtime paths — near-zero cost, kills the classic v1 gadget shape at the sites
this project newly added.

---

## S4 — Cage coverage under one shared heap

**Where:** `Source/bmalloc/bmalloc/BPlatform.h:476-482` (`GIGACAGE_ENABLED` only on
Darwin/Linux, x86_64/arm64-LP64, and `!BUSE(MIMALLOC)` — Windows builds and
mimalloc-based musl builds have NO cage); `Source/bmalloc/bmalloc/Gigacage.h:48-49`
(the only kind is `Primitive`); `Source/JavaScriptCore/runtime/ArrayBuffer.cpp:660-662`
(ArrayBuffer/typed-array payloads allocate from `Gigacage::Primitive`).

**Governing spec:** SPEC-heap §4-5 (heap server; per-thread allocators all carve from
the same server heap). No SPEC section places shared-object payloads in a cage.

**Adversarial walk:** Gigacage's promise is that a typed-array-relative speculative
(or architectural) OOB stays inside a region holding only primitive payloads — no
pointers, no structures, no code addresses. That promise still holds here for
typed-array bases, unchanged by threads. What the threads project ADDS is entirely
outside the cage: butterflies, segmented spines/fragments (GC auxiliary, butterfly
subspace — SPEC-objectmodel I25), cell memory, JIT'd-code data pointers — one
address space, one heap server, N mutators. A v1 gadget rooted at a BUTTERFLY bounds
check (S3) was never cage-protected in stock JSC either; the delta is again
concentration: pre-threads, "another context's secrets" usually meant another
process; post-threads they are guaranteed co-resident.

**Verdict: susceptible-suspected** — structural amplification, no discrete hole. No
JS test (same PoC objection as S3). Disposition: documentation of the platform
matrix is the deliverable — embedders on Windows/musl-mimalloc should know they run
ZERO cage; and any future "shared typed-array fast path" work must keep allocating
from `Gigacage::Primitive` (a regression here is testable by the scanners pass, not
from JS).

---

## S5 — Spectre v2: indirect-branch surfaces in JIT code

**Where:** every JIT-emitted indirect transfer — polymorphic call stubs, IC chains
(`jit/Repatch.cpp`), vtable dispatch in the runtime, `JITCode` entry thunks. No
retpoline emission exists anywhere in the MacroAssembler layer (grep: no
retpoline/IBRS artifacts in `Source/**`).

**Governing spec:** none — correctly so; no SPEC could fix this in software we own.

**Adversarial walk:** v2 (branch target injection) is cross-context BTB poisoning.
Threads of one process share the BTB trivially — but they shared it BEFORE this
project too (any two VMs on two native threads in one Bun process). The flag's only
contribution is, once more, the S1 clock plus the guarantee of co-residency. The
operative mitigations are hardware/OS (IBRS/eIBRS, IBPB on context switch, STIBP),
all outside the engine; per-indirect-branch software hardening (retpolines) in a JIT
that exists to make indirect dispatch fast is not a realistic demand and upstream
never shipped it.

**Verdict: susceptible-suspected** — structural, out of engine scope; recorded so
the audit trail shows it was considered, not missed. No test (not observable from
JS deterministically; hardware-dependent). Disposition: embedder rule below; note
for ops-facing docs that the usual server-side v2 posture (updated microcode, eIBRS)
is assumed.

---

## Closing: the one real mitigation, stated as an embedder obligation [LANDED — INTEGRATE-api.md 9.2-11 + OptionsList.h:691]

Isolation is the industry answer to MC-SPEC, and for this engine isolation means:
**`--useJSThreads` must be treated as equivalent to granting native code execution
for confidentiality purposes.** Concretely (LANDED as the CVE-AUDIT Tier-B B15
disposition: docs/threads/INTEGRATE-api.md 9.2-11 is the embedder-facing source
of record, and the `useJSThreads` help text at `runtime/OptionsList.h:691`
carries the obligation verbatim so `--options` surfaces it):

1. Never enable `--useJSThreads` for code you would not also trust with
   `process.hrtime` + a Worker + SAB — the flag is a strict superset of that timing
   capability (S1), independent of `--useSharedArrayBuffer` (S2).
2. No in-process secret is confidentiality-protected from JS running under the flag
   — not by bounds checks (S3), not by the cage (S4), not by branch hygiene (S5).
   Multi-tenant = multi-process. Same conclusion the browsers reached in 2018;
   shared-memory threads simply cannot exist on the other side of that line.
3. Cheap engine-side hardening worth taking (tracked, not blocking): branchless
   index clamping on the new C4/I33 runtime paths (S3 disposition c); keep
   `Gigacage::Primitive` on any future shared-buffer allocation path (S4).

Test written: `JSTests/threads/cve/mc-spec-timer-capability.js` (S1/S2 witness;
deterministic gating assertions + clock-resolution report; execute post-ungil).
