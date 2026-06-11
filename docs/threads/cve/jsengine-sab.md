# JS-Engine Shared-Memory CVE Survey: SharedArrayBuffer / Atomics / Workers / wasm threads

Scope: V8, JavaScriptCore, SpiderMonkey vulnerabilities involving SharedArrayBuffer (SAB),
Atomics, Workers/agents, wasm shared memory and threads — detach/resize races, waiter-list
bugs, shared-memory JIT bounds bugs, plus structural Spectre-class notes. Compiled
2026-06-07 from primary sources (NVD, vendor advisories, bug trackers, Project Zero / ZDI /
phoenhex writeups). Each entry carries a root-cause CLASS from the taxonomy at the end.

Exhaustiveness caveat: Chrome and Apple advisories are deliberately terse ("type confusion
in V8"), and most engine security bugs are never publicly root-caused. The list below is as
exhaustive as public attribution permits; the taxonomy is the durable artifact. A striking
negative result: the *waiter-list* class (W below) has essentially no public CVEs in any
engine — either the code is genuinely solid (it is small and lock-dominated in all three
engines) or it is under-audited. Treat that as a hint, not comfort.

---

## 1. Core entries — shared memory raced by a second JS agent

### CVE-2017-5116 — V8 (Chrome 61)
- **Mechanism:** wasm bytecode placed in a SharedArrayBuffer and passed to
  `WebAssembly.compile` while a Worker concurrently rewrites the bytes; the parser
  validates one byte sequence, the compiler consumes another (TOCTOU on the module
  bytes). Used by Guang Gong (Qihoo 360 Alpha) in a full Android Chrome remote chain.
- **Class:** A — double-fetch of shared bytes by a trusted consumer.
- **Sources:** [tunz js-vuln-db entry](https://github.com/tunz/js-vuln-db/blob/master/v8/CVE-2017-5116.md),
  NVD CVE-2017-5116.

### Mozilla bug 1352681 — SpiderMonkey/Gecko (Firefox 53–55, pre-release only)
- **Mechanism:** structured clone of a `SharedArrayBufferObject` increments the
  `SharedArrayRawBuffer::refcount_` at *serialize* time; if deserialization then fails
  (e.g. unclonable object later in the payload), the increment is never paired with an
  object. `refcount_` is `uint32_t` with no overflow check, so repeated leaks via workers
  wrap it to 1; one free then releases the shared mapping under all live views → cross-
  thread UAF on the shared backing store. Found by saelo; exploited end-to-end by phoenhex.
  Release Firefox was never affected (SAB still pref'd off — this bug contributed to that).
- **Class:** L — shared-backing-store lifetime accounting (refcount across agents).
- **Sources:** [phoenhex writeup](https://phoenhex.re/2017-06-21/firefox-structuredclone-refleak),
  [bugzilla 1352681](https://bugzilla.mozilla.org/show_bug.cgi?id=1352681).

### CVE-2026-5893 — V8 (Chrome, 2026)
- **Mechanism (per third-party advisory; Google advisory is terse):** race condition in
  V8's handling of concurrent operations on shared memory resources — TOCTOU between
  validation and use leading to heap corruption.
- **Class:** A (claimed). Flag: third-party summary only; re-verify against the Chromium
  bug when it unrestricts.
- **Source:** [SentinelOne vuln DB](https://www.sentinelone.com/vulnerability-database/cve-2026-5893/).

### Cross-engine Atomics.wait memory-ordering bug (no CVE, 2025–2026 reports)
- **Mechanism:** a waiter whose `Atomics.wait` returns `"not-equal"` does not synchronize-
  with prior stores from the notifying agent on weakly-ordered hardware (ARM): the
  comparison load was not given seq-cst/acquire strength on the fast-fail path. Correctness
  not memory-safety, but it silently breaks user mutexes built on Atomics — i.e. it converts
  *user* code into class-A bugs.
- **Class:** W — waiter-list / wait-protocol semantics.
- **Source:** [cross-engine report](https://dev.to/lostbeard/we-found-a-memory-ordering-bug-in-every-major-browser-engine-heres-the-fix-3fgk).

---

## 2. wasm Memory / Table grow and detach — the resize-race family

These are mostly *sequential* races (re-entrancy via JS callbacks rather than a second
thread), but the broken invariant — "base/length observed at T0 still valid at T1" — is
exactly the invariant a second mutator attacks, which is why this family is the best
single-threaded predictor of shared-memory bug shapes.

### CVE-2017-15399 — V8 (Chrome 62, Pwn2Own-class, Zhao Qixun / Qihoo 360 Vulcan)
- **Mechanism:** `WebAssembly.Memory.grow()` frees and reallocates the backing store; the
  grow-guard checks were applied to `WasmInstanceObject` but not the asm.js-translated
  path, so compiled asm.js code kept the old hard-coded buffer address → UAF of the old
  backing store while code still reads/writes it.
- **Class:** R — grow/detach invalidates a cached base/length someone still holds.
- **Sources:** [Haboob case study](https://blog.haboob.sa/blog/chrome-exploitation-an-old-but-good-case-study),
  Silvanovich, *The Problems and Promise of WebAssembly*, Black Hat USA 2018.

### CVE-2018-5093 — SpiderMonkey (Firefox 58)
- **Mechanism:** integer/buffer overflow in WebAssembly during `Memory`/`Table` resizing
  (OSS-Fuzz find): resize arithmetic overflows, committed length disagrees with accessible
  length.
- **Class:** R + I — resize-time length arithmetic overflow.
- **Sources:** MFSA 2018-03, Black Hat 2018 deck (above).

### CVE-2017-5122 — V8 (Chrome 61)
- **Mechanism:** `Table.grow`/wasm OOB after `Symbol.toPrimitive` overwrite: argument
  coercion runs user JS *after* limits were validated; callback mutates the table/memory →
  stale bound used for the subsequent access.
- **Class:** S — side-effect re-entrancy violating a single-mutation assumption
  (sequential cousin of "JIT assumes single mutator").
- **Source:** Black Hat 2018 deck; Chromium advisory for 61.0.3163.100.

### CVE-2017-15401 — V8 (Chrome 62, Pwn2Own)
- **Mechanism:** import-object getter (user JS) runs during `WebAssembly.Instance`
  creation; attacker instantiates a second module / calls `memory.grow()` mid-
  instantiation, corrupting memory layout assumptions of the half-built instance → OOB
  write.
- **Class:** S.
- **Source:** [tunz js-vuln-db entry](https://github.com/tunz/js-vuln-db/blob/master/v8/CVE-2017-15401.md), crbug 766260.

### V8 bug 826434 — V8 (2018, no CVE located)
- **Mechanism:** mutating a wasm Table during a call through that table drops the handle
  to the callee's instance → UAF; first fix forbade table change during call, still
  reachable via element-segment initialization.
- **Class:** R/S — container mutation under an active consumer.
- **Source:** Black Hat 2018 deck; crbug 826434.

### CVE-2018-4222 — JSC (Safari 11.1.1)
- **Mechanism:** `WebAssembly.Module` compiled from a TypedArray view: compilation read
  out of bounds of the source buffer (view offset/length handling), leaking heap bytes
  into compiled-module state. The compile-from-user-visible-buffer pipeline again.
- **Class:** A-adjacent — privileged consumer mis-reads an attacker-controlled buffer
  whose bounds/identity can shift.
- **Source:** Black Hat 2018 deck; Apple security notes for Safari 11.1.1.

### CVE-2018-4121 — JSC (Safari 11.1)
- **Mechanism:** wasm section-order validation bypass (`validateOrder` treats anything
  after a Custom section as ordered) → duplicate/out-of-order sections reach code that
  assumed validated state.
- **Class:** V — validator/consumer disagreement (the same shape a racing writer induces).
- **Source:** Black Hat 2018 deck; lokihardt/P0.

### CVE-2018-6092 — V8 (Chrome 66)
- **Mechanism:** `count + type_list->size()` overflows 32 bits when validating wasm
  function local counts → undersized allocation, OOB during function decode.
- **Class:** I — integer overflow at a trust boundary (becomes R-class once buffers are
  shared and growable).
- **Source:** Black Hat 2018 deck; Chromium advisory 66.

---

## 3. Growable SAB / resizable ArrayBuffer era (2023→)

### CVE-2024-2887 — V8 (Chrome, Pwn2Own Vancouver 2024, Manfred Paul)
- **Mechanism:** wasm type-section parser lets type indices exceed `kV8MaxWasmTypes` via
  recursive type groups, so user-defined heap types alias internal heap types → universal
  cast / type confusion. The published exploit's **V8-sandbox escape leg used an integer
  underflow in growable SharedArrayBuffer length calculation** — GSAB length arithmetic is
  now load-bearing for the sandbox boundary.
- **Class:** V (main bug) + I/R (GSAB length-underflow leg).
- **Source:** [ZDI writeup](https://www.thezdi.com/blog/2024/5/2/cve-2024-2887-a-pwn2own-winning-bug-in-google-chrome).

### Structural: length-tracking TypedArrays over RAB/GSAB in optimizing JITs
- With resizable/growable buffers, a TypedArray's length **can no longer be cached** and
  must be recomputed at every access; TurboFan's length-access building
  (`graph-assembler.cc`) and its bounds-check elimination are now correctness-critical on
  every tier. Any tier that hoists or CSEs a GSAB length/bounds check across a potential
  grow (which, for *shared* buffers, can happen on another thread with no interleaving
  point visible to the compiler) is an OOB factory. V8's typer-driven BCE has already
  produced this shape without shared memory (CVE-2019-5782).
- **Class:** J — JIT caches/elides a bound that another agent can move.
- **Sources:** [ZDI CVE-2024-2887](https://www.thezdi.com/blog/2024/5/2/cve-2024-2887-a-pwn2own-winning-bug-in-google-chrome),
  [P0 in-the-wild Chrome series](https://projectzero.google/2021/01/in-wild-series-chrome-exploits.html),
  [TC39 resizable-buffer proposal notes on hoistability](https://github.com/tc39/proposal-resizablearraybuffer).

---

## 4. Worker / agent lifecycle

### CVE-2020-12387 — Gecko workers (Firefox 76 / ESR 68.8, MFSA 2020-16/17)
- **Mechanism:** race in Web Worker *shutdown* path (worker teardown vs in-flight
  XMLHttpRequest completion) → UAF, "systemic problems around worker shutdown",
  instruction-pointer control per the bug. Browser-side rather than SpiderMonkey-side, but
  the failing invariant — thread teardown vs work still targeting that thread's heap — is
  the one any JS-thread runtime must pin down.
- **Class:** T — agent-teardown vs in-flight work.
- **Sources:** [NVD](https://nvd.nist.gov/vuln/detail/CVE-2020-12387),
  [bugzilla 1545345](https://bugzilla.mozilla.org/show_bug.cgi?id=1545345),
  [MFSA 2020-16](https://www.mozilla.org/en-US/security/advisories/mfsa2020-16/).

(Adjacent, not enumerated: the long tail of Gecko/WebKit DOM-worker UAFs. They are
browser-embedding bugs, but nearly all reduce to class T.)

---

## 5. Recent wasm-pipeline entries with thread-relevant structure

### CVE-2025-13016 — SpiderMonkey (Firefox, 2025)
- **Mechanism:** wasm-GC inline-array copy computed byte counts with byte-addressed
  pointers but copied through `uint16_t*` → boundary mis-copy / corruption.
- **Class:** I — unit-confusion in size arithmetic on engine-managed shared-ish storage.
- **Source:** [AISLE analysis](https://aisle.com/blog/a-high-severity-webassembly-boundary-condition-vulnerability-in-firefox-cve-2025-13016), MFSA.

### CVE-2026-2796 — SpiderMonkey (Firefox, 2026)
- **Mechanism:** wasm JIT miscompiles certain instructions' type information (`&` vs `|`
  pointer-tag typo lineage) → OOB / arbitrary R/W in the renderer.
- **Class:** V/J — compiler emits code that disagrees with the validator's type model.
- **Sources:** [kqx writeup](https://kqx.io/post/firefox0day/),
  [SentinelOne](https://www.sentinelone.com/vulnerability-database/cve-2026-2796/).

---

## 6. Spectre-class (structural only)

- **CVE-2017-5753 / CVE-2017-5715 (Spectre v1/v2):** not engine bugs, but SAB+Atomics gave
  a no-permission ~nanosecond timer that made speculative side channels practical from JS.
  Every vendor shipped the same structural response: **January 2018 — SAB disabled in all
  browsers**; reintroduced only behind cross-origin isolation (COOP/COEP,
  `crossOriginIsolated`) plus process-per-site; JSC re-enabled SAB/Atomics 2020-11
  ([webkit bug 218944](https://bugs.webkit.org/show_bug.cgi?id=218944)).
- **Structural lesson for us:** a shared-memory clock is a *capability*. Any embedder
  flag like `--useJSThreads` is also granting timing-attack capability to all code in the
  process; bounds-check masking (index masking / pointer poisoning, which all three
  engines added in 2018) must be assumed necessary on every shared-heap load reachable
  from speculation, not just typed-array paths.
- **Sources:** [tc39/security #3](https://github.com/tc39/security/issues/3), vendor
  Spectre notes (V8 "Untrusted code mitigations", Mozilla/WebKit Jan 2018 posts).

---

## 7. Root-cause class taxonomy (summary)

| Class | Name | Definition | Exemplars |
|---|---|---|---|
| **A** | Double-fetch of shared bytes | Trusted consumer (parser, compiler, structured clone, sort) reads attacker-shared memory more than once, or validates then consumes, while another agent can write between fetches. | CVE-2017-5116, CVE-2026-5893, CVE-2018-4222 (adjacent) |
| **R** | Resize/detach vs cached base+length | grow/detach/transfer replaces a backing store or moves a bound while some holder (JIT code, native frame, view, half-built instance) retains the old base or length. | CVE-2017-15399, CVE-2018-5093, V8 826434 |
| **S** | Side-effect re-entrancy under a single-mutation assumption | User JS runs (coercion, getter, import resolution) inside a privileged operation that pre-validated state; the sequential twin of "second mutator appears mid-operation". Every S bug becomes an A/R bug when real threads exist. | CVE-2017-5122, CVE-2017-15401 |
| **J** | JIT assumes single mutator | Compiler caches/hoists/CSEs a length, base pointer, shape, or bounds check across a point where another agent can legally mutate it (GSAB grow, shared-struct shape, shared length). Includes BCE over now-movable bounds. | CVE-2024-2887 GSAB leg, RAB/GSAB length-tracking structure, CVE-2019-5782 (shape) |
| **L** | Shared-backing-store lifetime accounting | Refcount/ownership of the shared raw buffer mismanaged across agent boundaries (serialize/deserialize failure paths, unbalanced increments, width-limited counters). | Mozilla 1352681 |
| **T** | Agent-teardown vs in-flight work | Thread/worker shutdown frees state still targeted by queued or executing work (callbacks, IPC completions, pending compiles). | CVE-2020-12387 |
| **W** | Waiter-list lifetime & wait-protocol semantics | Futex/waiter-queue node lifetime across agent death, timeout vs notify races, and memory-ordering strength of wait/notify fast paths. **No public memory-safety CVE in any engine** — under-reported, not absent; the cross-engine `not-equal` ordering bug shows latent defects exist. | ordering bug (no CVE), Deno waitAsync hang |
| **V** | Validator/consumer disagreement | Code consuming "already validated" input under different assumptions than the validator enforced (section order, type-index limits, type models). A racing writer manufactures V-bugs from correct validators — A is the racing special case of V. | CVE-2018-4121, CVE-2024-2887 main, CVE-2026-2796 |
| **I** | Trust-boundary integer arithmetic | Overflow/underflow/unit-confusion computing sizes/limits for shared or growable storage. Feeder class: I at a resize site yields R; I at a length site yields J. | CVE-2018-6092, CVE-2024-2887 GSAB leg, CVE-2025-13016 |
| **SP** | Speculative side channel enabled by shared memory | SAB-as-timer; structural, mitigated by isolation + index masking, not by fixing a bug. | Spectre v1/v2 |

### Reading for our implementation (one paragraph)
The historical record says the bugs do not live in `Atomics.*` itself — they live wherever
**a privileged single-threaded pipeline touches memory another agent can move or mutate**:
compilers reading shared bytes (A), grow paths invalidating cached bases (R), JIT tiers
caching bounds (J), and serialization-failure paths unbalancing shared lifetimes (L).
Phase-2 GIL removal converts *every* S-class pattern in JSC (toPrimitive hooks, getters
inside privileged ops) into a genuine A/R race, and our TID/SW-tagged object model plus
segmented butterflies put us in J-class territory on every hoisted shape/butterfly-bound
check — those, plus W (which the industry has never publicly audited), are where the
susceptibility tests should concentrate.

---

*Compiled for the defensive audit of the jarred/threads shared-memory work. Companion
files: see docs/threads/cve/ for the JVM/HotSpot survey and the mapping/susceptibility
matrix (separate workflow steps).*
