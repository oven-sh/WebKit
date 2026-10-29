/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTEffects.h"
#include "AOTInstance.h"
#include "AOTMemoryLocation.h"
#include "AOTVector.h"
#include "B3AbstractHeapRepository.h"
#include "B3Effects.h"
#include "JSArrayBuffer.h"
#include "JSGlobalObject.h"
#include "JSPropertyNameEnumerator.h"
#include <array>
#include <memory>
#include <optional>
#include <wtf/HashMap.h>

namespace JSC {

namespace B3 {
class Procedure;
}

namespace AOT {

using B3HeapList = Vector<const B3::AbstractHeap*, 8>;

#define FOR_EACH_AOT_HEAP_OUTSIDE_IR(v) \
    v(collectorState) \
    v(cards) \
    v(argumentBuffer) \
    v(frameSlots) \
    v(counters)

// AOTObject_shapeWord is read of every cell. In one that is no compiled object other things lie there and change. None of them has compiledObjectShapeBit, so every test of the word fails for them all.
// A word of AOTInstance_fieldsFilledOnce is zero until it gets its value and keeps that value. Whoever reads zero goes the way that needs no word, so a read may stand anywhere.
#define FOR_EACH_AOT_IMMUTABLE_FIELD_HEAP(v) \
    v(AOTInstance_vm, AOT_INSTANCE_OFFSET(vm)) \
    v(AOTInstance_fixedFields, 0) \
    v(AOTInstance_fieldsFilledOnce, 0) \
    v(AOTClosure_entry, offsetOfClosureEntry) \
    v(AOTCompiledStructure_flags, CompiledStructure::offsetOfFlags()) \
    v(AOTObject_shapeWord, offsetOfShapeWord) \
    v(ArrayBuffer_shared, ArrayBuffer::offsetOfShared()) \
    v(JSArrayBuffer_impl, JSArrayBuffer::offsetOfImpl()) \
    v(JSGlobalObject_nullSetterStrictFunction, JSGlobalObject::offsetOfNullSetterStrictFunction()) \
    v(JSPropertyNameEnumerator_cachedOrderWord, JSPropertyNameEnumerator::cachedOrderWordOffset()) \
    v(JSPropertyNameEnumerator_cachedOrderWordMask, JSPropertyNameEnumerator::cachedOrderWordMaskOffset()) \
    v(JSPropertyNameEnumerator_endOwnPropertyIndex, JSPropertyNameEnumerator::endOwnPropertyIndexOffset()) \
    v(AOTImage_tables, 0)

#define FOR_EACH_AOT_MUTABLE_FIELD_HEAP(v) \
    v(AOTInstance_mutableFields, 0, RuntimeState) \
    v(AOTInstance_stringCells, 0, AllocatorState) \
    v(AOTInstance_siteCaches, 0, RuntimeState) \
    v(AOTInstance_closureCells, 0, RuntimeState) \
    v(AOTInstance_misfit, 0, RuntimeState) \
    v(AOTInstance_globalBindings, 0, RuntimeState) \
    v(hostObjectCachedValues, 0, ExternalState) \
    v(AOTObject_orderWord, offsetOfOrderWord, OrderWords) \
    v(AOTSlimClosure_word, offsetOfSlimClosureWord, ShapeWords) \
    v(AOTExtension_granuleBits, offsetOfExtensionGranuleBits, ObjectFields) \
    v(ArrayBuffer_sizeInBytes, ArrayBuffer::offsetOfSizeInBytes(), TypedArrayStorage) \
    v(AOTStringBuilderStorage_fields, 0, StringBuilderContents) \
    v(AOTStackStringBuilder_fields, 0, StringBuilderContents) \
    v(stringBuilderBytes, 0, StringBuilderContents) \
    v(weakTableStorage, 0, CollectionStorage) \
    v(weakTableEntries, 0, CollectionEntries) \
    v(JSGlobalObject_globalLexicalBindingEpoch, JSGlobalObject::globalLexicalBindingEpochOffset(), RuntimeState)

struct CoveredLocations {
    Typed::AbstractHeap heap;
    uint64_t mask;
};

struct AOTHeaps final : B3::AbstractHeapRepository {
    AOTHeaps();
    ~AOTHeaps() final;

    static AOTHeaps& of(B3::Procedure&);

#define AOT_DECLARE_HEAP(name, ...) B3::AbstractHeap name;
    FOR_EACH_AOT_HEAP_OUTSIDE_IR(AOT_DECLARE_HEAP)
    FOR_EACH_AOT_IMMUTABLE_FIELD_HEAP(AOT_DECLARE_HEAP)
    FOR_EACH_AOT_MUTABLE_FIELD_HEAP(AOT_DECLARE_HEAP)
#undef AOT_DECLARE_HEAP

    B3::IndexedAbstractHeap AOTClosure_captures;
    B3::IndexedAbstractHeap AOTSlimClosure_captures;

    const B3::AbstractHeap& entire(Typed::AbstractHeap heap) const LIFETIME_BOUND { return m_entireHeaps[static_cast<size_t>(heap)]; }
    const B3::AbstractHeap& at(const Typed::Program&, const Typed::MemoryLocation&) LIFETIME_BOUND;

    void appendHeaps(const Typed::LocationMasks&, B3HeapList&) const;
    void appendHeapsWrittenByCollection(B3HeapList&) const;
    bool mayCollect(const B3::Effects&) const;
    B3::HeapRange rangeWrittenByWhateverMayCollect() const;
    // Memory outside the heap that a cell owns. A pointer into it keeps nothing alive.
    bool isFreedWithOwner(const B3::HeapRange&) const;
    static bool mayConvertCells(const Typed::Effects& effectsOfCall);
    void appendHeapsOfConvertedCells(B3HeapList&) const;

    std::optional<CoveredLocations> locationsOf(const B3::AbstractHeap&) const;
    bool isCoveredBy(const B3::AbstractHeap& written, const Typed::LocationMasks& declaredWrites) const;

private:
    static constexpr unsigned numberOfBuckets = 64;

    struct Bucket {
        B3::AbstractHeap heap;
        size_t numberOfRetainedChildren { 0 };
    };

    using Buckets = std::array<Bucket, numberOfBuckets>;
    using ExactHeaps = UncheckedKeyHashMap<uint64_t, std::unique_ptr<B3::AbstractHeap>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    void removeTemporaryClientHeaps() final;
    bool hasTemporaryClientHeaps() const final;
    void appendGrownClientHeaps(WTF::Vector<GrownHeap, 16>&) final;

    void nestUpstreamHeaps();
    void indexHeaps();

    B3::AbstractHeap& mutableEntire(Typed::AbstractHeap heap) LIFETIME_BOUND { return m_entireHeaps[static_cast<size_t>(heap)]; }
    Bucket& bucketOf(Typed::AbstractHeap heap, unsigned bit) LIFETIME_BOUND { return m_buckets[Typed::maskIndexOf(heap)][bit]; }
    const Bucket& bucketOf(Typed::AbstractHeap heap, unsigned bit) const LIFETIME_BOUND { return m_buckets[Typed::maskIndexOf(heap)][bit]; }

    std::array<B3::AbstractHeap, Typed::numberOfHeaps> m_entireHeaps;
    std::array<Buckets, Typed::numberOfIndexedHeaps> m_buckets;
    std::array<ExactHeaps, Typed::numberOfIndexedHeaps> m_exactHeaps;
    Vector<Bucket*, 16> m_bucketsWithExactHeaps;
    UncheckedKeyHashMap<const B3::AbstractHeap*, CoveredLocations> m_locationsOfHeap;
};

std::unique_ptr<B3::AbstractHeapRepository> createAOTHeaps();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
