/*
 * Copyright (C) 2012-2022 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#include <wtf/Compiler.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "AllocatorForMode.h"
#include "AllocatorInlines.h"
#include "CPU.h"
#include "CallFrameInlines.h"
#include "CompleteSubspaceInlines.h"
#include "DeferGCInlines.h"
#include "FreeListInlines.h"
#include "Handle.h"
#include "HeapInlines.h"
#include "IsoSubspaceInlines.h"
#include "JSBigInt.h"
#include "JSCast.h"
#include "JSDestructibleObject.h"
#include "JSFunction.h"
#include "JSObject.h"
#include "JSString.h"
#include "LocalAllocatorInlines.h"
#include "MarkedBlock.h"
#include "SlotVisitorInlines.h"
#include "Structure.h"
#include "Symbol.h"
#include <wtf/CompilationThread.h>

namespace JSC {

inline JSCell::JSCell(CreatingEarlyCellTag)
{
    // V7 (TSAN, cell-header family): ctor header init can race with foreign
    // cellHeaderConcurrentLoad readers on a recycled cell (the cell address
    // was previously published; stale-header readers re-dispatch per OM
    // §3.0/GT#2). Store relaxed so the pair is atomic/atomic under TSAN;
    // non-TSAN this is the identical plain store the init-list produced.
    cellHeaderConcurrentStore(m_cellState, CellState::DefinitelyWhite);
    ASSERT(!isCompilationThread());
}

inline JSCell::JSCell(VM&, Structure* structure)
    : JSCell(CreatingWellDefinedBuiltinCell, structure->id(), structure->typeInfoBlob())
{
    ASSERT(!isCompilationThread());
    ASSERT(m_cellState == CellState::DefinitelyWhite);

    // Note that in the constructor initializer list above, we are only using values
    // inside structure but not necessarily the structure pointer itself. All these
    // values are contained inside Structure::m_blob. Note also that this constructor
    // is an inline function. Hence, the compiler may choose to pre-compute the address
    // of structure->m_blob and discard the structure pointer itself. There's a chance
    // that the GC may run while allocating this cell. In the event that the structure
    // is newly instantiated just before calling this constructor, there may not be any
    // other references to it. As a result, the structure may get collected before this
    // cell is even constructed. To avoid this possibility, we need to ensure that the
    // structure pointer is still alive at this point.
    ensureStillAliveHere(structure);
    static_assert(JSCell::atomSize >= MarkedBlock::atomSize);
}

// This constructor should not be used directly. Exceptions are for quite few well-defined builtin objects, e.g. JSString, empty JSFinalObject etc.
// Structure must be kept alive somehow (e.g. by JSGlobalObject, or ensureStillAliveHere).
#if TSAN_ENABLED && USE(JSVALUE64) && CPU(LITTLE_ENDIAN)
ALWAYS_INLINE JSCell::JSCell(CreatingWellDefinedBuiltinCellTag, StructureID structureID, uint32_t blob)
{
    // V7 (TSAN, cell-header family): on a recycled cell this init races with
    // foreign cellHeaderConcurrentLoad readers holding a stale reference
    // (blessed reader side, OM §3.0/GT#2 — stale headers re-dispatch). Make
    // the writer side atomic too: assemble the full 8-byte header
    // {m_structureID, m_blob} and publish it with a single 64-bit relaxed
    // store. TSAN-build-only path; the non-TSAN ctor below is the original
    // member-init-list (zero codegen delta flag-off).
    //
    // Recorded residual: m_structureID is omitted from the mem-init-list, so
    // its NSDMI default-init still emits a plain 4-byte zero store before
    // this atomic store. It is dead (unconditionally overwritten by the
    // covering atomic store) and DSE runs before the TSan instrumentation
    // pass, so it is expected to vanish; if ctor frames persist in the next
    // snapshot, that store is the cause and StructureID needs a no-init tag
    // constructor (next wave).
    static_assert(OBJECT_OFFSETOF(JSCell, m_structureID) + sizeof(StructureID) == OBJECT_OFFSETOF(JSCell, m_blob));
    static_assert(sizeof(StructureID) == sizeof(uint32_t));
    uint64_t header = static_cast<uint64_t>(structureID.bits()) | (static_cast<uint64_t>(blob) << 32);
    __atomic_store_n(reinterpret_cast<uint64_t*>(&m_structureID), header, __ATOMIC_RELAXED);
}
#else
ALWAYS_INLINE JSCell::JSCell(CreatingWellDefinedBuiltinCellTag, StructureID structureID, uint32_t blob)
    : m_structureID(structureID)
    , m_blob(blob)
{
}
#endif

// finishCreation() marks the end of the cell's construction: after this the cell pointer may escape.
// Between allocateCell() and here there must be no safepoint (whose conservative stack scan would
// find this cell), and the cell pointer must not be stored into an already-reachable object (which
// markers can follow). Either would let the partially constructed cell be visited. Callers that
// construct a cell whose auxiliary storage is deliberately left uninitialized must extend that
// window themselves: see ObjectInitializationScope.
inline void JSCell::finishCreation(VM& vm)
{
    // Ensure the cell's initializing stores are visible before any store of this cell into an already-reachable object.
    vm.mutatorFence();
#if ENABLE(GC_VALIDATION)
    ASSERT(vm.isInitializingObject());
    vm.setInitializingObjectClass(0);
#else
    UNUSED_PARAM(vm);
#endif
    ASSERT(m_structureID);
}

inline void JSCell::finishCreation(VM& vm, Structure* structure, CreatingEarlyCellTag)
{
#if ENABLE(GC_VALIDATION)
    ASSERT(vm.isInitializingObject());
    vm.setInitializingObjectClass(0);
    if (structure) {
#endif
        // V7 (TSAN, cell-header family): relaxed header stores pairing with
        // cellHeaderConcurrentLoad readers; plain stores non-TSAN.
        cellHeaderConcurrentStore(m_structureID, structure->id());
        cellHeaderConcurrentStore(m_indexingTypeAndMisc, structure->indexingModeIncludingHistory());
        cellHeaderConcurrentStore(m_type, structure->typeInfo().type());
        cellHeaderConcurrentStore(m_flags, structure->typeInfo().inlineTypeFlags());
#if ENABLE(GC_VALIDATION)
    }
#else
    UNUSED_PARAM(vm);
#endif
    // Very first set of allocations won't have a real structure.
    ASSERT(m_structureID || !vm.structureStructure);
}

template<typename Visitor>
void JSCell::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    visitor.appendUnbarriered(cell->structure());
}

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(inline, JSCell);

template<typename Visitor>
ALWAYS_INLINE void JSCell::visitOutputConstraintsImpl(JSCell*, Visitor&)
{
}

DEFINE_VISIT_OUTPUT_CONSTRAINTS_WITH_MODIFIER(inline, JSCell);

template<typename Type>
inline Allocator allocatorForConcurrently(VM& vm, size_t allocationSize, AllocatorForMode mode)
{
    // SPEC-ungil §B / I4, JIT-codegen leg: this is the single funnel every
    // JIT emitter bakes a JITAllocator::constant from (DFG
    // createOSREntries/NewObject/MakeRope, FTL allocateObject/MakeRope,
    // AssemblyHelpers emitAllocateJSObject templates). GIL-off there is NO
    // client whose LocalAllocator may be
    // baked into an artifact: the artifact is executed by EVERY lite of the
    // VM, and a baked per-client iso LocalAllocator makes N threads pop ONE
    // FreeList unlocked (observed: JIT inline-allocation segfault under
    // races/counter-lock.js — scrambled-head pop returned null past the
    // empty check). CompleteSubspace already returns an empty Allocator
    // under useSharedGCHeap (§5.5 server arrays never populated); this gate
    // extends the same rule to the per-client ISO subspaces. Baking an
    // empty Allocator makes every inline allocation take the slow path,
    // which re-dispatches per-thread through allocationClientForCurrentThread
    // at run time. Interim until U-T7 §B.4 item (1) (lite-relative TLC/iso
    // emission) lands. GIL-on/flag-off: one predicted-false compiler-side
    // branch; baked artifacts are byte-identical.
    if (vm.gilOff()) [[unlikely]]
        return { };
    if (auto* subspace = subspaceForConcurrently<Type>(vm))
        return subspace->allocatorFor(allocationSize, mode);
    return { };
}

// H-VMLITE-TLCPTR (SPEC-heap §5.3/§B.4; GILOFF-TAX #1/#2): resolve the
// per-thread TLC flat-table SLOT a JIT inline-allocate emitter bakes for
// `allocationSize` in `subspace`. tlcIndexBase is per-subspace WRITE-ONCE
// (under directoryLock), so the slot is a process-wide constant once
// non-nullopt; the per-thread part is the lite-relative
// `tlcTable[slot * sizeof(Allocator)]` load the emitter generates against the
// VMLite mirror (offsetOfTlcTable / offsetOfTlcTableBound). Returns nullopt
// for: any subspace that is not a CompleteSubspace (iso subspaces never enter
// m_table — m_perDirectory lookup-only; PreciseSubspace has no LocalAllocators
// at all, and only CompleteSubspace carries a tlcIndexBase); a subspace whose
// tlcIndexBase has not been reserved yet (first directory not created — the
// runtime slow path will reserve it, and a later recompile will see it); sizes
// past largeCutoff (precise path). GIL-off callers only (the call is reached
// through a vm.gilOff() codegen gate, so flag-off emission never evaluates it —
// flag-off byte-identity).
inline std::optional<unsigned> tlcSlotForSubspace(Subspace* subspace, size_t allocationSize)
{
    if (!subspace || subspace->kind() != SubspaceKind::CompleteSubspace)
        return std::nullopt;
    if (allocationSize > MarkedSpace::largeCutoff)
        return std::nullopt;
    unsigned base = static_cast<CompleteSubspace*>(subspace)->tlcIndexBase();
    if (base == BlockDirectory::invalidTlcIndex)
        return std::nullopt;
    return base + static_cast<unsigned>(MarkedSpace::sizeClassToIndex(allocationSize));
}

template<typename Type>
inline std::optional<unsigned> tlcSlotForConcurrently(VM& vm, size_t allocationSize)
{
    auto* subspace = subspaceForConcurrently<Type>(vm);
    // subspaceForConcurrently<Type> may yield CompleteSubspace*, IsoSubspace*,
    // or GCClient::IsoSubspace* (NOT a JSC::Subspace) — the latter is the
    // per-client iso allocator the IT-9 comment above forbids baking; it is
    // never table-addressable.
    if constexpr (std::is_convertible_v<decltype(subspace), Subspace*>)
        return tlcSlotForSubspace(subspace, allocationSize);
    else {
        UNUSED_PARAM(subspace);
        UNUSED_PARAM(allocationSize);
        return std::nullopt;
    }
}

template<typename T, AllocationFailureMode failureMode>
ALWAYS_INLINE void* tryAllocateCellHelper(VM& vm, size_t size, GCDeferralContext* deferralContext)
{
    ASSERT(deferralContext || vm.heap.isDeferred() || !AssertNoGC::isInEffectOnCurrentThread());
    ASSERT(size >= sizeof(T));
    JSCell* result = static_cast<JSCell*>(subspaceFor<T>(vm)->allocate(vm, WTF::roundUpToMultipleOf<T::atomSize>(size), deferralContext, failureMode));
    if constexpr (failureMode == AllocationFailureMode::ReturnNull) {
        if (!result)
            return nullptr;
    }
#if ENABLE(GC_VALIDATION)
    ASSERT_WITH_MESSAGE(
        !vm.isInitializingObject(),
        "Allocating JSCell while initializing an object is not allowed. Currently initializing '%s'\n"
        "This means you either forgot `Base::finishCreation(...)` or you are actually allocating.",
        vm.initializingObjectClass()->className.characters());
    vm.setInitializingObjectClass(T::info());
#endif
    result->clearStructure();
    return result;
}

template<typename T>
void* allocateCell(VM& vm, size_t size)
{
    return tryAllocateCellHelper<T, AllocationFailureMode::Assert>(vm, size, nullptr);
}

template<typename T>
void* tryAllocateCell(VM& vm, size_t size)
{
    return tryAllocateCellHelper<T, AllocationFailureMode::ReturnNull>(vm, size, nullptr);
}

template<typename T>
void* allocateCell(VM& vm, GCDeferralContext* deferralContext, size_t size)
{
    return tryAllocateCellHelper<T, AllocationFailureMode::Assert>(vm, size, deferralContext);
}

template<typename T>
void* tryAllocateCell(VM& vm, GCDeferralContext* deferralContext, size_t size)
{
    return tryAllocateCellHelper<T, AllocationFailureMode::ReturnNull>(vm, size, deferralContext);
}

// FIXME: Consider making getCallData concurrency-safe once NPAPI support is removed.
// https://bugs.webkit.org/show_bug.cgi?id=215801
template<Concurrency concurrency>
ALWAYS_INLINE TriState JSCell::isCallableWithConcurrency()
{
    if (!isObject())
        return TriState::False;
    // JSFunction and InternalFunction assert during construction that derived classes don't override getCallData,
    // which guarantees that CallData::Type::None is never returned.
    if (type() == JSFunctionType || type() == InternalFunctionType)
        return TriState::True;
    if (inlineTypeFlags() & OverridesGetCallData) {
        if constexpr (concurrency == Concurrency::MainThread)
            return (methodTable()->getCallData(this).type != CallData::Type::None) ? TriState::True : TriState::False;
        return TriState::Indeterminate;
    }
    return TriState::False;
}

template<Concurrency concurrency>
inline TriState JSCell::isConstructorWithConcurrency()
{
    if (!isObject())
        return TriState::False;
    if constexpr (concurrency == Concurrency::MainThread)
        return (methodTable()->getConstructData(this).type != CallData::Type::None) ? TriState::True : TriState::False;
    // We know that both getConstructData of both types are concurrency aware. Plus, derived classes of JSFunction and InternalFunction
    // never override getConstructData (this is ensured by ASSERT in JSFunction and InternalFunction).
    if (type() == JSFunctionType || type() == InternalFunctionType)
        return (methodTable()->getConstructData(this).type != CallData::Type::None) ? TriState::True : TriState::False;
    return TriState::Indeterminate;
}

ALWAYS_INLINE bool JSCell::isCallable()
{
    auto result = isCallableWithConcurrency<Concurrency::MainThread>();
    ASSERT(result != TriState::Indeterminate);
    return result == TriState::True;
}

ALWAYS_INLINE bool JSCell::isConstructor()
{
    auto result = isConstructorWithConcurrency<Concurrency::MainThread>();
    ASSERT(result != TriState::Indeterminate);
    return result == TriState::True;
}

ALWAYS_INLINE void JSCell::setStructure(VM& vm, Structure* structure)
{
    ASSERT(structure->classInfoForCells() == this->structure()->classInfoForCells());
    ASSERT(!this->structure()
        || this->structure()->transitionWatchpointSetHasBeenInvalidated()
        || structure->id().decode() == structure);
    // V7 (TSAN, cell-header family): header-byte writers store relaxed so
    // they pair with the (blessed, OM §3.0/GT#2) cellHeaderConcurrentLoad
    // readers; identical plain stores non-TSAN. The m_indexingTypeAndMisc
    // reads feeding the CAS loop below load relaxed for the same reason —
    // the CAS itself was already atomic.
    cellHeaderConcurrentStore(m_structureID, structure->id());
    cellHeaderConcurrentStore(m_flags, TypeInfo::mergeInlineTypeFlags(structure->typeInfo().inlineTypeFlags(), cellHeaderConcurrentLoad(m_flags)));
    cellHeaderConcurrentStore(m_type, structure->typeInfo().type());
    IndexingType newIndexingType = structure->indexingModeIncludingHistory();
    if (cellHeaderConcurrentLoad(m_indexingTypeAndMisc) != newIndexingType) {
        ASSERT(!(newIndexingType & ~AllArrayTypesAndHistory));
        for (;;) {
            IndexingType oldValue = cellHeaderConcurrentLoad(m_indexingTypeAndMisc);
            IndexingType newValue = (oldValue & ~AllArrayTypesAndHistory) | structure->indexingModeIncludingHistory();
            if (WTF::atomicCompareExchangeWeakRelaxed(&m_indexingTypeAndMisc, oldValue, newValue))
                break;
        }
    }
    vm.writeBarrier(this, structure);
}

inline const MethodTable* JSCell::methodTable() const
{
    Structure* structure = this->structure();
#if ASSERT_ENABLED
    if (Structure* rootStructure = structure->structure())
        ASSERT(rootStructure == rootStructure->structure());
#endif
    return &structure->classInfoForCells()->methodTable;
}

// With shared-memory threads the Structure passed here may be the pre-transition
// one decoded from a transiently nuked StructureID (see JSCell::structure()).
// Reading through its offsets is still safe because the out-of-line leg of
// JSObject::locationForOffset() dispatches on the butterfly regime itself when
// Options::useJSThreads() is on.
ALWAYS_INLINE JSValue JSCell::fastGetOwnProperty(VM& vm, Structure& structure, PropertyName name)
{
    ASSERT(canUseFastGetOwnProperty(structure));
    PropertyOffset offset = structure.get(vm, name);
    if (offset != invalidOffset)
        return asObject(this)->locationForOffset(offset)->get();
    return JSValue();
}

inline bool JSCell::canUseFastGetOwnProperty(const Structure& structure)
{
    return !structure.hasAnyKindOfGetterSetterProperties()
        && !structure.typeInfo().overridesGetOwnPropertySlot();
}

inline bool JSCell::toBoolean(JSGlobalObject* globalObject) const
{
    if (isString())
        return static_cast<const JSString*>(this)->toBoolean();
    if (isHeapBigInt())
        return static_cast<const JSBigInt*>(this)->toBoolean();
    return !structure()->masqueradesAsUndefined(globalObject);
}

inline TriState JSCell::pureToBoolean() const
{
    if (isString())
        return static_cast<const JSString*>(this)->toBoolean() ? TriState::True : TriState::False;
    if (isHeapBigInt())
        return static_cast<const JSBigInt*>(this)->toBoolean() ? TriState::True : TriState::False;
    if (isSymbol())
        return TriState::True;
    return TriState::Indeterminate;
}

// Debug builds keep the per-thread hold depth (GCCellLockDepth) that the
// cell-lock-no-park asserts in Heap.cpp consult: counted after a successful
// acquire, uncounted before the release, so the depth is exact between the
// two. Lock and unlock of one hold always run on the same thread (RAII).
inline void JSCellLock::lock()
{
    Atomic<IndexingType>* lock = std::bit_cast<Atomic<IndexingType>*>(&m_indexingTypeAndMisc);
    if (!IndexingTypeLockAlgorithm::lockFast(*lock)) [[unlikely]]
        lockSlow();
#if ASSERT_ENABLED
    GCCellLockDepth::increment();
#endif
}

inline bool JSCellLock::tryLock()
{
    Atomic<IndexingType>* lock = std::bit_cast<Atomic<IndexingType>*>(&m_indexingTypeAndMisc);
    bool locked = IndexingTypeLockAlgorithm::tryLock(*lock);
#if ASSERT_ENABLED
    if (locked)
        GCCellLockDepth::increment();
#endif
    return locked;
}

inline void JSCellLock::unlock()
{
#if ASSERT_ENABLED
    GCCellLockDepth::decrement();
#endif
    Atomic<IndexingType>* lock = std::bit_cast<Atomic<IndexingType>*>(&m_indexingTypeAndMisc);
    if (!IndexingTypeLockAlgorithm::unlockFast(*lock)) [[unlikely]]
        unlockSlow();
}

inline bool JSCellLock::isLocked() const
{
    Atomic<IndexingType>* lock = std::bit_cast<Atomic<IndexingType>*>(&m_indexingTypeAndMisc);
    return IndexingTypeLockAlgorithm::isLocked(*lock);
}

inline bool JSCell::perCellBit() const
{
    return TypeInfo::perCellBit(inlineTypeFlags());
}

inline void JSCell::setPerCellBit(bool value)
{
    if (value == perCellBit())
        return;

#if USE(JSVALUE64)
    // SPEC-objectmodel §3.0 (review round 4): flag-on, m_flags' per-cell-bit
    // lane is one of the header's VOLATILE lanes (cellHeaderVolatileMask,
    // ConcurrentButterfly.h) - it can be mutated with no cell lock while a
    // transition's header CAS/DCAS is in flight, and a plain |=/&= RMW here
    // could both (a) write back a stale flags byte that undoes a concurrent
    // publication's mergeInlineTypeFlags store, and (b) lose this flip to a
    // racing header CAS. Byte-sized CAS closes both directions: the transition
    // side CAS-merges this lane from the freshest read (taxonomy (a)), and our
    // CAS retries across any concurrent flags-byte movement. Flag-off the
    // plain RMW below is byte-identical to today (I22).
    if (Options::useJSThreads()) [[unlikely]] {
        auto* flagsByte = std::bit_cast<Atomic<TypeInfo::InlineTypeFlags>*>(&m_flags);
        while (true) {
            TypeInfo::InlineTypeFlags oldFlags = flagsByte->load(std::memory_order_relaxed);
            TypeInfo::InlineTypeFlags newFlags = value
                ? static_cast<TypeInfo::InlineTypeFlags>(oldFlags | TypeInfoPerCellBit)
                : static_cast<TypeInfo::InlineTypeFlags>(oldFlags & ~TypeInfoPerCellBit);
            if (oldFlags == newFlags || flagsByte->compareExchangeWeak(oldFlags, newFlags, std::memory_order_seq_cst))
                return;
        }
    }
#endif

    if (value)
        m_flags |= static_cast<TypeInfo::InlineTypeFlags>(TypeInfoPerCellBit);
    else
        m_flags &= ~static_cast<TypeInfo::InlineTypeFlags>(TypeInfoPerCellBit);
}

inline JSObject* JSCell::toObject(JSGlobalObject* globalObject) const
{
    if (isObject())
        return uncheckedDowncast<JSObject>(const_cast<JSCell*>(this));
    return toObjectSlow(globalObject);
}

ALWAYS_INLINE JSString* JSCell::toStringInline(JSGlobalObject* globalObject) const
{
    Structure* structure = this->structure();
    if (structure->hasRareData()) {
        auto* rareData = structure->rareData();
        if (rareData->cachedSpecialProperty(CachedSpecialPropertyKey::ToPrimitive).isUndefinedOrNull()) {
            if (rareData->cachedSpecialProperty(CachedSpecialPropertyKey::ToString) == globalObject->objectProtoToStringFunction()) {
                if (auto result = rareData->cachedSpecialProperty(CachedSpecialPropertyKey::ToStringTag))
                    return asString(result);
            }
        }
    }
    if (isObject())
        return asObject(this)->toString(globalObject);
    if (isString())
        return asString(this);
    return toStringSlowCase(globalObject);
}

inline bool isWebAssemblyInstance(const JSCell* cell)
{
    return cell->type() == WebAssemblyInstanceType;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
