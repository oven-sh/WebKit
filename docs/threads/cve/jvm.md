# JVM / HotSpot / OpenJDK Concurrency & Memory-Model Vulnerability Catalog

Status: research catalog for the JSC threads security audit (defensive). Compiled 2026-06-07.
**Revised 2026-06-15**: second research pass added ~30 verified JBS/CVE ids (System.arraycopy
TOCTOU, async-monitor-deflation crash family, C2 SATB barrier-split, lock-coarsening miscompile,
nmethod-sweeper UAFs, missing-barrier family, interned-table races); three new §3 families
(3.9/3.10/3.11). Taxonomy unchanged — new ids mapped to existing 12 classes so the
`map-MC-*.md` mechanism-class files remain valid.

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

### CVE-2014-0456 — System.arraycopy() element race → type confusion (Hotspot, JDK-8029858) [V]
- Mechanism: `System.arraycopy()` on `Object[]` verifies each source element is assignable to the
  destination component type, then stores it; a second thread mutates the source slot between
  the type check and the store. A wrong-typed oop lands in a typed array → heap type confusion
  → JVM memory corruption → full sandbox escape / RCE. Weaponized (ZDI-14-114).
- Sources: [Red Hat bz#1087413](https://bugzilla.redhat.com/show_bug.cgi?id=1087413),
  [ZDI-14-114](https://www.zerodayinitiative.com/advisories/ZDI-14-114/),
  [PoC repo](https://github.com/zhuowei/loljava).
- CLASS: **double-fetch of shared length/bounds** (here: double-fetch of *element*, not length —
  same TOCTOU shape: check-then-use on attacker-mutable shared slot).
- JSC-threads note: this is the *exact* pattern for our `arraycopy`/`slice`/spread/`Array.from`
  fast paths over a shared butterfly: any check-element-type → store loop must either snapshot
  the source under the cell lock or re-check at store time. Higher priority than CVE-2020-14803
  because it yields type confusion, not just OOB.

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

### CVE-2017-3272 — Atomic*FieldUpdater protected-field bypass → type confusion (Libraries, JDK-8165344) [V]
- Mechanism: `AtomicReferenceFieldUpdater` (and Integer/Long siblings) failed to restrict access
  to *protected* fields properly; an attacker-crafted updater performs an unchecked CAS of an
  arbitrary oop into a privileged field → type confusion → SecurityManager disable → sandbox
  escape. The atomic store path is `Unsafe`-backed and skips the normal `aastore` type check.
- Sources: [NVD](https://nvd.nist.gov/vuln/detail/CVE-2017-3272),
  [Red Hat](https://access.redhat.com/security/cve/cve-2017-3272),
  [Phrack 69-12](https://www.exploit-db.com/papers/45517).
- CLASS: **trusted-primitive invariant bypass** (atomic write path is the least-checked store).
- JSC-threads note: every DFG/FTL-inlined `Atomics.*` and every CAS-based IC install is our
  `Unsafe`; the type/structure guard must be on the *store* side, not just at updater
  construction.

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

### CVE-2018-3169 — field-link-resolution access check skipped (Hotspot) [V]
- Mechanism: HotSpot's field link resolution did not properly perform access checks on one path;
  a crafted class resolves and binds to a field it should not see. Not a data race, but the
  resolution result is *cached* — the concurrency analog is "resolve once under one set of
  assumptions, reuse forever from N threads."
- Sources: [NVD](https://nvd.nist.gov/vuln/detail/CVE-2018-3169),
  [Red Hat](https://access.redhat.com/security/cve/cve-2018-3169).
- CLASS: **trusted-primitive invariant bypass** (cached-resolution variant).

### CVE-2022-21541 — Hotspot integrity flaw (MethodHandle intrinsic guards) [V-id]
- Mechanism: Oracle does not publish details; OpenJDK fix touches interpreter / method-handle
  intrinsic guards. Allows sandboxed code to create/delete/modify accessible data.
- Sources: [NVD](https://nvd.nist.gov/vuln/detail/CVE-2022-21541),
  [Red Hat](https://access.redhat.com/security/cve/cve-2022-21541).
- CLASS: **trusted-primitive invariant bypass**.

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
- **JDK-8028073** [V] — race in `ObjectMonitor` implementation causing deadlocks: wait/notify
  list manipulation races with another thread's enter/exit on the same monitor.
  ([bugs.java.com](https://bugs.java.com/bugdatabase/view_bug.do?bug_id=8028073))
  CLASS: **waiter-list / monitor lifetime**.
- **JDK-8153224** [M-id] (async monitor deflation, landed JDK 15) — historically monitors were
  deflated only at safepoints because a monitor freed/recycled while another thread spins on
  `ObjectMonitor::enter`, or while a waiter is parked on it, is a use-after-free; the async
  deflation project introduced ref-counted/`is_being_async_deflated` guarded transitions.
  ([wiki](https://wiki.openjdk.org/display/HotSpot/Async+Monitor+Deflation))
- **JDK-8319137** [V] — `ObjectMonitor` dtor releases `_object` too late; async-deflation thread
  races with hashcode-install thread reading the displaced header.
  ([JBS](https://bugs.openjdk.org/browse/JDK-8319137)) CLASS: **lock-state transition race**.
- **JDK-8332506** [V] — SIGFPE in `ObjectSynchronizer::is_async_deflation_needed()`: divides by
  a counter another thread zeroed. ([JBS](https://bugs.openjdk.org/browse/JDK-8332506))
  CLASS: **lock-state transition race** (unsynchronized stat read).
- **JDK-8352414** [V] — JFR `JavaMonitorDeflate` event reads the monitor's object after
  concurrent GC reclaimed it → UAF crash. ([PR 24121](https://github.com/openjdk/jdk/pull/24121))
  CLASS: **waiter-list / monitor lifetime** (monitor outlives referent).
- **JDK-8315884** [V] — new Object→ObjectMonitor side-table mapping introduced specifically
  because storing the monitor pointer in the mark word races with GC forwarding-pointer install
  and hashcode install. ([PR 20067](https://github.com/openjdk/jdk/pull/20067))
  CLASS: **lock-state transition race** (one header word, N writers).
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
- **JDK-8221734 → JDK-8226705** [V] — "Deoptimize with handshakes" original patch let a target
  thread re-enter a not-entrant nmethod between the handshake ack and the patch of the entry;
  required a full REDO. ([hotspot-dev RFR](https://mail.openjdk.org/pipermail/hotspot-dev/2019-September/039458.html))
  CLASS: **safepoint reachability / state-at-poll mismatch** (ack ≠ quiesced).
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
- **JDK-8295214** [V] — Generational ZGC patches nmethod instructions outside safepoints; the
  return-through-stack-watermark path lacked the cross-modifying-code fence → executing CPU
  reads stale icache bytes. ([PR 11042](https://github.com/openjdk/jdk/pull/11042))
  CLASS: **code-patching vs concurrent instruction fetch**.
- **JDK-8217717** [V] — ZGC race in `ZNMethodTable::register_nmethod` lets load-barrier stub
  patching collide with concurrent registration → wrong patch target.
  ([JBS](https://bugs.openjdk.org/browse/JDK-8217717))
  CLASS: **code-patching vs concurrent instruction fetch**.
- **JDK-8212155** [V-id] — JVMTI `dynamic_code_generated` event posted for a vtable stub while
  another thread is still writing it. ([JBS](https://bugs.openjdk.org/browse/JDK-8212155))
  CLASS: **metadata/initialization publication race** (publish-before-init, code edition).
- The nmethod sweeper ("zombie" nmethod reclamation) had a long tail of use-after-free crashes —
  freeing compiled code while a stack still returns into it or an inline cache still points at
  it — culminating in the sweeper's removal/redesign in modern JDKs [M].
  CLASS: **code lifetime vs stack/IC references** (a waiter-list-lifetime analog for code).
- JSC-threads note: direct analog of our per-tier JIT checks, IC patching and jettison protocol.
  JSC under one mutator could patch ICs with only mutator/compiler coordination; with N mutators
  every IC patch is cross-modifying code for the *other* mutators. Jettisoned `JITCode` lifetime
  = nmethod sweeper problem.

### 3.5 Deopt / OSR races
- **JDK-8247992** [V] — `HotSpotNmethod.executeVarargs` (JVMCI) reads the verified-entry-point,
  thread is preempted, sweeper makes the nmethod zombie, thread resumes and jumps into freed/
  repurposed code. ([bugs.java.com](https://bugs.java.com/bugdatabase/view_bug.do?bug_id=JDK-8247992))
  CLASS: **invalidate-vs-execute race** (VEP fetch then call, no keep-alive).
- **JDK-8234662** [V] — sweeper does not keep the *current* nmethod alive while processing it;
  concurrent GC unloads it mid-scan. ([JBS](https://bugs.openjdk.org/browse/JDK-8234662))
  CLASS: **invalidate-vs-execute race**.
- **JDK-8290451** [V-id] — incorrect result on C1→C2 OSR transition (profile-driven assumption
  invalidated between tier-up decision and OSR entry).
  ([JBS](https://bugs.openjdk.org/browse/JDK-8290451)) CLASS: **invalidate-vs-execute race**.
- **JDK-6351173** [V-id] — OSR/GC crash only reproducible with a thread spinning `System.gc()`;
  OSR frame setup races with relocation.
  ([bugs.java.com](https://bugs.java.com/bugdatabase/view_bug.do?bug_id=6351173))
  CLASS: **invalidate-vs-execute race** × **GC vs raw-pointer window**.
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
- **JDK-4151836** [V] — race in class initialization (JDK 1.1 native threads): two threads
  observe a class as "initialized" before static fields are published → NPE.
  ([bugs.java.com](https://bugs.java.com/bugdatabase/view_bug.do?bug_id=4151836))
- **JDK-7122142** [V] — `isAnnotationPresent` vs `getAnnotations` race: annotation-class init on
  thread A, per-class annotation cache on thread B, lock order inverted → deadlock (hit in
  Spring/OSGi). ([bugs.java.com](https://bugs.java.com/bugdatabase/view_bug.do?bug_id=7122142))
- **JDK-8156584** [V] — `sun.security.x509.AlgorithmId.oidTable` lazily built without sync; two
  threads each build & install a map; readers observe a half-built one.
  ([JBS](https://bugs.openjdk.java.net/browse/JDK-8156584))
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
  wrong thread.
- **JDK-8218880** [V] — G1 crashes when issuing a periodic GC while `GCLocker` is held (thread
  inside JNI critical): "would deadlock" assert.
  ([JBS](https://bugs.openjdk.org/browse/JDK-8218880))
  CLASS: **safepoint reachability** × **thread-lifetime**.
- **JDK-8192647** [V] — `GCLocker`-induced GCs starve allocator threads: stalled thread loses the
  freed-memory race to other allocators every cycle → spurious OOME.
  ([JBS](https://bugs.openjdk.org/browse/JDK-8192647)) CLASS: **unbounded shared-resource
  consumption** (livelock variant).
- **JDK-8048556** [V-id] — spurious `GCLocker`-initiated young GCs: critical-count decrement and
  "needs GC" flag read are not ordered → GC fires when count already 0.
  ([bugs.java.com](https://bugs.java.com/bugdatabase/view_bug.do?bug_id=JDK-8048556))
  CLASS: **metadata/initialization publication race** (missing acq/rel on protocol flag).
- `sun.misc.Unsafe` is by definition race-capable (raw loads/stores bypassing the JMM); its
  security history is not "Unsafe has a race" but "trusted code exposes an Unsafe-powered
  primitive whose guarding invariant can be broken" — CVE-2012-0507 / CVE-2017-3272 above are
  the type specimens. CLASS: **raw-memory escape hatch trusted with a breakable invariant**.
- JSC-threads note: our C++/JIT intrinsics that bypass the cell-lock fast path are our Unsafe;
  each needs an explicit statement of which invariant makes the bypass sound and which
  watchpoint/check enforces it. The `GCLocker` family maps to FFI / `napi` critical sections
  vs. our STW request, and to `ArrayBuffer` pinning.

### 3.8 Finalizer / resurrection family
- Finalizers run on a separate VM thread against objects the application believed dead;
  classic Java attacks override `finalize()` to resurrect partially-constructed objects
  (constructor threw after a security check failed) — a *designed-in* cross-thread lifecycle
  hazard rather than a race bug; mitigated by JDK-internal `Reference`-based cleanup and
  finalization deprecation (JEP 421) [M]. CVE-2018-2814 (above) is the GC-side cousin.
- **JDK-4243978** [V] — `Reference.enqueue()` race: the private `next` field is used for *two*
  different linked lists (GC's Pending list and the user `ReferenceQueue`); user `enqueue()` on
  a Pending ref corrupts the GC's list.
  ([bugs.java.com](https://bugs.java.com/bugdatabase/view_bug.do?bug_id=4243978))
  CLASS: **GC vs mutator lifecycle/publication race** (one field, two concurrent owners).

### 3.9 GC-barrier correctness vs JIT scheduling / missing memory barriers [new 2026-06-15]
- **JDK-8242115** [V] — C2 SATB barriers not safepoint-safe: C2 schedules a Java call between a
  referent load and its SATB pre-barrier; if the call deopts, the barrier never runs → object
  never marked → live object collected → **heap corruption**. Affects G1 *and* Shenandoah.
  ([JBS](https://bugs.openjdk.org/browse/JDK-8242115))
  CLASS: **GC vs mutator lifecycle/publication race** × **JIT proves invariant once**.
- **JDK-8295066** [V] — fix for 8242115 broke C2 load-folding (constant arrays no longer fold);
  demonstrates the barrier-correct ↔ optimizer-correct tension.
  ([JBS](https://bugs.openjdk.org/browse/JDK-8295066))
- **JDK-8233839** [V] — aarch64 `NewObjectArrayStub`/`NewTypeArrayStub` publish the new array oop
  *before* the `dmb ishst` → another thread reads length/elements as garbage.
  ([JBS](https://bugs.openjdk.org/browse/JDK-8233839))
  CLASS: **metadata/initialization publication race** (publish-before-init).
- **JDK-8248219** [V] — aarch64 interpreter `fast_storefield`/`fast_accessfield` (volatile fast
  path) missing the JMM-required barrier. ([JBS](https://bugs.openjdk.org/browse/JDK-8248219))
  CLASS: **metadata/initialization publication race** (missing acq/rel).
- **JDK-8147611** [V] — G1 `start_cset_region_for_worker` reads a region pointer without acquire;
  worker sees stale region table on weak-memory CPU.
  ([JBS](https://bugs.openjdk.org/browse/JDK-8147611)) CLASS: **GC vs mutator publication race**.
- **JDK-8007898** [V] — `Matcher::post_store_load_barrier()` incorrectly deletes a `MemBar` it
  thinks redundant; on some schedules it isn't.
  ([JBS](https://bugs.openjdk.java.net/browse/JDK-8007898))
  CLASS: **JIT proves invariant once** (barrier-elision variant).
- **JDK-8253183** [V] — fragile barrier-kind selection on non-multi-copy-atomic platforms
  (arm32, ppc): seq-cst downgraded to acq or rel.
  ([JBS](https://bugs.openjdk.org/browse/JDK-8253183))
  CLASS: **metadata/initialization publication race** (missing acq/rel).
- **JDK-8187577** [V-id] — G1 concurrent-marking SIGSEGV in `ClassLoaderData::oops_do`: marker
  walks a CLD another thread is unlinking. ([JBS](https://bugs.openjdk.org/browse/JDK-8187577))
  CLASS: **GC vs mutator lifecycle/publication race**.
- **JDK-8254980** [V] — ZGC `ZHeapIterator` visits *armed* nmethods (entry barrier not yet
  disarmed) under `-XX:-ClassUnloading`, reading oops mid-relocation.
  ([PR 801](https://github.com/openjdk/jdk/pull/801)) CLASS: **GC vs mutator publication race**.
- **JDK-8343607** [V-id] — Shenandoah barrier-expansion crash inside `Continuation::enter`
  (Loom): barrier inserted where the frame layout is mid-transition.
  ([PR 22663](https://github.com/openjdk/jdk/pull/22663)) CLASS: **safepoint state-at-poll
  mismatch**.
- JSC-threads note: this family is the highest-severity *new* mapping for SPEC-congc — our
  Riptide write-barrier emission in DFG/FTL must remain atomic with the store across any
  possible OSR-exit / safepoint placement; segmented-butterfly grow must fence before
  publishing the new segment pointer; B3 fence strength-reduction must assume weakest target.

### 3.10 Interned / weak-table races (StringTable, SymbolTable) [new 2026-06-15]
- **JDK-8278965** [V] — SIGSEGV in `SymbolTable::do_lookup`: reader walks a bucket while
  concurrent cleanup unlinks the node and drops its refcount to 0.
  ([JBS](https://bugs.openjdk.org/browse/JDK-8278965))
  CLASS: **GC vs mutator lifecycle/publication race** (weak-table read vs sweep).
- **JDK-8313678** [V] — `SymbolTable` leaks `Symbol`s during cleanup: `ConcurrentHashTable::
  delete_in_bucket` accidentally bumps the refcount → permanent leak.
  ([JBS](https://bugs.openjdk.org/browse/JDK-8313678)) CLASS: **waiter-list / monitor lifetime**
  (refcount-race variant — protocol object outlives intended scope).
- **JDK-6507007** [V-id] — `StringTable` entry published without store-store fence on IA-64;
  reader sees the bucket link but the `String`'s `value[]` still null.
  ([JBS](https://bugs.openjdk.org/browse/JDK-6507007))
  CLASS: **metadata/initialization publication race**.
- **JDK-8211821** [V] — `PrintStringTableStatistics` walks the table after the last `JavaThread`
  detached; `Thread::current()` is null → crash.
  ([JBS](https://bugs.openjdk.org/browse/JDK-8211821)) CLASS: **thread-lifetime**.
- JSC-threads note: this is our **sharded atom table** exactly. Audit: insert-vs-sweep ordering,
  `StringImpl` refcount atomicity, release fence before bucket publish, and teardown ordering
  vs. the last lite thread.

### 3.11 JIT lock-elision / coarsening miscompiles [new 2026-06-15]
- **JDK-8268347** [V] — C2 loop-unswitching clones a region but not its `BoxLock`; lock-coarsening
  then deletes the `Lock` in one clone as "coarsened" and the matching `Unlock` in the other as
  "nested" → emitted code has unbalanced monitor-exit → corrupted lock word /
  `IllegalMonitorStateException`. ([JBS](https://bugs.openjdk.org/browse/JDK-8268347))
- **JDK-8268571** [V] — fix: reorder coarsening after EA + nested-lock-opt (the optimizations are
  not commutative). ([bugs.java.com](https://bugs.java.com/bugdatabase/view_bug.do?bug_id=8268571))
- CLASS: **JIT proves invariant once, emits unchecked access** (lock-pairing variant: optimizer
  treats lock/unlock as movable nodes and breaks the structural pairing invariant).
- JSC-threads note: any DFG/FTL pass that hoists/sinks/clones across a `CheckCellLock`/`CellUnlock`
  pair (LICM, loop-unswitch, tail-dup) must preserve 1:1 pairing per path.

### 3.12 Metadata redefinition vs concurrent use [new 2026-06-15]
- **JDK-8022887** [V] — reflection reads a `Method*` while `RedefineClasses` rewrites the vtable
  on another thread → assertion / wrong dispatch.
  ([JBS](https://bugs.openjdk.java.net/browse/JDK-8022887))
- **JDK-8291830** [V-id] — `StressRedefine` crash: redefinition vs concurrent compilation of the
  same method (compiler holds stale `ConstMethod*`).
  ([bugs.java.com](https://bugs.java.com/bugdatabase/view_bug?bug_id=8291830))
- **JDK-6318850** [V] — `RedefineClasses` posts `NativeMethodBind` while the method's `Method*`
  is being swapped → callee reads half-installed metadata.
  ([bugs.java.com](https://bugs.java.com/bugdatabase/view_bug.do?bug_id=6318850))
- **JDK-8215889** [V-id] — ZGC concurrent class-unloading: resolution touches a `ClassLoaderData`
  the GC already flagged `_unloading`. ([JBS](https://bugs.openjdk.org/browse/JDK-8215889))
- CLASS: **metadata/initialization publication race** (swap-vs-use: a `Structure`/`Method`/class
  replaced while another thread holds a raw pointer into the old version).
- JSC-threads note: `Structure` transition vs IC `StructureStubInfo`; `FunctionExecutable`
  re-link; watchpoint fire while another thread is mid-`ensureRareData`.

---

## 4. Root-cause class taxonomy (summary)

| # | CLASS | JVM exemplars | One-line definition |
|---|-------|---------------|---------------------|
| 1 | **double-fetch of shared length/bounds** | CVE-2014-0456, CVE-2020-14803 | check and use independently read attacker-mutable shared state (length, element, pointer) |
| 2 | **lock-state transition race** | JDK-6444286, JDK-6805108, JDK-8319137, JDK-8315884, bulk rebias, mark-word inflation | object's lock/header word observed mid-transition between lock encodings; one header word, N writers |
| 3 | **waiter-list / monitor lifetime** | JDK-8028073, JDK-8153224 family, JDK-8352414, JDK-8313678 | synchronization object freed/recycled while its own slow path or parked waiters still reference it |
| 4 | **GC vs mutator lifecycle/publication race** | CVE-2023-21954, CVE-2018-2814, JDK-8242115, JDK-8278965, JDK-4243978, JDK-8187577 | collector's view of reachability/lifecycle diverges from what a mutator can still observe or forge; includes barrier-split-by-safepoint |
| 5 | **safepoint reachability / state-at-poll mismatch** | JDK-8161147, JDK-8226705, JDK-8218880, JDK-8343607 | a thread can't be brought to a stop (unbounded TTSP) or stops with frame/oop-map state that misdescribes reality; ack ≠ quiesced |
| 6 | **code-patching vs concurrent execution** | hotspot-cmc constraints, JDK-8295214, JDK-8217717, AArch64 deopt traps | instruction bytes mutated while another core may fetch them; ordering/i-cache coherence violated |
| 7 | **invalidate-vs-execute (deopt/OSR) race** | JDK-8247992, JDK-8234662, JDK-8290451, nmethod not_entrant/zombie/sweeper UAF | compiled code invalidated or freed while a thread is entering/inside/returning into it |
| 8 | **JIT proves invariant once, emits unchecked access** | CVE-2021-2388, CVE-2023-22044/5, CVE-2024-20918/52, JDK-8268347, JDK-8007898 | compiler-eliminated check/barrier/lock whose premise a later path (for us: another mutator) can falsify |
| 9 | **metadata/initialization publication race** | JDK-8233839, JDK-8248219, JDK-6507007, JDK-8022887, JDK-4151836, JDK-8156584, JDK-8253183 | half-built class/structure/object/table observable by a concurrent reader; missing acq/rel on the publish |
| 10 | **trusted-primitive invariant bypass** | CVE-2012-0507, CVE-2017-3272, CVE-2018-3169, CVE-2022-21541, Unsafe-exposure pattern | a privileged/atomic primitive performs raw accesses trusting a construction-time invariant that other machinery lets the attacker break |
| 11 | **thread-lifetime (walker vs exiting thread)** | bias revocation vs exiting owner, handshake vs exit, JDK-8211821 | one thread introspects/modifies another's stack or state while that thread is being torn down |
| 12 | **unbounded shared-resource consumption** | CVE-2026-22003, JDK-8192647 | availability-only; shared structure growable without quota, or livelock starvation |

### Priority mapping to JSC-threads (highest expected yield for our audit)
1. Class 8 + 1 — every DFG/FTL-eliminated check (structure, butterfly length, typed-array bounds)
   under a second mutator; our segmented-butterfly republish windows. These are the JS-engine
   shape of the only two classes that produced *exploitable* JVM CVEs (now including
   CVE-2014-0456, the cleanest race→type-confusion specimen).
2. Class 6 + 7 — IC patching, TTL watchpoint fire → jettison → reroute, JITCode lifetime
   (AB-17/AB-17B territory; HotSpot says: one funnel, entry barriers, no shortcuts).
3. Class 4 + 9 — §3.9 is new and severe: write-barrier emission must be atomic with the store
   across any OSR-exit/safepoint; allocation stubs must fence before publish (JDK-8233839);
   sharded atom table = §3.10.
4. Class 2 + 3 — our per-object cell locks and shared-heap-server park queues; JEP 374 is the
   cautionary tale against asymmetric lock fast paths; JDK-8315884 against header-word overload.
5. Class 5 — STW watchdog (TTSP boundedness); JDK-8226705 says handshake-ack is not quiescence.

### Source index
- NVD: https://nvd.nist.gov/vuln/detail/CVE-2014-0456 , /CVE-2020-14803 , /cve-2021-2388 ,
  /cve-2023-21954 , /CVE-2017-3272 , /CVE-2018-3169 , /CVE-2022-21541
- Red Hat Bugzilla: #1087413 (2014-0456), #1889895 (14803), #1983075 (2388), #2187441 (21954),
  #1567121 (2018-2814), #1356987 (2016-3587)
- ZDI-14-114: https://www.zerodayinitiative.com/advisories/ZDI-14-114/
- OpenJDK vulnerability advisories index: https://openjdk.org/groups/vulnerability/advisories/
- JEP 374 (Deprecate and Disable Biased Locking): https://openjdk.org/jeps/374
- Async Monitor Deflation wiki: https://wiki.openjdk.org/display/HotSpot/Async+Monitor+Deflation
- JBS: JDK-6444286, JDK-8161147, JDK-8240723, JDK-8244136, JDK-8264066, JDK-8298191,
  JDK-8028073, JDK-8319137, JDK-8332506, JDK-8352414, JDK-8315884, JDK-8226705, JDK-8247992,
  JDK-8234662, JDK-8290451, JDK-6351173, JDK-8242115, JDK-8295066, JDK-8233839, JDK-8248219,
  JDK-8147611, JDK-8007898, JDK-8253183, JDK-8187577, JDK-8254980, JDK-8343607, JDK-8278965,
  JDK-8313678, JDK-6507007, JDK-8211821, JDK-8268347, JDK-8268571, JDK-8022887, JDK-8291830,
  JDK-6318850, JDK-8215889, JDK-4151836, JDK-7122142, JDK-8156584, JDK-4243978, JDK-8218880,
  JDK-8192647, JDK-8048556, JDK-8212155, JDK-8295214, JDK-8217717
- John Rose, "How HotSpot cross-modifies code": https://cr.openjdk.org/~jrose/hotspot-cmc.html
- SEI CERT DCL00-J (class-init cycles): https://wiki.sei.cmu.edu/confluence/display/java/DCL00-J.+Prevent+class+initialization+cycles
- Phrack "Twenty years of Escaping the Java Sandbox": https://www.exploit-db.com/papers/45517
