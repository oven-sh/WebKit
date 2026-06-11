# JVM / HotSpot / OpenJDK Concurrency & Memory-Model Vulnerability Catalog

Status: research catalog for the JSC threads security audit (defensive). Compiled 2026-06-07.
Scope: HotSpot/OpenJDK CVEs and notable JDK bug-tracker (JBS) concurrency defects whose
*mechanism* is a race, a lock-state transition, a safepoint bug, a GC/mutator interaction,
or a JIT/runtime invariant that concurrency can break. Each entry carries a root-cause
CLASS from the taxonomy at the end.

Confidence labels:
- **[V]** id + mechanism verified against a primary source during this research pass (source linked).
- **[M]** well-known item recalled from training; id believed correct but re-verify against NVD/JBS
  before citing externally.

An important meta-finding up front: **the JVM has remarkably few public CVEs whose root cause
is a true data race inside the VM.** This is *not* because HotSpot has no concurrency bugs — the
JBS tracker is full of them — but because (a) until recently the attacker model was "untrusted
applet inside the sandbox," and races that merely crash the VM were filed as quality bugs, not
CVEs; (b) Java gives untrusted code no shared-memory primitive as raw as SharedArrayBuffer —
*every* Java object is shared-memory-visible to all threads, so the VM was forced to make the
core object model thread-safe from day one (header word CAS, GC-safe publication), and the
residual bugs concentrate in the *optimizations layered on top of that model* (biased locking,
monitor deflation, safepoint elision, code patching). That is exactly the layer we are building
now, so the JBS section below is at least as important as the CVE section.

---

## 1. CVEs — concurrency / memory-model root cause

### CVE-2020-14803 — NIO Buffer boundary-check race (Libraries, JDK-8244136) [V]
- Mechanism: `java.nio.Buffer` bounds checks read `position`/`limit`/`address` fields that another
  thread can mutate between the check and the memory access; a racing thread shrinks/redirects the
  buffer after the check passes, yielding out-of-bounds access through what is otherwise a
  memory-safe API. Used to bypass sandbox restrictions / leak memory.
- Sources: [Red Hat bz#1889895](https://bugzilla.redhat.com/show_bug.cgi?id=1889895),
  [NVD](https://nvd.nist.gov/vuln/detail/CVE-2020-14803),
  [Red Hat CVE page](https://access.redhat.com/security/cve/cve-2020-14803).
- CLASS: **double-fetch of shared length/bounds** (check and use read shared mutable state twice).
- JSC-threads note: this is the canonical pattern for our segmented butterflies — any
  `length`-check-then-index path where length/butterfly pointer can be republished between check
  and use. TypedArray `byteLength`/`vector` pairs and `ArrayBuffer` detach/resize are the same shape.

### CVE-2023-21954 — incorrect enqueue of references in the garbage collector (Hotspot, JDK-8298191) [V]
- Mechanism: GC reference processing enqueued `java.lang.ref` references incorrectly, producing an
  object-lifecycle violation (object observable in a state the collector believed retired);
  exploitable for information disclosure per Oracle/Red Hat scoring.
- Sources: [Red Hat bz#2187441](https://bugzilla.redhat.com/show_bug.cgi?id=2187441),
  [NVD](https://nvd.nist.gov/vuln/detail/cve-2023-21954),
  [OpenJDK advisory 2023-04-18](https://openjdk.org/groups/vulnerability/advisories/2023-04-18).
- CLASS: **GC vs mutator lifecycle/publication race** (reference-processing state machine
  disagrees with mutator-visible reachability).
- JSC-threads note: maps to our WeakRef/FinalizationRegistry handling and to the shared-heap
  server's reference processing once N mutators can resurrect/observe weak targets concurrently.

### CVE-2018-2814 — incorrect handling of Reference clones → sandbox bypass (Hotspot, JDK-8192025) [V]
- Mechanism: cloning a `java.lang.ref.Reference` let attacker code obtain a Reference the GC's
  reference-processing pipeline didn't know about, breaking the GC↔Reference protocol
  (resurrection-style lifecycle confusion) and enabling a sandbox escape.
- Source: [Red Hat bz#1567121](https://bugzilla.redhat.com/show_bug.cgi?id=1567121).
- CLASS: **GC vs mutator lifecycle/publication race** (mutator-forgeable handle into a
  GC-private protocol).
- JSC-threads note: any object that participates in a private VM↔GC protocol (our TTL
  watchpoint cells, per-object lock words, shared-heap server handles) must be unforgeable and
  un-clonable from JS.

### CVE-2012-0507 — AtomicReferenceArray deserialization type confusion [M]
- Mechanism: `AtomicReferenceArray` performed `Unsafe`-based raw array stores trusting that its
  backing array was `Object[]`; deserialization let an attacker install a typed array (e.g.
  `Helper[]`) underneath, so the "atomic" setter became an unchecked covariant store → type
  confusion → full sandbox escape (weaponized in the Flashback Mac botnet).
- CLASS: **trusted concurrency primitive bypasses type/bounds checks** (the primitive uses raw
  memory ops on the assumption that an invariant established at construction still holds).
- JSC-threads note: our Atomics fast paths and any JIT-inlined atomic access must re-validate
  (or watchpoint) the invariants they assume — exactly our TID/SW-tag + structure checks; the
  lesson is that the *atomic* op is often the least-checked store in the engine.

### CVE-2026-22003 — Hotspot resource-exhaustion DoS [V]
- Mechanism: unbounded resource consumption in a Hotspot component (availability only).
- Source: [SentinelOne vuln DB](https://www.sentinelone.com/vulnerability-database/cve-2026-22003/).
- CLASS: **unbounded shared-resource consumption** (weak class; listed for completeness).

---

## 2. CVEs — JIT-correctness family (the single-threaded analog of "JIT assumes single mutator")

These are not races, but they are the dominant modern Hotspot RCE family and their root cause —
*the compiler proves an invariant once and emits unchecked code, then the invariant is broken by
a path the proof didn't cover* — is structurally identical to the bug class we create the moment
a second mutator can break a JIT-assumed invariant between proof and use. Cataloged so the
taxonomy covers it explicitly.

- **CVE-2021-2388** (Hotspot, JDK-8264066) [V] — incorrect comparison during C2 range-check
  elimination; crafted class bypasses bounds checks → OOB → sandbox escape.
  Sources: [Red Hat bz#1983075](https://bugzilla.redhat.com/show_bug.cgi?id=CVE-2021-2388),
  [NVD](https://nvd.nist.gov/vuln/detail/cve-2021-2388). CLASS: **JIT proves invariant once,
  emits unchecked access**.
- **CVE-2022-21540** (Hotspot) [M] — class-compilation flaw, information exposure
  ([Snyk](https://security.snyk.io/vuln/SNYK-UPSTREAM-OPENJDKJRE-2953459)). Same class.
- **CVE-2023-22044 / CVE-2023-22045** (Hotspot, C2 loop/range-check optimizations) [M] —
  OOB-read info disclosure via mis-eliminated range checks. Same class.
- **CVE-2024-20918 / CVE-2024-20952** (Jan 2024 Hotspot, C2 array/range issues) [M] — OOB
  access via JIT-eliminated checks; see
  [OpenJDK advisory 2024-01-16](https://openjdk.org/groups/vulnerability/advisories/2024-01-16). Same class.
- **CVE-2016-3587** (Hotspot, JDK-8154475) [V-id] — insufficient protection of
  `MethodHandle.invokeBasic()` ([Red Hat bz#1356987](https://bugzilla.redhat.com/show_bug.cgi?id=CVE-2016-3587));
  trusted-internal-entry-point reachable from untrusted code. CLASS: **privileged internal
  entry point exposed** — relevant to us because un-GIL'd threads multiply the paths by which a
  half-initialized internal object can be observed and invoked.

The concurrency analog ("JIT assumes single mutator") has, to our knowledge, **no public JVM CVE**
— because HotSpot never assumed a single mutator: every compiled access to a heap field must be
correct under concurrent mutation, and the deopt machinery is safepoint-synchronized. The places
where HotSpot *did* let compiled code trust cross-thread-mutable state are exactly its JBS crash
families (below). For JSC-threads this is the headline class: every DFG/FTL structure check,
watchpoint, and butterfly load that was sound under one mutator is now a proof that a second
mutator can invalidate between check and use.

---

## 3. Notable JBS concurrency defect families (mostly not CVEs — filed as crashes)

### 3.1 Biased locking: revocation races — the family that killed the feature
- **JDK-6444286** [V-id] — "Possible naked oop related to biased locking revocation safepoint in
  `jni_exit()`": revocation runs at a safepoint while a raw (unhandled) oop is live; GC moves the
  object, revocation writes the mark word through a stale pointer.
  CLASS: **lock-state transition race** × **GC vs raw-pointer window**.
- **JDK-6805108** [M] — biased-locking revocation vs suspended/exiting thread: revoking a bias
  requires reconstructing the bias owner's lock records from *another thread*; getting the owner
  to a well-defined state (safepoint/handshake) while it may be exiting was a recurring crash
  source. CLASS: **lock-state transition race** × **thread-lifetime (walker vs exiting thread)**.
- **JDK-8240723** [V-id] + bulk revocation behavior — per-class "bulk rebias/bulk revoke"
  epochs mutate `Klass` state observed concurrently by lock fast paths.
  CLASS: **lock-state transition race** (epoch read vs epoch bump).
- **JEP 374** [V] ([openjdk.org/jeps/374](https://openjdk.org/jeps/374)) deprecated and disabled
  biased locking in JDK 15, explicitly citing the complexity and maintenance burden of the
  revocation machinery. The takeaway for us: *an asymmetric lock optimization whose
  "deoptimize the lock" path requires stopping or introspecting another thread is a permanent
  race generator.* Our per-object cell locks + TID/SW-tagged headers are intentionally
  symmetric; any future "owner-biased" fast path must budget for a revocation protocol of
  JEP-374 complexity.

### 3.2 ObjectMonitor lifecycle: deflation vs concurrent enter (waiter-list lifetime)
- **JDK-8153224** [M-id] (async monitor deflation, landed JDK 15) — historically monitors were
  deflated only at safepoints because a monitor freed/recycled while another thread spins on
  `ObjectMonitor::enter`, or while a waiter is parked on it, is a use-after-free; the async
  deflation project introduced ref-counted/`is_being_async_deflated` guarded transitions.
  The pre-existing design and several follow-up bugs are the canonical **waiter-list lifetime**
  family: the monitor's free is racing its own contended slow path.
- Lock inflation (stack-lock → monitor) is a two-phase publication: the displaced mark word and
  the `ObjectMonitor*` install race against concurrent hashCode installation and against other
  enters. Multiple JBS crashes over the years trace to mark-word transitions
  (neutral ↔ stack-locked ↔ inflated ↔ hashed) observed in mixed states.
  CLASS: **lock-state transition race** + **waiter-list lifetime**.
- JSC-threads note: our per-object cell locks deliberately have no inflation step, but the
  shared-heap server's park/unpark queues are exactly an ObjectMonitor waiter list; the
  invariant to enforce is *a queue node's memory may not be reclaimed until every thread that
  could CAS on it has passed a quiescence point*.

### 3.3 Safepoint bugs
- **JDK-8161147** [V-id] — JVM crashes with `-XX:+UseCountedLoopSafepoints`
  ([bugs.java.com](https://bugs.java.com/bugdatabase/view_bug.do?bug_id=JDK-8161147)):
  safepoint-poll placement/elision in counted loops produced wrong execution state at the stop.
  The dual failure modes of this family: (a) poll elision → a thread that *never reaches* a
  safepoint → time-to-safepoint unbounded → effectively a VM-wide DoS and a watchdog problem
  (compare our `watchdogAssertStopProgress` timeouts); (b) poll placement where the recorded
  oop-map/frame state doesn't match actual state → GC walks a wrong frame.
  CLASS: **safepoint reachability / state-at-poll mismatch**.
- Handshakes (JDK 10+) exist because global safepoints for per-thread operations (like biased
  lock revocation) were both slow and a serialization choke; the migration introduced its own
  races (thread exiting while handshake pending — same thread-lifetime class as 3.1).

### 3.4 Code patching vs concurrent execution (cross-modifying code)
- HotSpot patches call sites, inline caches, and nmethod entry points while other threads may be
  executing the same bytes. The constraints and historical bugs are summarized in John Rose's
  ["How HotSpot cross-modifies code"](https://cr.openjdk.org/~jrose/hotspot-cmc.html) [V] —
  including the AArch64 port emitting deopt traps because the processor may *never* observe the
  second patched instruction without an ISB-class barrier.
  CLASS: **code-patching vs concurrent instruction fetch** (patch-ordering + i-cache coherence).
- The nmethod sweeper ("zombie" nmethod reclamation) had a long tail of use-after-free crashes —
  freeing compiled code while a stack still returns into it or an inline cache still points at
  it — culminating in the sweeper's removal/redesign in modern JDKs [M].
  CLASS: **code lifetime vs stack/IC references** (a waiter-list-lifetime analog for code).
- JSC-threads note: direct analog of our per-tier JIT checks, IC patching and jettison protocol.
  JSC under one mutator could patch ICs with only mutator/compiler coordination; with N mutators
  every IC patch is cross-modifying code for the *other* mutators. Jettisoned `JITCode` lifetime
  = nmethod sweeper problem.

### 3.5 Deopt / OSR races
- Recurring JBS family (multiple ids over the years; no single canonical CVE): deoptimization of
  a frame racing the thread executing it, OSR entry racing invalidation of the OSR nmethod, and
  `not_entrant` transitions racing new entries. HotSpot's answer is that *all* invalidation goes
  through safepoints/handshakes plus `nmethod` entry barriers — the bugs were in paths that
  skipped that funnel. CLASS: **invalidate-vs-execute race** (a specialization of 3.4).
- JSC-threads note: our TTL watchpoint fire → jettison → reroute path is this exact funnel; the
  AB-17 ladder failures (stop-the-world watchdog on jettison-requested stops) show we already
  live in this family.

### 3.6 Class initialization races
- The JVM spec requires `<clinit>` to run exactly once under the class-init lock with other
  threads blocked until `initialized`; the historical bug families are (a) deadlocks via
  cross-class init cycles (see [SEI CERT DCL00-J](https://wiki.sei.cmu.edu/confluence/display/java/DCL00-J.+Prevent+class+initialization+cycles)
  [V]), and (b) parallel-capable classloader races (same class defined twice / observed
  pre-initialized) [M].
- Security relevance is mostly indirect: a thread observing a class in state
  `being_initialized` sees default-valued statics — a *sanctioned* form of observing
  half-initialized state that exploit chains have leaned on (static fields read before the
  initializer's security checks ran). No clean Hotspot CVE id for the race itself [M];
  the deserialization-side analog (static initializers run before allowlisting) appears in
  ecosystem CVEs like Apache MINA CVE-2026-42778.
  CLASS: **metadata/initialization publication race** (half-built metadata observable).
- JSC-threads note: maps to lazily-materialized per-global structures, lazy property table /
  Structure transitions, and our sharded atom table fill paths: "creation under lock, then
  racy publish" must publish fully-built objects with a release fence, and readers must
  tolerate *only* the two end states.

### 3.7 JNI / Unsafe races
- JNI gives native code raw access plus `GetPrimitiveArrayCritical` regions that pin/suspend GC
  interaction; historical crashes involve critical regions held across safepoint-requiring
  operations (deadlock or naked-pointer access after a moving GC) and JNI handles used from the
  wrong thread [M; JBS family, not CVEs].
- `sun.misc.Unsafe` is by definition race-capable (raw loads/stores bypassing the JMM); its
  security history is not "Unsafe has a race" but "trusted code exposes an Unsafe-powered
  primitive whose guarding invariant can be broken" — CVE-2012-0507 above is the type specimen.
  CLASS: **raw-memory escape hatch trusted with a breakable invariant**.
- JSC-threads note: our C++/JIT intrinsics that bypass the cell-lock fast path are our Unsafe;
  each needs an explicit statement of which invariant makes the bypass sound and which
  watchpoint/check enforces it.

### 3.8 Finalizer / resurrection family
- Finalizers run on a separate VM thread against objects the application believed dead;
  classic Java attacks override `finalize()` to resurrect partially-constructed objects
  (constructor threw after a security check failed) — a *designed-in* cross-thread lifecycle
  hazard rather than a race bug; mitigated by JDK-internal `Reference`-based cleanup and
  finalization deprecation (JEP 421) [M]. CVE-2018-2814 (above) is the GC-side cousin.
  CLASS: **GC vs mutator lifecycle/publication race** (resurrection edge).

---

## 4. Root-cause class taxonomy (summary)

| # | CLASS | JVM exemplars | One-line definition |
|---|-------|---------------|---------------------|
| 1 | **double-fetch of shared length/bounds** | CVE-2020-14803 | check and use independently read attacker-mutable shared state |
| 2 | **lock-state transition race** | JDK-6444286, JDK-6805108, bulk rebias, mark-word inflation | object's lock/header word observed mid-transition between lock encodings |
| 3 | **waiter-list / monitor lifetime** | pre-JDK-15 monitor deflation (JDK-8153224 family) | synchronization object freed/recycled while its own slow path or parked waiters still reference it |
| 4 | **GC vs mutator lifecycle/publication race** | CVE-2023-21954, CVE-2018-2814, finalizer resurrection, naked oops at safepoints | collector's view of reachability/lifecycle diverges from what a mutator can still observe or forge |
| 5 | **safepoint reachability / state-at-poll mismatch** | JDK-8161147 family | a thread can't be brought to a stop (unbounded TTSP) or stops with frame/oop-map state that misdescribes reality |
| 6 | **code-patching vs concurrent execution** | hotspot-cmc constraints; AArch64 deopt traps; IC patching | instruction bytes mutated while another core may fetch them; ordering/i-cache coherence violated |
| 7 | **invalidate-vs-execute (deopt/OSR) race** | nmethod not_entrant/zombie families, sweeper UAF | compiled code invalidated or freed while a thread is entering/inside/returning into it |
| 8 | **JIT proves invariant once, emits unchecked access** | CVE-2021-2388, CVE-2023-22044/5, CVE-2024-20918/52 | compiler-eliminated check whose premise a later path (for us: another mutator) can falsify |
| 9 | **metadata/initialization publication race** | class-init/`<clinit>` family, parallel classloading | half-built class/structure/table observable by a concurrent reader |
| 10 | **trusted-primitive invariant bypass** | CVE-2012-0507, Unsafe-exposure pattern | a privileged/atomic primitive performs raw accesses trusting a construction-time invariant that other machinery lets the attacker break |
| 11 | **thread-lifetime (walker vs exiting thread)** | bias revocation vs exiting owner; handshake vs exit | one thread introspects/modifies another's stack or state while that thread is being torn down |
| 12 | **unbounded shared-resource consumption** | CVE-2026-22003 | availability-only; shared structure growable without quota |

### Priority mapping to JSC-threads (highest expected yield for our audit)
1. Class 8 + 1 — every DFG/FTL-eliminated check (structure, butterfly length, typed-array bounds)
   under a second mutator; our segmented-butterfly republish windows. These are the JS-engine
   shape of the only two classes that produced *exploitable* JVM CVEs.
2. Class 6 + 7 — IC patching, TTL watchpoint fire → jettison → reroute, JITCode lifetime
   (AB-17/AB-17B territory; HotSpot says: one funnel, entry barriers, no shortcuts).
3. Class 2 + 3 — our per-object cell locks and shared-heap-server park queues; JEP 374 is the
   cautionary tale against asymmetric lock fast paths.
4. Class 4 + 5 — shared heap server vs N mutators; STW watchdog (TTSP boundedness) and weak
   reference processing.
5. Class 9 — sharded atom table, lazy Structure/global materialization: publish-complete-or-nothing.

### Source index
- NVD: https://nvd.nist.gov/vuln/detail/CVE-2020-14803 , /cve-2021-2388 , /cve-2023-21954
- Red Hat Bugzilla: #1889895 (14803), #1983075 (2388), #2187441 (21954), #1567121 (2018-2814), #1356987 (2016-3587)
- OpenJDK vulnerability advisories index: https://openjdk.org/groups/vulnerability/advisories/
- JEP 374 (Deprecate and Disable Biased Locking): https://openjdk.org/jeps/374
- JBS: JDK-6444286, JDK-8161147, JDK-8240723, JDK-8244136, JDK-8264066, JDK-8298191
- John Rose, "How HotSpot cross-modifies code": https://cr.openjdk.org/~jrose/hotspot-cmc.html
- SEI CERT DCL00-J (class-init cycles): https://wiki.sei.cmu.edu/confluence/display/java/DCL00-J.+Prevent+class+initialization+cycles
- Phrack "Twenty years of Escaping the Java Sandbox" (negative result: no race-based escapes in the canonical applet-era catalog): https://www.exploit-db.com/papers/45517
