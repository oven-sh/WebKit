# Concurrency CVEs and Exploit Techniques in Non-JS Managed Runtimes (CLR/.NET, Go, Erlang/BEAM) + Classic VM Race-Exploitation Techniques

Status: research compendium for the JSC threads security audit (defensive).
Scope: CLR/.NET, Go runtime + stdlib, Erlang/OTP (BEAM/ERTS), plus the classic published
race-exploitation literature (double fetch, TOCTOU, heap-shape races, race-window widening).
Companion docs cover JVM/HotSpot and JS-engine shared-memory CVEs.

Sourcing notes: every entry cites the most primary source found (NVD/MSRC/vendor advisory/bug
tracker/Project Zero). Entries marked **[no CVE]** are vendor-acknowledged bugs fixed in release
notes or bug trackers without CVE assignment — included because BEAM in particular almost never
assigns CVEs to VM-internal races, and the mechanisms are exactly what our implementation must
defend against. Concurrency bugs that are memory-safe-by-construction in the affected runtime
(e.g. Go map-write throw) are included where the *mitigation itself* is instructive.

Root-cause classes use the taxonomy defined in §5; each entry carries its class tag inline.

---

## 1. CLR / .NET (CoreCLR, ASP.NET Core runtime layer)

The CLR's exploitable concurrency surface historically clusters in (a) native runtime services
shared across trust boundaries, (b) the GC/finalizer vs. mutator boundary, and (c) the memory
model itself (torn multi-word values, P/Invoke marshaling double-fetches). Modern .NET dropped
the in-process sandbox, so most *security-graded* races are now in the server stack (Kestrel)
and local IPC.

| ID | Mechanism (one line) | Root-cause class |
|---|---|---|
| **CVE-2024-35264** (MSRC; dotnet/announcements #314) | Kestrel HTTP/3: attacker closes a QUIC stream while the request body is still being processed; the request-processing path keeps using freed stream state → use-after-free → RCE. Race must be won repeatedly; affected .NET 6.0.x/8.0.0–8.0.6. | **Object-lifetime vs in-flight-operation race** (waiter/stream lifetime) |
| **CVE-2023-33127** (MSRC; bohops write-up) | .NET CLR diagnostic-port IPC: local attacker wins a startup race to squat the predictable diagnostic named-pipe name before the target process creates it, then drives the diagnostics protocol to inject code across privilege/session boundary. | **External-resource squatting / startup TOCTOU** |
| **CVE-2024-9355** (NVD; RH bz#2315719) — golang-fips, listed here for the pattern | (See Go section; the same "result published before initialization" pattern recurs in CLR crypto/native interop wrappers.) | **Publication-before-initialization** |
| **[no CVE] .NET finalization race** (MS C++ team blog: "A Race Condition in .NET Finalization") | GC may finalize an object *while an instance method on it is still executing* if `this` is dead after the last field load; finalizer frees native resources the still-running method then touches → UAF. Mitigated by `GC.KeepAlive`/`HandleRef`; a standing footgun class for any runtime with eager liveness + finalizers. | **GC vs mutator reclamation race** (premature reclaim of "still in use by native frame") |
| **[no CVE] ECMA-335 torn reads/writes** (ECMA-335 §I.12.6.6) | CLR only guarantees atomicity for properly-aligned values ≤ native word; racing writers to `long`/`decimal`/large structs on 32-bit produce torn values; if the torn value is a (handle, length) or (ptr, type) pair this is a type-confusion / OOB primitive. | **Torn multi-word publication** |
| **[no CVE] P/Invoke pinned-buffer double-fetch** (classic technique; cf. Bochspwn class) | Native code called via P/Invoke reads a length/offset from a managed (or shared) buffer twice — validate then use — while another thread mutates it between fetches → OOB in the native half of the runtime. Same shape as kernel double-fetch but at the managed/native boundary. | **Double-fetch of shared metadata** |
| **[no CVE, historical] `Thread.Abort` asynchronous interruption** (deprecation rationale, dotnet docs; partial-trust escape literature) | Asynchronous abort could fire between a check and the action it guarded (e.g., inside `finally`-less CER gaps, mid-lock-acquisition), leaving runtime invariants broken; a recurring partial-trust sandbox-escape vector until aborts were removed in .NET Core (throws `PlatformNotSupportedException`). | **Asynchronous interruption at unsafe point** |
| **[no CVE] CLR thread-pool lock convoy / thread-injection feedback loop** (dotnet/runtime #7657) | Not directly exploitable, but documents that the runtime's own scheduler had lock-state feedback races (spinning waiters counted as busy → more injection); relevant as a DoS-amplification class. | **Lock-state transition race** (liveness, not safety) |

Honest gap: searches did not surface additional *memory-safety* race CVEs inside CoreCLR proper
(GC, JIT) with public root-cause detail; MSRC RCE advisories for the runtime are typically
opaque. The two CVEs above are the well-documented concurrency ones.

---

## 2. Go runtime + standard library

Go is the most instructive comparison for us: it is a "memory-safe" language whose safety
guarantee is explicitly **forfeited under data races** because interface values, slice headers,
and string headers are multi-word and published non-atomically — exactly the hazard class our
TID/SW-tagged structure IDs and segmented butterflies are designed to close.

| ID | Mechanism (one line) | Root-cause class |
|---|---|---|
| **CVE-2025-47907** (Go issue/announce; NVD) | `database/sql`: cancelling a query via context during `Rows.Scan` while other queries run in parallel lets the connection be returned/reused mid-scan → Scan returns *another query's* rows or errors; cross-request data leak. Fixed 1.24.6/1.23.12. | **Cancellation/completion race** (shared completion state reused across logical owners) |
| **CVE-2020-15586** (NVD; golang.org/issue/34902 advisory) | `net/http`: data race on the request body between the connection-serving goroutine and a handler/ReverseProxy for Expect:100-continue requests → races on shared buffer state → crash/DoS in servers. | **Double consumption of shared connection state** (cancellation/handoff race) |
| **CVE-2021-36221** (NVD; Go announce) | `net/http/httputil` ReverseProxy: race between `ErrAbortHandler` panic path and concurrent body read after request cancellation → panic/DoS. | **Cancellation/completion race** |
| **CVE-2024-9355** (NVD; RH bz#2315719) | golang-fips/openssl: race lets a zeroed buffer + uninitialized length be returned from HMAC/key-derivation in FIPS mode → attacker can force false-positive HMAC compare or all-zero derived keys. | **Publication-before-initialization** (result visible before init complete) |
| **[no CVE] Interface-value tearing → type confusion → arbitrary code** (StalkR 2015, "Golang data races to break memory safety"; Russ Cox "Off to the Races") | Interface = (itab/type ptr, data ptr) written as two separate stores; a racing reader observes type-of-A with data-of-B → confuse a struct holding a func ptr with one holding an int → control PC entirely from "safe" Go. The canonical user-level torn-fat-pointer exploit. | **Torn multi-word publication → type confusion** |
| **[no CVE] Slice-header tearing** (same write-ups; qouteall gist "fat pointer tearing") | Slice = (ptr, len, cap) published non-atomically; racing reader pairs old ptr with new len/cap → out-of-bounds read/write at chosen extent. Direct analog of "double-fetch of shared length" against array storage. | **Torn multi-word publication / shared-length confusion** |
| **[no CVE] Concurrent map write** (runtime `fatal: concurrent map writes`) | Pre-detection Go corrupted hashmap internals under racing writers; the modern runtime *deliberately crashes* on detected concurrent map mutation rather than risk corruption — an example of "detect-and-die" as a containment policy for an unsynchronized structure. | **Unsynchronized structural mutation** (mitigated by fail-stop) |
| **[no CVE] `sync.Pool`/buffer-reuse leaks** (recurring class across Go ecosystem, e.g. multiple `net/http2` fixes) | Object returned to a pool while a racing reader retains it → cross-request data disclosure; the GC keeps it memory-safe but not *information-safe*. | **Object-lifetime vs in-flight-operation race** (logical UAF) |

Key takeaway for us: a GC'd runtime converts UAF into *information-flow* bugs but tearing of
multi-word cells converts straight back into full memory-unsafety. Every multi-word "value"
visible to two threads in our design (structureID+butterfly, length+vector pointer) must be
published with the SW-tag / release-store discipline or be confined per-thread.

---

## 3. Erlang/OTP (BEAM / ERTS)

BEAM is shared-nothing at the language level, so its concurrency bugs concentrate exactly where
sharing leaks back in: ETS tables, the scheduler/allocator internals, signal queues, ports, and
NIF resource lifetimes. Vendor practice is to fix VM races in ERTS release notes without CVEs;
the one headline OTP CVE (SSH RCE) is *not* a race and is listed only for completeness.

| ID | Mechanism (one line) | Root-cause class |
|---|---|---|
| **CVE-2025-32433** (NVD; Unit42) | SSH server pre-auth message-handling logic flaw → unauth RCE. Not a concurrency bug; excluded from taxonomy stats, listed to preempt "what about the famous OTP CVE". | — (protocol state machine, not a race) |
| **[no CVE] ETS fixation vs delete race** (ERTS release notes: "bug in ETS could cause VM crash if process A terminates after fixating a table and process B deletes the table at 'the same time'") | Table fixation (a reader pin) raced with table deletion → VM crash (freed table accessed). The pin/unpin vs teardown handshake is the same shape as our cell-lock vs heap-server teardown ordering. | **Object-lifetime vs in-flight-operation race** (pin vs delete) |
| **[no CVE] Signal-queue inconsistency race** (ERTS release notes, OTP 26.x: "race condition which was very rarely triggered could cause the signal queue of a process to become inconsistent causing the runtime system to crash") | Concurrent senders + receiver-local queue maintenance corrupted the per-process signal queue → crash. Cross-thread message/waiter queue with a lock-free fast path. | **Waiter/queue-list lifetime & linkage race** |
| **[no CVE] `process_flag(priority, Prio)` race** (erlang.org OTP 23.1 patch notes) | System-task scheduling raced with a concurrent priority change → priority elevation for the system task silently ignored; harmless here, but it is a textbook *lost state transition* under concurrent flag mutation. | **Lock-state/flag transition race** |
| **[no CVE] Allocator carrier deletion race** (ERTS release notes: scheduler stuck deleting a memory-allocator carrier while adjacent carriers deleted/inserted by other schedulers) | Concurrent carrier (superblock) insert/delete in the shared allocator deadlocked schedulers system-wide. Direct analog for our shared-heap-server block teardown. | **Lock-state transition race** (allocator metadata) |
| **ERL-90 / erlang/otp #3471 [no CVE]** | `inet:tcp_controlling_process` race: socket ownership handoff raced with incoming data → messages delivered to the wrong process or lost. Ownership-transfer races = our thread-affinity handoff for natives. | **Ownership-handoff race** (sub-class of cancellation/handoff) |
| **[no CVE] ETS `update_counter/4`/`update_element/4` keypos bug** (ERTS notes) | Accepted a default tuple smaller than keypos → internally inconsistent table → crash on later access by any process. Not a race per se, but a shared-structure invariant violation observed concurrently. | **Unsynchronized structural-invariant violation** |
| **[no CVE] NIF resource lifetime races** (enif_release_resource discipline, erl_nif docs) | Standing class: NIF resource destructors run on a scheduler thread when refcount hits zero; native code that stashes raw pointers past `enif_release_resource` races the destructor → UAF in the VM process. | **GC vs mutator reclamation race** (refcount edition) |

---

## 4. Classic published VM/kernel race-exploitation techniques

These are the technique papers every entry above instantiates. They matter to us both as attack
patterns and because several are *attacker tooling for making tiny windows winnable* — i.e., our
"the window is only 3 instructions" arguments are not defenses.

| ID / Reference | Mechanism (one line) | Root-cause class (or technique role) |
|---|---|---|
| **Bochspwn** (Jurczyk & Coldwind 2013; googleprojectzero/bochspwn; MS13-016/-017/-031/-036) | Full-system instrumentation found ~37+ Windows-kernel **double fetches**: fetch length/ptr from shared user memory, validate, re-fetch for use; flip the value between fetches → OOB/priv-esc. The defining double-fetch corpus. | **Double-fetch of shared metadata** |
| **j00ru, "Kernel double-fetch race condition exploitation on x86 — further thoughts" (2013)** | Micro-techniques for winning instruction-scale windows: cache-line splitting, TLB/paging tricks, sibling-hyperthread flipping loops; demonstrates windows of *a few instructions* are reliably winnable. | Technique: **race-window amplification** |
| **Project Zero, "Racing against the clock" (Jann Horn, 2022)** | Hit a ~tiny Linux kernel race at ~30% by widening the window with a forced cache miss, expiring a timerfd *inside* the window, and making its wakeup churn 50k epoll waitqueue items; UAF evidenced via huge fd table + userfaultfd. Proof that "unwinnable" windows fall to scheduler/IRQ shaping. | Technique: **race-window amplification** |
| **Project Zero, "Windows Exploitation Tricks: Trapping Virtual Memory Access" (Forshaw, 2021)** | Make the *victim's* access to attacker-controlled memory fault and block (sections/placeholders), converting probabilistic double-fetch/TOCTOU races into **deterministic** wins. (Linux analog: userfaultfd.) | Technique: **deterministic window control** |
| **Watson, "Exploiting Concurrency Vulnerabilities in System Call Wrappers" (WOOT 2007)** | The original systematic TOCTOU-via-concurrent-argument-mutation paper: wrapper checks syscall args, kernel later re-reads them after another thread rewrote them → complete bypass of the checking layer. Canonical "check on one fetch, act on another" in a multi-threaded address space. | **TOCTOU between check and use under threads** |
| **Dirty COW, CVE-2016-5195** (NVD; dirtycow.ninja) | Linux COW: retry loop in `get_user_pages` raced with `madvise(MADV_DONTNEED)` so a write intended for a private copy landed on the read-only original → write to any readable file → root. Exploits a *state-machine transition* (COW broken-ness) not a pointer. | **Lock-state/state-machine transition race** |
| **Bochspwn Reloaded** (Jurczyk, Black Hat USA 2017) | Taint-tracking variant: kernel→user copies of uninitialized stack/heap memory (70+ Windows, 10+ Linux bugs); the disclosure twin of the publication-before-initialization class. | **Publication-before-initialization** (disclosure form) |
| **Go interface-tear exploit** (StalkR 2015; see §2) | The reference *user-level managed-runtime* race exploit: torn fat pointer → type confusion → PC control without `unsafe`. | **Torn multi-word publication** |
| **Shared-memory heap-shape races** (generic; detailed per-engine entries in the JS-engine companion doc) | One thread flips an object's shape/length/backing store between another thread's check and access (the SAB/worker pattern); in any shared-heap VM this is the composite of double-fetch-of-length + TOCTOU-of-type. Our butterfly segmentation + SW-tag exist precisely to make the check and the use cover one atomically-published snapshot. | **Heap-shape race** (composite: double-fetch + TOCTOU) |
| **Asynchronous interruption** (Thread.Abort lineage, POSIX signal-in-VM bugs) | Interrupt delivery between invariant-breaking and invariant-restoring instructions; any VM with safepoint-less interruption re-derives this bug. | **Asynchronous interruption at unsafe point** |

---

## 5. Root-cause class taxonomy (summary)

Classes ordered roughly by relevance to our implementation. "Our surface" names where the class
lands in the threads design (SPEC-objectmodel / SPEC-heap / SPEC-vmstate / UNGIL-HANDOUT terms).

1. **Torn multi-word publication** — a logically-atomic value (type ptr + data ptr; ptr + len)
   is written as multiple stores; racing reader pairs halves from different writes → type
   confusion / OOB. *Instances:* Go interface/slice tears; CLR torn structs. *Our surface:*
   StructureID + butterfly pointer pairing; SW-tag is the designed mitigation — audit every
   place a (structure, storage, length) tuple is read non-atomically.
2. **Double-fetch of shared metadata** — same field fetched twice (validate, then use) with a
   writer in between. *Instances:* Bochspwn corpus; P/Invoke buffers. *Our surface:* any JIT- or
   runtime-emitted re-load of butterfly length / structure bits after a bounds or shape check;
   per-tier check placement must load once into a register and use that copy.
3. **TOCTOU between type check and use under threads** — the check and the dependent operation
   span a window in which another mutator legally changes the object. *Instances:* Watson 2007;
   heap-shape races. *Our surface:* IC fast paths, DFG/FTL check hoisting (a check valid at
   hoist point is not valid at use point once N mutators exist), TTL watchpoint validity.
4. **Object-lifetime vs in-flight-operation race (incl. waiter-list lifetime)** — teardown/free
   races a still-running user of the object. *Instances:* CVE-2024-35264 (stream UAF);
   ETS fixation-vs-delete; sync.Pool leaks; waiter lists in any park/unpark design. *Our
   surface:* per-object cell-lock vs sweep, Thread object teardown vs threads parked on its
   structures, heap-server block retirement.
5. **GC vs mutator publication/reclamation race** — collector frees or finalizes while a mutator
   (often a native frame) still holds a raw reference; or object becomes visible to GC before
   fully initialized. *Instances:* .NET finalization race; NIF resource UAF; Bochspwn-Reloaded
   (disclosure form); CVE-2024-9355 (init form). *Our surface:* concurrent-GC marking with N
   mutators (SPEC-congc), pre-initialization publication of cells, conservative-scan windows.
6. **Cancellation / completion / ownership-handoff race** — an async cancel or ownership
   transfer races the completion path; shared completion state gets observed by the wrong
   logical owner. *Instances:* CVE-2025-47907, CVE-2020-15586, CVE-2021-36221, ERL-90. *Our
   surface:* Thread.join/terminate vs running thread, promise/microtask handoff cross-thread,
   native-affinity handoff (SPEC-nativeaffinity).
7. **Lock-state / state-machine transition race** — a multi-step state machine (COW, allocator
   carrier, priority flag) admits an interleaving that skips or repeats a transition.
   *Instances:* Dirty COW; ERTS carrier deletion; process_flag priority. *Our surface:* cell-lock
   acquire/inflate/deflate transitions, safepoint state machine (the AB-17b watchdog finding is
   exactly this class), TTL transition ordering.
8. **External-resource squatting / startup TOCTOU** — predictable-name resource created by the
   runtime is pre-claimed by a local attacker. *Instances:* CVE-2023-33127. *Our surface:* low —
   but any debug/inspector socket the threads work adds must bind before advertising.
9. **Publication-before-initialization** — result/object made reachable before its contents are
   initialized (missing release ordering or error-path init skip). *Instances:* CVE-2024-9355;
   Bochspwn Reloaded. *Our surface:* zero-fill + release-store ordering on fresh allocations
   visible to other mutators; error paths in shared-heap allocation.
10. **Asynchronous interruption at unsafe point** — abort/signal lands between breaking and
    restoring an invariant. *Instances:* Thread.Abort; signal handling in VMs. *Our surface:*
    thread termination requests must be safepoint-polled, never delivered asynchronously.
11. **Unsynchronized structural mutation (fail-stop as containment)** — Go's concurrent-map
    fatal error shows "detect and crash" is a legitimate last-line policy for structures we
    cannot afford to lock; candidate posture for any JSC structure we declare non-shareable.
12. **Race-window amplification (attacker technique, not a bug class)** — cache-miss shaping,
    IRQ/timerfd injection, waitqueue churn, userfaultfd/section trapping make 3-instruction
    windows reliably winnable (j00ru 2013; Horn 2022; Forshaw 2021). *Audit rule:* no finding
    may be downgraded on "window too small" grounds; only on "no writer can exist" grounds.

### Cross-runtime observations

- GC'd runtimes do not eliminate the classes — they transmute UAF (class 4/5) into data
  disclosure, while tearing (class 1) remains full memory-unsafety everywhere.
- The two classes with public RCE-grade outcomes in *managed* runtimes are exactly class 1
  (Go interface tear) and class 4 (Kestrel HTTP/3 UAF) — the two our object-model design spends
  the most machinery on (SW-tag, segmented butterflies; cell locks + safepointed teardown).
- Shared-nothing (BEAM) pushes all bugs into the small shared substrate (ETS/allocator/queues);
  expect the same concentration in our shared heap server and atom-table shards.

## Primary sources

- NVD: CVE-2024-35264, CVE-2023-33127, CVE-2025-47907, CVE-2020-15586, CVE-2021-36221,
  CVE-2024-9355, CVE-2016-5195, CVE-2025-32433
- MSRC update guide: msrc.microsoft.com/update-guide/vulnerability/CVE-2024-35264
- dotnet/announcements #314 (CVE-2024-35264 advisory); bohops.com 2023-11-27 (CVE-2023-33127)
- devblogs.microsoft.com/cppblog — "A Race Condition in .NET Finalization and its Mitigation for C++/CLI"
- Red Hat Bugzilla #2315719 (CVE-2024-9355)
- blog.stalkr.net/2015/04/golang-data-races-to-break-memory-safety.html; research.swtch.com "Off to the Races"
- erlang.org ERTS release notes (OTP 23.1, 26.x, 27.x); erlang/otp #3471 (ERL-90)
- github.com/googleprojectzero/bochspwn; j00ru.vexillium.org (double-fetch exploitation, Bochspwn Reloaded)
- projectzero.google 2022-03 "Racing against the clock"; 2021-01 "Trapping Virtual Memory Access"
- Watson, WOOT'07, "Exploiting Concurrency Vulnerabilities in System Call Wrappers"
