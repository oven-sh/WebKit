# INTEGRATE-api.md — shared-hot-file manifest for the api workstream

Authority: docs/threads/SPEC-api.md (rev 14) §9.2. This file is the ONLY place
the api workstream records text destined for shared hot files (OptionsList.h,
JSGlobalObject.cpp, Sources.txt, CMakeLists.txt, JSObject.* hook sites,
threads.yaml). The api implementer never edits those files directly.

Status legend: VERIFIED-IN-TREE = the prep step (GIL stub) already landed text
equivalent to the canonical hunk; the integrator only needs to audit it against
the canonical text below. READY = exact ready-to-apply diff recorded below,
context-verified against the working tree; the integrator applies it. PENDING =
supplied by a later task of SPEC-api §10; placeholder recorded now.

FINALIZED at §10 task 14. "Build-tested" status of each hunk, per the
fan-out's no-build rule (this task could not invoke the compiler):
9.2-1/2/4/5 are LIVE in the working tree (landed by the prep stub) and that
tree builds and runs the corpus — those hunks ARE build-tested as they stand;
the finalization diffs below (alias removal, guard respelling) are removals/
one-token substitutions whose post-state is the canonical SPEC text.
9.2-6/7/8 are NOT yet applied; every context line in their diffs was
re-verified verbatim against the working tree at task-14 time (file+anchor
listed per hunk), and all referenced declarations
(threadRestrictCheck, VMLite/VMLiteRegistry, the butterfly-TID-tag entry
points) were checked to exist with the exact signatures used. First compile
of those three hunks happens at INT; any drift surfaces as a trivial context
mismatch, not a semantic question.

---

## 9.2-1 runtime/OptionsList.h — the four §3 options [VERIFIED-IN-TREE]

Canonical text (SPEC-api 9.2-1; this text governs; format of :638/680; nothing
after a continuation backslash):

```
v(Bool, useJSThreads, false, Normal, "enable shared-memory Thread/Lock/Condition/ThreadLocal API"_s) \
v(Unsigned, maxJSThreads, 32766, Normal, nullptr) \
v(Unsigned, jsThreadGILTimeSliceMs, 0, Normal, "reserved, inert in phase 1 (SPEC-api Deviation 9)"_s) \
v(Unsigned, jsThreadStackSizeKB, 0, Normal, nullptr) \
```

Verified pre-applied at OptionsList.h:681-684, byte-identical to the canonical
text (Task 1 gate: present, no STOP; re-verified at task 14).

Dedupe rule: vs jit M1 / objectmodel 10-1, exactly ONE copy of these four
entries lands, and this text is canonical. `useConcurrentJS` must NOT exist
(grep lint; SPEC-api G33; re-grepped clean at task 14).

DEVIATIONS TO RESOLVE AT INT — the tree additionally carries TWO
non-canonical options directly after the four (re-audited at task 14;
:686 was not recorded in earlier revisions of this manifest):

- OptionsList.h:685 `useThreads` — prep-stub alias. As of round 3 it has
  ZERO consumers: `useJSThreadsEnabled()` (runtime/ThreadManager.h,
  api-owned) no longer reads it and the 16 prep-corpus test headers were
  migrated (both paired edits LANDED, see below). The entry is a dead
  option awaiting deletion.
- OptionsList.h:686 `useThreadGIL` — prep-stub reservation with NO code
  consumer anywhere in Source/ (sole mention is a comment in
  ThreadManager.h:52; verified by grep at task 14). SPEC-api §3 reserves the
  GIL-knob name as `jsThreadGILTimeSliceMs` (already landed, line :683);
  `useThreadGIL` is dead weight.

SPEC-api 9.2-1 is "dedupe/no-alias". Final ready-to-apply diff (context
verified verbatim at OptionsList.h:684-687). NOTE the two deletion lines have
DIFFERENT preconditions — see the cross-WS conflict note immediately below;
do not apply the second line blind:

```diff
--- a/Source/JavaScriptCore/runtime/OptionsList.h
+++ b/Source/JavaScriptCore/runtime/OptionsList.h
@@ after the four canonical entries (anchor: jsThreadStackSizeKB) @@
     v(Unsigned, jsThreadStackSizeKB, 0, Normal, nullptr) \
-    v(Bool, useThreads, false, Normal, "alias for useJSThreads"_s) \
-    v(Bool, useThreadGIL, true, Normal, "serialize all JS Thread execution under a global lock (always on in phase 1; reserved)"_s) \
     v(Bool, useMoreCurrencyDisplayChoices, false, Normal, "Enable more currencyDisplay choices for Intl.NumberFormat"_s) \
```

CROSS-WS CONFLICT NOTE (mirrors INTEGRATE-vmstate items 14 and 16; owed by
this manifest since vmstate round 3). The "no call site reads
Options::useThreads()/Options::useThreadGIL()" grep is true of the WORKING
TREE but falsified by two READY hunks in docs/threads/INTEGRATE-vmstate.md:

- `useThreadGIL` (:686): vmstate M4's install-path backstop is
  `RELEASE_ASSERT(!Options::useJSThreads() || Options::useThreadGIL());`
  (INTEGRATE-vmstate item 13/16) — the ONLY mechanical fail-stop against a
  GIL-off run reaching the shared-tid-0 main-carrier install path (a
  heap-corruption class per SPEC-objectmodel). The :686 deletion line may be
  applied ONLY together with the agreed vmstate-item-16 resolution: either
  (a) KEEP `useThreadGIL` (drop that deletion line from this diff; only the
  `useThreads` line lands), or (b) delete it AND in the same commit
  re-express the M4 backstop against the GIL predicate the api WS honors
  (e.g. a ThreadManager-exported `jsThreadGILEnabled()`); the assert must
  never be simply dropped. Until the integrator picks (a) or (b) with the
  vmstate WS, treat the `useThreadGIL` line as DEFERRED.
- `useThreads` (:685): vmstate M_opts2's leading normalization line reads
  `Options::useThreads()` (INTEGRATE-vmstate item 14). Whichever lands first
  is fine, but they must be reconciled: if M_opts2 lands first, drop its
  normalization line together with this alias removal; if this lands first,
  M_opts2 is applied without that line.

Sequencing consequence: this entry must NOT be applied unconditionally first.
The corrected apply order is in the task-14 checklist at the end of this
file ("Recommended apply order at INT", updated at round 2).

The two paired api-owned edits below are LANDED in the working tree at
review round 3 (they were previously deferred to INT; a round-3 finding
correctly flagged the live enable-predicate divergence as cross-WS risk —
every other workstream gates on Options::useJSThreads() only, while the
alias let a --useThreads=1 run spawn real OS threads with all other
flag-gated concurrent-mode machinery, including vmstate's planned
RELEASE_ASSERT(!useJSThreads || useThreadGIL) backstop, switched off).
Effect of landing them ahead of the OptionsList.h deletion: `--useThreads`
is still accepted by the option parser but read by NOTHING — a
--useThreads=1 run is now a flag-OFF run, so no apply order can produce
thread spawning with mismatched gating, and the 9.2-6 / jit / objectmodel
entries carry no alias precondition. The only remaining 9.2-1 INT action is
the OptionsList.h deletion diff above (shared hot file, not api-editable),
still under the useThreadGIL conditions of the cross-WS conflict note.

(a) runtime/ThreadManager.h — gate reduced to the canonical option (LANDED;
    the in-tree comment at the site documents the rationale). Diff as
    applied:

```diff
--- a/Source/JavaScriptCore/runtime/ThreadManager.h
+++ b/Source/JavaScriptCore/runtime/ThreadManager.h
 // Master gate for the phase-1 GIL'd shared-memory Thread API
-// (docs/threads/SPEC-api.md). --useThreads is accepted as an alias for
-// --useJSThreads. The GIL is the shared VM's JSLock; --useThreadGIL is
-// reserved and inert in phase 1 (the GIL is always on).
+// (docs/threads/SPEC-api.md). The GIL is the shared VM's JSLock and is
+// always on in phase 1.
 ALWAYS_INLINE bool useJSThreadsEnabled()
 {
-    return Options::useJSThreads() || Options::useThreads();
+    return Options::useJSThreads();
 }
```

    (The landed comment is longer than the two lines shown — it spells out
    the 9.2-1 rationale.) `useJSThreadsEnabled()` is kept (3 call-site
    files: ThreadManager.h consumers, AtomicsObject.cpp,
    JSGlobalObject.cpp); no call site anywhere reads
    `Options::useThreads()` or `Options::useThreadGIL()`
    (re-grep-verified at round 3 — both options are now dead).

(b) prep-corpus header migration — LANDED for exactly these 16 files (the
    full result of `grep -rl 'useThreads=' JSTests/threads`, now empty; the
    §8 corpus api/, atomics/, races/ and the lifecycle/invariants/
    shared-objects prep dirs already used `--useJSThreads=1`): the
    `//@ requireOptions("--useThreads=true")` directive became
    `//@ requireOptions("--useJSThreads=1")`. Note: in
    sync/condition-worker-waiter.js and
    sync/condition-notify-all-multi-waiter.js the directive sits on line 2,
    below their `//@ skip` (round-2 revisions said "line 1" — corrected).

```
JSTests/threads/arrays/copy-on-write.js
JSTests/threads/arrays/holes.js
JSTests/threads/arrays/push-resize-multithread.js
JSTests/threads/arrays/shared-element-read-write.js
JSTests/threads/arrays/typed-arrays-sab.js
JSTests/threads/sync/atomics-futex-lock.js
JSTests/threads/sync/atomics-object-basic.js
JSTests/threads/sync/condition-notify-all-multi-waiter.js
JSTests/threads/sync/condition-notify-all-shared-lock.js
JSTests/threads/sync/condition-notify-all.js
JSTests/threads/sync/condition-wait-notify.js
JSTests/threads/sync/condition-worker-waiter.js
JSTests/threads/sync/lock-async-hold.js
JSTests/threads/sync/lock-hold-basic.js
JSTests/threads/sync/lock-hold-mutual-exclusion.js
JSTests/threads/sync/thread-local-isolation.js
```

    Without (b), (a) would have silently turned those 16 prep tests into
    flag-off runs; with both landed together the prep corpus runs flag-on
    under the canonical option in every apply order.

## 9.2-2 runtime/JSGlobalObject.cpp — global constructor registration [VERIFIED-IN-TREE]

Canonical hunk (SOLE mechanism; in init() after the useSharedArrayBuffer
block, G12), plus `#include "ThreadObject.h"`:

```cpp
if (Options::useJSThreads()) {
    // Shared-memory Thread API (docs/threads/SPEC-api.md 9.2-2).
    putDirectWithoutTransition(vm, Identifier::fromString(vm, "Thread"_s), createThreadProperty(vm, this), static_cast<unsigned>(PropertyAttribute::DontEnum));
    putDirectWithoutTransition(vm, Identifier::fromString(vm, "Lock"_s), createLockProperty(vm, this), static_cast<unsigned>(PropertyAttribute::DontEnum));
    putDirectWithoutTransition(vm, Identifier::fromString(vm, "Condition"_s), createConditionProperty(vm, this), static_cast<unsigned>(PropertyAttribute::DontEnum));
    putDirectWithoutTransition(vm, Identifier::fromString(vm, "ThreadLocal"_s), createThreadLocalProperty(vm, this), static_cast<unsigned>(PropertyAttribute::DontEnum));
    putDirectWithoutTransition(vm, Identifier::fromString(vm, "ConcurrentAccessError"_s), createConcurrentAccessErrorProperty(vm, this), static_cast<unsigned>(PropertyAttribute::DontEnum));
}
```

Verified in tree at JSGlobalObject.cpp:1628-1635 (re-verified at task 14),
directly after the `Options::useSharedArrayBuffer()` block, with
`#include "ThreadObject.h"` at :37. Tree text is identical except the guard
reads `useJSThreadsEnabled()` (the ThreadManager.h inline). Once the 9.2-1
alias removal lands, `useJSThreadsEnabled()` IS `Options::useJSThreads()`,
so no JSGlobalObject.cpp edit is strictly required; for canonical-text parity
the integrator MAY additionally apply this one-token respelling (context
verified at :1628):

```diff
--- a/Source/JavaScriptCore/runtime/JSGlobalObject.cpp
+++ b/Source/JavaScriptCore/runtime/JSGlobalObject.cpp
@@ init(), after the Options::useSharedArrayBuffer() block @@
-    if (useJSThreadsEnabled()) {
+    if (Options::useJSThreads()) {
         // Shared-memory Thread API (docs/threads/SPEC-api.md 9.2-2).
```

Either spelling satisfies I1 (flag off => no own property). The five
`createXXXProperty(VM&, JSObject*)` factories are declared in
runtime/ThreadObject.h and defined in api-owned files (ThreadObject.cpp,
LockObject.cpp, ConditionObject.cpp, ThreadLocalObject.cpp).

## 9.2-3 — removed (no JSTypeInfo.h edit; 5.7 uses no TypeInfo flag).

## 9.2-4 Sources.txt — six new runtime/*.cpp [VERIFIED-IN-TREE]

```
runtime/ConditionObject.cpp
runtime/LockObject.cpp
runtime/ThreadAtomics.cpp
runtime/ThreadLocalObject.cpp
runtime/ThreadManager.cpp
runtime/ThreadObject.cpp
```

Verified present, alphabetically placed: Sources.txt:795 (ConditionObject),
:1004 (LockObject), :1135-1138 (ThreadAtomics, ThreadLocalObject,
ThreadManager, ThreadObject). Re-verified at task 14 (same lines). No diff
needed: the hunk is already applied; on a clean re-apply, insert each line at
its alphabetical position in the `runtime/` block. runtime/AtomicsObject.cpp
needs no entry (pre-existing, G17).

## 9.2-5 CMakeLists.txt — six new .h into JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS [VERIFIED-IN-TREE]

```
runtime/ConditionObject.h
runtime/LockObject.h
runtime/ThreadAtomics.h
runtime/ThreadLocalObject.h
runtime/ThreadManager.h
runtime/ThreadObject.h
```

Verified present at CMakeLists.txt:1209 (ConditionObject), :1561
(LockObject), :1779-1782 (ThreadAtomics, ThreadLocalObject, ThreadManager,
ThreadObject), inside JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS. Re-verified
at task 14 (same lines). Already applied; on a clean re-apply, insert each at
its alphabetical position in that list.

## 9.2-6 Thread.restrict choke-point hook [READY — exact diff supplied by §10 task 11/14]

INTEGRATOR-applied after the obj-model diff lands; this workstream supplies
`threadRestrictCheck` (declared runtime/ThreadManager.h, frozen §7 signature,
in tree, backed by the 5.7.2 affinity table in ThreadManager.cpp), the 5.7.1
conversions+pin (ThreadObject.cpp `threadFuncRestrict`), and
JSTests/threads/api/thread-restrict.js (I14; `//@ skip`ped — the integrator
DELETES that line when applying this entry).

Hook text (5.7.3, normative): every generic-path entry point of Dev 8's
enforced set — getOwnPropertySlotImpl (JSObject.h), putInline*/putInlineSlow
family (JSObjectInlines.h), putByIndex, deleteProperty + deletePropertyByIndex,
defineOwnProperty, getOwnPropertyNames, setPrototype(Of), isExtensible,
preventExtensions (JSObject.cpp); plus any successor generic entry point in
the merged tree; MUST begin with:

```cpp
if (Options::useJSThreads() && structure->isUncacheableDictionary() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
    return /* op-appropriate failure */;
```

([[unlikely]] per the JSObject.cpp idiom.) Get-path entry points
(PropertySlot&) also skip on `slot.isVMInquiry()` (G31); the
`Options::useJSThreads()` guard is mandatory (I1/I19): flag off, the gate is
one option load on already-slow generic paths and `threadRestrictCheck` is
dead code; flag on with no restricted objects, `threadRestrictCheck` is one
relaxed counter load (the gate's `isUncacheableDictionary()` makes even that
rare).

`threadRestrictCheck` throws CAE and returns false on a foreign-thread
access; the hook then returns the op-appropriate failure value WITH the
exception pending (get/has => false-not-found, put/delete/define/setPrototype/
preventExtensions/isExtensible => false, getOwnPropertyNames => void). All
hooked paths are exception-checked in callers today, so the pending CAE
propagates.

The two prototype-walk loops (JSObject.h getPropertySlot,
JSObjectInlines.h getNonIndexPropertySlot) are "successor generic entry
points" in this tree: a named GET takes them WITHOUT passing through
getOwnPropertySlotImpl, so they get the same hook (applied per visited
object — a restricted object reached as a prototype enforces too, matching
Dev 8 "get").

### Exact per-site diff (vs the current merged tree, obj-model deltas included)

NOTE for the integrator: written at task-11 time against the jarred/threads
working tree; ALL 14 context sites re-verified verbatim at task 14 against
the same tree (anchors confirmed: JSObject.h:57 getJSFunction fwd-decl,
:1631 getOwnPropertySlotImpl, :1652 getPropertySlot<checkNullStructure>;
JSObjectInlines.h getNonIndexPropertySlot walk and :452
putInlineForJSObject; JSObject.cpp putInlineSlow, putByIndex,
setPrototypeWithCycleCheck, deleteProperty, deletePropertyByIndex,
getOwnPropertyNames, preventExtensions, isExtensible(JSObject*,
JSGlobalObject*) — second parameter still unnamed, so the site-13 rename
hunk applies — and defineOwnProperty). If the obj-model merge has moved
these functions, anchor on the quoted context lines, not on file offsets. Sites 1-3 are JSObject.h, 4-5
JSObjectInlines.h, 6-14 JSObject.cpp. No #include is needed: Options.h is
already visible in all three files (obj-model deltas already call
`Options::useJSThreads()` there), and the hook declaration is added once in
JSObject.h (site 1; same frozen §7 signature as runtime/ThreadManager.h — a
deliberate duplicate declaration, NOT a second definition, so JSObject.h need
not include ThreadManager.h).

```diff
--- a/Source/JavaScriptCore/runtime/JSObject.h
+++ b/Source/JavaScriptCore/runtime/JSObject.h
@@ (1) forward declaration, after the getJSFunction forward declaration @@
 inline JSCell* getJSFunction(JSValue); // Defined in JSObjectInlines.h
 
+// SPEC-api 5.7/9.2-6 Thread.restrict choke point (defined in
+// runtime/ThreadManager.cpp; duplicate of the runtime/ThreadManager.h
+// declaration so generic-path headers need not include it). Returns true if
+// the access is allowed; otherwise throws ConcurrentAccessError and returns
+// false. Callers gate on isUncacheableDictionary() first (5.7.3).
+JS_EXPORT_PRIVATE bool threadRestrictCheck(JSGlobalObject*, JSObject*);
+
 class ArrayProfile;
@@ (2) getOwnPropertySlotImpl @@
 ALWAYS_INLINE bool JSObject::getOwnPropertySlotImpl(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
 {
     VM& vm = getVM(globalObject);
     Structure* structure = object->structure();
+    if (Options::useJSThreads() && structure->isUncacheableDictionary() && !slot.isVMInquiry() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
+        return false;
     if (object->getOwnNonIndexPropertySlot(vm, structure, propertyName, slot))
         return true;
@@ (3) getPropertySlot<checkNullStructure> walk (successor generic get entry) @@
         ASSERT(object->type() != ProxyObjectType);
         Structure* structure = object->structureID().decode();
 #if USE(JSVALUE64)
         if (checkNullStructure) {
             if (!structure) [[unlikely]]
                 CRASH_WITH_INFO(object->type(), object->structureID().bits());
         }
 #endif
+        if (Options::useJSThreads() && structure->isUncacheableDictionary() && !slot.isVMInquiry() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
+            return false;
         if (object->getOwnNonIndexPropertySlot(vm, structure, propertyName, slot))
             return true;
```

```diff
--- a/Source/JavaScriptCore/runtime/JSObjectInlines.h
+++ b/Source/JavaScriptCore/runtime/JSObjectInlines.h
@@ (4) getNonIndexPropertySlot walk (successor generic get entry) @@
     JSObject* object = this;
     while (true) {
         Structure* structure = object->structureID().decode();
+        if (Options::useJSThreads() && structure->isUncacheableDictionary() && !slot.isVMInquiry() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
+            return false;
         if (!TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags())) [[likely]] {
@@ (5) putInlineForJSObject (putInline* family head; routes putByIndex too) @@
     JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
     ASSERT(value);
     ASSERT(!Heap::heap(value) || Heap::heap(value) == Heap::heap(thisObject));
 
+    if (Options::useJSThreads() && thisObject->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, thisObject)) [[unlikely]]
+        return false;
+
     // Try indexed put first. This is required for correctness, since loads on property names that appear like
     // valid indices will never look in the named property storage.
```

```diff
--- a/Source/JavaScriptCore/runtime/JSObject.cpp
+++ b/Source/JavaScriptCore/runtime/JSObject.cpp
@@ (6) putInlineSlow @@
 bool JSObject::putInlineSlow(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
 {
     ASSERT(!parseIndex(propertyName));
 
     VM& vm = globalObject->vm();
     auto scope = DECLARE_THROW_SCOPE(vm);
 
+    if (Options::useJSThreads() && structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, this)) [[unlikely]]
+        return false;
+
@@ (7) putByIndex @@
 bool JSObject::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned propertyName, JSValue value, bool shouldThrow)
 {
     VM& vm = globalObject->vm();
     JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
 
+    if (Options::useJSThreads() && thisObject->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, thisObject)) [[unlikely]]
+        return false;
+
     if (propertyName > MAX_ARRAY_INDEX) {
@@ (8) setPrototypeWithCycleCheck (generic setPrototype(Of) entry; the static
     JSObject::setPrototype trampolines here) @@
 bool JSObject::setPrototypeWithCycleCheck(VM& vm, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
 {
     auto scope = DECLARE_THROW_SCOPE(vm);
 
+    if (Options::useJSThreads() && structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, this)) [[unlikely]]
+        return false;
+
     if (this->structure()->isImmutablePrototypeExoticObject()) {
@@ (9) deleteProperty @@
 bool JSObject::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, DeletePropertySlot& slot)
 {
     JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
     VM& vm = globalObject->vm();
     
+    if (Options::useJSThreads() && thisObject->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, thisObject)) [[unlikely]]
+        return false;
+
     if (std::optional<uint32_t> index = parseIndex(propertyName))
@@ (10) deletePropertyByIndex @@
 bool JSObject::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned i)
 {
     VM& vm = globalObject->vm();
     JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
     
+    if (Options::useJSThreads() && thisObject->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, thisObject)) [[unlikely]]
+        return false;
+
     if (i > MAX_ARRAY_INDEX)
@@ (11) getOwnPropertyNames (ownKeys) @@
 void JSObject::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
 {
+    if (Options::useJSThreads() && object->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
+        return;
     object->getOwnIndexedPropertyNames(globalObject, propertyNames, mode);
     object->getOwnNonIndexPropertyNames(globalObject, propertyNames, mode);
 }
@@ (12) preventExtensions @@
 bool JSObject::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
 {
     VM& vm = globalObject->vm();
+    if (Options::useJSThreads() && object->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
+        return false;
     if (!object->isStructureExtensible()) {
@@ (13) isExtensible (parameter must be named) @@
-bool JSObject::isExtensible(JSObject* obj, JSGlobalObject*)
+bool JSObject::isExtensible(JSObject* obj, JSGlobalObject* globalObject)
 {
+    if (Options::useJSThreads() && obj->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, obj)) [[unlikely]]
+        return false;
     return obj->isStructureExtensible();
 }
@@ (14) defineOwnProperty @@
 bool JSObject::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool throwException)
 {
+    if (Options::useJSThreads() && object->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
+        return false;
     // If it's an array index, then use the indexed property storage.
```

Post-apply steps for the integrator:
1. Delete the `//@ skip` line of JSTests/threads/api/thread-restrict.js (I14
   gate; the `//@ requireOptions("--useJSThreads=1")` line below it stays).
2. Sanity: flag-off bench gate (I19) — every hook is behind
   `Options::useJSThreads()` on already-generic paths, so flag-off codegen
   deltas should be one folded option-load+branch per site.
3. NOT enforced by design (Dev 8, doc'd): getPrototypeOf, call/construct,
   indexed GET. Do not add hooks to getOwnPropertySlotByIndex or
   getPrototype. AMENDED at round 4 (D13): the soundness argument for
   skipping the indexed-GET hook — "5.7.1 pins restricted objects on
   SlowPut shapes, which get_by_val never fast-paths" — holds ONLY for
   receivers whose method table does not override the indexed entry points
   (typed arrays, StringObject, arguments objects, etc. would bypass both
   the pin and the hooks entirely). threadFuncRestrict therefore now
   REJECTS any such receiver at restrict time
   (restrictReceiverStaysOnHookedPaths, ThreadObject.cpp; allowlist =
   JSObject-default enforced slots, plus JSArray exactly). With that gate,
   this instruction stands as written.

## 9.2-7 Test-runner wiring — JSTests/threads.yaml [READY — exact yaml below; run-tests.sh landed as interim runner]

Tools/threads/run-tests.sh (api-owned) LANDED with task 12 and is the runner
until threads.yaml lands: it globs JSTests/threads/{api,atomics,races}/*.js
plus, when present, threads/heap-*.js, threads/{objectmodel,vmstate}/*.js and
threads/jit/**/*.js; honors the JSC env var, --filter=, --amplify (wraps runs
in Tools/threads/amplify.sh when present, else warns once and runs plain);
parses //@ skip / //@ requireOptions / //@ runDefault headers (ta-path-
unchanged.js runs both ways); appends --can-block-is-false for
api/blocking-gate.js; and finishes with the §6 coverage grep (every
API-I1..API-I24 cited under api/, atomics/, races/ — missing citations fail
the run).

Two run-tests.sh caveats recorded at round 2 are FIXED in the script at
round 3 (the round-3 review surface includes Tools/threads/run-tests.sh, so
the owed fixes were applied rather than re-deferred — see the "Ownership
adjudication" section at the end of this file):
- Unknown-option probe: before each run, the run's option set is probed
  against an empty program (`"$JSC" <opts> -e ''`); a rejected set => SKIP
  (not FAIL), with a message attributing it to a not-yet-landed
  OptionsList.h hunk. This makes a plain run green on the CURRENT tree:
  the two vmstate files needing --useVMLite /
  --useStructureAllocationLock (absent until vmstate M_opts) now SKIP.
  Probe results are cached per option set; once vmstate M_opts lands the
  probes pass and the files run automatically — no script change needed.
- The §6 coverage grep now ignores //@ skip'ped files. API-I14 is
  special-cased: while its sole citation (api/thread-restrict.js) is
  skipped per the SPEC-mandated I14 deferral, every run prints
  "API-I14 coverage deferred to INT 9.2-6" instead of counting it green;
  any OTHER invariant cited only in skipped files is a COVERAGE FAIL
  ("only-in-skipped-files"). A future re-skip of thread-restrict.js after
  9.2-6 lands therefore stays visible on every run.

threads.yaml — COMPLETE ready-to-create file content (create
JSTests/threads.yaml verbatim; header format matches the sibling yamls, e.g.
JSTests/modules.yaml). The runDefault family + parseRunCommands covers every
header in the corpus: blocking-gate.js carries its own
`//@ runDefault("--can-block-is-false")` run command (its requireOptions line
still supplies --useJSThreads=1 via $testSpecificRequiredOptions), so no
per-file stanza is needed, and the test self-guards (it throws if the flag is
missing, so a misconfigured runner cannot pass it vacuously). The threads/jit
stanza is included immediately (directory exists, its file's options exist).

CORRECTED at round 2 — threads/vmstate is NOT immediately runnable:
directory existence is not sufficiency. Two vmstate files carry
requireOptions for options that DO NOT EXIST in this tree's OptionsList.h
until vmstate M_opts lands (grep-confirmed; INTEGRATE-vmstate M_opts says the
same): vmstate/vmlite-single-thread-identity.js needs `--useVMLite=1` and
vmstate/structure-lock-single-thread.js needs
`--useStructureAllocationLock=1`. run-jsc-stress-tests' requireOptions
appends the flags verbatim and jsc exits nonzero on an unknown option, so
those two tests FAIL (not skip) — a guaranteed-red suite if the stanza lands
before vmstate M_opts. The stanza is therefore moved to the
append-when-options-land section below, keyed on vmstate M_opts:

```yaml
# Copyright (C) 2026 Oven, Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1.  Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
# 2.  Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

# Shared-memory Thread API corpus (docs/threads/SPEC-api.md section 8 /
# 9.2-7). Every test is self-checking (failure = throw) and carries its own
# //@ requireOptions header; parseRunCommands honors per-file //@ skip,
# //@ runDefault(...) and requireOptions directives.

- path: threads/api
  cmd: runDefault unless parseRunCommands

- path: threads/atomics
  cmd: runDefault unless parseRunCommands

- path: threads/races
  cmd: runDefault unless parseRunCommands

- path: threads/jit
  cmd: runDefault unless parseRunCommands
```

Append when their preconditions are met (paths must exist before a stanza is
added — run-javascriptcore-tests rejects missing paths — AND every option
named by the directory's requireOptions headers must exist):

```yaml
# PRECONDITION: vmstate M_opts applied (supplies --useVMLite /
# --useStructureAllocationLock, required by two files in this directory).
- path: threads/vmstate
  cmd: runDefault unless parseRunCommands

- path: threads/objectmodel
  cmd: runDefault unless parseRunCommands

# heap lands flat files, not a directory: one stanza per threads/heap-*.js
# file, e.g.
# - path: threads/heap-allocator-stress.js
#   cmd: runDefault unless parseRunCommands
```

Out of scope by design (SPEC-api 9.2-7 lists api/, atomics/, races/ plus the
other workstreams' dirs only): the prep-stub corpus under threads/{sync,
arrays, lifecycle, invariants, shared-objects}/ and threads/smoke.js, and the
bench substrate threads/bench/ (G15: driven by Tools/threads/bench-gate.sh,
never by the test runner). If the integrator wants the prep dirs in yaml-land
too, the same `runDefault unless parseRunCommands` stanza shape applies — but
apply the 9.2-1(b) header migration first (16 files still say
`--useThreads=true`).

run-javascriptcore-tests stanza: add `runAndMergeAllJSCTests`-visible
coverage by listing threads.yaml wherever the local harness enumerates yamls
(Bun CI invokes Tools/threads/run-tests.sh directly until then).

Note for the integrator: api/thread-restrict.js stays `//@ skip` until the
9.2-6 hook is applied (see that section's apply checklist);
atomics/property-wait-termination.js intentionally exits via an uncaught
termination under `--watchdog=500 --watchdog-exception-ok` (exit 0) — a
FAILURE print + nonzero exit means API-I24 regressed.

## 9.2-8 VMLite + butterfly-tag wiring (5.2) [READY — exact diff below; both header sets now exist in this tree]

Status change at task 14: the dependency headers LANDED in this working tree
since this entry was first recorded —

- vmstate: runtime/VMLite.h (struct VMLite: `uint16_t tid { 0 }` at :160,
  `VM* vm` at :163 written solely by registerLite per 6.5.1, default ctor,
  `static VMLite* setCurrent(VMLite*)` at :196) and runtime/VMLiteShared.h
  (`VMLiteRegistry::singleton()` :149, `void registerLite(VMLite&, VM&)`
  :156, `void unregisterLite(VMLite&)` :158 — leaf lock, 5.9-legal).
- jit-1b: jit/ConcurrentButterflyOperations.h (NOT "ButterflyTIDTag.h" as
  earlier revisions guessed): `JS_EXPORT_PRIVATE void
  initializeButterflyTIDTagForCurrentThread()` :71 (idempotent; first call
  also registers the CS3 setCurrent hook) and `JS_EXPORT_PRIVATE void
  clearButterflyTIDTagForCurrentThread()` :74.

All signatures above re-verified at task 14; the diff below compiles against
exactly those declarations. The two parts remain separable: the three
`lite`-only lines per hunk are the vmstate part, the two
`*ButterflyTIDTag*` lines (and that #include) are the jit-1b part — either
can be deferred by dropping its lines. Note on redundancy, by design:
VMLite::setCurrent itself invokes the registered TID-tag hook with
`lite ? lite->tid : 0` (VMLite.h:190-196), but the explicit initialize call
stays — it is what registers that hook on a fresh thread (its first
invocation), and the normative 5.2 order is setCurrent THEN initialize
(CS3: before any JS). The explicit clear after setCurrent(nullptr) is then
an idempotent re-zero; both calls are kept to match the frozen 5.2 text.

Both hunks splice at the `// THREADS-INTEGRATE(api)` markers placed in
threadMain by task 2 (ThreadObject.cpp:131 and :209; context re-verified
verbatim at task 14). `makeUnique` is already visible (wtf, via JSCInlines).

```diff
--- a/Source/JavaScriptCore/runtime/ThreadObject.cpp
+++ b/Source/JavaScriptCore/runtime/ThreadObject.cpp
@@ #include block (after "ArrayBufferSharingMode.h") @@
 #include "ArrayBufferSharingMode.h"
+#include "ConcurrentButterflyOperations.h"
 #include "CustomGetterSetter.h"
@@ #include block (after "TypedArrayType.h") @@
 #include "TypedArrayType.h"
+#include "VMLite.h"
+#include "VMLiteShared.h"
 #include <wtf/StackAllocation.h>
@@ threadMain spawn handshake (replaces the task-2 marker comment) @@
 static void threadMain(VM& vm, Ref<ThreadState> state)
 {
     state->nativeThread = &Thread::currentSingleton();
     setCurrentThreadState(state.copyRef());
-    // THREADS-INTEGRATE(api): the VMLite + butterfly-TID-tag handshake
-    // (SPEC-api 5.2 / 9.2-8) goes HERE, before the JSLockHolder, once the
-    // vmstate (VMLite/VMLiteRegistry) and jit-1b (butterfly tag) headers
-    // exist; exact ready-to-apply diff: docs/threads/INTEGRATE-api.md 9.2-8.
+    // THREADS-INTEGRATE(api): VMLite + butterfly-TID-tag handshake
+    // (SPEC-api 5.2 / vmstate 6.4.4 / jit P5+CS3), before the JSLockHolder.
+    auto lite = makeUnique<VMLite>();
+    lite->tid = state->tid; // TM is the sole TID allocator; written before setCurrent (vmstate 6.7)
+    VMLiteRegistry::singleton().registerLite(*lite, vm); // sole writer of lite->vm (vmstate 6.5.1)
+    VMLite::setCurrent(lite.get());
+    initializeButterflyTIDTagForCurrentThread(); // jit P5; after setCurrent, before any JS (CS3)
     {
         // The GIL: all JS execution is serialized by the shared VM's JSLock
         // (SPEC-api 5.2). Atom table and stack limits migrate on acquisition.
         JSLockHolder locker(vm);
@@ threadMain teardown (replaces the task-2 marker comment; still under the final JSLock) @@
         state->threadLocals.clear();
         state->jsThread.clear();
         ThreadManager::singleton().unregisterThread(state);
 
-        // THREADS-INTEGRATE(api): VMLite teardown — unregisterLite,
-        // VMLite::setCurrent(nullptr), clearButterflyTIDTagForCurrentThread()
-        // — goes HERE, still under the final JSLock (SPEC-api 5.2 / vmstate
-        // N8; registry lock is a 5.9-legal leaf); the lite itself is
-        // destroyed after the JSLock release. Diff: INTEGRATE-api.md 9.2-8.
-        // The TID is retired forever (Deviation 10).
+        // THREADS-INTEGRATE(api): VMLite teardown (SPEC-api 5.2 / vmstate
+        // N8; registry lock is a 5.9-legal leaf), still under the final
+        // JSLock. The TID is retired forever (Deviation 10).
+        VMLiteRegistry::singleton().unregisterLite(*lite);
+        VMLite::setCurrent(nullptr);
+        clearButterflyTIDTagForCurrentThread();
     }
+    lite = nullptr; // destroy AFTER the JSLock release (SPEC-api 4.6.1); ~VMLite asserts uninstalled+unregistered
     setCurrentThreadState(nullptr);
 }
```

Apply gating: threadMain runs only when uJT() is on (spawning is
flag-gated), and the hunk lands together with vmstate M_opts2 (G36: uJT()
implies useVMLite and friends), so no additional Options guard is needed in
the spliced code — matching the frozen 5.2 text, which has none. Until the
integrator applies this entry the markers stay comments and the api-owned
sources reference neither header (verified: no VMLite/ConcurrentButterfly
include in any api-owned file today).

## 9.2-9 runtime/JSLock.{h,cpp} + GILDroppedSection splice — suppress the park-site microtask drain (D11) [READY — exact diff below]

Round-4 blocker fix. JSLock::willReleaseLock() guards its
VM::drainMicrotasks() call on `!m_lockDropDepth || useLegacyDrain`
(JSLock.cpp, the `if (!m_lockDropDepth || useLegacyDrain)` line), and
JSLock::dropAllLocks() increments m_lockDropDepth BEFORE unlocking precisely
so that dropping the lock at a park site runs no JS. GILDroppedSection's
plain `apiLock.unlock()` loop (LockObject.cpp) has depth 0, so the FINAL
unlock of every section currently drains the shared microtask queue — user
JS runs synchronously inside the host call at every park site (join,
contended lock.hold, cond.wait, property Atomics.wait, the D4 TA wait, and
notify's D2 handoff yield), with reentrancy into the very
NativeLockState/NativeConditionState mid-operation and
exception-pending-then-park hazards. This violates the 5.2 yield-point
contract far beyond the recorded D2 deviation (deviation entry: D11 below).

The fix cannot land from api-owned files: it needs one new JSLock member
(JSLock.{h,cpp} are outside this part's owned paths — same constraint class
as 9.2-6). The depth bump is strictly local to the new member — bumped and
restored while m_lock is still HELD — so it is invisible to the
DropAllLocks strict-LIFO unwind protocol and the D1 livelock fix is fully
preserved (no depth slot is ever held across a park, and no non-holder ever
touches m_lockDropDepth, so there is no race on the unsynchronized counter).

```diff
--- a/Source/JavaScriptCore/runtime/JSLock.h
+++ b/Source/JavaScriptCore/runtime/JSLock.h
@@ public section, after the static lock/unlock(VM&) declarations @@
     static void lock(VM&);
     static void unlock(VM&);
 
+    // Shared-memory Thread API (docs/threads/SPEC-api.md 5.2 /
+    // INTEGRATE-api.md 9.2-9): fully releases the lock for a thread that is
+    // about to PARK, without running willReleaseLock()'s microtask drain
+    // (which would execute user JS inside the parking host call). The
+    // m_lockDropDepth bump is bumped AND restored while m_lock is still
+    // held, so it never escapes into the DropAllLocks strict-LIFO unwind
+    // protocol — N threads can park and wake in any order (the
+    // GILDroppedSection livelock fix is preserved). Returns the number of
+    // lock counts released; the caller reacquires with that many lock()
+    // calls. Sole caller: GILDroppedSection (runtime/LockObject.cpp).
+    JS_EXPORT_PRIVATE unsigned unlockAllForThreadParking();
+
     VM* vm() { return m_vm; }
```

```diff
--- a/Source/JavaScriptCore/runtime/JSLock.cpp
+++ b/Source/JavaScriptCore/runtime/JSLock.cpp
@@ after JSLock::willReleaseLock() @@
+unsigned JSLock::unlockAllForThreadParking()
+{
+    RELEASE_ASSERT(currentThreadIsHoldingLock());
+    unsigned droppedLockCount = static_cast<unsigned>(m_lockCount);
+    // Suppress willReleaseLock()'s drainMicrotasks() (guarded on
+    // !m_lockDropDepth): a park site must not run user JS mid-host-call.
+    // Every other willReleaseLock side effect (atom-table restore,
+    // releaseDelayedReleasedObjects, stackPointerAtVMEntry clear,
+    // conditional clearLastException, heap-access release) runs exactly as
+    // it does for dropAllLocks(). Bump and restore both happen while m_lock
+    // is still held, so no other thread can observe the transient depth and
+    // the DropAllLocks LIFO protocol is unaffected.
+    ++m_lockDropDepth;
+    willReleaseLock();
+    --m_lockDropDepth;
+    m_lockCount = 0;
+    m_hasOwnerThread.store(false, std::memory_order_release);
+    m_lock.unlock();
+    return droppedLockCount;
+}
+
```

GILDroppedSection splice (runtime/LockObject.cpp is api-owned, but the line
cannot land before the JSLock member exists; the `// THREADS-INTEGRATE(api)`
marker is in place at the site):

```diff
--- a/Source/JavaScriptCore/runtime/LockObject.cpp
+++ b/Source/JavaScriptCore/runtime/LockObject.cpp
@@ GILDroppedSection::GILDroppedSection, replacing the marked unlock loop @@
     JSLock& apiLock = vm.apiLock();
     ASSERT(apiLock.currentThreadIsHoldingLock());
-    // THREADS-INTEGRATE(api): replace this unlock loop with [...marker
-    // comment block, delete in full...]
-    while (apiLock.currentThreadIsHoldingLock()) {
-        apiLock.unlock();
-        m_lockCount++;
-    }
+    // 9.2-9: depth-suppressed release — no microtask drain at park sites
+    // (the D11 fix); see JSLock::unlockAllForThreadParking.
+    m_lockCount = apiLock.unlockAllForThreadParking();
 }
```

Post-apply steps for the integrator:
1. Delete the `//@ skip` line of JSTests/threads/api/park-no-microtask-drain.js
   (the D11 regression test: queued promise reactions must not run inside
   join / cond.wait / contended hold / notify-yield / property Atomics.wait).
2. Also update the GILDroppedSection class comment in LockObject.h:
   constraint (3) — the KNOWN-OPEN D11 paragraph — becomes "fixed by
   unlockAllForThreadParking" (one-paragraph rewrite; the rest of the
   comment stands).
3. Caveat, recorded: on Cocoa with pre-DoesNotDrainTheMicrotaskQueue SDK
   linking, willReleaseLock uses `useLegacyDrain` and drains REGARDLESS of
   depth — JSLock::DropAllLocks has the identical exposure there, so 9.2-9
   makes park sites exactly as sound as stock DAL on every platform, and
   strictly sound on the platforms Bun ships (Linux/macOS-current/Windows).

## 9.2-10 Embedder ArrayBuffer contract under GIL-off (CVE-AUDIT Tier-B B10/B11; map-MC-LIFE S4/S7/S11) [LANDED — doc + atomicization]

No shared-hot-file diff. The engine-side half (B10) is LANDED in
`runtime/ArrayBuffer.h`: `m_pinCount` is `std::atomic<unsigned>` and
`m_locked` is `std::atomic<bool>` (relaxed; the Checked<> overflow/underflow
crash is preserved as RELEASE_ASSERT in pin()/unpin()). That closes the
lost-update on concurrent embedder pin()/unpin() and makes
`isDetachable()`/`isLocked()` data-race-free for TSAN. This entry records
the EMBEDDER obligations the engine cannot enforce (B11), for the Bun
integration to gate on; nothing here changes flag-off behavior.

**ArrayBufferDestructorFunction thread-affinity (B11 / S7 / S11).** Under
`--useJSThreads=1 --useThreadGIL=0` an `ArrayBufferDestructorFunction`
supplied to `ArrayBuffer::createFromBytes` / `SharedArrayBufferContents`
MAY run on ANY thread, with NO `JSLock` held and NO `VMLite` installed:

- on whichever thread performs the final deref of the contents (any spawned
  JS thread, main, or an embedder thread), or
- on the heap §10 stop CONDUCTOR thread, when the mapping was retired via
  the annex-N6 quarantine drain (`arrayBufferQuarantineSafepointHook`,
  `runtime/ArrayBuffer.cpp`), or
- on a thread other than the buffer's CREATOR — a spawned thread may create
  a buffer, hand it to main, and exit; the destructor then runs after the
  creator's `VMLite`/`GCClient::Heap` are gone (S11).

The destructor runs EXACTLY ONCE, after sole ownership is established (S2/S5
guarantee that). The embedder MUST therefore supply only THREAD-AGNOSTIC
destructors: they may call process-global allocators (Gigacage, malloc/free,
mmap/munmap, `BufferMemoryManager::singleton()`), but MUST NOT touch
per-thread or per-creator state (no `napi_env`, no per-isolate bookkeeping,
no thread-affine native handles), MUST NOT acquire the `JSLock` or call back
into JS, and MUST NOT assume a current `VMLite`/`ThreadState`. Thread-affine
finalizers (napi external buffers, FFI callbacks) MUST be marshalled by the
embedder to their owning thread — exactly as Bun already must for napi
post-ungil; the engine will not marshal for it.

**pin()/unpin() balance (B10 / S4).** The atomicization makes concurrent
`pin()`/`unpin()` count-correct, but does NOT make `isDetachable()` a fence:
a `pin()` does not retroactively block a detach whose relaxed
`isDetachable()` load on another thread already observed 0 — the pin must be
ESTABLISHED (with a happens-before edge to any would-be detacher) before any
other thread can attempt detach. Under that discipline, a raw `bytesPtr`
obtained via `JSObjectGetTypedArrayBytesPtr` (which calls `pinAndLock()` —
sticky-true, monotone, so the C-API path is establish-once) or via an
`ArrayBufferView` with `setDetachable(false)` is safe against any detach
whose `isDetachable()` check observes the pin/lock; engine-side memory
safety for the captured pointer then rests on the annex-N6 quarantine, not
on this counter. An embedder that reads `data()` without pinning gets no
lifetime guarantee against a concurrent `transfer()`/`detach()` from another
thread (GIL-off only; under the GIL the `JSLock` serializes it).
`pin()`/`unpin()` calls MUST balance; an unbalanced `unpin()` now
`RELEASE_ASSERT`s (previously `Checked<>`-crashed identically).

Gate: TSAN native arm clean on `m_pinCount`/`m_locked` (thread-scanners
battery); no JSTests case is possible (no JS-visible pin).

## 9.2-11 `--useJSThreads` is native-code-equivalent for confidentiality (CVE-AUDIT Tier-B B15; map-MC-SPEC S1-S5) [LANDED — doc + OptionsList help text]

No engine code change. MC-SPEC is STRUCTURAL: speculative execution side
channels (Spectre v1/v2) are not bugs in any one check, they are a property
of running attacker JS in the same address space as secrets while handing it
a high-resolution clock — and `--useJSThreads` IS that clock, independent of
`--useSharedArrayBuffer`. The disposition is therefore an EMBEDDER
OBLIGATION, recorded here as the source of record for the Bun integration
and surfaced verbatim in the `useJSThreads` OptionsList.h help text so no
embedder can enable the flag without seeing it in `--options`.

**The capability grant (S1/S2).** With `--useJSThreads=1
--useSharedArrayBuffer=0`, `Thread`/`Lock`/`Atomics`-on-properties are
present and `SharedArrayBuffer` is absent (independent gates,
`OptionsList.h:691` vs `:712`, `JSGlobalObject.cpp` reads them in separate
`if`s). A spawned `Thread` captures any plain shared-heap object and spins
`Atomics.add(o, "c", 1)`; the observer reads `Atomics.load(o, "c")` — a
free-running counter at near cache-coherency rate, 4-5 orders of magnitude
finer than a coarsened browser timer. The 2018-era SAB-removal lever does
not exist for this engine: the timer is inherent to the Thread API's reason
for existing, and jitter cannot blunt a counting clock (which is WHY SAB was
the thing browsers removed). `JSTests/threads/cve/mc-spec-timer-capability.js`
is the capability WITNESS: it asserts the gating shape and that the counter
advances between back-to-back loads. It is not a failure detector; PASS
means the grant is present as designed.

**Structural exposure (S3-S5).** This tree contains zero
speculation-hardening primitives — no index masking between bounds branch
and load (DFG `speculationCheck(OutOfBounds, ...)`, FTL
`compileCheckInBounds`, runtime `canGetIndexQuickly` / I33-C4 clamps are all
branch-only), no retpolines in the MacroAssembler layer, and
`GIGACAGE_ENABLED` is FALSE on Windows and musl-mimalloc builds
(`bmalloc/BPlatform.h`). What the threads flag changes vs stock JSC is (a)
the S1 clock makes the cache-readback leg practical with no embedder-visible
API, and (b) the shared heap server places every thread's objects in one
address space by design — a transient OOB from thread A's butterfly lands in
memory that holds every other thread's cells. None of this is fixable by a
patch this project could demand; v2 mitigation is hardware/OS (eIBRS, IBPB)
and per-indirect-branch retpolines in a JIT are not realistic.

**Embedder obligation (the deliverable).** Treat `--useJSThreads` as
equivalent to granting native code execution for confidentiality purposes:

1. Never enable `--useJSThreads` for code you would not also trust with
   `process.hrtime` + Workers + SAB. The flag is a strict superset of that
   timing capability and is NOT gated by `--useSharedArrayBuffer`.
2. No in-process secret is confidentiality-protected from JS running under
   the flag — not by bounds checks, not by the cage (and there IS no cage on
   Windows / musl-mimalloc), not by branch hygiene. Multi-tenant deployments
   (plugins, user-supplied lambdas, sandboxes) MUST use multi-process
   isolation; the JS sandbox does not hold against a co-resident reader with
   this clock. Same conclusion the browsers reached in 2018.
3. Server-side v2 posture (updated microcode, eIBRS) is assumed by ops, not
   provided by the engine.

Cheap engine-side hardening worth taking but NOT blocking (tracked in
map-MC-SPEC.md S3/S4 dispositions, for thread-scanners): branchless index
clamping on the new C4/I33 runtime paths; keep `Gigacage::Primitive` on any
future shared-buffer allocation path.

LANDED in tree: `OptionsList.h:691` help text carries the obligation
verbatim (string-literal only; default `false` and option type unchanged,
flag-off behavior byte-identical). Gate:
`JSTests/threads/cve/mc-spec-timer-capability.js` under the pinned GIL-off
env with `--useSharedArrayBuffer=0` — PASS = timer demonstrated; the doc is
the deliverable.

---

## Landed deviations from the frozen SPEC-api text (escalation list for the spec owner / post-GIL re-freeze)

The fan-out's change-control rule is "FROZEN: implement as written; ambiguity
= spec bug". The items below are the places the landed code KNOWINGLY differs
from the frozen text; each is a requested rev-15 amendment, recorded here
because this manifest is the api workstream's only writable docs surface.
Numbered D1-D5 so code comments can cite them.

- D1 — GILDroppedSection replaces JSLock::DropAllLocks (DAL) at every park
  site (5.2/5.3/5.4/F4/F5: join, contended hold, cond.wait, property
  Atomics.wait). DAL's strict-LIFO m_lockDropDepth unwind livelocks when N
  threads park and wake in arbitrary order (observed live:
  lifecycle/join-semantics.js, sync/condition-notify-all-shared-lock.js), so
  5.2-as-written is unimplementable for N waiters; the depth-free section
  (LockObject.h, full state-set list in its comment) is the replacement.
  Requested amendment: name GILDroppedSection normatively — its
  saved/restored state set duplicates JSLock::willReleaseLock/grabAllLocks
  invariants and must be kept in sync by hand (vmstate also touches that
  surface, SPEC-vmstate §0) — and rule on coexistence with genuine
  DropAllLocks users. Phase-1 constraint until then: the embedder must not
  run DropAllLocks on the shared VM's JSLock while spawned Threads are live
  (the jsc shell never does; Bun integration must gate on this).
- D2 — notify()/notifyAll() are a yield point (ConditionObject.cpp
  notifyImpl ends with jsThreadGILHandoffYield). 5.2 says the blocking parks
  are "the only yield points"; that is unimplementable for cond.notify under
  a cooperative GIL — a JS-looping notifier (notify-until-everyone-reports)
  would starve its own waiters forever, since cond.wait can only finish by
  reacquiring the GIL. Consequences, stated plainly: foreign JS can run
  inside a notify() call, including while the caller holds the JS Lock's
  rank-4 m_lock (same lock shape as the 5.4 wait-side reacquisition, so no
  new rank edge); the GIL-phase semantic oracle (THREAD.md) includes this
  yield, and the corpus' notify-loop rendezvous tests
  (condition-notify-all*, wait-notify-storm) green-light BECAUSE of it. The
  post-GIL re-freeze must either adopt notify-as-yield-point or re-derive
  those tests' interleaving assumptions.
- D3 — property-Atomics receiver exclusions (ThreadAtomics.cpp
  getOwnPropertyForAtomics; the frozen 4.5 table does not enumerate exotic
  receivers). Rejected with TypeError: (a) ProxyObject/JSGlobalProxy — traps
  run arbitrary JS inside the "one atomic step", which can reach a
  GIL-dropping park site and break CAS/RMW atomicity (cross-thread TOCTOU);
  (b) own data properties not backed by plain structure/butterfly storage
  (JSArray "length", RegExpObject "lastIndex", StringObject chars,
  sparse-map indices, global var-scope bindings) — the method table reports
  them as own data, but putDirect/putDirectIndex would install a duplicate
  shadow property (heap state no sequential program can create, violating
  THREAD.md's indistinguishable-heap requirement). Ordinary
  structure/butterfly properties — including dictionary-mode and indexed
  butterfly elements — are unaffected; the read is now a non-reentrant
  structure/butterfly probe matching the slot the write targets.
- D4 — main/embedder-thread typed-array sync Atomics.wait parks with the GIL
  dropped when useJSThreads is on (AtomicsObject.cpp atomicsWaitImpl wraps
  WaiterListManager::waitSync in GILDroppedSection under the flag). Without
  this, a main-thread SAB wait whose notifier is a spawned Thread deadlocks
  the whole VM (the notifier can never acquire the GIL). Spawned Threads
  remain gated off the TA sync wait entirely (4.5 step 1a / I21). Flag off,
  the branch is dead and today's body is textually unchanged (I1).
- D5 — 5.6's waitAsync finite-timeout timer task captures Ref<VM> (not VM&)
  and bails out (clearing the ticket's promise Strong under the lock) when
  DWT shutdown already cancelled the ticket. G28's "vm.runLoop().
  dispatchAfter" mechanism is kept; the amendment request is to make the
  Ref<VM> capture + cancelled-bailout normative (RunLoops are independently
  ref-counted and outlive the VM, so the frozen text as written is a
  use-after-free on embedder VM teardown). ~AsyncTicket now asserts the API
  lock is held whenever it destroys a still-set promise Strong (5.10
  discipline made checkable).
  ROUND-4 COMPANION (the case D5 does not cover — INFINITE-timeout
  waitAsync that is never notified, which has no timer task and no other
  clearing point): PropertyWaiterTable now registers ONE per-waited-cell
  heap finalizer (deduped via m_sweepFinalizerCells, public Heap::addFinalizer
  only — the registerThreadStateFinalizer pattern). The list's cellProtect
  roots the cell, so the finalizer fires either after removeListIfEmpty
  (normal GC; no-op sweep, address unregistered) or at VM teardown via
  lastChanceToFinalize, where sweepCellAtFinalization removes every list
  keyed on the dying cell and clears cellProtect and each abandoned async
  waiter's ticket promise Strong under the JSLock — closing the
  dangling-HandleSet leak for repeated VM create/destroy embeddings
  (unreachable as a crash in the one-VM jsc shell, hence no executable
  corpus coverage; same precedent as D8). Requested rev-15 amendment: make
  the per-cell teardown sweep normative in 5.6/5.10.
- D6 — 4.3(b) "live m_asyncHolder ticket" is tightened to "live AND
  DELIVERED" (ConditionObject.cpp asyncWait + AsyncTicket::grantDelivered,
  set by settleLockGrant's settle tasks). The pump/immediate-grant path
  installs m_asyncHolder BEFORE the grant's settle task runs, so 4.3(b)
  as frozen lets `lock.asyncHold(fn); cond.asyncWait(lock);` in one turn
  consume the pending grant and unlock m_lock; the with-fn settle task then
  runs fn WITHOUT the lock (E "not an error" only contemplates consumption
  DURING fn) — a mutual-exclusion hole (I6) — and the no-fn arm would
  resolve with a release fn whose first call throws the 4.2 Error for a
  release the user never performed. With the gate, an undelivered grant is
  "not held" => the 4.3 TypeError; I23 (fn calling asyncWait) is unaffected
  (delivered is set before fn starts). Requested rev-15 amendment: make the
  delivered gate normative in 4.3(b)/5.5a.
- D7 — compareExchange and the add/sub/and/or/xor family throw store's
  "property is not writable" TypeError on a ReadOnly own data property
  (ThreadAtomics.cpp), thrown unconditionally after the own-data check
  (matching store/exchange placement, before SVZ / the stored-number check).
  The frozen 4.5 rows say only "stores rep"/"stores result"; without the
  check, putExistingOwnDataPropertyForAtomics' define-semantics putDirect
  replaced ReadOnly slot values in place — a heap state no sequential JS
  program can create (THREAD.md indistinguishable-heap), and a lock word on
  a later-frozen object kept mutating instead of failing. Requested rev-15
  amendment: spell the writability rule into the cmpxchg/RMW rows. Covered
  by atomics/property-errors.js (frozen-object CAS/RMW block).

- D8 — per-VM single-flight gate on the D4 GIL-dropped typed-array sync
  Atomics.wait (AtomicsObject.cpp syncTAWaitGateLock /
  vmsWithSyncTAWaitInFlight). WaiterListManager::waitSyncImpl parks the ONE
  per-VM vm.syncWaiter() intrusive-list node — sound in stock JSC only
  because the parked thread holds the API lock, a guarantee D4 removes for
  NON-spawned threads (the 4.5-1a gate excludes only spawned Threads). Two
  embedder threads (or main + one embedder thread) sharing the VM could
  otherwise both drop the GIL and double-insert the same Waiter node:
  native heap corruption plus crossed wakeups. Unreachable in the jsc shell
  (exactly one non-spawned thread; $262 agents use separate VMs) — hence no
  executable corpus coverage — but a real trap for the Bun embedding. With
  the gate, the SECOND concurrent non-spawned sync TA wait on a VM throws
  TypeError "Atomics.wait is already in progress on another thread sharing
  this VM". Embedder constraint until the post-GIL re-freeze (Dev 12,
  per-wait waiter nodes): at most one non-spawned thread of a shared VM may
  sync-TA-wait at a time; further such waits throw rather than corrupt.
  Lifted together with D4.
- D9 — termination polling at ALL GIL-dropped park sites, not just 5.6-4's
  property wait. The frozen text gives cond.wait an infinite ParkingLot
  park (5.4-4), join an untimed joinCondition.wait (F5), and the contended
  hold a bare m_lock.lock() (5.3) — none of which VMTraps can wake, so a
  watchdog termination whose would-be notifier/releaser/joinee dies first
  hangs the process un-interruptibly (the exact failure mode the 5.6-4 poll
  was specified to close). Landed: cond.wait parks in 10ms
  parkConditionally quanta polling vm.hasTerminationRequest() (quantum
  timeouts re-loop, never surface as spurious returns; on termination the
  waiter dequeues itself under queueLock exactly like the spurious arm —
  dequeued <=> flipped preserved — and does NOT reacquire the lock, so the
  enclosing hold's epilogue guard skips its release, the 4.3(a) shape);
  cond.wait's lock reacquisition and the contended hold use 10ms
  tryLockWithTimeout quanta with the same poll (on termination: throw
  without the lock); join uses 10ms joinCondition.waitUntil quanta (a
  completion observed under joinLock wins over a concurrent termination).
  All three throw via vm.throwTerminationException() back under the GIL,
  matching 5.6-7. Tests: api/condition-wait-termination.js,
  api/lock-hold-termination.js (both --watchdog=500
  --watchdog-exception-ok, hang = poll missing); the join poll has no
  deterministic hang-detection shape (every park its joinee can occupy now
  polls termination itself, so the joinee always completes and wakes the
  joiner) and is belt-and-braces — exercised incidentally by
  atomics/property-wait-termination.js's join() under watchdog. Requested
  rev-15 amendment: make the poll normative at all 5.2 park sites.
- D10 — sync lock.hold inside an asyncHold(fn)-delivered fn throws "Lock is
  not recursive" instead of self-deadlocking. Async holds are invisible to
  m_holder, so the frozen 5.3 recursion check let lock.hold(g) inside the
  delivered fn pass, fail tryLock, and park in m_lock — whose only release
  point is that fn's own post-fn epilogue (E): guaranteed self-deadlock
  from straightforward user code. Landed:
  NativeLockState::m_asyncGrantRunner (std::atomic<WTF::Thread*>, compared
  never dereferenced) is set by settleLockGrant's with-fn settle task
  around JSC::call(fn) and cleared by asyncReleaseInternal (covers both E
  and cond.asyncWait's 4.3(b) consumption — after consumption the lock is
  free and a sync hold from the rest of fn is again legal);
  lockProtoFuncHold treats runner==current && m_asyncHeld as held-by-
  current-thread. Deliberately NOT applied: lockProtoFuncAsyncHold (frozen
  4.2: "async-held is NOT recur (callers queue)" — the queued ticket is
  granted after E, no deadlock) and m_holder itself (setting m_holder
  during fn would let cond.asyncWait's (a) arm releaseSyncHold a
  ticket-owned hold => double-unlock when E's tryConsume later succeeds).
  Sync cond.wait inside the fn still throws the 4.3 TypeError (frozen: 4.3
  requires a 5.3 SYNC hold; the (b) arm of asyncWait is the supported
  path). Covered by api/lock-async-hold.js test 8. Requested rev-15
  amendment: spell the delivered-grant recursion rule into 4.2/5.3.
- D11 — KNOWN-OPEN until the 9.2-9 hunk applies: GILDroppedSection's plain
  unlock loop bypasses m_lockDropDepth, and JSLock::willReleaseLock()
  guards its VM::drainMicrotasks() on that depth — so today the FINAL
  unlock at every park site (join, contended hold, cond.wait, property
  Atomics.wait, the D4 TA wait, notify's D2 yield) synchronously runs all
  queued promise reactions inside the host call: the 5.2 yield-point
  contract is false beyond D2 (reentrancy into NativeLockState/
  NativeConditionState mid-operation; termination installed by a drained
  task while the frame then proceeds to park). The earlier LockObject.h
  claim that the only DAL divergence was LIFO bookkeeping was WRONG and has
  been corrected (constraint (3) on the class comment). Fix = the 9.2-9
  JSLock::unlockAllForThreadParking hunk (depth bumped/restored while
  m_lock is still held: drain suppressed, livelock fix preserved, every
  other willReleaseLock side effect identical to dropAllLocks). JSLock.{h,
  cpp} are outside this part's owned paths, so the hunk is INT-gated like
  9.2-6; the splice marker is in GILDroppedSection's constructor, and the
  regression test (api/park-no-microtask-drain.js) lands //@ skip'ped with
  an integrator unskip instruction in 9.2-9. Requested rev-15 amendment:
  name unlockAllForThreadParking normatively next to GILDroppedSection
  (D1).
- D12 — 4.3(b) tightened again at round 4 (on top of D6): a DELIVERED
  with-fn grant is "held" only FOR THE THREAD RUNNING fn
  (ConditionObject.cpp asyncWait (b) arm now also requires
  NativeLockState::asyncGrantRunByCurrentThread(), the D10 runner
  identity). Without it, any foreign thread could consume the live grant
  while fn was parked at a GIL-dropping site, unlocking m_lock
  mid-critical-section (I6 violation reachable from plain API calls under
  the phase-1 GIL). Same-thread consumption from inside fn (I23) is
  unaffected (runner == current; markGrantDelivered and the runner store
  are adjacent under the JSLock, so a delivered live with-fn grant always
  has its runner set before any other JS can observe it). The NO-FN arm's
  release fn remains a transferable capability — cross-thread (b)
  consumption of a delivered no-fn grant stays legal per the frozen
  "unvalidated consumption" text; the spec owner is asked to either
  confirm that asymmetry or extend the runner rule at rev-15. Covered by
  the new racy block in api/condition-async-wait.js (foreign asyncWait
  during a live with-fn grant => TypeError, lock stays held, E intact).
- D13 — Thread.restrict rejects method-table-overrider receivers
  (ThreadObject.cpp restrictReceiverStaysOnHookedPaths, called from
  isExcludedRestrictReceiver). The 5.7.3/9.2-6 enforcement argument
  ("every enforced op lands on a hooked generic path once the object is
  pinned uncacheable-dictionary+SlowPut") is false for receivers whose
  ClassInfo method table overrides the enforced entry points with
  non-delegating implementations — typed-array views index through
  TypedArrayType-keyed paths that never consult the butterfly or
  dictionary-ness (foreign read AND write of a "restricted" Float64Array
  with no CAE), StringObject chars, DirectArguments/ScopedArguments/
  ClonedArguments, JSFunction lazy own properties, RegExpObject lastIndex,
  etc. Landed rule: ALLOWLIST — enforced slots all equal to the JSObject
  defaults (pointer-compared; C++ resolves un-overridden &Derived::op to
  &JSObject::op), plus JSArray exactly (audited-delegating; arrays are
  I14-covered). Everything else throws the 5.7 TypeError at restrict
  time, so unenforceable objects are never accepted. Narrowing vs the
  frozen "could be any object" THREAD.md text is deliberate — soundness
  over surface; rev-15 should either bless the allowlist or charter the
  per-overrider hook sites. Tests: overrider-instance TypeError block in
  api/thread-restrict.js (still //@ skip with the file per I14; present
  for the INT gate). The 9.2-6 "do not hook getOwnPropertySlotByIndex"
  instruction is now CONDITIONALLY valid: it relies on this allowlist (see
  the amended note in that entry).

Round-1 hardening fixes in the same sweep (bug fixes, not spec deviations):
- Thread.restrict affinity table (ThreadManager.cpp): entries whose Weak is
  not Live-and-equal-to-the-probed-object are treated as ABSENT everywhere,
  restrictObject replaces stale entries for recycled cell addresses, and the
  pruning finalizer carries the entry's address as its Weak context so a
  late finalizer can never evict a successor entry (deleted-slot-reuse
  hazard closed; counts stay exact).
- harness.js sleepMs re-homed onto the property-path Atomics.wait (see the
  corrected Task-12 note below): the corpus' waitUntil rendezvous now
  genuinely releases the GIL.

## Audit notes (Task 1 scaffolding verification, SPEC-api §10-1)

- Six 9.1 file pairs present and conformant: runtime/{ThreadObject,
  ThreadManager, ThreadAtomics, ThreadLocalObject, LockObject,
  ConditionObject}.{h,cpp}. Each cell class (JSThread, JSLockObject,
  JSConditionObject, JSThreadLocalObject) is a final JSDestructibleObject
  subclass with `needsDestruction = NeedsDestruction`, `subspaceFor` ->
  `vm.destructibleObjectSpace()` (G13), DECLARE_EXPORT_INFO + ClassInfo,
  ordinary prototype with Symbol.toStringTag, and a createStructure helper.
  Type names per §7 (JSC::JSLock is taken). No ThreadRestricted TypeInfo flag.
- §7 frozen signatures present verbatim in ThreadManager.h
  (singleton/mainThreadTID/notTTLTID/currentTID/isJSThreadCurrent/
  forEachThreadState(const Invocable<void(ThreadState&)> auto&)/
  threadRestrictCheck) and ThreadAtomics.h (AtomicsRMWOp + the four
  atomics*OnProperty entry points). currentButterflyTID() is NOT defined by
  this workstream (sole provider: vmstate 6.7; ODR).
- runtime/AtomicsObject.cpp: prep stub already routes non-view objects to
  ThreadAtomics under the master flag in the shared helpers (4.5-0..3
  placement); audited/extended by §10 task 8 (I1: TA path textually intact).

## Audit notes (Task 2 — TM + GIL spawn / completion / sync join, SPEC-api §10-2)

No NEW shared-hot-file text is required by this task; everything landed in
api-owned files (runtime/ThreadManager.h, runtime/ThreadObject.{h,cpp}).
Changes made to bring the prep-stub spawn/join machinery to rev-14
conformance:

- F1/5.1: the thread result moved from a WriteBarrier on the JSThread cell to
  `ThreadState::result` (Strong<Unknown>), written in the completion sequence
  strictly BEFORE the Phase release-store; join()/settle readers load-acquire
  Phase first, then read the result under the JSLock.
- 5.10 finalizer hook: ONE `vm.heap.addFinalizer(jsThread cell, lambda)`
  (public Heap API only — confirms the "no VM.h/.cpp edit" rule) is registered
  at every TS::jsThread creation: spawner-side in constructThread (under the
  GIL, pre Thread::create) and lazy-side in jsThreadForState. The lambda holds
  Ref<ThreadState> and clears any still-set jsThread/threadLocals/result
  Strongs; it is the SOLE clearer of TS::result. `~ThreadState` now
  RELEASE_ASSERTs result/fnSlot/argSlots/threadLocals/jsThread empty (5.10).
- 5.1: added the (phase-1 inert) post-GIL ticket-inbox fields to ThreadState
  (inboxLock rank 3 / inbox / inboxOpen).
- 4.6.1/F5 completion sequence brought to exact spec order: fnSlot/argSlots
  cleared right after fn returns/throws (pre-drain), one microtask-queue
  drain, F1 result publish, then under joinLock {Phase release-store,
  joinCondition.notifyAll(), swap asyncJoiners out}, drop joinLock, settle
  moved tickets via the 5.5 schedule (never waits for tickets), clear
  threadLocals/jsThread Strongs, ThreadManager::unregisterThread (m_threads
  erase under rank-1 m_lock; TID retired forever, Dev 10), then JSLock
  release.
- 5.2/9.2-8: `// THREADS-INTEGRATE(api)` markers now sit at the two exact
  splice points in threadMain (pre-JSLockHolder spawn handshake; in-final-JSL
  teardown) matching the 9.2-8 ready-to-apply diff above.
- I17 hardening: constructThread fetches the callee's `prototype` (the only
  JS-running/throwing step) BEFORE ThreadManager::allocateSpawnedThreadState,
  so a throw can no longer leak a forever-Running TS into m_threads (which
  counted against maxJSThreads). TID space remains [1, 0x7ffe]; exhaustion or
  live-cap excess => RangeError at spawn.

DEPENDENCY NOTE for §10 tasks 3/7 (lazy TS Strongs): the 5.10 finalizer hook
is registered only where a TS::jsThread cell exists. If ThreadLocal's setter
(ThreadLocalObject.cpp) creates the FIRST Strong on a lazy main/embedder TS
(threadLocals value with no prior Thread.current access), it must first
materialize the jsThread cell via the Thread.current path so the hook gets
registered — otherwise `~ThreadState`'s RELEASE_ASSERT(threadLocals.isEmpty())
can fire on embedder-thread TLS destruction. Spec basis: 5.10 "Finalizer hook
(EVERY TS, spawned+lazy): at TS::jsThread creation ... or first lazy-TS
Strong".

Known landed deviation (prep-stub, kept): the GIL is released at blocking
park sites via GILDroppedSection (LockObject.h) rather than
JSLock::DropAllLocks — this is Landed deviation D1 above (escalation,
state-set sync obligation, and the phase-1 embedder-DropAllLocks constraint
are recorded there). Semantics required by 5.2 ("GIL released while
blocked") are preserved; flag-off code is untouched (I1).

## Audit notes (Task 4 — asyncJoin via 5.5 tickets, SPEC-api §10-4)

No new shared-hot-file hunks. Changes are confined to owned files
(runtime/ThreadManager.h/.cpp, runtime/ThreadObject.cpp):

- AsyncTicket brought to the full 5.5 member set: added `Ref<ThreadState>
  m_registrant` (captured via ensureCurrentThreadState() at registration;
  4.6.2 — tickets outlive their registering thread) and
  `Strong<JSPromise> m_promise` (created at registration under the JSLock,
  cleared by the settle task per the 5.10 table; never-settled tickets fall
  to the DWT VM-shutdown cancelPendingWork backstop). Ctor/dtor moved fully
  out of line; create()/settle() signatures unchanged, so the prep-stub call
  sites in LockObject.cpp / ConditionObject.cpp / ThreadAtomics.cpp are
  unaffected.
- settle() now wraps the caller's task: runs it on the run-loop turn under
  the JSLock (scheduleWorkSoon, I12 — never synchronous in the registering
  call), then clears the promise Strong; the wrapper's Ref keeps the ticket
  alive through settlement.
- threadProtoFuncAsyncJoin (F5): the Finished-vs-Failed decision is now
  captured under joinLock together with the phase re-check (store + re-check
  both under joinLock; no lost wakeup), instead of re-loading phase after
  dropping the lock. settleJoinTicket reads the promise from the ticket's
  Strong rather than dwtTicket->target().
- I20 liveness is addPendingWork-at-registration (AsyncTicket::create), NOT
  settle-time; documented at the create() site.

## Audit notes (Task 5 — Lock: NLS 5.3, hold I6-I8, 5.5a asyncHold/E/pump/locked)

No new shared-hot-file hunks. Changes are confined to owned files
(runtime/LockObject.h/.cpp):

- Contended lock.hold now matches the frozen 5.3 sequence: on tryLock
  failure, G11-gate then GILDroppedSection (the landed depth-free DAL
  equivalent, see deviation note above) + blocking m_lock.lock(); the GIL is
  reacquired by the section destructor WITH m_lock held — the one permitted
  rank-4-leaf shape of 5.9(e). This replaces the prep-stub tryLock +
  jsThreadGILHandoffYield spin (which acquired m_lock only while holding the
  GIL and busy-waited). jsThreadGILHandoffYield itself is retained with
  exactly ONE caller: ConditionObject.cpp notifyImpl's fair-handoff yield
  (Landed deviation D2 above). cond.wait does NOT use it — its reacquisition
  is GILDroppedSection + a direct m_lock.lock() (corrected at review round 1;
  an earlier revision of this note wrongly named the 5.4 reacquisition as the
  caller).
- 5.5a schedPump now dispatches P on the HEAD ticket's vm.runLoop() (spec
  text: "dispatch P on head tkt's vm.runLoop()"), not the releasing caller's
  VM. Phase 1 is a single shared VM so behavior is identical; the routing
  matters post-GIL. NativeLockState::pump() lost its VM& parameter (it never
  used it); releasePump/enqueueAsyncAcquirer/asyncReleaseInternal signatures
  are unchanged, so ConditionObject.cpp call sites are unaffected.
- Audited as already conformant (no change): A success/failure paths
  (R2-1 enqueue-AND-schedPump shape), P clear-pending-before-tryLock with
  empty-waiters unlock (R1-7/R4-1), E post-fn consumed-CAS epilogue settling
  with fn's result/exc either way (R4-5/I23), R after every m_lock release,
  release-fn double-call Error (4.2), sync hold epilogue m_holder guard
  (R2-2/4.3(a)), locked getter = m_lock.isLocked() || m_asyncHeld.

## Audit notes (Task 7 — ThreadLocal: 5.8, I13)

No new shared-hot-file hunks. The ThreadLocal registration line (9.2-2),
Sources.txt entry, and CMakeLists PRIVATE_FRAMEWORK_HEADERS entry for
runtime/ThreadLocalObject.{cpp,h} are already recorded above (Task 1).
Changes are confined to owned files (runtime/ThreadLocalObject.h/.cpp):

- value getter is now a pure probe of the CURRENT ThreadState's
  threadLocals map via currentThreadStateIfExists() (5.8: get/set touch
  only that map, lock-free). A thread with no ThreadState cannot have run
  the setter, so a cold read returns the initial undefined (I13) WITHOUT
  allocating/installing a lazy TS. Previously the getter called
  ensureCurrentThreadState() and materialized a ThreadState on every
  first read from an embedder thread.
- value setter unchanged in behavior; documented the normative clear
  points: HashMap::set destroys the prior Strong = the 5.10 "overwrite"
  clear (under the JSLock); thread-exit clear = completion sequence
  (spawned, ThreadObject.cpp) / 5.10 finalizer hook (lazy TS, registered
  via ensureJSThreadForState() before the first Strong — see the Task 3
  note above on hook ordering).
- I13 leak documented on the class (5.8): a dead ThreadLocal cell leaks
  its slots in other live threads' maps until those threads exit; keys
  are monotonic and never reused, so leaked slots can never alias a
  later-created ThreadLocal.
- Mechanism deviation noted for the integrator (no action needed): 5.8
  says the process-unique monotonic key is allocated by TM "u/m_lock";
  the landed allocator (Task 1, ThreadManager.h
  allocateThreadLocalKey()) uses a std::atomic<uint64_t> fetch-increment
  instead. Guarantees delivered are identical (process-unique,
  monotonic, no reuse); ThreadManager.h is outside Task 7's file list so
  it was left as is.

## Audit notes (Task 8 — Atomics dispatch split, SPEC-api §10-8 / 4.5 steps 0-3, I1/I21)

No new shared-hot-file hunks. runtime/AtomicsObject.cpp is api-owned (9.1,
"dispatch split only"); it was already in Sources.txt before this workstream
(G17), so no 9.2-4 entry is needed for it.

- Placement audited as conformant with the frozen 4.5 text: steps 0-3 live in
  the SHARED helpers — atomicReadModifyWrite(globalObject, vm, args, Func)
  (the AO:182 overload) and atomicStore — so the host functions AND the
  DFG/FTL untyped operationAtomicsAdd/And/CompareExchange/Exchange/Load/Or/
  Store/Sub/Xor all route through the same dispatch; tier-up cannot change
  semantics. wait/waitAsync/notify dispatch sits in their host functions only
  (no JIT operation exists for them, per 4.5).
- Step 0 (I1): the flag gate is useJSThreadsEnabled() (== the canonical
  Options::useJSThreads() since the round-3 9.2-1 paired-edit landing); flag
  off => the property branch is never taken and today's typed-array body
  below it is textually intact (steps 1-3 dead). Step 1: any JSArrayBufferView arg0 (including
  float-typed views and DataView) skips the property branch and keeps
  today's path, results and errors included. Step 2: non-view object arg0 =>
  property path, arg1 via ToPropertyKey, routed to the ThreadAtomics.h entry
  points (atomicsLoad/Store/RMW/CompareExchangeOnProperty;
  atomicsWait/WaitAsync/NotifyOnProperty). Step 3: non-object arg0 falls
  through to today's body and its TypeErrors.
- Conformance fix landed this task: the 4.5-1a TA sync-wait gate (GPO, I21)
  in atomicsFuncWait previously fired for ANY non-property-path arg0 on a
  spawned Thread, including non-objects; per the frozen text 1a is a
  carve-out of step 1 only (arg0 is a view). The gate is now nested under
  the isObject()/view check, so a spawned-thread Atomics.wait(42, ...)
  takes step 3 and throws today's validateTypedArray TypeError, while a
  spawned-thread sync wait on a view still throws TypeError
  ("Atomics.wait cannot be called from the current thread.") before the
  body, with no side effects. Gate is GIL-phase-only; deleted at the
  post-GIL re-freeze (Dev 12), covered by I21/ta-wait-thread-gate.js.
- Landed JSTests/threads/atomics/ta-path-unchanged.js (API-I1; first file of
  the §8 atomics/ manifest). Per annex T2 it is the one test WITHOUT
  //@ requireOptions("--useJSThreads=1"): it declares
  `//@ runDefault` + `//@ runDefault("--useJSThreads=1")` and runs both
  ways, pinning identical TA-path behavior (RMW/load/store on
  Int32/Uint8/Uint16/Uint32/BigInt64 views, SAB wait/waitAsync/notify fast
  paths incl. zero-timeout "timed-out", exact error messages for
  non-integer views / non-shared wait / OOB indices, non-object arg0
  TypeErrors, isLockFree/pause, function lengths/toStringTag).

## Audit notes (Task 10 — PWT: prop wait/waitAsync/notify, SPEC-api §10-10)

No new shared-hot-file hunks. Changes are confined to owned files
(runtime/ThreadAtomics.cpp; ThreadAtomics.h unchanged — the §7 frozen
surface plus the host-only wait/waitAsync/notify entry points were already
declared). The prep-stub PropertyWaiterTable (5.6: process singleton in
runtime/ThreadAtomics.cpp, rank-2 table lock, per-list rank-3 listLock,
(JSCell*, UniquedStringImpl*) keying per Deviation 3, first-waiter
Strong<JSObject>+Ref<UniquedStringImpl> liveness per the 5.10 table) and
the F4 sync-wait body (steps 1-7, 10ms termination-poll quantum because
VMTraps pokes only vm.syncWaiter and cannot wake PWT waiters) were audited
line-by-line against the frozen text and kept; this task closed four
conformance gaps:

- G11 gate placement (4.5 wait row): the gate guards the *block*, not the
  call — "!SVZ(current,exp)=>'not-equal';else block (G11-gated)". The gate
  now runs AFTER the step-1 own-data read + SameValueZero short-circuit
  (still under the JSLock, before any enqueue side effect), matching
  lock.hold which G11-gates only on contention (5.3). A
  cannot-block thread calling prop Atomics.wait with a not-equal value gets
  "not-equal"; with an equal value it gets the I18 TypeError.
- Missing exception checks after SameValueZero in both wait and waitAsync
  (string comparison can resolve ropes => OOM throw); the CAS path already
  had them.
- F4 notify: "async: flip tkt Notified u/LL" — the AsyncTicket's 5.5 state
  byte (Waiting->Notified|TimedOut) is now release-stored under the
  listLock in the notify path, and TimedOut in the G28 timer task; the
  PropertyWaiter state values (Waiting=0, Notified=1, TimedOut=2) coincide
  with the 5.5 ticket mapping by construction. Settles still happen strictly
  after the listLock is released (never u/LL), via AsyncTicket::settle
  (one-shot; the waiter-state flip under listLock is the arbitration, so
  notify and the timeout timer can never both settle one ticket).
- 5.6 waitAsync timer-task order: Waiting => findAndRemove (now asserted to
  succeed: dequeued <=> flipped, both under the listLock), THEN TimedOut;
  cleanup per step 7 (removeListIfEmpty re-checks under table-then-list
  rank order, under the JSLock of the timer's JSLockHolder per 5.9(e)).
  Timer remains armed at registration on vm.runLoop().dispatchAfter (G28),
  never RunLoop::currentSingleton() (G26); infinite timeout arms no timer.
- Also moved the sync-wait deadline computation before the step-2 enqueue
  (timeout window starts at the call, matching WaiterListManager).

Invariant coverage by this code: I10 (JSLock held from step-1 read through
enqueue = lost store+notify closure; notify flips state under listLock
before condition.notifyOne), I11 (waiter identity = (cell,uid); TA waiters
live in WaiterListManager, never cross-woken), I22 (waitAsync finite
timeout settles "timed-out" via the 5.5 settle task on a run-loop turn),
I24 (termination => waiter removed + Terminated => throwTerminationException
in step 7, never "timed-out"/"ok" — except a Notified flip that already won
under listLock, which step 5's state-first check honors; quantum wakeups
re-loop and can never return spuriously). The §8 test files exercising
these (atomics/property-wait-notify.js, property-wait-termination.js,
property-wtr-isolation.js, property-waitasync-timeout.js) land with the
Task-12 corpus.

Park-site note: the F4 step-3 "DAL" is GILDroppedSection per the known
landed deviation recorded above (DropAllLocks strict-LIFO unwind livelocks
with timed waiters waking in arbitrary order); steps 3-6 are one dropped
scope with Locker{listLock} strictly inside, and the listLock is released
before the GIL is reacquired (5.9(e); no rank-4 leaf is held here, unlike
the 5.3 contended-hold shape).

## Audit notes (Task 11 — Thread.restrict + CAE, SPEC-api §10-11 / 5.7 / Dev 8+11)

Shared-hot-file text for this task is the 9.2-6 hook diff above (now READY;
INTEGRATOR-applied after the obj-model diff). Everything else landed in owned
files (runtime/ThreadObject.cpp, runtime/ThreadManager.{h,cpp},
JSTests/threads/api/thread-restrict.js):

- Dev-8 excluded-receiver set completed (ThreadObject.cpp
  isExcludedRestrictReceiver / isSpeciesProtectedBuiltin). The prep stub
  compared only the calling realm's Array/Object/RegExp/Promise PROTOTYPES;
  now: any global object + environment/scope objects (isEnvironment(), which
  spans GlobalObjectType..StrictEvalActivationType, plus isWithScope()), any
  Proxy/JSGlobalProxy, and the full frozen species-protected pair list —
  Array, Promise, RegExp (eager WriteBarrier slots, void*-ptr-compared since
  some return types are forward-declared), ArrayBuffer + SharedArrayBuffer in
  BOTH sharing modes, each %TypedArray% view pair, and the %TypedArray% super
  pair — all against o's OWN globalObject() (never the calling realm's).
  Never-force rule (Dev 8 "never force lazy slots"): lazy pairs are gated on
  the non-forcing Concurrently accessors (arrayBufferStructureConcurrently,
  typedArrayStructureConcurrently); only when the LazyClassStructure is
  already materialized are its prototype()/constructor() accessors consulted
  (then they no longer run the initializer). Deviation, recorded: the
  %TypedArray% super pair has NO public non-forcing slot accessor
  (m_typedArrayProto/m_typedArraySuperConstructor are private LazyProperty
  members), so it is detected by ClassInfo identity
  (inherits<JSTypedArrayViewPrototype>/inherits<JSTypedArrayViewConstructor>)
  — never forces, exact (one instance of each class per global), and excludes
  other realms' super pairs as well (a sound superset). Second recorded
  delta: the prep stub also excluded Object.prototype; Dev 8's frozen list
  does not include the Object pair, so that exclusion was REMOVED
  (restricting Object.prototype is now legal, catastrophic-but-perf-only per
  5.7.1; residual escapes are exactly the Dev-8 excluded receivers).
- Dev 11 kept: hijacksIndexingHeader() structures => TypeError (5.7.1(a)
  needs ArrayStorage).
- 5.7.1 conversion sequence in threadFuncRestrict now carries the normative
  step structure and the (d) post-conditions:
  (0) affinity hit (owner => return o, foreign => CAE) before any conversion;
  (a) ensureArrayStorage with non-null assert (post-Dev-11);
  (b) guarded switchToSlowPutArrayStorage (guard mandatory: already-SlowPut
      shapes CRASH() in the conversion switch — reachable via
      restrict-after-bad-time, covered by the test's SlowPut block);
  (c) guarded convertToUncacheableDictionary (keeps indexing mode);
  (d) setHasBeenFlattenedBefore(true) pin + ASSERTs
      isUncacheableDictionary() && hasSlowPutArrayStorage(indexingType()).
- 5.7.2 affinity table brought to the frozen text: entries are now
  { Ref<ThreadState> owner, Weak<JSObject> } (ThreadAffinityEntry,
  ThreadManager.h) and are PRUNED by a per-insert Weak finalizer
  (ThreadAffinityWeakHandleOwner in ThreadManager.cpp;
  ThreadManager::pruneRestrictedObject removes the entry and decrements
  m_restrictedCount under the rank-2 affinity lock — finalizers run holding
  the JSLock with no rank 1-3 lock held, 5.9-legal). The prep stub kept a
  strong Ref-keyed map forever (leaked entries AND left the relaxed
  m_restrictedCount fast path permanently non-zero). Owner identity remains
  the Ref<ThreadState>/nativeThread compare, never a TID; the mandatory fast
  path (one relaxed m_restrictedCount load, zero => allow, no lock) is
  unchanged in threadRestrictCheck.
- JSTests/threads/api/thread-restrict.js (API-I14) landed `//@ skip`ped per
  I14 ("INT gate via 9.2-6"); the integrator deletes the skip line with the
  hook. Covers: full enforced named set (get/set/has/delete/defineProperty/
  ownKeys/setPrototypeOf/isExtensible/preventExtensions) + indexed
  set/delete/define on an array + indexed set on a plain {} after the owner
  adds o[0], all from a foreign Thread => CAE; owner unaffected with values
  unchanged; IC warm-up loops before AND after restrict (5.7.1 warm-ups);
  owner double-restrict idempotency; foreign re-restrict => CAE (also with a
  finished owner thread — restriction outlives the owner); post-bad-time
  SlowPut array restrict; Dev-8/11 exclusion TypeErrors (lazy builtins
  materialized first so the never-force gating cannot mask a miss); CAE
  shape (Error subclass, name). Dev-8 unenforced set untested per I14.

## Audit notes (Task 12 — test corpus: every §8 file, run-tests.sh, coverage grep)

- JSTests/threads/harness.js landed as the §8 entry point: loads
  resources/assert.js (shouldBe/shouldThrow(type,fn)/spawnN/withTimeout plus
  shouldBeTrue/False, shouldNotThrow, joinAll) and adds two cooperative-GIL
  utilities used corpus-wide: sleepMs(ms) and waitUntil(cond, maxMs=30s,
  step=5ms) (bounded rendezvous; throws on deadline per annex T2 "race tests
  bound blocking ops"). CORRECTED at review round 1: sleepMs originally slept
  on a private SharedArrayBuffer lane and claimed to release the GIL — false:
  the typed-array sync-wait path (WaiterListManager::waitSyncImpl) parks
  while still holding the JSLock, so every main-thread waitUntil whose
  condition needed spawned-thread progress deadlocked until its 30s deadline.
  sleepMs now uses the PROPERTY-path Atomics.wait on a harness-private plain
  object, which parks under GILDroppedSection (ThreadAtomics.cpp); isolation
  from property-waiter assertions holds by construction (PWT waiters are
  keyed (cell, uid) and the lane object is harness-private). The TA lane
  remains only as the flag-off fallback, where no spawned Threads exist.
- api/ per annex §T: thread-basic.js (API-I2/I4/I5), thread-exc.js (I3),
  thread-ctor-errors.js (4.1 exact messages + CAE shape + DontEnum globals +
  toStringTags), thread-id-bounds.js (I17; --maxJSThreads=4 makes the live
  cap cheap; monotonic-fresh-TID assert is explicitly pre-rebias and will be
  relaxed by Dev-10/task 15), thread-lifecycle.js (I20 + 4.6.2 dead-registrant
  asyncHold ticket), lock-basic.js (I6 small-N with main contending, I7, I8),
  lock-async-hold.js (I12, I23, release-fn exactly-once contract, FIFO,
  deterministic barge: sync hold overtakes a queued ticket before the pump's
  RL turn), condition-basic.js (I9; predicate loops; ticketed 2-waiter
  handover with main = 3rd thread; notify counts asserted with spurious-
  wakeup-tolerant bounds), condition-async-wait.js (I12 on both 4.3(a)/(b)
  consumption paths + consumed-release Error + mixed sync/async notifyAll
  count), threadlocal-basic.js (I13), blocking-gate.js (I18; self-guards
  against a missing --can-block-is-false via a TA-wait probe; carries
  `//@ runDefault("--can-block-is-false")` so parseRunCommands appends the
  flag in yaml-land while run-tests.sh appends it itself — the duplicate CLI
  flag is idempotent).
- atomics/ per annex §T: property-load-store.js (4.5 value/key semantics,
  attribute preservation, symbol/index/ToPropertyKey keys, inline vs
  out-of-line slots), property-rmw.js (I15 single-thread edges: double
  add/sub, ToInt32 and/or/xor with raw old-value return, operand coercion
  side effects, exchange identity; 2e4-iteration default-JIT Atomics.add
  loop + 1e4 exchange/sub loops per the "tiered" clause),
  property-cas-samevaluezero.js (NaN/NaN, +0/-0, identity/string/bigint SVZ,
  NaN-tolerant CAS retry loop), property-wait-notify.js (I10 handshake +
  50-round ping-pong; I24 quantum half: 250ms unnotified wait returns
  "timed-out" never early, multi-quantum notified wait returns exactly "ok"),
  property-wait-termination.js (I24 termination half; --watchdog=500
  --watchdog-exception-ok; FAILURE print + Error if the wait ever RETURNS),
  property-wtr-isolation.js (I11 via notify-count assertions — wrong-target
  notifies must report 0 with a live waiter parked; includes uid-canonical-
  ization probes "1" vs 1 vs "01"), property-waitasync-timeout.js (I22:
  spawned-thread 100ms timer settles a dead registrant's ticket; TA result
  shapes for not-equal/zero/negative timeouts; main-thread timer; notified
  waitAsync settles "ok"), ta-wait-thread-gate.js (I21: gate precedes the
  body — mismatch and zero-timeout also throw from a spawned Thread; TA
  waitAsync/notify and PROPERTY waits unaffected), property-errors.js (4.5
  error matrix incl. step-3 non-objects, float-view/DataView discriminators
  — a Float64Array HAS own "0", so wrong-branch dispatch is observable —
  ToPropertyKey ordering/exceptions, exchange-vs-RMW non-number asymmetry).
  ta-path-unchanged.js (pre-existing) gained the API-I19 citation: the
  flag-off PERF half is measured by Tools/threads/bench-gate.sh (I19 is an
  INT gate), the corpus citation lives in that header for the §6 grep.
- races/ per annex §T, all GI (post-GIL semantics), all deterministic-green
  under the GIL and designed for Tools/threads/amplify.sh (+TSAN no-JIT,
  G15), all with bounded blocking and full join coverage:
  counter-lock.js (I6 at scale, N=8 M=1e5, gate opened while main HOLDS the
  lock so all 8 first holds park together — >=2 parked waiters by
  construction), counter-atomics.js (I15 at scale + per-thread 1000-step CAS
  retry loops), transition-vs-read.js + transition-vs-write.js (I16,
  THREAD.md invariants by name: no lost properties, no torn shapes, no
  time-travel — Atomics-published watermark + monotonic in-place slot +
  disjoint-range adds + same-name races + delete-quarantine churn with a
  boxed payload so stale aliasing surfaces as a wrong object),
  wait-notify-storm.js (I10 under contention: 6 waiters x 200 rounds,
  one-then-rest two-step notify, exact done-count), join-storm.js (I4 at
  scale: 8 sync + 8 async joiners racing completion, rejection storm with
  exception identity, nested join chains, post-completion waves).
- Tools/threads/run-tests.sh landed (see 9.2-7 for behavior); `bash -n`
  clean; --list verified against the working tree (39 files resolved:
  12 api + 10 atomics + 6 races + 10 vmstate + 1 jit; heap/objectmodel dirs
  picked up automatically when they appear). [Round 3: api/ gained
  condition-wait-termination.js and lock-hold-termination.js => 41 files;
  the script also gained the option probe and skip-aware coverage grep, see
  the updated 9.2-7 caveats.]
- Coverage grep verified in-tree: every API-I1..API-I24 is cited under
  JSTests/threads/{api,atomics,races} (the grep run-tests.sh performs;
  I24's skippable termination half is additionally covered by
  property-wait-termination.js).
- NOT runnable here (no build allowed): the corpus is written against the
  audited in-tree behavior of the task 1-11 code (exact error messages,
  waitAsync {async,value} shape, raw-old-value RMW returns, notify counts,
  CustomAccessor reflection) — first real jsc run happens at task 13 gates.

## Audit notes (Task 13 — gates: races / TSAN no-JIT / bench self-gate, SPEC-api §10-13)

- Entry point: `Tools/threads/run-tests.sh --gates` (sole option; rejects
  combination with --filter/--amplify/--list). Implements §10 task 13 /
  hist rev-3 text verbatim, degrading gracefully per G15:
  - gate[races]: re-invokes the runner with `--filter=/races/ --amplify`
    against the resolved `$JSC` (env override honored). The runner's own
    --amplify fallback (warn once, run plain) is the frozen §8 fallback
    when `Tools/threads/amplify.sh` is absent. Header directives
    (`//@ requireOptions("--useJSThreads=1")` etc.) are honored as in
    normal runs; AMPLIFY_RUNS is forwarded by environment.
  - gate[tsan]: only if a TSAN no-JIT target exists (default
    `WebKitBuild/TSan/bin/jsc`, override `TSAN_JSC=`; absent => SKIP, not
    FAIL). Runs races/ plain (not amplified — TSAN is the race oracle and
    its 5-15x slowdown makes amplified campaigns impractical) with
    TSAN.md's recommended options unless the caller set TSAN_OPTIONS:
    `suppressions=Tools/tsan/suppressions.txt history_size=7
    second_deadlock_stack=1 halt_on_error=1 exitcode=66`. A `FAIL ...
    (exit 66)` line therefore identifies a TSAN report vs an ordinary
    test failure; the FAIL message points at TSAN.md's CLoop
    shared-stack known limitation (phase-1 GIL stub: intermittent
    CLoop::execute SEGV is the shared interpreter stack, not a race).
  - gate[bench] (API-I19, implement half): `bench-gate.sh --record
    --baseline <mktemp>` then `bench-gate.sh --baseline <mktemp>` on the
    SAME build must exit 0. The throwaway baseline keeps the
    integrator-recorded `Tools/threads/baseline.json` untouched
    (self-recorded = vacuous per §6 I19; the authoritative pre-WS
    comparison runs at INT). Runs per benchmark default to bench-gate's 9
    (`BENCH_GATE_RUNS=` to override). Missing bench substrate => SKIP.
  - Exit: 0 iff every gate that EXECUTED passed; SKIPs (absent substrate,
    G15) are reported but never failures; 1 on any gate failure; 2 on
    usage/env error.
- In THIS tree all three substrates are present (WebKitBuild/{Debug,
  Release,TSan}/bin/jsc, Tools/threads/amplify.sh, JSTests/threads/bench/
  + Tools/threads/bench-gate.sh), so a `--gates` run executes all three
  gates with zero SKIPs.
- Verified without execution (no-build/no-run rule for this fan-out):
  `bash -n` clean; `--help` (header autoprinted via awk, immune to header
  growth), `--list`, and the `--gates`-exclusivity error path exercised.
  Actually RUNNING `--gates` (amplified race campaign + TSAN corpus +
  2x9-run bench sweep) is minutes-long and is the Verify phase's job;
  first real execution happens there. Gate semantics are mechanical
  re-invocations of already-landed, separately-audited tooling
  (run-tests.sh task 12, amplify.sh/bench-gate.sh/tsan.sh from prep).
- CI wiring suggestion (NOT a shared-file edit; for the integrator's
  workflow when adding a threads job): the single command
  `Tools/threads/run-tests.sh --gates` is the task-13 gate; for the
  authoritative I19 half, replace gate[bench] with
  `Tools/threads/bench-gate.sh /path/to/jsc` against the
  integrator-recorded `Tools/threads/baseline.json` at INT.

## Audit notes (Task 14 — manifest finalization, SPEC-api §10-14)

Task 14 touched ONLY this file. What changed, and the integrator's ordered
apply checklist:

- 9.2-1: recorded the previously-missed second non-canonical option
  (`useThreadGIL`, OptionsList.h:686, zero code consumers) alongside the
  known `useThreads` alias (:685); both now have one exact removal diff plus
  the two mandatory paired api-owned edits — the ThreadManager.h
  useJSThreadsEnabled() reduction and the 16-file prep-corpus header
  migration (`--useThreads=true` -> `--useJSThreads=1`; exhaustive file list
  in the entry). The four canonical §3 options re-verified byte-identical at
  :681-684.
- 9.2-2: re-verified live at JSGlobalObject.cpp:1628-1635 + include at :37;
  added the optional one-token guard respelling diff
  (useJSThreadsEnabled() -> Options::useJSThreads()) for canonical parity
  once 9.2-1 lands.
- 9.2-4/9.2-5: re-verified at the recorded Sources.txt/CMakeLists.txt lines;
  no diffs needed (live).
- 9.2-6: all 14 hook-site context anchors re-verified verbatim against the
  working tree (per-site anchor list added to the entry's NOTE); diff
  unchanged from task 11.
- 9.2-7: replaced the bare stanza list with the COMPLETE ready-to-create
  JSTests/threads.yaml (license header per sibling yamls); promoted
  threads/vmstate and threads/jit into the immediate stanzas (both dirs
  exist in this tree — "when present" satisfied; run-javascriptcore-tests
  rejects missing paths so objectmodel/heap stay commented); documented the
  out-of-scope prep dirs and their 9.2-1(b) dependency.
  [SUPERSEDED at round 2: the threads/vmstate promotion was wrong — two of
  its files require options that do not exist until vmstate M_opts; the
  stanza moved back to the preconditioned append section. See the corrected
  9.2-7 entry.]
- 9.2-8: PENDING -> READY. The vmstate (runtime/VMLite.h,
  runtime/VMLiteShared.h) and jit-1b (jit/ConcurrentButterflyOperations.h —
  not the guessed "ButterflyTIDTag.h") headers now exist in this tree; every
  signature the hunk calls was verified, and the entry is now one exact
  unified diff against ThreadObject.cpp's task-2 splice markers (:131/:209),
  including the #include hunks, the post-JSLock `lite = nullptr` destruction
  point, the vmstate/jit line separability map, and the
  setCurrent-hook-vs-explicit-call redundancy rationale.

Recommended apply order at INT (REVISED at round 2 — the original order
applied 9.2-1 unconditionally first and 9.2-7 before vmstate M_opts, both
wrong; see the 9.2-1 cross-WS conflict note and the 9.2-7 correction):

1. 9.2-1 `useThreads` line only (the two paired api-owned edits —
   ThreadManager.h gate reduction, 16-file header migration — LANDED at
   round 3, so this is now just the one OptionsList.h deletion line; it can
   land at any point, the alias is already dead). If vmstate M_opts2
   already landed, drop its `Options::useThreads()` normalization line in
   the same commit (vmstate item 14).
2. 9.2-2 respelling (optional).
2b. [ADDED at round 4] 9.2-9 JSLock::unlockAllForThreadParking + the
   GILDroppedSection splice + unskip api/park-no-microtask-drain.js.
   Independent of every other entry; apply EARLY — it closes the live D11
   deviation (park-site microtask drain), the round-4 blocker.
3. 9.2-7 yaml WITHOUT the threads/vmstate stanza.
4. obj-model diff -> 9.2-6 (+delete thread-restrict.js skip line; the
   runner's coverage grep is already skip-aware and announces the I14
   deferral until then).
5. vmstate M_opts2/6.4.4 + jit-1b; THEN append the threads/vmstate stanza to
   threads.yaml (its options now exist; run-tests.sh picks the files up
   automatically — its option probe stops SKIPping them once the options
   parse).
6. 9.2-8.
7. 9.2-1 `useThreadGIL` line LAST, and only under the agreed vmstate item-16
   resolution: (a) keep the option (skip this step), or (b) delete it together
   with the same-commit re-expression of vmstate M4's GIL-off backstop. Never
   drop the backstop assert.

9.2-4/9.2-5 are already live. Verification limits: per the fan-out no-build
rule, 9.2-6/7/8 are context- and signature-verified, not compiler-verified;
first build at INT (see the header preamble).

## Ownership adjudication request (round 2; ORCHESTRATOR action)

A round-2 blocker finding observed that the orchestrator's owned-path globs
for this part (runtime/{JSThread*,ThreadGIL*,JSLockObject*,JSConditionObject*,
JSThreadLocal*,AtomicsObject.cpp}) do not match the file names this
workstream's code actually lives at. The tree follows SPEC-api §9.1 naming —
runtime/{ThreadObject,ThreadManager,ThreadAtomics,ThreadLocalObject,
LockObject,ConditionObject}.{h,cpp} (cell classes JSThread/JSLockObject/
JSConditionObject/JSThreadLocalObject live INSIDE those files; the GIL
helpers GILDroppedSection/GILParkSavedExecutionState/jsThreadGILHandoffYield
live in LockObject.{h,cpp}) — and these names were created by the prep stub
and are already referenced by Sources.txt:795/1004/1135-1138 and
CMakeLists.txt:1209/1561/1779-1782 (9.2-4/9.2-5, live). Tools/threads/
run-tests.sh (task 12) is likewise api-authored but outside the stated globs.

Requested resolution, option (a) of the finding (far cheaper than renaming,
which would force edits to the shared hot files Sources.txt/CMakeLists.txt
and break the live 9.2-4/9.2-5 entries): amend the part's owned-path globs to
`Source/JavaScriptCore/runtime/{ThreadObject*,ThreadManager*,ThreadAtomics*,
ThreadLocalObject*,LockObject*,ConditionObject*,AtomicsObject.cpp}` plus
`Tools/threads/run-tests.sh`, `JSTests/threads/**`, and this file. Until the
orchestrator records that amendment, round-2 fixes were confined to the §9.1
runtime files + JSTests/threads/** + this manifest; Tools/threads/
run-tests.sh was NOT touched at round 2 (its two owed fixes were recorded in
the 9.2-7 caveats).

ROUND-4 STATUS: STILL no recorded amendment (a round-4 finding re-filed the
mismatch, as predicted). This remains a pure ORCHESTRATOR action — no code
change is possible from this side: the file names are frozen into the live
Sources.txt:795/1004/1135-1138 and CMakeLists.txt:1209/1561/1779-1782
entries (shared hot files), so renaming to match the stated globs would
itself violate the hard rules. Round 4, like round 3, proceeded under
de-facto option (a) — the round-4 brief again handed the §9.1 paths as the
review/fix surface. Requested amendment text, verbatim, for the
orchestrator to record: owned globs become
`Source/JavaScriptCore/runtime/{ThreadObject*,ThreadManager*,ThreadAtomics*,
ThreadLocalObject*,LockObject*,ConditionObject*,AtomicsObject.cpp}` plus
`Tools/threads/run-tests.sh`, `JSTests/threads/**`,
`docs/threads/INTEGRATE-api.md`.

ROUND-3 STATUS: still no recorded amendment, but the round-3 task brief
itself (a) handed the §9.1 file paths as this part's review surface with the
instruction to FIX real findings in them, and (b) included two findings
whose only possible fix is editing Tools/threads/run-tests.sh. Round 3
therefore proceeded under de-facto option (a): fixes landed in the §9.1
runtime files, run-tests.sh (both owed fixes applied — see the 9.2-7
caveats), JSTests/threads/**, and this manifest. The explicit glob amendment
is STILL REQUESTED so subsequent rounds stop re-tripping on this; nothing
beyond the option-(a) path set was touched, and no renaming is needed.

## Audit notes (round 2 — adversarial-review fixes)

All in api-owned (§9.1) files; no new shared-hot-file text.

- D6 fix (mutual-exclusion hole): AsyncTicket gained m_grantDelivered
  (ThreadManager.h), set at the top of BOTH settleLockGrant settle tasks
  (LockObject.cpp) under the JSLock; cond.asyncWait's (b) arm treats an
  undelivered m_asyncHolder as not-held => 4.3 TypeError
  (ConditionObject.cpp). New regression block in
  api/condition-async-wait.js (asyncTestStart 3 -> 4).
- Pump VM lifetime: NativeLockState::schedPumpLocked's run-loop dispatch now
  captures Ref<VM> (the D5 pattern; LockObject.cpp). Closes pump-after-VM-
  teardown UAF (pump -> settleLockGrant -> AsyncTicket::settle dereferences
  the ticket's VM) and forestalls DWT-shutdown cancellation racing settle()'s
  isCancelled() check (cancelPendingWork(VM&) runs only during VM teardown,
  which the Ref pins out; independently, DeferredWorkTimer::doWork drops
  cancelled tickets' tasks under the JSLock, so even a lost race is benign).
  AsyncTicket deliberately does NOT hold Ref<VM>: DWT settle-task wrappers
  hold Ref<AsyncTicket>, so a ticket-held Ref<VM> would cycle
  VM -> DWT task -> AsyncTicket -> VM and leak the VM if the run loop stops.
- D7 fix: ReadOnly writability TypeErrors added to
  atomicsCompareExchangeOnProperty and the numeric RMW arm
  (ThreadAtomics.cpp); frozen-object CAS/RMW cases added to
  atomics/property-errors.js.
- asyncJoiners teardown drain: the 5.10 finalizer hook
  (ThreadObject.cpp registerThreadStateFinalizer) now swaps asyncJoiners out
  under joinLock and clears each abandoned ticket's promise Strong (under
  the JSLock); ~ThreadState RELEASE_ASSERTs asyncJoiners empty
  (ThreadManager.h). Closes the off-lock/post-VM Strong destruction at an
  embedder thread's TLS teardown for asyncJoin on never-completing
  ThreadStates. A forever-pending asyncJoin(main Thread.current) still pins
  the shell — that is the documented 4.6.3 addPendingWork liveness behavior,
  same as an un-notified infinite waitAsync (comment at
  threadProtoFuncAsyncJoin).
- Finding refuted (with in-code comment at ThreadManager.cpp
  threadRestrictCheck): "Thread.restrict enforcement absent / land the
  9.2-6 hooks now" — the deferral is the FROZEN spec's own design, not a
  scope decision this workstream may revisit: SPEC-api I14 reads "INT gate
  via 9.2-6; //@ skipped until then" (SPEC-api.md:308), and §9.2's preamble
  makes JSObject.h/JSObjectInlines.h/JSObject.cpp integrator-applied surface
  (they are the objectmodel WS's merge target; the api WS writing them would
  collide with that workstream in-flight). The coverage-grep half of the
  finding is acknowledged and recorded as an owed runner fix (9.2-7
  caveats).

## Audit notes (round 3 — adversarial-review fixes)

All within the de-facto option-(a) path set (§9.1 runtime files,
Tools/threads/run-tests.sh, JSTests/threads/**, this manifest); no new
shared-hot-file text. Per-finding disposition:

1. "cond.wait / join / contended hold park forever with no termination
   polling" — REAL (verified: ConditionObject.cpp parked with
   ParkingLot::Time::infinity(); ThreadObject.cpp used untimed
   joinCondition.wait; LockObject.cpp blocked in m_lock.lock(); none
   VMTraps-wakeable). FIXED as landed deviation D9 (full mechanism and the
   dequeued<=>flipped argument in the D9 entry above). New tests:
   api/condition-wait-termination.js, api/lock-hold-termination.js; new
   regression surface in the existing files is comment-anchored "D9".
2. "D4 GIL-dropped TA waitSync makes vm.syncWaiter() concurrently
   reachable" — REAL for >1 non-spawned thread of one VM (unreachable in
   the jsc shell, so no executable test; the corpus cannot cover it).
   FIXED as landed deviation D8: per-VM single-flight gate in
   AtomicsObject.cpp; second concurrent non-spawned sync TA wait throws
   TypeError instead of corrupting the waiter list. The embedder constraint
   is now recorded (D8 entry) next to D4 for the Bun integration to gate
   on.
3. "sync hold inside an asyncHold-delivered fn self-deadlocks" — REAL.
   FIXED as landed deviation D10 (m_asyncGrantRunner; reviewer's suggested
   shape adopted, including NOT setting m_holder — see the D10 entry for
   the double-unlock hazard that forbids it). lock-async-hold.js gained
   test 8 (sync hold => "Lock is not recursive"; sync cond.wait => 4.3
   TypeError; post-4.3(b)-consumption sync hold legal again). The
   cond.wait-inside-fn TypeError is frozen-4.3-conformant (sync wait
   requires a 5.3 sync hold; asyncWait's (b) arm is the supported path) —
   comment added at the ConditionObject.cpp check site.
4. /6. "code lives outside the declared owned-path globs" — ORCHESTRATOR
   action, still pending; round-3 status recorded in the "Ownership
   adjudication request" section above (de-facto option (a) followed; glob
   amendment still requested).
5. /8. run-tests.sh: "coverage grep counts skipped files" and "plain run
   guaranteed-red on missing vmstate options" — REAL; both FIXED in the
   script (skip-aware coverage grep with the explicit API-I14 deferral
   message; per-run option-set probe => SKIP, cached). `bash -n` clean.
6. (see 4.)
7. "enable-predicate divergence (useThreads alias honored only by api)" —
   REAL as a cross-WS ordering hazard. FIXED by landing the two paired
   api-owned 9.2-1 edits now (ThreadManager.h gate reduction; 16-file
   header migration — entry above updated to LANDED); the alias is dead in
   every apply order and only its OptionsList.h line remains for INT.

Verification limits (no-build rule): all C++ changes are
context/signature-verified only (WTF::Lock::tryLockWithTimeout —
wtf/Lock.h:89; Condition::waitUntil(Lock&, TimeWithDynamicClockType) —
wtf/Condition.h:75; ParkingLot::parkConditionally timed form;
VM::hasTerminationRequest/throwTerminationException — already used by this
workstream's ThreadAtomics.cpp/AtomicsObject.cpp). First compile and first
run of the new termination tests happen at the Verify phase.

## Audit notes (round 4 — adversarial-review fixes)

All within the de-facto option-(a) path set; one new INT-gated shared-file
entry (9.2-9, JSLock). Per-finding disposition:

1. "GILDroppedSection bypasses m_lockDropDepth => drainMicrotasks (user JS)
   runs inside every park-site host call" — REAL, BLOCKER (verified:
   JSLock::unlock -> willReleaseLock drains when `!m_lockDropDepth`,
   JSLock.cpp; dropAllLocks bumps the depth BEFORE unlocking; the
   GILDroppedSection loop never does). The reviewer's "drain explicitly
   before the section" alternative was evaluated and REJECTED — it still
   runs user JS inside the call (the demanded regression test would fail
   either way) and leaves the notify-yield path exposed; the JSLock-level
   suppression is the only sound shape. JSLock.{h,cpp} are outside this
   part's owned paths (the finding itself says to route the hunk through
   this manifest), so the fix lands as: 9.2-9 READY hunk
   (unlockAllForThreadParking — depth bumped AND restored while m_lock is
   still held: no escaped depth, no DAL-protocol race, livelock fix
   preserved) + splice marker in GILDroppedSection's constructor + the
   corrected LockObject.h class comment (the old "only divergence is LIFO
   bookkeeping" claim was wrong, now constraint (3)) + landed deviation
   D11 + regression test api/park-no-microtask-drain.js (//@ skip'ped,
   unskipped by the integrator with 9.2-9 — covers join, cond.wait,
   contended hold, notify yield, property Atomics.wait).
2. "cond.asyncWait (b) lets ANY thread consume another thread's live
   with-fn grant" — REAL (the D6 delivered-gate never checked WHO was
   calling). FIXED as D12: the (b) arm additionally requires
   asyncGrantRunByCurrentThread() for with-fn grants
   (ConditionObject.cpp); no-fn grants stay cross-thread-consumable per
   the frozen "unvalidated consumption" text (escalated for confirmation
   in the D12 entry). New racy regression block in
   api/condition-async-wait.js (asyncTestStart 4 -> 5): fn parks via the
   harness property-wait, a foreign thread's asyncWait must TypeError with
   the lock still held, then E releases normally.
3. "Thread.restrict bypassed for method-table overriders (typed arrays,
   StringObject, arguments, ...)" — REAL (a Float64Array passes
   isExcludedRestrictReceiver yet its element accesses never touch the
   hooked generic paths or the shadow ArrayStorage). FIXED as D13:
   restrictReceiverStaysOnHookedPaths allowlist (JSObject-default enforced
   method-table slots, pointer-compared, plus JSArray exactly), called
   from isExcludedRestrictReceiver => restrict-time TypeError. Verified
   non-regressions: plain objects/arrays (the I14 surface) pass;
   ObjectPrototype has no enforced overrides, so the round-3
   "Object.prototype restrictable" delta stands. thread-restrict.js gained
   the overrider-instance TypeError block (typed arrays incl. BigInt64,
   DataView, String object, both arguments flavors, function, RegExp
   instance); 9.2-6 post-apply note 3 amended (the no-indexed-GET-hook
   argument is now explicitly conditional on this allowlist).
4. "never-notified infinite-timeout property waitAsync leaks Strongs past
   VM teardown" — REAL (notify path and the D5 finite-timeout timer were
   the only clearing points; DWT cancelPendingWork never touches
   AsyncTicket::m_promise; the table is process-global). FIXED with the
   per-waited-cell heap finalizer sweep (PropertyWaiterTable::
   sweepCellAtFinalization + m_sweepFinalizerCells dedupe,
   ThreadAtomics.cpp; public Heap::addFinalizer only, the
   registerThreadStateFinalizer pattern; registered outside the rank-2
   table lock). Mechanism recorded as the ROUND-4 COMPANION paragraph of
   the D5 entry. Like D8, unreachable in the one-VM jsc shell => no
   executable corpus coverage (documented there).
5. "ownership-glob mismatch unadjudicated" — ORCHESTRATOR action, round-4
   status + verbatim requested amendment text recorded in the "Ownership
   adjudication request" section. Not fixable from this side without
   touching shared hot files (renames would churn Sources.txt/
   CMakeLists.txt).

Verification limits (no-build rule, round 4): pointer-comparison of
MethodTable entries verified against ClassInfo.h:47-128 (METHOD_TABLE_ENTRY
ptrauth qualifier applies to both operands; CREATE_METHOD_TABLE resolves
un-overridden &Derived::op to &JSObject::op); JSObject/JSArray
DECLARE_EXPORT_INFO verified (JSObject.h:1050, JSArray.h); Heap::addFinalizer
(JSCell*, LambdaFinalizer) verified (Heap.h:489-492); HashMap::removeIf /
HashSet<JSCell*> standard WTF surface; VM::DrainMicrotaskDelayScope was
evaluated for the D11 fix and rejected (its count-reaches-zero destructor
drains under a fresh JSLockHolder — in-frame again — and holding it across a
park suppresses the SHARED queue for unrelated threads, including explicit
shell drains, which the m_lockDropDepth mechanism does not). First compile
and first run of the new/changed tests happen at the Verify phase.
