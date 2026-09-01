/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ThreadAtomics.h"
#include "LockObject.h"

#include "ArrayStorage.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "JSPromise.h"
#include "ObjectConstructor.h"
#include "SparseArrayValueMap.h"
#include "ThreadManager.h"
#include "ThreadObject.h"
#include "VMLite.h" // UNGIL §J.3 (U-T11): the spawned park lite is the CURRENT lite (TERM1 rule 4).
#include <wtf/HashSet.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/Scope.h>

namespace JSC {

// UNGIL same-library seams (U-T11; the currentThreadHoldsEntryToken pattern —
// redeclared here, not in any header, matching WaiterListManager.cpp): the
// §A.2.4 rule-4 PARK-LITE D9 poll predicates (VMTraps.cpp), the §J.3
// captured-lite record (JSLock.cpp), and the annex-W W1 parked-carrier
// watchdog service episode (JSLock.cpp).
bool parkLitePollTerminationRequested(VM&, VMLite* parkLite);
bool parkLitePollWatchdogCheckRequested(VM&, VMLite* parkLite);
VMLite* capturedParkLiteOfCurrentThreadIfAny(VM&);
bool reacquireParkedCarrierAndServiceWatchdogCheck(VM&);

// ---------------- S2-parallel-cpu-waste instrumentation ----------------
//
// Process-wide property-Atomics RMW retry counters (declared in
// ThreadAtomics.h). Bumped only under Options::logJSLockContention() so the
// off-option / flag-off path is one predicted-not-taken branch. The bench
// (Tools/threads/scalebench/js/bench.js) does Atomics.add(counters, '…', 1)
// on a SINGLE shared object every doc/query; at W=16 that is ~W writers on
// the same inline slot. The CAS-retry burn for that shape lives INSIDE
// atomicSlotLockFreeLoop (ConcurrentButterfly.cpp), which loops internally on
// CAS failure and never escapes Restart — so g_threadAtomicsRMWRestarts here
// captures STRUCTURAL restarts only, and g_threadAtomicsSlotCASRetries is
// the hook for that loop's fall-through-and-retry arm — DEFINED here,
// INCREMENTED THERE (one-liner, owned by the ConcurrentButterfly implementer
// slot; reads 0 until landed). dumpThreadAtomicsRMWStats() is called from
// LockObject.cpp's atexit dump so both halves of the S2 instrumentation
// report together.
std::atomic<uint64_t> g_threadAtomicsRMWCalls { 0 };
std::atomic<uint64_t> g_threadAtomicsRMWRestarts { 0 };
std::atomic<uint64_t> g_threadAtomicsSlotCASRetries { 0 };

void dumpThreadAtomicsRMWStats()
{
    uint64_t calls = g_threadAtomicsRMWCalls.load(std::memory_order_relaxed);
    uint64_t restarts = g_threadAtomicsRMWRestarts.load(std::memory_order_relaxed);
    uint64_t casRetries = g_threadAtomicsSlotCASRetries.load(std::memory_order_relaxed);
    dataLogLn("[logJSLockContention] ThreadAtomics property-RMW: calls=", calls,
        " outerRestarts=", restarts,
        " slotCASRetries=", casRetries,
        casRetries ? "" : " (slotCASRetries hook not yet wired in ConcurrentButterfly.cpp::atomicSlotLockFreeLoop)");
}

// ---------------- own-data-property helpers ----------------

enum class OwnPropertyKind : uint8_t { Missing, Data, Accessor };

#if USE(JSVALUE64)
// I5 fix - SPEC-ungil ANNEX C1 third arm / OM SPEC-objectmodel ANNEX Q (I31):
// flag-on, the quickly-family deliberately answers FALSE for every
// ArrayStorage shape so generic callers fall to the E5 / 4.6 cell-locked
// path. For Atomics property ops that "not lock-free-quickly readable" must
// NOT be conflated with "not a plain data property": an in-vector, non-hole
// AS element with no sparse map is exactly the slot the 9.5 locked AS arm
// (atomicSlotReadModifyWriteAtIndex's hasAnyArrayStorage arm,
// ConcurrentButterfly.cpp) operates on. Probe it under the cell lock,
// mirroring that arm's gates VERBATIM (m_sparseMap || index >= vectorLength
// || index >= length => NotPlain; empty slot => Hole) so a probe that
// classifies Data can never hand the accessor a persistently-Restarting slot
// (livelock). butterfly() is legal here per the 9.5 accessor contract: AS
// never segments (I31), and the shape is re-checked under the lock.
// Read-only - never allocates or parks under the cell lock (OM I20); the SW
// pre-lock protocol is the WRITE side's job and stays inside the 9.5
// accessor. NOTE: this is a cell-lock site OUTSIDE ConcurrentButterfly.cpp,
// so it is unobserved by the TU-private O3 depth witness
// (t_cellLocksHeldByConcurrentButterfly); its obligation (no allocation, no
// park, no safepoint under the lock) must be preserved by future edits.
enum class ArrayStorageElementProbe : uint8_t { NotArrayStorage, Plain, Hole, NotPlain };
static ArrayStorageElementProbe probeArrayStorageElementForAtomics(JSObject* object, uint32_t index, JSValue& value)
{
    ASSERT(Options::useJSThreads());
    if (!hasAnyArrayStorage(object->indexingType()))
        return ArrayStorageElementProbe::NotArrayStorage;
    Locker locker { object->cellLock() }; // I31/L5: every flag-on runtime AS access is cell-locked, reads included.
    if (!hasAnyArrayStorage(object->indexingType())) // Shape moved before the lock landed: caller re-classifies.
        return ArrayStorageElementProbe::NotArrayStorage;
    ArrayStorage* storage = object->butterfly()->arrayStorage();
    if (storage->m_sparseMap || index >= storage->vectorLength() || index >= storage->length())
        return ArrayStorageElementProbe::NotPlain; // Sparse/out-of-bounds: exotic, matches the locked arm's reject.
    JSValue stored = storage->m_vector[index].get();
    if (!stored)
        return ArrayStorageElementProbe::Hole;
    value = stored;
    return ArrayStorageElementProbe::Plain;
}
#endif // USE(JSVALUE64)

// Returns Missing with an exception pending for the two rejected receiver
// classes (see the 4.5 atomicity comment below); every caller
// RETURN_IF_EXCEPTIONs immediately after this call, so a gated receiver can
// never fall into a Missing-create path.
static OwnPropertyKind getOwnPropertyForAtomics(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue& value, unsigned& attributes)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Gate 1: reentrant receivers. After this check, the method-table probe
    // below runs no user JS (other exotic getOwnPropertySlot implementations
    // may reify lazy properties or allocate, but never call out to JS), and
    // atomicsStoreOnProperty's isExtensible() is a plain structure-flag read.
    if (object->type() == ProxyObjectType || object->type() == GlobalProxyType) [[unlikely]] {
        throwTypeError(globalObject, scope, "Atomics property operations cannot be performed on a Proxy"_s);
        return OwnPropertyKind::Missing;
    }

    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    bool hasProperty = object->methodTable()->getOwnPropertySlot(object, globalObject, propertyName, slot);
    RETURN_IF_EXCEPTION(scope, OwnPropertyKind::Missing);
    if (!hasProperty)
        return OwnPropertyKind::Missing;
    attributes = slot.attributes();
    if (!slot.isValue())
        return OwnPropertyKind::Accessor;

    // Gate 2: the own data property must be plain structure/butterfly
    // storage, so the read here and the later putDirect/putDirectIndex hit
    // the SAME slot. The probe above may have reified a lazy property (e.g.
    // function name/length), so the re-validation runs after it on purpose.
    if (std::optional<uint32_t> index = parseIndex(propertyName)) {
        if (!object->canGetIndexQuickly(index.value())) [[unlikely]] {
#if USE(JSVALUE64)
            // I5 fix: flag-on, AS shapes answer "not quickly" BY DESIGN (OM
            // ANNEX Q/I31) so generic callers fall to cell-locked access -
            // that is not "not a plain data property". Probe the locked AS
            // arm's slot; under the GIL the read below and the later
            // putDirectIndex (which routes through the I31-locked generic
            // path flag-on) remain one atomic step. The Hole arm is
            // unreachable here (no GIL drop between getOwnPropertySlot and
            // this probe), so any non-Plain result falls to the TypeError.
            if (Options::useJSThreads()) {
                JSValue stored;
                if (probeArrayStorageElementForAtomics(object, index.value(), stored) == ArrayStorageElementProbe::Plain) {
                    value = stored;
                    attributes = 0; // In-vector AS elements are writable/enumerable/configurable.
                    return OwnPropertyKind::Data;
                }
            }
#endif
            throwTypeError(globalObject, scope, "Atomics property operations require a plain data property"_s);
            return OwnPropertyKind::Missing;
        }
        value = object->getIndexQuickly(index.value());
        attributes = 0; // Butterfly elements are writable/enumerable/configurable.
        return OwnPropertyKind::Data;
    }
    unsigned structureAttributes = 0;
    PropertyOffset offset = object->structure()->get(vm, propertyName, structureAttributes);
    if (!isValidOffset(offset)) [[unlikely]] {
        throwTypeError(globalObject, scope, "Atomics property operations require a plain data property"_s);
        return OwnPropertyKind::Missing;
    }
    attributes = structureAttributes;
    value = object->getDirect(offset);
    return OwnPropertyKind::Data;
}

static bool sameValueZeroForAtomics(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    if (a.isNumber() && b.isNumber()) {
        double x = a.asNumber();
        double y = b.asNumber();
        if (std::isnan(x) && std::isnan(y))
            return true;
        return x == y;
    }
    return sameValue(globalObject, a, b);
}

// SPEC-api 4.5: every property op is "one atomic step". Under the phase-1 GIL
// that holds only if NO user JS can run between the own-property read and the
// write below (operand coercions happen before the read). Two receiver
// classes would break that mechanically, so getOwnPropertyForAtomics rejects
// them with TypeError up front (landed deviation; rationale recorded in
// docs/threads/INTEGRATE-api.md "Landed deviations" — the frozen 4.5 table
// does not enumerate exotic receivers):
//
// 1. Reentrant receivers — ProxyObject / JSGlobalProxy: their
//    getOwnPropertySlot (and isExtensible) run arbitrary trap JS, which can
//    reach a GIL-dropping park site (join, cond.wait, contended lock.hold,
//    property Atomics.wait) mid-step; another thread could then mutate the
//    property between a CAS/RMW's read and its write — a cross-thread TOCTOU
//    that would falsify the advertised CAS atomicity.
//
// 2. Exotic own data properties not backed by plain structure/butterfly
//    storage — e.g. JSArray "length", RegExpObject "lastIndex", StringObject
//    indexed chars, sparse-map indices, global var-scope bindings: the method
//    table reports them as own data properties, but putDirect/putDirectIndex
//    would install a DUPLICATE shadow property next to the exotic one (an
//    object state no sequential JS program can create, violating THREAD.md's
//    indistinguishable-heap requirement).
//
// After these gates the read is a non-reentrant structure/butterfly probe and
// the write targets exactly the probed slot. Post-GIL these bodies re-home
// onto the object-model atomic slot CAS/RMW helpers (OM §9.5) per Deviation
// 12; the §7 signatures are frozen so only the bodies change.
// THREADS-INTEGRATE(api): Dev 12 re-freeze point (atomic slot CAS/RMW).
// THREADS-INTEGRATE(ungil): U-T10 LANDED the re-home - GIL-off the four value
// ops dispatch to the *GilOff bodies below (SPEC-ungil ANNEX C1 accessors in
// ConcurrentButterfly.cpp); GIL-on keeps the bodies in this section verbatim.

// Writes an EXISTING own data property's value, preserving its attributes.
// putDirect/putDirectMayBeIndex default to attributes 0, and putDirectInternal
// (PutModeDefineOwnProperty) performs an attribute-change Structure transition
// whenever newAttributes != currentAttributes — which would silently strip
// DontEnum/DontDelete/ReadOnly. 4.5 ops only ever change the value.
static void putExistingOwnDataPropertyForAtomics(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue value, unsigned attributes)
{
    unsigned preservedAttributes = attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    if (std::optional<uint32_t> index = parseIndex(propertyName)) {
        object->putDirectIndex(globalObject, index.value(), value, preservedAttributes, PutDirectIndexLikePutDirect);
        return;
    }
    object->putDirect(globalObject->vm(), propertyName, value, preservedAttributes);
}

#if USE(JSVALUE64)

// ---------------- GIL-off re-home (SPEC-ungil §C.1/§C.2, U-T10) ----------------
//
// GIL-off (vm.gilOff()), the VM lock no longer serializes mutators, so the
// "one atomic step" bodies above are re-homed onto the OM §9.5 atomic slot
// accessors (ANNEX C1): probe -> accessor -> status dispatch, looping on
// Restart (the accessor re-validates structure/shape/offset provenance and
// runs the write-side SW protocols internally; restarts are I33-bounded by
// the forward-only shape order, plus the same adversarial-progress caveat as
// the §C.3(b) dequeue-and-restart class). CARRIED across the re-home (§C.2):
//   - D3 exotic-receiver TypeErrors: the probe below keeps the GIL probe's
//     Proxy/GlobalProxy gate and plain-slot gates, messages identical;
//   - D7 writability inside the atomic body: probe-time ReadOnly TypeErrors
//     here, re-validated in the accessor's locked arm (Restart on mismatch -
//     the fresh probe then throws).
// GIL-on (and flag-off) keeps the bodies above byte-for-byte: every gilOff
// branch below is unreachable there (U19 oracle; SD4-class deltas are owned
// by U-T11, not this file's value ops).
//
// Store's Missing arm stays on the OM's generic ADD machinery - a fresh-
// property ADD is a structure/shape transition and §9.5 accessors only cover
// EXISTING slots - but BOTH legs are conditional adds now
// (putDirectForAtomicsMissingAdd / putDirectIndexForAtomicsMissingAdd): a
// lost race restarts the probe instead of clobbering a racer's descriptor.

struct ConcurrentAtomicsProbe {
    OwnPropertyKind kind { OwnPropertyKind::Missing };
    unsigned attributes { 0 };
    std::optional<uint32_t> index;
    PropertyOffset offset { invalidOffset };
    StructureID structureID;
    // I5 fix amendment: getOwnPropertySlot saw a value but the cell-locked AS
    // probe then saw a hole (racing delete/shrink). No sequential
    // interleaving yields a "plain data property" TypeError there - the
    // caller's outer loop must re-probe, and the fresh getOwnPropertySlot
    // classifies Missing (store ADDS, load throws its precise "no own
    // property" error). Progress: each restart requires an external mutation
    // in the window - the same bounded-adversarial class as C.3(b).
    bool restart { false };
};

// GIL-off twin of getOwnPropertyForAtomics: same gates, same TypeError
// messages, but resolves the named offset with the lock-free walker
// (Structure::getConcurrently - Structure::get(VM&) may materialize the
// property table, a GIL-on-only luxury) and records {offset, structureID}
// provenance for the accessor's I34 validation instead of reading the value
// here (the accessor's seq_cst load is the read that counts).
static ConcurrentAtomicsProbe probeOwnPropertyForAtomicsConcurrent(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ConcurrentAtomicsProbe probe;

    // Gate 1 (D3): reentrant receivers.
    if (object->type() == ProxyObjectType || object->type() == GlobalProxyType) [[unlikely]] {
        throwTypeError(globalObject, scope, "Atomics property operations cannot be performed on a Proxy"_s);
        return probe;
    }
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    bool hasProperty = object->methodTable()->getOwnPropertySlot(object, globalObject, propertyName, slot);
    RETURN_IF_EXCEPTION(scope, probe);
    if (!hasProperty)
        return probe;
    probe.attributes = slot.attributes();
    if (!slot.isValue()) {
        probe.kind = OwnPropertyKind::Accessor;
        return probe;
    }
    // Gate 2 (D3): plain structure/butterfly storage only. §C.2 routes
    // parseIndex hits to the indexed accessor (one arm per shape, 8g).
    if (std::optional<uint32_t> index = parseIndex(propertyName)) {
        if (!object->canGetIndexQuickly(index.value())) [[unlikely]] {
            // I5 fix (ANNEX C1: "ArrayStorage/dict-indexed - third arm; C.2
            // routes parseIndex hits here"): classify the locked AS arm's
            // slots as Data so dispatchAtomicSlotRequest reaches
            // atomicSlot*AtIndex's cell-locked arm. The probe mirrors that
            // arm's gates EXACTLY (sparse map / out-of-bounds => TypeError
            // here; hole => restart, see the struct comment), so kind=Data
            // can never feed the accessor a persistently-Restarting slot.
            // Race shape: this probe and the accessor take the cell lock
            // SEPARATELY - if thread B mutates between them (shrink, delete,
            // sparse conversion, AS->flat shape move), the accessor's
            // under-lock re-checks report Restart and the NEXT probe
            // re-classifies on fresh state (succeeds or throws); an
            // adversarial add/delete flip-flop is the same
            // bounded-adversarial-progress class as C.3(b)'s
            // dequeue-and-restart. The write-side SW=0 foreign pre-lock
            // protocol stays inside the accessor (AS PRE-LOCK, r8 item 6).
            JSValue ignoredValue;
            switch (probeArrayStorageElementForAtomics(object, index.value(), ignoredValue)) {
            case ArrayStorageElementProbe::Plain:
                probe.index = index;
                probe.attributes = 0; // In-vector AS elements are writable/enumerable/configurable.
                probe.kind = OwnPropertyKind::Data;
                return probe;
            case ArrayStorageElementProbe::Hole:
                probe.restart = true;
                return probe;
            case ArrayStorageElementProbe::NotArrayStorage:
            case ArrayStorageElementProbe::NotPlain:
                break;
            }
            throwTypeError(globalObject, scope, "Atomics property operations require a plain data property"_s);
            return probe;
        }
        probe.index = index;
        probe.attributes = 0; // Butterfly elements are writable/enumerable/configurable.
        probe.kind = OwnPropertyKind::Data;
        return probe;
    }
    Structure* structure = object->structure();
    unsigned structureAttributes = 0;
    PropertyOffset offset = structure->getConcurrently(propertyName.uid(), structureAttributes);
    if (!isValidOffset(offset)) [[unlikely]] {
        throwTypeError(globalObject, scope, "Atomics property operations require a plain data property"_s);
        return probe;
    }
    // U-T10 amend: the accessor-ness decision above was made against the
    // structure the methodTable walk saw; THIS structure (re-read after it)
    // is the one whose {offset, structureID} provenance the §9.5 accessor
    // validates (I34). A racing data->accessor reconfiguration between the
    // two reads would otherwise hand the LOCK-FREE arm a kind=Data probe
    // whose structureID check PASSES while the slot holds a GetterSetter -
    // an Exchange/RMW would CAS a primitive over it (type confusion). Reject
    // non-plain attributes against the SAME structure the provenance is
    // taken from, mirroring the third arm's under-lock re-check; this also
    // keeps CustomValue slots (which can answer slot.isValue()) out of the
    // lock-free arms, per ANNEX C1's "plain ... own NAMED data slots only".
    if (structureAttributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue)) [[unlikely]] {
        probe.attributes = structureAttributes;
        probe.kind = OwnPropertyKind::Accessor;
        return probe;
    }
    probe.attributes = structureAttributes;
    probe.offset = offset;
    probe.structureID = structure->id();
    probe.kind = OwnPropertyKind::Data;
    return probe;
}

static ALWAYS_INLINE JSValue dispatchAtomicSlotRequest(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, const ConcurrentAtomicsProbe& probe, const AtomicSlotRequest& request, AtomicSlotStatus& status)
{
    if (probe.index)
        return object->atomicSlotReadModifyWriteAtIndex(globalObject, probe.index.value(), request, status);
    return object->atomicSlotReadModifyWrite(globalObject, propertyName.uid(), probe.offset, probe.structureID, request, status);
}

static JSValue atomicsLoadOnPropertyGilOff(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    while (true) {
        auto probe = probeOwnPropertyForAtomicsConcurrent(globalObject, object, propertyName);
        RETURN_IF_EXCEPTION(scope, { });
        if (probe.restart) [[unlikely]] // I5 fix amendment: racing delete/shrink made a hole; re-probe on fresh state.
            continue;
        if (probe.kind != OwnPropertyKind::Data) {
            throwTypeError(globalObject, scope, "Atomics.load: object has no own property"_s);
            return { };
        }
        AtomicSlotRequest request; // operation = Load
        AtomicSlotStatus status = AtomicSlotStatus::Restart;
        JSValue result = dispatchAtomicSlotRequest(globalObject, object, propertyName, probe, request, status);
        if (status == AtomicSlotStatus::Applied)
            return result;
        ASSERT(status == AtomicSlotStatus::Restart);
    }
}

// ---------------- §C.2 Missing arm, INDEXED conditional add ----------------
//
// Receiver gate: the fast-put classes whose attribute-0 indexed adds the
// loop below models. Replica of JSObject.cpp's TU-private
// canDoFastPutDirectIndex, with two deliberate deltas:
//   - NO arguments-type admission (review round 2 (b)): Direct/Scoped
//     Arguments index their own mapped storage, not the shapes the loop
//     models; they keep the generic path (today's behavior, no new risk).
//   - NO inSparseIndexingMode()/CoW exclusion: a sparse-mode receiver's map
//     is exactly what the conditional add handles (the generic
//     defineOwnProperty leg it would otherwise take re-opens the descriptor
//     clobber through defineOwnIndexedProperty's reconfiguration arm), and
//     CoW words are materialized by the loop itself.
static bool canUseConditionalIndexedMissingAdd(JSObject* object)
{
    return (isJSArray(object) || is<JSFinalObject>(object)) && !TypeInfo::isArgumentsType(object->type());
}

// U-T10 amend: the INDEXED twin of putDirectForAtomicsMissingAdd - closes the
// KNOWN RESIDUAL previously recorded at the Missing-arm call site below
// (INTEGRATE-ungil, U-T10 amend item 3). The old leg called putDirectIndex
// verbatim; its ArrayStorage terminal (SparseArrayValueMap::putDirect,
// LikePutDirect) force-sets an EXISTING sparse entry to a plain attributes-0
// data property, silently clobbering a racing indexed defineProperty's
// accessor / non-writable descriptor (MC-PRIM P4 / MC-REENT S3c).
//
// Contract (same as the named helper): null on success; non-null = LOST RACE
// - the caller restarts, and the fresh probe re-classifies on settled state
// (Accessor / non-plain => the precise D3/D7 TypeError; plain data => the
// value-only Exchange leg). Unlike the named helper this one CAN throw
// (exotic-receiver generic fallback, conversion/map-allocation OOM), so the
// caller must RETURN_IF_EXCEPTION before testing the returned error.
//
// Publication protocol (review round 1 amendment (c)): the sparse-map
// conditional add and the value publish run in ONE critical section under
// the OBJECT's cellLock - the same lock defineOwnIndexedProperty holds
// around ITS map->add. With add and publish atomic w.r.t. define's add,
// isNewEntry alone decides the winner:
//   - we win the add: define's later add returns !isNewEntry and takes its
//     reconfiguration path against our already-published plain entry -
//     linearizes as store-then-define. No define-side descriptor write can
//     interleave between our add and our publish: BOTH putIndexedDescriptor
//     sites (defineOwnIndexedProperty's new-entry arm and its
//     reconfiguration arm) are sequenced after define's object-cellLocked
//     add, which our window excludes;
//   - define wins the add: our add returns !isNewEntry and we write NOTHING
//     - the restart's fresh probe sees the settled descriptor and throws the
//     precise TypeError (or Exchange-legs a plain data racer).
// Unlocked attribute-0 value writers (putEntry/putDirect reached from plain
// JS puts, which never carry descriptors on these receiver classes - every
// descriptor write on a fast-put receiver routes through
// defineOwnIndexedProperty's object-cellLocked add) can interleave with the
// publish; absorbing one is a value-only overwrite of a plain attributes-0
// entry under the MAP's lock and linearizes as their-put-then-our-store.
// The publish terminal is the map's locked putDirect: it re-checks ReadOnly
// under the map's cellLock and force-sets the value - on OUR fresh
// attributes-0 entry (reconfiguration excluded above) that is a plain value
// publish, never a descriptor change.
//
// Loop shape (every arm either finishes, returns lost-race, or makes the
// shape strictly more settled before re-dispatching - no shape can spin):
//   1. dense stores via trySetIndexQuickly (flag-on it self-dispatches to
//      trySetIndexQuicklyConcurrent; AS and Undecided answer false by
//      design, §Q/I31, so no sparse map is ever consulted here);
//   2. CoW materializes (§4.8 cell-locked materializer, concurrent-correct
//      flag-on) and re-dispatches;
//   3. AS in-vector: locked fill - the same one-locked-window store the
//      I31-routed in-vector arm of
//      putDirectIndexBeyondVectorLengthWithArrayStorage uses;
//   4. AS beyond-vector, no map: dense-enough indexes grow the vector
//      OUTSIDE the lock and re-dispatch into arm 3; sparse-worthy indexes
//      (or growth failure) allocate a map OUTSIDE the lock and install it
//      install-if-absent under the lock (allocateSparseIndexMap
//      unconditionally REPLACES m_sparseMap, which would orphan a racing
//      define's whole map - never used here);
//   5. AS with map: the conditional add + publish described above, then the
//      same I21 map-identity revalidation as JSObject.cpp's flag-on
//      map->putDirect sites (a racing map replacement orphans the entry =>
//      lost race; the restart re-derives and re-stores on the live map);
//   6. non-AS shapes the quick store rejected (beyond-vector dense, blank
//      sparse-worthy/slow-put) convert to ArrayStorage and re-dispatch into
//      arms 3-5. Deliberately NOT putByIndexBeyondVectorLengthWithoutAttributes:
//      its sparse terminal is putEntry, which CALLS a racing accessor's
//      setter - wrong for Atomics.store, which must restart and throw. The
//      AS arm's conditional add is the only sparse terminal this helper
//      permits. Shape conservatism is deliberate and JS-unobservable: a
//      beyond-vector Atomics.store add takes ArrayStorage rather than
//      growing the dense shape (GIL-on/flag-off paths untouched).
//
// Known residual (narrowed, recorded): a racing preventExtensions can still
// be overtaken between the post-add isStructureExtensible re-check and the
// publish - the named leg closes this exactly via the E4 structureID CAS;
// the indexed sparse add has no structure CAS to hang the re-check on. The
// window is one lock-internal interval (was: the whole probe->putDirectIndex
// span), and the failure mode is an extra plain property on a freshly
// non-extensible object - the same state the pre-fix code produced, never a
// descriptor clobber. Mirrors defineOwnIndexedProperty's own post-add
// re-check discipline.
ASCIILiteral JSObject::putDirectIndexForAtomicsMissingAdd(JSGlobalObject* globalObject, uint32_t i, JSValue value)
{
    ASSERT(Options::useJSThreads());
    ASSERT(i <= MAX_ARRAY_INDEX); // parseIndex never yields larger.
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!canUseConditionalIndexedMissingAdd(this)) {
        // Exotic receivers (typed arrays, StringObject, arguments, ...) keep
        // today's generic path: their per-class defineOwnProperty runs,
        // exactly as GIL-on; the sparse-map force-set terminal is not the
        // shape they reach.
        putDirectIndex(globalObject, i, value);
        RETURN_IF_EXCEPTION(scope, ASCIILiteral { });
        return { };
    }

    SparseArrayValueMap* pendingMap = nullptr; // GC-visible via conservative stack scan.
    while (true) {
        // (1) Plain dense store on a fast flat/segmented word.
        if (trySetIndexQuickly(vm, i, value))
            return { };

        if (isCopyOnWrite(indexingMode())) [[unlikely]] {
            convertFromCopyOnWrite(vm); // §4.8: routes through the cell-locked materializer.
            continue;
        }

        IndexingType type = indexingType();

        if (hasAnyArrayStorage(type)) {
            {
                Locker locker { cellLock() };
                ArrayStorage* storage = arrayStorageOrNull();
                if (!storage)
                    continue; // Shape moved before the lock landed: re-dispatch.
                if (i < storage->vectorLength()) {
                    // (3) Locked in-vector fill (handles holes; bumps
                    // m_numValuesInVector and length itself).
                    //
                    // Map-governance gate (MC-REENT S3c close-out): the
                    // in-vector arm must not bypass a sparse map that governs
                    // this index. A racing indexed define can interleave with
                    // a foreign generic-path vector grow as
                    //   define: createArrayStorage (no map yet)
                    //   peer:   increaseVectorLength(i+1) (locked AS-COPY)
                    //   define: allocateSparseIndexMap + setSparseMode +
                    //           cellLocked add(i) + descriptor publish
                    //   us:     i < vectorLength -> unconditional vector fill
                    // leaving a non-empty vector slot that SHADOWS the map's
                    // accessor/non-writable descriptor (OM I31: in sparse
                    // mode the vector must stay hole-only; a map entry for a
                    // sub-vectorLength index is define's, never ours). Both
                    // probes are safe here: define's map->add runs under this
                    // same object cellLock, and contains() self-locks the
                    // map's cell lock — the same map-under-object order arm
                    // (5)'s map->putDirect terminal already uses.
                    SparseArrayValueMap* governingMap = storage->m_sparseMap.get();
                    if (governingMap && (governingMap->sparseMode() || governingMap->contains(i))) [[unlikely]] {
                        if (governingMap->contains(i))
                            return "lost indexed-add race (existing sparse entry)"_s; // Restart reclassifies on the settled descriptor.
                        // Sparse mode, no entry for i (reachable stably when
                        // a foreign grow raced the mode flip): the map is
                        // the only legal terminal — fall through to arm (5)'s
                        // conditional add below instead of returning
                        // lost-race, which would livelock (the re-probe still
                        // answers Missing on this settled state).
                    } else {
                        setIndexQuicklyForArrayStorageIndexingType(vm, i, value);
                        return { };
                    }
                }
                if (i >= storage->length())
                    storage->setLength(i + 1); // LikePutDirect semantics: no length-writability gate.
                SparseArrayValueMap* map = storage->m_sparseMap.get();
                if (!map) {
                    if (pendingMap) {
                        // (4) Install-if-absent under the cell lock.
                        storage->m_sparseMap.set(vm, this, pendingMap);
                        map = pendingMap;
                        pendingMap = nullptr;
                    }
                    // else: fall out to allocate a map outside the lock.
                    //
                    // NO dense-growth (increaseVectorLength) arm here, by
                    // ruling on TSAN family growarrayright-vs-sparsemap-atomics
                    // (OM §4 — NOT blessed; CVE map mc-reent-store-missing-
                    // indexed-define-race). The previous mirror of
                    // putDirectIndexBeyondVectorLengthWithArrayStorage's !map
                    // dense-growth leg dropped this lock and called
                    // increaseVectorLength, whose growArrayRight memcpy plain-
                    // reads the WHOLE old butterfly (including the
                    // ArrayStorage::m_sparseMap header word) under our
                    // cellLock. defineOwnIndexedProperty's blank-shape arm of
                    // ensureArrayStorageExistsAndEnterDictionaryIndexingMode
                    // does createArrayStorage(0,0) then allocateSparseIndexMap
                    // — and that m_sparseMap.set runs WITHOUT the cellLock, so
                    // increaseVectorLength's locked sparse-mode re-check cannot
                    // synchronize with it: the memcpy and the storeCell race on
                    // the same word (UB), and our AS-COPY publish can drop
                    // define's freshly-installed map. Removing the grow arm
                    // leaves arm-(4)'s locked, atomic m_sparseMap.set as the
                    // ONLY butterfly-header write this function performs in the
                    // AS arm; define's later cellLocked re-read + pendingMap
                    // re-derive + heal loop linearize against it via this same
                    // lock. Density is a non-goal on the Atomics.store
                    // indexed-miss slow path; a foreign generic putter that
                    // wants vector growth still grows via the §4.6-locked
                    // JSObject.cpp paths, and arm-(3)'s in-vector fill above
                    // services any such peer-grown vector.
                }
                if (map) {
                    // (5) Conditional sparse add + publish, ONE object-cellLock
                    // window (see the protocol comment above). map->add only
                    // fastMallocs (no GC allocation, no JS), so it is
                    // wrappable under the cell lock (O1) - the exact call
                    // defineOwnIndexedProperty makes under this same lock.
                    SparseArrayValueMap::AddResult result = map->add(this, i);
                    if (!result.isNewEntry)
                        return "lost indexed-add race (existing sparse entry)"_s; // NEVER write a foreign entry.
                    if (!isStructureExtensible()) [[unlikely]] {
                        // Same post-add re-check + remove defineOwnIndexedProperty
                        // performs; remove by KEY - the AddResult iterator can
                        // dangle across the map's internal unlock (AB18-G).
                        map->remove(i);
                        return "lost indexed-add race (became non-extensible)"_s;
                    }
                    bool ok = map->putDirect(globalObject, this, i, value, 0, PutDirectIndexLikePutDirect);
                    RETURN_IF_EXCEPTION(scope, ASCIILiteral { });
                    if (!ok) [[unlikely]]
                        return "lost indexed-publish race"_s; // Entry went ReadOnly/removed under the map lock.
                    // I21 map-identity revalidation, same discipline as the
                    // flag-on map->putDirect sites in JSObject.cpp.
                    ArrayStorage* freshStorage = arrayStorageOrNull();
                    if (!freshStorage || freshStorage->m_sparseMap.get() != map) [[unlikely]]
                        return "lost indexed-publish race (sparse map replaced)"_s;
                    return { };
                }
            }
            pendingMap = SparseArrayValueMap::create(vm); // GC allocation: never under the cell lock (I20/O1).
            continue;
        }

        if (hasUndecided(type)) {
            convertUndecidedForValue(vm, value);
            continue;
        }

        if (!hasIndexedProperties(type)
            && !indexingShouldBeSparse() && !needsSlowPutIndexing()
            && !indexIsSufficientlyBeyondLengthForSparseMap(i, 0) && i < MIN_SPARSE_ARRAY_INDEX) {
            // Blank, vector-worthy index: dense first install, ordered as the
            // generic blank arm orders it (N3 loser re-dispatches: a racer
            // installed first, so the shape moved).
            if (tryCreateInitialForValueAndSetConcurrent(vm, i, value))
                return { };
            continue;
        }

        // (6) Everything else: take ArrayStorage and re-dispatch into the AS
        // arm. ensureArrayStorageSlow is the §4.6 stop-routed flag-on
        // converter and self-handles a racing AS install (loser leg).
        ArrayStorage* storage = indexingShouldBeSparse()
            ? ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm)
            : ensureArrayStorage(vm);
        if (!storage) [[unlikely]] {
            // hijacksIndexingHeader receivers cannot take AS: generic path,
            // as GIL-on.
            putDirectIndex(globalObject, i, value);
            RETURN_IF_EXCEPTION(scope, ASCIILiteral { });
            return { };
        }
        continue;
    }
}

static JSValue atomicsStoreOnPropertyGilOff(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    while (true) {
        auto probe = probeOwnPropertyForAtomicsConcurrent(globalObject, object, propertyName);
        RETURN_IF_EXCEPTION(scope, { });
        if (probe.restart) [[unlikely]] // I5 fix amendment: racing delete/shrink made a hole; re-probe on fresh state.
            continue;
        switch (probe.kind) {
        case OwnPropertyKind::Accessor:
            throwTypeError(globalObject, scope, "Atomics.store: property is an accessor"_s);
            return { };
        case OwnPropertyKind::Data: {
            if (probe.attributes & PropertyAttribute::ReadOnly) {
                throwTypeError(globalObject, scope, "Atomics.store: property is not writable"_s);
                return { };
            }
            // Existing slot: a §9.5 Exchange (value-only - no putDirect, no
            // attribute-changing transition; result discarded).
            AtomicSlotRequest request;
            request.operation = AtomicSlotOperation::Exchange;
            request.replacement = value;
            AtomicSlotStatus status = AtomicSlotStatus::Restart;
            dispatchAtomicSlotRequest(globalObject, object, propertyName, probe, request, status);
            if (status == AtomicSlotStatus::Applied)
                return value;
            ASSERT(status == AtomicSlotStatus::Restart);
            continue;
        }
        case OwnPropertyKind::Missing: {
            bool extensible = object->isExtensible(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            if (!extensible) {
                throwTypeError(globalObject, scope, "Atomics.store: cannot add a property to a non-extensible object"_s);
                return { };
            }
            if (std::optional<uint32_t> index = parseIndex(propertyName)) {
                // Fresh INDEXED element: conditional add (the indexed twin of
                // the named leg below) - closes the formerly-recorded KNOWN
                // RESIDUAL (INTEGRATE-ungil, U-T10 amend item 3). A non-null
                // error means we LOST a race with a concurrent indexed
                // define/remove/reshape: restart the probe, which
                // re-classifies on settled state (accessor / non-writable =>
                // the precise TypeError above; plain data => the value-only
                // Exchange leg). Exception checked FIRST: unlike the named
                // helper, the indexed one can throw (exotic-receiver generic
                // fallback, conversion/map-allocation OOM).
                ASCIILiteral error = object->putDirectIndexForAtomicsMissingAdd(globalObject, index.value(), value);
                RETURN_IF_EXCEPTION(scope, { });
                if (!error.isNull()) [[unlikely]]
                    continue;
                return value;
            }
            // Fresh NAMED data property (writable/enumerable/configurable).
            // U-T10 amend: NOT putDirect (define-own semantics) - GIL-off,
            // a key that materialized between the Missing probe and the put
            // would be attribute-clobbered (a racing accessor replaced, a
            // racing ReadOnly stripped, via the attribute-change transition
            // to attributes 0), and a racing preventExtensions could be
            // overtaken - none of which any sequential interleaving of
            // Atomics.store can produce. The conditional add re-derives
            // existence/extensibility inside the OM's E4-published §2 loop:
            // a non-null error means we LOST such a race - restart, and the
            // fresh probe re-classifies and throws the precise D3/D7/
            // non-extensible TypeError. A racing plain writable data add is
            // absorbed as a value-only replace (attributes preserved), which
            // linearizes as define-then-store.
            PutPropertySlot addSlot(object, true);
            ASCIILiteral error = object->putDirectForAtomicsMissingAdd(vm, propertyName, value, addSlot);
            if (!error.isNull()) [[unlikely]]
                continue;
            return value;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static JSValue atomicsCompareExchangeOnPropertyGilOff(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue expected, JSValue replacement)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const bool logContention = Options::logJSLockContention();
    if (logContention) [[unlikely]]
        g_threadAtomicsRMWCalls.fetch_add(1, std::memory_order_relaxed);
    while (true) {
        auto probe = probeOwnPropertyForAtomicsConcurrent(globalObject, object, propertyName);
        RETURN_IF_EXCEPTION(scope, { });
        if (probe.restart) [[unlikely]] // I5 fix amendment: racing delete/shrink made a hole; re-probe on fresh state.
            continue;
        if (probe.kind != OwnPropertyKind::Data) {
            throwTypeError(globalObject, scope, "Atomics.compareExchange: object has no own data property"_s);
            return { };
        }
        // D7 (see the GIL body's rationale): thrown unconditionally, not only
        // when SVZ matches.
        if (probe.attributes & PropertyAttribute::ReadOnly) {
            throwTypeError(globalObject, scope, "Atomics.compareExchange: property is not writable"_s);
            return { };
        }
        AtomicSlotStatus status = AtomicSlotStatus::Restart;
        JSValue current;
        if (probe.index)
            current = object->atomicSlotCompareExchangeAtIndex(globalObject, probe.index.value(), expected, replacement, status);
        else
            current = object->atomicSlotCompareExchange(globalObject, propertyName.uid(), probe.offset, probe.structureID, expected, replacement, status);
        switch (status) {
        case AtomicSlotStatus::Applied:
        case AtomicSlotStatus::NotEqual:
            return current; // SVZ semantics: returns the value READ either way.
        case AtomicSlotStatus::NeedsStringResolution: {
            // Resolve the rope(s) OUTSIDE any lock (§N.2 single-flight; may
            // allocate and throw OOM), then re-probe. Resolution rewrites the
            // rope in place, so an unchanged slot makes progress next pass;
            // a storm of fresh rope stores re-enters here - the same
            // adversarial-progress class as §C.3(b)'s dequeue-and-restart.
            if (expected.isString()) {
                auto resolvedExpected = asString(expected)->value(globalObject);
                RETURN_IF_EXCEPTION(scope, { });
                UNUSED_VARIABLE(resolvedExpected);
            }
            if (current.isString()) {
                auto resolvedCurrent = asString(current)->value(globalObject);
                RETURN_IF_EXCEPTION(scope, { });
                UNUSED_VARIABLE(resolvedCurrent);
            }
            if (logContention) [[unlikely]]
                g_threadAtomicsRMWRestarts.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        case AtomicSlotStatus::Restart:
            if (logContention) [[unlikely]]
                g_threadAtomicsRMWRestarts.fetch_add(1, std::memory_order_relaxed);
            continue;
        case AtomicSlotStatus::NotNumber:
        case AtomicSlotStatus::LockedRevalidate: // Accessor-internal; never escapes.
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static JSValue atomicsRMWOnPropertyGilOff(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, AtomicsRMWOp op, JSValue operand)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Operand coercions first (may run JS), exactly as the GIL bodies order
    // them; the atomic step is the accessor call below.
    AtomicSlotRequest request;
    switch (op) {
    case AtomicsRMWOp::Exchange:
        request.operation = AtomicSlotOperation::Exchange;
        request.replacement = operand;
        break;
    case AtomicsRMWOp::Add:
    case AtomicsRMWOp::Sub:
        request.operation = op == AtomicsRMWOp::Add ? AtomicSlotOperation::Add : AtomicSlotOperation::Sub;
        request.operandNumber = operand.toNumber(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        break;
    case AtomicsRMWOp::And:
    case AtomicsRMWOp::Or:
    case AtomicsRMWOp::Xor:
        request.operation = op == AtomicsRMWOp::And ? AtomicSlotOperation::And
            : op == AtomicsRMWOp::Or ? AtomicSlotOperation::Or : AtomicSlotOperation::Xor;
        request.operandInt = operand.toInt32(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        break;
    }

    bool isExchange = op == AtomicsRMWOp::Exchange;
    const bool logContention = Options::logJSLockContention();
    if (logContention) [[unlikely]]
        g_threadAtomicsRMWCalls.fetch_add(1, std::memory_order_relaxed);
    while (true) {
        auto probe = probeOwnPropertyForAtomicsConcurrent(globalObject, object, propertyName);
        RETURN_IF_EXCEPTION(scope, { });
        if (probe.restart) [[unlikely]] // I5 fix amendment: racing delete/shrink made a hole; re-probe on fresh state.
            continue;
        if (probe.kind != OwnPropertyKind::Data) {
            throwTypeError(globalObject, scope, isExchange ? "Atomics.exchange: object has no own data property"_s : "Atomics RMW: object has no own data property"_s);
            return { };
        }
        if (probe.attributes & PropertyAttribute::ReadOnly) {
            throwTypeError(globalObject, scope, isExchange ? "Atomics.exchange: property is not writable"_s : "Atomics RMW: property is not writable"_s);
            return { };
        }
        AtomicSlotStatus status = AtomicSlotStatus::Restart;
        JSValue current = dispatchAtomicSlotRequest(globalObject, object, propertyName, probe, request, status);
        switch (status) {
        case AtomicSlotStatus::Applied:
            return current;
        case AtomicSlotStatus::NotNumber:
            throwTypeError(globalObject, scope, "Atomics RMW: stored value is not a number"_s);
            return { };
        case AtomicSlotStatus::Restart:
            if (logContention) [[unlikely]]
                g_threadAtomicsRMWRestarts.fetch_add(1, std::memory_order_relaxed);
            continue;
        case AtomicSlotStatus::NotEqual:
        case AtomicSlotStatus::NeedsStringResolution:
        case AtomicSlotStatus::LockedRevalidate: // Accessor-internal; never escapes.
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
}

#endif // USE(JSVALUE64)

JSValue atomicsLoadOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName)
{
    VM& vm = globalObject->vm();
#if USE(JSVALUE64)
    if (vm.gilOff()) [[unlikely]]
        return atomicsLoadOnPropertyGilOff(globalObject, object, propertyName);
#endif
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value;
    unsigned attributes = 0;
    auto kind = getOwnPropertyForAtomics(globalObject, object, propertyName, value, attributes);
    RETURN_IF_EXCEPTION(scope, { });
    if (kind != OwnPropertyKind::Data) {
        throwTypeError(globalObject, scope, "Atomics.load: object has no own property"_s);
        return { };
    }
    return value;
}

JSValue atomicsStoreOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue value)
{
    VM& vm = globalObject->vm();
#if USE(JSVALUE64)
    if (vm.gilOff()) [[unlikely]]
        return atomicsStoreOnPropertyGilOff(globalObject, object, propertyName, value);
#endif
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue existing;
    unsigned attributes = 0;
    auto kind = getOwnPropertyForAtomics(globalObject, object, propertyName, existing, attributes);
    RETURN_IF_EXCEPTION(scope, { });
    switch (kind) {
    case OwnPropertyKind::Accessor:
        throwTypeError(globalObject, scope, "Atomics.store: property is an accessor"_s);
        return { };
    case OwnPropertyKind::Data:
        if (attributes & PropertyAttribute::ReadOnly) {
            throwTypeError(globalObject, scope, "Atomics.store: property is not writable"_s);
            return { };
        }
        break;
    case OwnPropertyKind::Missing: {
        bool extensible = object->isExtensible(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!extensible) {
            throwTypeError(globalObject, scope, "Atomics.store: cannot add a property to a non-extensible object"_s);
            return { };
        }
        break;
    }
    }
    if (kind == OwnPropertyKind::Data)
        putExistingOwnDataPropertyForAtomics(globalObject, object, propertyName, value, attributes);
    else
        object->putDirectMayBeIndex(globalObject, propertyName, value); // Fresh data property: writable/enumerable/configurable.
    RETURN_IF_EXCEPTION(scope, { });
    return value;
}

JSValue atomicsCompareExchangeOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue expected, JSValue replacement)
{
    VM& vm = globalObject->vm();
#if USE(JSVALUE64)
    if (vm.gilOff()) [[unlikely]]
        return atomicsCompareExchangeOnPropertyGilOff(globalObject, object, propertyName, expected, replacement);
#endif
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue current;
    unsigned attributes = 0;
    auto kind = getOwnPropertyForAtomics(globalObject, object, propertyName, current, attributes);
    RETURN_IF_EXCEPTION(scope, { });
    if (kind != OwnPropertyKind::Data) {
        throwTypeError(globalObject, scope, "Atomics.compareExchange: object has no own data property"_s);
        return { };
    }
    // 4.5 "stores rep" inherits store's writability rule (same as exchange's
    // "store but requires own data k"): putExistingOwnDataPropertyForAtomics
    // uses putDirect define-semantics, which would replace a ReadOnly slot's
    // value in place — a heap state no sequential JS program can create
    // (THREAD.md indistinguishable-heap requirement; a lock word CASed on a
    // later-frozen object must fail, not keep mutating). Thrown
    // unconditionally, matching store/exchange (not only when SVZ matches).
    if (attributes & PropertyAttribute::ReadOnly) {
        throwTypeError(globalObject, scope, "Atomics.compareExchange: property is not writable"_s);
        return { };
    }
    // SVZ, not ===: NaN compares equal to NaN and +0 to -0, so CAS retry
    // loops over NaN-valued properties terminate (4.5 table).
    bool equal = sameValueZeroForAtomics(globalObject, current, expected);
    RETURN_IF_EXCEPTION(scope, { }); // String comparison can resolve ropes (OOM).
    if (equal) {
        putExistingOwnDataPropertyForAtomics(globalObject, object, propertyName, replacement, attributes);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return current;
}

JSValue atomicsRMWOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, AtomicsRMWOp op, JSValue operand)
{
    VM& vm = globalObject->vm();
#if USE(JSVALUE64)
    if (vm.gilOff()) [[unlikely]]
        return atomicsRMWOnPropertyGilOff(globalObject, object, propertyName, op, operand);
#endif
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (op == AtomicsRMWOp::Exchange) {
        // 4.5: "store but requires own data k" — inherits store's accessor /
        // non-writable TypeErrors, but never creates the property.
        JSValue current;
        unsigned attributes = 0;
        auto kind = getOwnPropertyForAtomics(globalObject, object, propertyName, current, attributes);
        RETURN_IF_EXCEPTION(scope, { });
        if (kind != OwnPropertyKind::Data) {
            throwTypeError(globalObject, scope, "Atomics.exchange: object has no own data property"_s);
            return { };
        }
        if (attributes & PropertyAttribute::ReadOnly) {
            throwTypeError(globalObject, scope, "Atomics.exchange: property is not writable"_s);
            return { };
        }
        putExistingOwnDataPropertyForAtomics(globalObject, object, propertyName, operand, attributes);
        RETURN_IF_EXCEPTION(scope, { });
        return current;
    }

    // Numeric RMW family: convert the operand first (may run JS), then
    // perform the read-modify-write as one atomic step under the GIL.
    double operandNumber = 0;
    int32_t operandInt = 0;
    bool isBitwise = op == AtomicsRMWOp::And || op == AtomicsRMWOp::Or || op == AtomicsRMWOp::Xor;
    if (isBitwise) {
        operandInt = operand.toInt32(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        operandNumber = operand.toNumber(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }

    JSValue current;
    unsigned attributes = 0;
    auto kind = getOwnPropertyForAtomics(globalObject, object, propertyName, current, attributes);
    RETURN_IF_EXCEPTION(scope, { });
    if (kind != OwnPropertyKind::Data) {
        throwTypeError(globalObject, scope, "Atomics RMW: object has no own data property"_s);
        return { };
    }
    // 4.5 "stores result" inherits store's writability rule (see the
    // compareExchange comment above): a ReadOnly slot must never be
    // mutated in place. Checked before the stored-value type check, so a
    // frozen non-number slot reports the writability error.
    if (attributes & PropertyAttribute::ReadOnly) {
        throwTypeError(globalObject, scope, "Atomics RMW: property is not writable"_s);
        return { };
    }
    if (!current.isNumber()) {
        throwTypeError(globalObject, scope, "Atomics RMW: stored value is not a number"_s);
        return { };
    }

    JSValue newValue;
    switch (op) {
    case AtomicsRMWOp::Add:
        newValue = jsNumber(current.asNumber() + operandNumber);
        break;
    case AtomicsRMWOp::Sub:
        newValue = jsNumber(current.asNumber() - operandNumber);
        break;
    case AtomicsRMWOp::And:
        newValue = jsNumber(JSC::toInt32(current.asNumber()) & operandInt);
        break;
    case AtomicsRMWOp::Or:
        newValue = jsNumber(JSC::toInt32(current.asNumber()) | operandInt);
        break;
    case AtomicsRMWOp::Xor:
        newValue = jsNumber(JSC::toInt32(current.asNumber()) ^ operandInt);
        break;
    case AtomicsRMWOp::Exchange:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
    putExistingOwnDataPropertyForAtomics(globalObject, object, propertyName, newValue, attributes);
    RETURN_IF_EXCEPTION(scope, { });
    return current;
}

// ---------------- property waiter table (SPEC-api 5.6) ----------------

class PropertyWaiter final : public ThreadSafeRefCounted<PropertyWaiter> {
public:
    enum State : uint8_t { Waiting, Notified, TimedOut, Terminated };
    enum class Kind : uint8_t { Sync, Async };

    static Ref<PropertyWaiter> create(Kind kind) { return adoptRef(*new PropertyWaiter(kind)); }

    Kind kind;
    std::atomic<uint8_t> state { Waiting }; // flipped exactly once, under the list lock
    Condition condition; // sync
    RefPtr<AsyncTicket> ticket; // async

private:
    explicit PropertyWaiter(Kind kind)
        : kind(kind)
    {
    }
};

class PropertyWaiterList final : public ThreadSafeRefCounted<PropertyWaiterList> {
public:
    static Ref<PropertyWaiterList> create() { return adoptRef(*new PropertyWaiterList()); }

    Lock listLock; // rank 3
    Deque<Ref<PropertyWaiter>> waiters WTF_GUARDED_BY_LOCK(listLock);
    // GC-protects the waited-on object while the list is non-empty; created
    // and cleared only under the JSLock (SPEC-api 5.10).
    Strong<Unknown> cellProtect;
    RefPtr<UniquedStringImpl> uidProtect;

private:
    PropertyWaiterList() = default;
};

using PropertyWaiterKey = std::pair<JSCell*, UniquedStringImpl*>;

class PropertyWaiterTable final {
    WTF_MAKE_NONCOPYABLE(PropertyWaiterTable);
public:
    static PropertyWaiterTable& singleton()
    {
        static LazyNeverDestroyed<PropertyWaiterTable> table;
        static std::once_flag onceKey;
        std::call_once(onceKey, [&] {
            table.construct();
        });
        return table;
    }

    Lock m_lock; // rank 2
    UncheckedKeyHashMap<PropertyWaiterKey, Ref<PropertyWaiterList>> m_lists WTF_GUARDED_BY_LOCK(m_lock);
    // Cells that already carry the per-cell teardown-sweep finalizer (round-4
    // fix, companion to D5 — see sweepCellAtFinalization below). One entry
    // per live waited-on cell; removed when the finalizer runs, so a recycled
    // cell address re-registers correctly.
    HashSet<JSCell*> m_sweepFinalizerCells WTF_GUARDED_BY_LOCK(m_lock);

    // Must hold the JSLock (creates the first-waiter Strong, registers the
    // teardown finalizer).
    // ENQUEUES |waiter| before releasing the table lock (list lock rank 3
    // nested inside table lock rank 2 — the same order removeListIfEmpty
    // uses). Closeout review fix (lost-waiter): the previous shape returned
    // the list and had the caller enqueue under the list lock only, leaving
    // a window where a concurrent notify could findList the just-created
    // (or existing-empty) entry, dequeue zero waiters, observe it empty,
    // and removeListIfEmpty it — after which the waiter appended to an
    // ORPHANED list no future notify can reach (permanent lost sync
    // waiter; observed as the property-wtr-isolation.js GIL-off ~8%
    // flake/hang: register immediately followed by removeListIfEmpty of
    // the same (cell, uid), find-side table empty while the waiter stays
    // parked). With the append under m_lock, removeListIfEmpty's
    // emptiness re-check (also m_lock, then the list lock) can no longer
    // interleave between create and enqueue.
    Ref<PropertyWaiterList> findOrCreateList(VM& vm, JSObject* object, UniquedStringImpl* uid, PropertyWaiter& waiter)
    {
        bool registerSweepFinalizer = false;
        RefPtr<PropertyWaiterList> list;
        {
            Locker locker { m_lock };
            auto result = m_lists.ensure(PropertyWaiterKey { object, uid }, [&] {
                return PropertyWaiterList::create();
            });
            list = result.iterator->value.copyRef();
            if (result.isNewEntry) {
                list->cellProtect.set(vm, object);
                list->uidProtect = uid;
            }
            {
                Locker listLocker { list->listLock };
                list->waiters.append(Ref { waiter });
            }
            registerSweepFinalizer = m_sweepFinalizerCells.add(object).isNewEntry;
        }
        // Round-4 fix (recorded next to D5 in docs/threads/INTEGRATE-api.md):
        // a never-notified INFINITE-timeout waitAsync has no other clearing
        // point for its Strongs — the notify path and the D5 finite-timeout
        // timer are the only consumers, DWT VM-shutdown cancelPendingWork
        // cancels the underlying ticket but never touches its
        // Strong<JSPromise>, and this table is a process-global singleton.
        // cellProtect roots the cell for the waiters' lifetime (5.10
        // liveness, by design), so the cell can die EARLY only after
        // removeListIfEmpty (normal GC; the sweep is then a no-op that
        // unregisters the address) and otherwise dies exactly at VM teardown
        // via lastChanceToFinalize — where this finalizer clears every
        // surviving Strong under the JSLock, before the VM's HandleSet is
        // destroyed (the 5.10 / VM-UAF class ~AsyncTicket RELEASE_ASSERTs
        // against). Public Heap API only, same pattern as
        // registerThreadStateFinalizer (ThreadObject.cpp); registered
        // outside m_lock so the rank-2 table lock never nests heap-internal
        // locks.
        if (registerSweepFinalizer) {
            vm.heap.addFinalizer(object, +[](JSCell* cell) {
                PropertyWaiterTable::singleton().sweepCellAtFinalization(cell);
            });
        }
        return list.releaseNonNull();
    }

    // Runs under the JSLock (GC finalization / lastChanceToFinalize), only
    // when the waited-on cell is dead. Removes every list keyed on the cell
    // (any uid) and clears all Strongs owned by the lists and their abandoned
    // async waiters' tickets. Sync waiters are left enqueued on the (now
    // unreachable) list: a sync waiter parked across VM teardown is an
    // embedder protocol violation, and its thread owns its own dequeue
    // (atomicsWaitOnProperty step 5) — the dequeued <=> flipped invariant
    // stays intact because async waiters are flipped under the list lock
    // here, in the same critical section that dequeues them.
    void sweepCellAtFinalization(JSCell* cell)
    {
        Vector<Ref<PropertyWaiterList>> sweptLists;
        {
            Locker locker { m_lock };
            m_sweepFinalizerCells.remove(cell);
            m_lists.removeIf([&](auto& entry) {
                if (entry.key.first != cell)
                    return false;
                sweptLists.append(entry.value.copyRef());
                return true;
            });
        }
        for (auto& list : sweptLists) {
            Vector<Ref<PropertyWaiter>> abandonedAsync;
            {
                Locker listLocker { list->listLock };
                Deque<Ref<PropertyWaiter>> kept;
                while (!list->waiters.isEmpty()) {
                    Ref<PropertyWaiter> waiter = list->waiters.takeFirst();
                    if (waiter->kind == PropertyWaiter::Kind::Async) {
                        waiter->state.store(PropertyWaiter::TimedOut, std::memory_order_release);
                        if (waiter->ticket)
                            waiter->ticket->state.store(PropertyWaiter::TimedOut, std::memory_order_release);
                        abandonedAsync.append(WTF::move(waiter));
                    } else
                        kept.append(WTF::move(waiter));
                }
                list->waiters = WTF::move(kept);
            }
            // Settle would be a no-op (the ticket is cancelled at DWT
            // shutdown, or about to be); clear the promise Strong here,
            // under the JSLock — mirroring the D5 timer-task bailout.
            for (auto& waiter : abandonedAsync) {
                if (waiter->ticket)
                    waiter->ticket->promise().clear();
            }
            list->cellProtect.clear();
            list->uidProtect = nullptr;
        }
    }

    RefPtr<PropertyWaiterList> findList(JSCell* cell, UniquedStringImpl* uid)
    {
        Locker locker { m_lock };
        auto it = m_lists.find(PropertyWaiterKey { cell, uid });
        if (it == m_lists.end())
            return nullptr;
        return RefPtr<PropertyWaiterList> { it->value.copyRef() };
    }

    // Must hold the JSLock (clears the Strong). Re-checks emptiness in rank
    // order (table lock, then list lock).
    void removeListIfEmpty(JSCell* cell, UniquedStringImpl* uid)
    {
        Locker locker { m_lock };
        auto it = m_lists.find(PropertyWaiterKey { cell, uid });
        if (it == m_lists.end())
            return;
        Ref<PropertyWaiterList> list = it->value;
        {
            Locker listLocker { list->listLock };
            if (!list->waiters.isEmpty())
                return;
        }
        list->cellProtect.clear();
        list->uidProtect = nullptr;
        m_lists.remove(it);
    }

    PropertyWaiterTable() = default; // for LazyNeverDestroyed only; use singleton()
};

static Seconds parseAtomicsTimeout(JSGlobalObject* globalObject, JSValue timeoutValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    double timeoutInMilliseconds = timeoutValue.toNumber(globalObject);
    RETURN_IF_EXCEPTION(scope, Seconds::infinity());
    Seconds timeout = Seconds::infinity();
    if (!std::isnan(timeoutInMilliseconds))
        timeout = std::max(Seconds::fromMilliseconds(timeoutInMilliseconds), 0_s);
    return timeout;
}

#if USE(JSVALUE64)

// ---------------- §C.3(b) under-listLock SVZ re-validation (U-T11) ----------------
//
// SPEC-ungil §C.3 (ANNEX C3 + r9 F1, BINDING): GIL-off, the landed I10
// lost-wakeup closure ("JSLock held from the step-1 read through the
// enqueue") is void — the JSLock is a token, not mutual exclusion of
// mutators — so a foreign store+notify landing between the wait's step-1
// read and its enqueue is LOST as landed: the notify finds an empty list,
// the waiter then parks against a value that already mismatches (the
// cross-engine "Atomics.wait not-equal ordering" exemplar; MC-WAIT S3a,
// mc-wait-property-wait-lost-wakeup.js). Closure, both normative arms:
//   (a) the pre-enqueue read routes through the §9.5 atomic load
//       (atomicsLoadOnProperty's gilOff branch — landed with U-T10),
//       forcing any CoW/Int32/Double conversion OUTSIDE listLock;
//   (b) after the enqueue, RE-VALIDATE SVZ(o[k], expected) via the §9.5
//       load under listLock — the helpers below. Mismatch => dequeue,
//       "not-equal". Rope re-read or shape-moved (accessor Restart) =>
//       dequeue too (eats one FIFO notify — the declared I10 class; r7 F3:
//       leaving the node enqueued would strand a genuine waiter's wakeup),
//       re-derive OUTSIDE any lock, FRESH enqueue. After a dequeue-and-
//       restart the waiter is indistinguishable from a first-time arrival.
//
// Ordering note: the enqueue runs inside findOrCreateList's listLock
// section (the closeout lost-waiter fix) and the re-validation RE-TAKES
// listLock here rather than extending that section. Equivalent for the
// notifier-orders-through-listLock argument: a store+notify pair ordered
// before our enqueue had its store made visible to the re-load by the
// notifier's listLock critical section (its unlock happens-before our
// lock), and a pair ordered after our enqueue finds the node and flips it.
//
// Lock discipline: the §C.3(a) monotonicity lemma makes the under-listLock
// re-load alloc/STW-free — the step-1 §9.5 load already converted any
// CoW/Int32/Double word, no transition re-creates those regimes on a
// §9.5-touched object (OM I34/I35 forward-only shape order), and the
// AS/dictionary cell-locked arms never allocate (OM I20); api rank 3 ->
// OM 10a is the sanctioned §LK cross edge. The SVZ comparison itself must
// not allocate under the lock either: string compares punt to
// NeedsResolution when either side is an unresolved rope. No explicit
// resolve step is needed on restart — the restart's step-1
// sameValueZeroForAtomics resolves both ropes in place outside any lock
// (so an unchanged slot makes progress next pass; a storm of fresh rope
// stores is the same bounded-adversarial-progress class as the accessor's
// Restart). GIL-on (and flag-off) never reaches any of this.

enum class LockedSVZOutcome : uint8_t { Equal, NotEqual, NeedsResolution };

static LockedSVZOutcome sameValueZeroForAtomicsUnderListLock(JSValue a, JSValue b)
{
    if (a.isNumber() && b.isNumber()) {
        double x = a.asNumber();
        double y = b.asNumber();
        if (std::isnan(x) && std::isnan(y))
            return LockedSVZOutcome::Equal;
        return x == y ? LockedSVZOutcome::Equal : LockedSVZOutcome::NotEqual;
    }
    if (a.isString() && b.isString()) {
        JSString* left = asString(a);
        JSString* right = asString(b);
        if (left == right)
            return LockedSVZOutcome::Equal;
        if (left->isRope() || right->isRope())
            return LockedSVZOutcome::NeedsResolution; // Resolution allocates: never under listLock.
        return WTF::equal(*left->tryGetValueImpl(), *right->tryGetValueImpl()) ? LockedSVZOutcome::Equal : LockedSVZOutcome::NotEqual;
    }
    if (a.isString() || b.isString())
        return LockedSVZOutcome::NotEqual; // SVZ across types is always false.
    if (a.isBigInt() && b.isBigInt()) {
        // Alloc-free limb compare (the strictEqualSlowCaseInline bigint arms).
#if USE(BIGINT32)
        if (a.isBigInt32() && b.isBigInt32())
            return a == b ? LockedSVZOutcome::Equal : LockedSVZOutcome::NotEqual;
        if (a.isBigInt32())
            return b.asHeapBigInt()->equalsToInt32(a.bigInt32AsInt32()) ? LockedSVZOutcome::Equal : LockedSVZOutcome::NotEqual;
        if (b.isBigInt32())
            return a.asHeapBigInt()->equalsToInt32(b.bigInt32AsInt32()) ? LockedSVZOutcome::Equal : LockedSVZOutcome::NotEqual;
#endif
        return JSBigInt::equals(a.asHeapBigInt(), b.asHeapBigInt()) ? LockedSVZOutcome::Equal : LockedSVZOutcome::NotEqual;
    }
    // undefined/null/booleans/symbols/objects (and any remaining cross-type
    // pair): SVZ degenerates to encoded-bits identity.
    return a == b ? LockedSVZOutcome::Equal : LockedSVZOutcome::NotEqual;
}

enum class PreParkRevalidation : uint8_t { Proceed, NotEqual, Restart };

// Runs under list.listLock with the JSLock held and heap access intact
// (BEFORE any GILDroppedSection — the §9.5 accessor touches the heap);
// never allocates (see the banner above). On NotEqual/Restart the waiter is
// dequeued and flipped in the same critical section (dequeued <=> flipped).
static PreParkRevalidation revalidateEnqueuedPropertyWaiterUnderListLock(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, const ConcurrentAtomicsProbe& probe, JSValue expected, PropertyWaiterList& list, PropertyWaiter& waiter) WTF_REQUIRES_LOCK(list.listLock)
{
    if (waiter.state.load(std::memory_order_acquire) != PropertyWaiter::Waiting) {
        // A notify consumed the node between findOrCreateList's unlock and
        // this re-lock: the consumed notify is honored — the caller's normal
        // Notified epilogue resolves it ("ok" / the in-flight settle).
        return PreParkRevalidation::Proceed;
    }
    AtomicSlotRequest request; // operation = Load
    AtomicSlotStatus status = AtomicSlotStatus::Restart;
    JSValue current = dispatchAtomicSlotRequest(globalObject, object, propertyName, probe, request, status);
    LockedSVZOutcome outcome;
    if (status != AtomicSlotStatus::Applied) {
        ASSERT(status == AtomicSlotStatus::Restart);
        outcome = LockedSVZOutcome::NeedsResolution; // Shape moved vs the probe (I34 provenance): re-derive outside.
    } else
        outcome = sameValueZeroForAtomicsUnderListLock(current, expected);
    if (outcome == LockedSVZOutcome::Equal)
        return PreParkRevalidation::Proceed;
    bool removed = list.waiters.removeFirstMatching([&](auto& entry) {
        return entry.ptr() == &waiter;
    });
    ASSERT_UNUSED(removed, removed); // State was Waiting under this lock => still enqueued.
    // Terminal flip under the same lock; the node is abandoned and never
    // consulted again (the caller exits "not-equal" or re-enqueues FRESH).
    waiter.state.store(PropertyWaiter::TimedOut, std::memory_order_release);
    return outcome == LockedSVZOutcome::NotEqual ? PreParkRevalidation::NotEqual : PreParkRevalidation::Restart;
}

#endif // USE(JSVALUE64)

JSValue atomicsWaitOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue expected, JSValue timeoutValue) WTF_IGNORES_THREAD_SAFETY_ANALYSIS // The W1 episode drops/retakes listLock under a live Locker (the WaiterListManager pattern).
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Seconds timeout = parseAtomicsTimeout(globalObject, timeoutValue);
    RETURN_IF_EXCEPTION(scope, { });

    // Hoisted out of the §C.3(b) restart loop: a dequeue-and-restart must
    // never extend the caller's deadline.
    MonotonicTime deadline = MonotonicTime::timePointFromNow(timeout);
    auto& table = PropertyWaiterTable::singleton();
    bool gilOff = vm.gilOff();
    bool isSpawnedParker = gilOff && ThreadManager::isJSThreadCurrent();

    // §C.3(b) dequeue-and-restart loop. GIL-on runs exactly one iteration
    // (every continue below is gilOff-gated) — the landed single-pass body,
    // byte-equivalent.
    while (true) {

    // Step 1 (F4): validate + read under the JSLock; GIL-off the read routes
    // through the §9.5 atomic load (§C.3(a)) and is re-validated under the
    // list lock below (§C.3(b)) — GIL-on, the JSLock held from this read
    // through the enqueue closes the lost store+notify window (I10) and no
    // re-read happens.
    JSValue current = atomicsLoadOnProperty(globalObject, object, propertyName);
    RETURN_IF_EXCEPTION(scope, { });
    bool equal = sameValueZeroForAtomics(globalObject, current, expected);
    RETURN_IF_EXCEPTION(scope, { }); // String comparison can resolve ropes (OOM).
    if (!equal)
        return vm.smallStrings.notEqualString();

    // 4.5: the G11 gate guards the *block*, not the call — "!SVZ=>'not-equal';
    // else block (G11-gated)". A not-equal value returns "not-equal" even on a
    // thread that may not block, matching lock.hold which gates only on
    // contention (I18). Still under the JSLock; no side effects yet.
    if (!jsThreadsCanBlockOnCurrentThread(vm))
        return throwTypeError(globalObject, scope, "Atomics.wait cannot be called from the current thread."_s), JSValue();

    UniquedStringImpl* uid = propertyName.uid();
    if (!uid)
        return throwTypeError(globalObject, scope, "Atomics.wait: invalid property name"_s), JSValue();

#if USE(JSVALUE64)
    // §C.3(b) prologue: take the {offset, structureID} provenance the
    // under-listLock §9.5 re-load validates (I34). Outside any rank-2/3 lock
    // (the probe may reify lazy properties / take the cell lock).
    ConcurrentAtomicsProbe probe;
    if (gilOff) [[unlikely]] {
        probe = probeOwnPropertyForAtomicsConcurrent(globalObject, object, propertyName);
        RETURN_IF_EXCEPTION(scope, { });
        if (probe.restart || probe.kind != OwnPropertyKind::Data) [[unlikely]]
            continue; // Shape raced past the step-1 read; the fresh step-1 load re-classifies (and throws the precise error if it settled non-plain).
    }
#endif

    // Step 2 (F4): still under the JSLock — table lock (rank 2), find-or-create
    // + first-waiter Strongs, drop; list lock (rank 3), enqueue Waiting, drop.
    // No list lock is held across the GIL drop.
    Ref<PropertyWaiter> waiter = PropertyWaiter::create(PropertyWaiter::Kind::Sync);
    // Enqueued inside findOrCreateList, under the table lock — see the
    // lost-waiter comment there (closeout review fix).
    Ref<PropertyWaiterList> list = table.findOrCreateList(vm, object, uid, waiter.get());

#if USE(JSVALUE64)
    if (gilOff) [[unlikely]] {
        // §C.3(b): re-validate SVZ(o[k], expected) under the list lock, with
        // heap access still intact (before the GILDroppedSection).
        PreParkRevalidation revalidation;
        bool revalidationListEmpty = false;
        {
            Locker listLocker { list->listLock };
            revalidation = revalidateEnqueuedPropertyWaiterUnderListLock(globalObject, object, propertyName, probe, expected, list.get(), waiter.get());
            revalidationListEmpty = list->waiters.isEmpty();
        }
        if (revalidation != PreParkRevalidation::Proceed) [[unlikely]] {
            if (revalidationListEmpty)
                table.removeListIfEmpty(object, uid);
            if (revalidation == PreParkRevalidation::NotEqual)
                return vm.smallStrings.notEqualString();
            continue; // Restart: FRESH enqueue after re-deriving outside the lock.
        }
    }
#endif

    uint8_t finalState;
    bool listNowEmpty = false;
    {
        // Steps 3-6: park with the GIL dropped; 10ms quantum to poll for
        // termination requests (VMTraps cannot wake property waiters).
        // Depth-free drop (GILDroppedSection, LockObject.h): timed waiters
        // wake in arbitrary order, which DropAllLocks' strict-LIFO unwind
        // protocol livelocks on. GIL-off the section's spawned arm is
        // token-only + access-released (§J.3, LockObject.cpp); a carrier's
        // full release stashed the §J.3-captured park lite consumed below.
        GILDroppedSection droppedSection(vm);
        // UNGIL §A.2.4 rule 4 / TERM1 rule 4 (the I3 apiLock-assumption
        // re-key): the D9 quanta must poll the PARK LITE's bits, not the VM
        // word — spawned = the CURRENT lite (the §J.3 spawned arm never runs
        // the §A.3.6 restore, so the spawned lite stays current across the
        // park); main/embedder = the §J.3-CAPTURED carrier lite
        // (unlockAllForThreadParking inside the section's ctor stashed it).
        // GIL-on parkLite stays null and parkLitePollTerminationRequested's
        // !gilOff arm is byte-equivalent to the landed
        // jsThreadParkTerminationRequested (watchdog-check folded in), so
        // flag-off/GIL-on behavior is identical.
        VMLite* parkLite = nullptr;
        if (gilOff)
            parkLite = isSpawnedParker ? VMLite::currentIfExists() : capturedParkLiteOfCurrentThreadIfAny(vm);
        Locker listLocker { list->listLock };
        while (waiter->state.load(std::memory_order_acquire) == PropertyWaiter::Waiting
            && !parkLitePollTerminationRequested(vm, parkLite)
            && MonotonicTime::now() < deadline) {
            if (gilOff && !isSpawnedParker && parkLitePollWatchdogCheckRequested(vm, parkLite)) [[unlikely]] {
                // Annex W W1 (carrier-only; GIL-off the termination poll
                // above no longer folds the watchdog-check bit in, so a
                // parked carrier must SERVICE it or become unkillable): full
                // §J.3 exit reacquisition + Watchdog::shouldTerminate under
                // the token. NO rank-3 lock across the episode — drop
                // listLock (the waiter stays enqueued; every notify
                // serializes through listLock, so the disposition below sees
                // a consistent state). Same shape as the SD6 per-wait TA
                // park (WaiterListManager.cpp).
                list->listLock.unlock();
                bool terminated = reacquireParkedCarrierAndServiceWatchdogCheck(vm);
                list->listLock.lock();
                if (!terminated)
                    continue; // Re-check waiter state/deadline before any quantum wait.
                // Terminate verdict: rule-3 VM-wide termination was raised.
                // If the waiter was notified DURING the episode, the consumed
                // notify is honored as "ok" (epilogue below) — but the W1
                // consumed-by-servicer shield was set on the SD8-fail
                // premise, so re-raise VM-wide to revoke it (idempotent OR);
                // the termination is then delivered at the caller's next
                // trap poll (the WaiterListManager disposition-(a) rule).
                if (waiter->state.load(std::memory_order_acquire) == PropertyWaiter::Notified) [[unlikely]]
                    vm.traps().fireTrapVMWide(VMTraps::NeedTermination);
                break; // Final park exit: the epilogue resolves Notified vs Terminated.
            }
            MonotonicTime quantum = MonotonicTime::now() + Seconds::fromMilliseconds(10);
            waiter->condition.waitUntil(list->listLock, std::min(deadline, quantum).approximate<WallTime>());
        }
        if (waiter->state.load(std::memory_order_acquire) == PropertyWaiter::Notified)
            finalState = PropertyWaiter::Notified;
        else {
            bool removed = list->waiters.removeFirstMatching([&](auto& entry) {
                return entry.ptr() == waiter.ptr();
            });
            ASSERT_UNUSED(removed, removed);
            finalState = parkLitePollTerminationRequested(vm, parkLite) ? PropertyWaiter::Terminated : PropertyWaiter::TimedOut;
            waiter->state.store(finalState, std::memory_order_release);
        }
        listNowEmpty = list->waiters.isEmpty();
    }

    // Step 7: back under the JSLock.
    if (listNowEmpty)
        table.removeListIfEmpty(object, uid);
    switch (finalState) {
    case PropertyWaiter::Notified:
        return vm.smallStrings.okString();
    case PropertyWaiter::TimedOut:
        return vm.smallStrings.timedOutString();
    case PropertyWaiter::Terminated:
        // Request-then-throw: throwTerminationException ASSERTs the request
        // flag, which a parked thread never had set — only trap BITS were
        // raised while we slept (same shape as LockObject/ConditionObject).
        vm.setHasTerminationRequest();
        vm.throwTerminationException();
        return { };
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return { };
    }

    } // while (true) — §C.3(b) dequeue-and-restart loop.
}

JSValue atomicsWaitAsyncOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue expected, JSValue timeoutValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Seconds timeout = parseAtomicsTimeout(globalObject, timeoutValue);
    RETURN_IF_EXCEPTION(scope, { });

    auto& table = PropertyWaiterTable::singleton();
    bool isAsync = false;
    JSValue value;
    JSPromise* promise = nullptr;
    RefPtr<AsyncTicket> ticket;
    // Any exit that leaves the registration unobservable (step-1 "not-equal"
    // on a restart iteration, the §C.3(b) not-equal dequeue, an exception)
    // retires the ticket so its DWT registration stops rooting the waited-on
    // cell + promise (MC-DOS S4). isAsync flips true only when the armed
    // promise is the result.
    auto ticketRetirer = makeScopeExit([&] {
        if (!isAsync && ticket)
            ticket->retireUnsettled();
    });

    // §C.3(b) dequeue-and-restart loop (see atomicsWaitOnProperty): GIL-on
    // runs exactly one iteration — the landed single-pass body.
    while (true) {

    JSValue current = atomicsLoadOnProperty(globalObject, object, propertyName);
    RETURN_IF_EXCEPTION(scope, { });

    bool equal = sameValueZeroForAtomics(globalObject, current, expected);
    RETURN_IF_EXCEPTION(scope, { }); // String comparison can resolve ropes (OOM).
    if (!equal) {
        value = vm.smallStrings.notEqualString();
        break;
    }
    if (!timeout) {
        value = vm.smallStrings.timedOutString();
        break;
    }
    {
        UniquedStringImpl* uid = propertyName.uid();
        if (!uid)
            return throwTypeError(globalObject, scope, "Atomics.waitAsync: invalid property name"_s), JSValue();

#if USE(JSVALUE64)
        // §C.3(b) prologue (see atomicsWaitOnProperty).
        ConcurrentAtomicsProbe probe;
        if (vm.gilOff()) [[unlikely]] {
            probe = probeOwnPropertyForAtomicsConcurrent(globalObject, object, propertyName);
            RETURN_IF_EXCEPTION(scope, { });
            if (probe.restart || probe.kind != OwnPropertyKind::Data) [[unlikely]]
                continue;
        }
#endif

        // Promise + ticket are created once and reused across restart
        // iterations (each restart abandons only its waiter NODE; the
        // registration itself was never observable).
        if (!promise) {
            promise = JSPromise::create(vm, globalObject->promiseStructure());
            ticket = AsyncTicket::create(globalObject, promise, { object });
        }
        Ref<PropertyWaiter> waiter = PropertyWaiter::create(PropertyWaiter::Kind::Async);
        waiter->ticket = ticket;
        // Enqueued inside findOrCreateList, under the table lock — see the
        // lost-waiter comment there (closeout review fix). The ticket must
        // be set BEFORE the enqueue publishes the waiter to notifiers.
        Ref<PropertyWaiterList> list = table.findOrCreateList(vm, object, uid, waiter.get());

#if USE(JSVALUE64)
        if (vm.gilOff()) [[unlikely]] {
            // §C.3(b): re-validate under the list lock (see atomicsWaitOnProperty).
            PreParkRevalidation revalidation;
            bool revalidationListEmpty = false;
            {
                Locker listLocker { list->listLock };
                revalidation = revalidateEnqueuedPropertyWaiterUnderListLock(globalObject, object, propertyName, probe, expected, list.get(), waiter.get());
                revalidationListEmpty = list->waiters.isEmpty();
            }
            if (revalidation != PreParkRevalidation::Proceed) [[unlikely]] {
                if (revalidationListEmpty)
                    table.removeListIfEmpty(object, uid);
                if (revalidation == PreParkRevalidation::NotEqual) {
                    // The node was published and revoked while still
                    // Waiting, both under the list lock: no settler ever
                    // observed it. Answer synchronously; the scope exit
                    // retires the ticket.
                    value = vm.smallStrings.notEqualString();
                    break;
                }
                continue; // Restart: FRESH waiter + enqueue after re-deriving outside the lock.
            }
        }
#endif

        if (timeout != Seconds::infinity()) {
            // Arm the timeout on the VM's run loop (SPEC-api 5.6 / G28).
            // The lambda holds Ref<VM>: WTF::RunLoop is independently
            // ref-counted and outlives the VM, so a dispatched task can run
            // after embedder VM teardown — nothing else guarantees the VM
            // outlives this timer (same rationale as constructThread's
            // protectedVM capture, ThreadObject.cpp). If DWT shutdown
            // already cancelled the ticket, bail out before touching VM
            // state beyond the lock (the JSLockHolder also keeps the
            // ticket's Strong-clearing destructor path lock-correct).
            JSCell* cell = object;
            vm.runLoop().dispatchAfter(timeout, [protectedVM = Ref { vm }, list = WTF::move(list), waiter = WTF::move(waiter), ticket = ticket.copyRef(), cell, uid = RefPtr<UniquedStringImpl> { uid }] {
                VM& timerVM = protectedVM.get();
                JSLockHolder locker(timerVM);
                if (ticket->isCancelled()) {
                    // DWT VM-shutdown cancelPendingWork ran: settle() would
                    // be a no-op, so clear the ticket's promise Strong HERE,
                    // under the lock — this lambda may hold the last Ref and
                    // a still-set Strong must never be destroyed off-lock
                    // (SPEC-api 5.10; ~AsyncTicket asserts it).
                    ticket->promise().clear();
                    return;
                }
                bool wasWaiting = false;
                {
                    Locker listLocker { list->listLock };
                    if (waiter->state.load(std::memory_order_acquire) == PropertyWaiter::Waiting) {
                        wasWaiting = true;
                        // 5.6 timer task: Waiting => findAndRemove (must succeed:
                        // dequeued <=> flipped, both under the list lock), then
                        // TimedOut; ticket state mirrors 5.5 Waiting->TimedOut.
                        bool removed = list->waiters.removeFirstMatching([&](auto& entry) {
                            return entry.ptr() == waiter.ptr();
                        });
                        ASSERT_UNUSED(removed, removed);
                        waiter->state.store(PropertyWaiter::TimedOut, std::memory_order_release);
                        ticket->state.store(PropertyWaiter::TimedOut, std::memory_order_release);
                    }
                }
                if (!wasWaiting)
                    return;
                PropertyWaiterTable::singleton().removeListIfEmpty(cell, uid.get());
                ticket->settle([](DeferredWorkTimer::Ticket dwtTicket) {
                    JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket->target());
                    JSGlobalObject* lexicalGlobalObject = promise->realm();
                    VM& innerVM = lexicalGlobalObject->vm();
                    promise->resolve(lexicalGlobalObject, innerVM, innerVM.smallStrings.timedOutString());
                });
            });
        }
        isAsync = true;
        value = promise;
        break;
    }

    } // while (true) — §C.3(b) dequeue-and-restart loop.

    JSObject* resultObject = constructEmptyObject(globalObject);
    resultObject->putDirect(vm, vm.propertyNames->async, jsBoolean(isAsync));
    resultObject->putDirect(vm, vm.propertyNames->value, value);
    return resultObject;
}

JSValue atomicsNotifyOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue countValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    double count = std::numeric_limits<double>::infinity();
    if (!countValue.isUndefined()) {
        count = countValue.toIntegerOrInfinity(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (count < 0)
            count = 0;
    }

    UniquedStringImpl* uid = propertyName.uid();
    unsigned woken = 0;
    if (uid) {
        auto& table = PropertyWaiterTable::singleton();
        if (RefPtr<PropertyWaiterList> list = table.findList(object, uid)) {
            Vector<Ref<AsyncTicket>> asyncWoken;
            bool listNowEmpty = false;
            {
                Locker listLocker { list->listLock };
                while (woken < count && !list->waiters.isEmpty()) {
                    Ref<PropertyWaiter> waiter = list->waiters.takeFirst();
                    waiter->state.store(PropertyWaiter::Notified, std::memory_order_release);
                    if (waiter->kind == PropertyWaiter::Kind::Sync)
                        waiter->condition.notifyOne();
                    else if (waiter->ticket) {
                        // F4 notify: flip the ticket Notified under the list
                        // lock (5.5 Waiting->Notified); collect, settle later.
                        waiter->ticket->state.store(PropertyWaiter::Notified, std::memory_order_release);
                        asyncWoken.append(*waiter->ticket);
                    }
                    woken++;
                }
                listNowEmpty = list->waiters.isEmpty();
            }
            for (auto& ticket : asyncWoken) {
                ticket->settle([](DeferredWorkTimer::Ticket dwtTicket) {
                    JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket->target());
                    JSGlobalObject* lexicalGlobalObject = promise->realm();
                    VM& innerVM = lexicalGlobalObject->vm();
                    promise->resolve(lexicalGlobalObject, innerVM, innerVM.smallStrings.okString());
                });
            }
            if (listNowEmpty)
                table.removeListIfEmpty(object, uid);
        }
    }
    return jsNumber(woken);
}

} // namespace JSC
