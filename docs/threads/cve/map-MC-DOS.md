# MC-DOS — Unbounded shared-resource consumption (availability-only)

Mechanism class (web-derived, data): a shared structure is growable without
quota, so one participant can consume the resource for everyone
(availability-only, never integrity). Folded minor subclass:
external-resource squatting of runtime-created named endpoints (bind a name
the runtime is about to claim). Exemplars: CVE-2026-22003 (HotSpot),
CVE-2023-33127 (CLR diagnostic-pipe squatting, the folded subclass).

Audit date: 2026-06-07. Tree: `jarred/threads` (UNGIL-HANDOUT rev 32).
Scope: both GIL-on phase 1 and GIL-off (`--useJSThreads=1 --useThreadGIL=0`)
— unlike the race classes, MC-DOS does not need parallelism; a single
spawned thread (or the main thread) can drive every arm below. Catalog
entry: docs/threads/CVE-AUDIT.md:249-258 ("per-thread quotas on shared-heap
growth, waiter-queue length, atom-table shard growth"); cross-cutting rule
applied: rule 2 (fail-stop is legitimate containment) — a deterministic
OOM/RangeError crash-or-throw is an acceptable verdict, silent unbounded
growth is not.

Governing design: SPEC-api §3/I17 (maxJSThreads + TID space), SPEC-ungil §D.1
+ SD9 (TID rebias liveness, docs/threads/SPEC-ungil.md:391-401; ANNEX D1,
docs/threads/UNGIL-HANDOUT.md:3337), SPEC-api 5.5/5.5a/5.6 (tickets, async
waiter queues, property-waiter table), SPEC-vmstate §4 (sharded atom table),
SPEC-heap §11/I10/I11 (epoch retire/reclaim), SPEC-jit §2/§4.4 + N6
(retired JIT artifacts, leak-until-integration charter).

---

## Surface inventory and verdicts

| # | Surface | Where | Verdict |
|---|---------|-------|---------|
| S1 | Live-Thread count | runtime/ThreadManager.cpp:303; OptionsList.h:693 | immune-by-construction |
| S2 | Spawned-TID space squatting (spawn-die churn) | ThreadManager.cpp:292-334; ThreadManager.h:423-444,649 | immune-by-construction (rebias; already tested) |
| S3 | Shared GC heap growth — no per-thread quota or attribution | heap server (SPEC-heap §5.3/§10A); GCClient allocators | susceptible-suspected (availability-only, design gap) |
| S4 | Property-waiter table: per-(cell,uid) lists, deques, Strong roots | runtime/ThreadAtomics.cpp:776-960 | needs-test → mc-dos-waiter-table-storm.js |
| S5 | AsyncTicket inboxes + lock/condition async-waiter queues | ThreadManager.h:83-200, 356-387; SPEC-api 5.5/5.5a | immune-by-construction |
| S6 | Epoch-deferred retire backlog between shared GCs | heap/GCSafepointEpoch.cpp:81, 120-170; SPEC-heap §11 | needs-test → mc-dos-retired-artifact-churn.js |
| S7 | Flag-on retired JIT artifact LEAK (handler chains + optimized JITCode) | bytecode/RetiredJITArtifacts.cpp:95-101, 165-201; .h:96-118 | **susceptible-suspected** (chartered) |
| S8 | Process-global sharded atom table growth | WTF/wtf/text/SharedAtomStringTable.h:72-118; SPEC-vmstate §4 | immune-by-construction |
| S9 | Named-endpoint squatting (folded subclass) | no surface exists | immune-by-construction (vacuous) + standing rule |

Tests written (DO NOT RUN until post-ungil; see //@ headers):
- `JSTests/threads/cve/mc-dos-waiter-table-storm.js` — S4 growth/reclaim/
  correctness storm (deterministic outcome; amplifier-ready schedule).
- `JSTests/threads/cve/mc-dos-retired-artifact-churn.js` — S6/S7 IC/jettison
  retire churn under explicit shared collections (availability probe; the S7
  leg documents the chartered-leak failure mode and is the post-integration
  regression).

Existing corpus already covering MC-DOS arms (cited, not duplicated):
- `JSTests/threads/cve/mc-tdwn-tid-recycle-storm.js` — S2: spawn/join past
  the 16383-TID spawned half; asserts the SD9 RangeError gate LIFTS within
  bounded retries (the availability half of U18's "spawn-storm past 2^15",
  UNGIL-HANDOUT.md:3240).
- `JSTests/threads/api/thread-id-bounds.js` — S1: live-cap RangeError with
  exact message at maxJSThreads.
- `JSTests/threads/heap-allocation-storm.js`, `heap-deferral-storm.js`,
  `heap-precise-storm.js` — S3 liveness under allocation pressure (but see
  S3: none asserts per-thread attribution, because none exists).

---

## S1. Live-Thread count — immune-by-construction

Mechanism leg: spawn threads until kernel thread/VA exhaustion (the classic
"shared structure" being the process thread table).

Why it cannot occur: `allocateSpawnedThreadStateInternal` refuses at
`m_threads.size() >= Options::maxJSThreads()` (ThreadManager.cpp:303;
default 32766, OptionsList.h:693) and the host call converts the null into a
deterministic `RangeError("too many live Threads (or thread-ID space
exhausted)")` (ThreadObject.cpp:368-370). The count is taken under
`TM::m_lock`, includes carriers (I17 accounting note, ThreadManager.h:428),
and the ThreadState is allocated only after the fallible prototype `get()`
(ThreadObject.cpp:352-358), so a throwing prototype getter cannot leak
forever-Running entries that squat the quota. Fail-stop posture per rule 2:
quota hit = RangeError, never degradation.

Adversarial self-check: the quota counts LIVE threads; can a hostile thread
keep entries alive after death? Entries are removed by the completion
sequence / finalizer (registerThreadStateFinalizer, ThreadObject.cpp:377);
a thread that never finishes is a live thread, correctly counted. An
embedder can lower `maxJSThreads`; nothing in JS can raise it.

## S2. Spawned-TID space squatting — immune-by-construction (tested)

Mechanism leg: the internal analog of endpoint squatting — TIDs are a
fixed namespace ([1, 0x4000) for spawned threads, ThreadManager.h:443,649);
spawn-die churn permanently consumes names, after which `Thread()` fails for
every other (well-behaved) thread: a squat on the TID namespace.

Why it cannot occur (GIL-off): SPEC-ungil §D.1 makes TID reissue a
liveness-guaranteed protocol: retire → arm at >=75% consumption
(`maybeArmAndSealRebiasLocked`, called on every allocation,
ThreadManager.cpp:331-333) → seal → in-stop restamp of dead-TID tags at the
next full shared collection → reissue via `m_freeTIDs` released post-resume
under `TM::m_lock` (ThreadManager.cpp:295-302). SD9 closes the liveness
hole: an exhausted winner spawn itself REQUESTS a full collection
(ThreadObject.cpp:359-365 comment + ThreadManager.cpp:314-321), so the
RangeError window closes without organic allocation pressure — the squatter
cannot hold the namespace, only rent it until the next stop. GIL-on the
namespace is consume-only by design (Deviation 10) and exhaustion is a
deterministic RangeError (fail-stop, rule 2).

Covered by `mc-tdwn-tid-recycle-storm.js` (asserts the gate lifts within
bounded retries) — the MC-DOS availability property and the MC-TDWN reuse
property are the two faces of the same test; no duplicate written.

Adversarial self-check: rebias requires a full SHARED collection; can the
squatter prevent collections? It would need to block the §10 stop, which is
MC-SAFE territory (STW watchdog, AB-17B), not a growth-quota gap — and the
SD9 explicit request means no allocation-pressure starvation path exists.

## S3. Shared GC heap growth — susceptible-suspected (availability-only)

Suspected hole, precisely: under the shared server heap, N threads allocate
into ONE Heap with ONE set of GC trigger/criticality thresholds. There is no
per-thread (per-GCClient) allocation quota, no per-thread byte accounting,
and no attribution: a single spawned thread in a tight allocation loop
drives the shared heap to its limit, and the resulting OOM
(throwOutOfMemoryError or fail-stop) lands on WHICHEVER thread allocates at
the threshold — including the main thread or an innocent sibling. This is
exactly the catalog's named gap ("per-thread quotas on shared-heap growth",
CVE-AUDIT.md:257-258).

Why this is genuinely new and not single-thread parity: in every shipping
JSC configuration, an agent that wants isolation gets its own Heap (workers
= own VM = own heap), so a worker's allocation storm hits its own limit.
`--useJSThreads` collapses N agents into one heap: the blast radius and the
misattribution are new. It remains availability-only by construction —
exhaustion produces OOM exceptions or fail-stop, never a dangling reference
(allocation failure paths are the same as today's).

Why no test: a "test" is just an allocation loop ending in OOM; the
existing heap storm corpus (heap-allocation-storm.js et al.) already proves
liveness-under-pressure, and rule 2 accepts fail-stop. The defect to record
is the missing DESIGN feature: a per-client byte counter at the TLC
slow-path refill (SPEC-heap §5.3 — the natural charging point, one counter
bump per block acquisition, zero fast-path cost) feeding (a) an optional
`Options::maxJSThreadHeapBytes` per-thread RangeError/termination and (b)
OOM attribution in the error message. Recommend chartering in the heap
workstream before ship; until then embedders must treat every spawned
thread as trusted with the whole heap budget.

## S4. Property-waiter table — **needs-test → TEST RAN, FOUND A REAL LEAK, FIXED (CLOSED 2026-06-10)**

**Outcome:** the reclamation arm (arm 3) of mc-dos-waiter-table-storm.js
failed 20/20 GIL-off — 0/128 waited-on-then-drained cells collectable. The
adversarial worry below ("a single path that ... converts the table into a
monotonic root set") was right in spirit but wrong in location: the table's
own drains are sound (cellProtect/uidProtect cleared, lists removed). The
leak was one layer down: a NOTIFIED `Atomics.waitAsync`'s settle ran through
`AsyncTicket::scheduleViaDeferredWorkTimer`, which never CANCELLED the
underlying `DeferredWorkTimer::TicketData` — and `JSGlobalObject::
visitChildren` marks target+dependencies of every live, un-cancelled
TicketData in the realm's `m_weakTickets` set. The 5.6 finite-timeout timer
lambda pins a `Ref<AsyncTicket>` (hence the TicketData) for the FULL
timeout, so a notified waiter's cell+promise stayed GC-rooted for the whole
60s window (discriminator: the TIMED-OUT path reclaimed — its timer lambda
dies at fire, dropping the last TicketData ref). Fix: the settle-tail
wrapper now does `dwtTicket->cancel()` after the settle task — the same
§E.4 retirement triple the registrant-routed ThreadTask path already
performed (ThreadManager.cpp `runQueuedSettleTaskOnRegistrant`). Test now
20/20 GIL-off Release and GIL-on Release. **Debug correction (2026-06-10,
family-2 re-verify):** Debug runs currently abort 6/6 BEFORE this test's
oracles run, on a PRE-EXISTING, flag-off-vanilla-reachable
waitAsync/DWT/microtask GC UAF (`ASSERTION FAILED: isSymbol()` in
synthesizePrototype from a PromiseReactionJob; deterministic minimal repros
+ triage in Tools/threads/bughunt/waitasync-dwt-uaf/EVIDENCE.md). NOT
caused by the S4 cancel() fix — the vanilla TA-lane repro never constructs
an AsyncTicket (WaiterListManager.cpp:436 banner) — and not a threads-flag
regression (crashes with no options at all, any GIL mode). Release passes
are masking the UAF (no cell scribbling outside ASSERT_ENABLED), so the S4
closure verdict stands on its own oracles; the Debug rung re-arms when the
bughunter fix lands. Original analysis follows:

Surface: `PropertyWaiterTable` (ThreadAtomics.cpp:814-960) is a
process-global singleton: `HashMap<(JSCell*, UniquedStringImpl*),
Ref<PropertyWaiterList>>` + per-list `Deque<Ref<PropertyWaiter>>`, with a
`Strong<> cellProtect` rooting the waited-on cell and (for async waiters) a
`Strong<JSPromise>` per ticket. This is the catalog's "waiter-queue length"
surface verbatim, and it is growable with no quota: nothing bounds the
number of keys, the depth of any one deque, or the number of
infinite-timeout async waiters.

Why it is PROBABLY contained (the argument the test must validate):

- Every unit of growth is charged to its creator. A sync waiter costs a
  parked thread (bounded by maxJSThreads, S1). An async waiter costs the
  registrant a JSPromise shell + AsyncTicket whose keepalive registration
  (§E.3, ThreadManager.h:160-200) is GC-visible — flooding charges the
  shared heap, and S3's fail-stop bounds it. The malloc'd
  PropertyWaiter/list/HashMap entries are O(1) per charged GC object, so
  GC-heap accounting is a valid proxy bound for the native side.
- Reclamation exists on every drain path: `removeListIfEmpty`
  (ThreadAtomics.cpp:941-959) clears `cellProtect`/`uidProtect` and removes
  the entry; notify, the D5 finite-timeout timer, and sync dequeue all
  funnel there; cell death sweeps every list for the cell
  (`sweepCellAtFinalization`, :889-930, registered once per cell via
  `m_sweepFinalizerCells`).
- The documented exception: a never-notified INFINITE-timeout async waiter
  roots cell + promise until VM teardown (round-4 comment, :853-870) —
  by-design parity with TA `Atomics.waitAsync` infinite waits (SPEC-api
  5.6 note 2). Self-charged, availability-only.

Why needs-test anyway (adversarial): the containment argument hangs on
"every drain path reaches removeListIfEmpty and clears BOTH Strongs". A
single path that drains the deque but skips the empty-list removal (or
clears the ticket promise but not cellProtect) converts the table into a
monotonic root set keyed by (live cell × every key ever waited on) — silent
unbounded growth, invisible to any correctness test, exactly the MC-DOS
failure shape. That is GC-observable from JS: if reclamation works, a
waited-on-then-drained object whose last reference is dropped MUST become
collectable. `mc-dos-waiter-table-storm.js` asserts (a) notify/timeout
correctness under a deep-deque, many-key, cross-thread storm and (b) the
reclamation property via FinalizationRegistry after the storm drains —
deterministic outcome, amplifier-ready schedule (rule 1: depth and key
count are the amplifier knobs).

## S5. AsyncTicket inboxes + async lock/condition waiter queues — immune-by-construction

Mechanism leg: flood ANOTHER thread's task inbox or a shared Lock's
`m_asyncWaiters` deque so its owner drowns.

Why it cannot occur: enqueue authority is structurally self-charging.
(1) Settles route to the REGISTRANT's own inbox — "settler never enqueues
into another's MicrotaskQueue (vmstate I11): under owner's inboxLock,
inboxOpen => append + wake owner RL" (SPEC-api 5.5,
docs/threads/SPEC-api.md:200) — so every inbox entry was paid for by that
inbox's owner when it registered the ticket (one GC-charged promise shell
each, §E.3 keepalive accounting, ThreadManager.h:160-200). A closed inbox
drains residue to the main inbox exactly once at completion (bounded by the
dead thread's own registrations). (2) `m_asyncWaiters` on a Lock grows only
via `asyncHold` calls by the queued thread itself, one ticket+promise each;
`schedPump` dispatches at most one pump per lock (G28, SPEC-api 5.5a:207).
There is no API by which thread A enqueues into a queue charged to thread
B. Exhaustion therefore reduces to S3 (self-allocated GC objects →
fail-stop OOM). The leaked-release-fn / never-notified shapes are the same
documented self-charged keepalive as S4's infinite waiter (SPEC-api 5.6
note 2: "MAY keep shell alive forever (=TA waitAsync infinite; ditto leaked
release fn)").

Adversarial self-check: `waitDeadlines` (ThreadManager.h:387) — appended
only by the CURRENT spawned thread for its own waits, popped on wake;
bounded by that thread's wait nesting. Condition notify moves waiters
between queues; it never creates entries. No cross-charge path found.

## S6. Epoch-deferred retire backlog — needs-test

Surface: `GCSafepointEpoch::m_retired` (GCSafepointEpoch.cpp:81) is an
unbounded `Vector<RetiredItem>` of malloc'd payloads; `bumpAndReclaim()`
runs ONLY at §10 step 7 of a shared collection or the legacy `runEndPhase`
site (SPEC-heap §11/I11, GCSafepointEpoch.h:84-94). Two quota observations:

1. Retired bytes are NOT reported to GC heuristics (no
   `reportExtraMemory`/byte counter anywhere in the retire path — verified
   GCSafepointEpoch.cpp). The reclaim trigger is a GC; the GC trigger is JS
   heap pressure. A workload whose retire rate is high while its JS
   allocation rate is low (IC handler-chain churn on long-lived shared
   objects is exactly that: retire is driven by code-shape churn, not
   allocation) grows native memory with nothing pushing the collector to
   run. Decoupled producer and reclaimer with no accounting bridge = the
   MC-DOS shape, even though each individual item is small.
2. Reclaim requires a FULL stop with every client stamped (I11(a)); epoch
   liveness is therefore hostage to stop liveness — but that is MC-SAFE's
   watchdog territory, already chartered (AB-17B); not double-counted here.

`mc-dos-retired-artifact-churn.js` drives sustained IC-churn retire traffic
with explicit `gc()` calls interleaved (post-integration, each shared
collection must drain the backlog: the test's survival + steady-state
assertion is the regression that reclaim actually runs and the I10 no-op
exemption doesn't silently skip it). Recommend additionally (design note,
heap workstream): a retired-bytes counter that feeds
`Heap::reportExtraMemoryAllocated`, closing observation 1 properly.

## S7. Flag-on retired-JIT-artifact LEAK — susceptible-suspected (chartered)

Suspected hole, precisely: under `--useJSThreads=1` BOTH retirement paths
currently leak unconditionally:

- `epochCoversEveryJSThread(VM&)` returns `!Options::useJSThreads()`
  (RetiredJITArtifacts.cpp:95-99) — so `retireHandlerChain` always takes the
  `head.leakRef()` stub flag-on (:169-176), leaking every displaced IC
  handler chain (malloc) AND keeping each node's GC-aware stub routine
  registered (executable memory never jettisoned).
- `retireOptimizedJITCode` flag-on does `jitCode.leakRef()`
  (:186-201), leaking every jettisoned DFG/FTL JITCode + CommonData +
  CallLinkInfos (header charter, RetiredJITArtifacts.h:96-118: dead
  callers' nodes stay on other CodeBlocks' m_incomingCalls lists forever).

Attacker-relevant because the leak rate is JS-controllable: TTL fires on
foreign transitions jettison optimized code (SPEC-jit §2 item 5), and IC
reset/megamorphic churn displaces handler chains — a loop touching shared
objects from alternating threads converts CPU into permanently-leaked
executable-pool bytes and malloc. The executable pool is FIXED-SIZE, so the
end state is pool exhaustion: compile failures/LLInt fallback or fail-stop
— availability-only, but unbounded and unaccounted, and unlike S3 it is NOT
fail-stop-contained at a clean threshold.

This is chartered ("leak-until-integration", sound for memory-safety, N6 /
THREADS-INTEGRATE(jit) markers) — recorded here so the charter's CLOSE
CONDITION is explicit for MC-DOS: per-thread epoch publication (flips
`epochCoversEveryJSThread` to a real check) bounds the handler-chain leg;
R2's N-stack conservative scan bounds the JITCode leg. The churn test's S7
leg is the regression to re-run at both landings; until then, ship-blocking
for any configuration that exposes `Thread()` to untrusted code.

## S8. Sharded atom table — immune-by-construction

Mechanism leg: intern unbounded strings into the process-global table
(CVE-2026-22003's shared-structure shape; also the classic interned-string
DoS).

Why it cannot occur as UNBOUNDED growth: entries are weak — an atom's
shard entry exists exactly while the StringImpl lives; the final deref runs
`removeDeadAtom` under the shard lock (SharedAtomStringTable.h:50-58
ordering contract; SPEC-vmstate §4, I1/I5/I7). Every JS-reachable
atomization charges a GC-visible object (JSString/Identifier) whose extra
memory includes the StringImpl, so growth raises GC pressure, collection
kills the strings, and deref-to-zero shrinks the shards: the shared heap's
own limit (S3) is the quota. Structure is fixed: 128 shards
(shardCountLog2=7, :84), shard choice a pure hash function (I5) — a
flooding thread cannot concentrate lock contention beyond 1/128 per key
class, and shard locks are leaves (I7: only fastMalloc under them), so
contention cannot be parlayed into a stop/GC stall.

Adversarial self-check: (a) C++-side atoms with no GC backing
(`AtomString` literals/idents in native code) are bounded by code, not
input. (b) `Symbol.for` registry routes through the same weak discipline
(U16). (c) The genuinely lifetime-unbounded entries are static atoms,
bounded at compile time. (d) Worst real cost is the documented one: a
deliberate atom storm is an S3 allocation storm with extra lock traffic —
fail-stop contained. No uncharged growth path found.

## S9. Named-endpoint squatting (folded subclass) — immune-by-construction (vacuous)

Rule from the catalog: "any debug/inspector endpoint the threads work adds
must bind before advertising" (CVE-AUDIT.md:255-256, from CVE-2023-33127:
attacker pre-creates the named pipe the runtime will connect through).

Finding: the threads implementation creates NO named OS endpoints. Verified
over ThreadManager.{h,cpp}, ThreadObject.cpp, ThreadAtomics.cpp,
JSThreadsSafepoint.{h,cpp}, GCSafepointEpoch.*, SharedAtomStringTable.*:
no socket/pipe/shm_open/mkfifo/bind, no filesystem or namespace names at
all. All synchronization is anonymous and in-process (WTF::ParkingLot
futex-style parking, WTF::Lock/Condition, run-loop dispatch); spawned
threads are `Thread::create(...)->detach()` with join via
`ThreadState::joinCondition` (ThreadObject.cpp:388-399), never a nameable
handle. No new inspector domain or transport is added by this work.

Standing rule recorded for future work (binding on any threads-adjacent
debug surface, e.g. a per-thread inspector channel): create-then-advertise
with O_EXCL/owner-only semantics — never connect to a name you did not
atomically create. Re-audit MC-DOS S9 if FUZZ.md's REPRL wiring or an
inspector transport ever ships in the flag-on configuration.

---

## Summary

| Verdict | Surfaces |
|---|---|
| immune-by-construction | S1, S2 (tested elsewhere), S5, S8, S9 |
| needs-test | S6 (retire backlog drain) |
| tested — leak found+fixed, CLOSED 2026-06-10 | S4 (waiter table reclaim: un-retired TicketData on the notify-settle tail; fixed in AsyncTicket::scheduleViaDeferredWorkTimer) |
| susceptible-suspected | S3 (no per-thread heap quota/attribution — design gap, availability-only), S7 (chartered flag-on JIT-artifact leak — close condition: per-thread epoch publication + R2 scan) |

Action items carried out of this audit: charter a per-client allocation
counter + OOM attribution (S3, heap workstream); add a retired-bytes →
`reportExtraMemoryAllocated` bridge (S6); keep S7's two leak legs on the
INTEGRATE checklist with `mc-dos-retired-artifact-churn.js` as the
regression at each landing.
