/*
 * Copyright (C) 2013-2016 Apple Inc. All rights reserved.
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

#include "config.h"
#include "B3AbstractHeapRepository.h"

#if ENABLE(B3_JIT)

#include "B3BulkMemoryValue.h"
#include "B3CCallValue.h"
#include "B3FenceValue.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include "B3WasmArrayGetValue.h"
#include "B3WasmArrayLengthValue.h"
#include "B3WasmArrayNewValue.h"
#include "B3WasmArraySetValue.h"
#include "B3WasmStructNewValue.h"
#include "ClonedArguments.h"
#include "ConcatKeyAtomStringCache.h"
#include "DateInstance.h"
#include "DirectArguments.h"
#include "GetterSetter.h"
#include "HasOwnPropertyCache.h"
#include "JSArrayBufferView.h"
#include "JSBoundFunction.h"
#include "JSGlobalObject.h"
#include "JSGlobalProxy.h"
#include "JSMap.h"
#include "JSPromise.h"
#include "JSPropertyNameEnumerator.h"
#include "JSSet.h"
#include "JSWeakMap.h"
#include "JSWebAssemblyArray.h"
#include "JSWebAssemblyInstance.h"
#include "JSWebAssemblyStruct.h"
#include "JSWrapperObject.h"
#include "KeyAtomStringCache.h"
#include "NumericStrings.h"
#include "RegExpObject.h"
#include "ScopedArguments.h"
#include "ShadowChicken.h"
#include "StructureArrayStorageInlines.h"
#include "StructureChain.h"
#include "StructureRareDataInlines.h"
#include "Symbol.h"
#include "WasmGlobal.h"
#include "WasmTable.h"
#include "WasmTypeDefinition.h"
#include "WebAssemblyModuleRecord.h"
#include <algorithm>
#include <array>
#include <wtf/TZoneMallocInlines.h>

namespace JSC::B3 {

WTF_MAKE_TZONE_ALLOCATED_IMPL(AbstractHeapRepository);

AbstractHeapRepository::AbstractHeapRepository()
    : root(nullptr, "jscRoot")

#define ABSTRACT_HEAP_INITIALIZATION(name) , name(&root, #name)
    FOR_EACH_ABSTRACT_HEAP(ABSTRACT_HEAP_INITIALIZATION)
#undef ABSTRACT_HEAP_INITIALIZATION

    , WebAssemblyMemory(TypedArrayProperties)

#define ABSTRACT_FIELD_INITIALIZATION(name, offset, mutability) , name(&root, #name, offset, mutability)
    FOR_EACH_ABSTRACT_FIELD(ABSTRACT_FIELD_INITIALIZATION)
#undef ABSTRACT_FIELD_INITIALIZATION

    , JSCell_freeListNext(JSCell_header)
    , ArrayStorage_publicLength(Butterfly_publicLength)
    , ArrayStorage_vectorLength(Butterfly_vectorLength)

#define INDEXED_ABSTRACT_HEAP_INITIALIZATION(name, offset, size) , name(&root, #name, offset, size)
    FOR_EACH_INDEXED_ABSTRACT_HEAP(INDEXED_ABSTRACT_HEAP_INITIALIZATION)
#undef INDEXED_ABSTRACT_HEAP_INITIALIZATION

#define NUMBERED_ABSTRACT_HEAP_INITIALIZATION(name) , name(&root, #name)
    FOR_EACH_NUMBERED_ABSTRACT_HEAP(NUMBERED_ABSTRACT_HEAP_INITIALIZATION)
#undef NUMBERED_ABSTRACT_HEAP_INITIALIZATION

    , JSString_value(JSRopeString_fiber0)
    , JSWrapperObject_internalValue(const_cast<AbstractHeap&>(JSInternalFieldObjectImpl_internalFields[static_cast<unsigned>(JSWrapperObject::Field::WrappedValue)]))

    , absolute(&root, "absolute")
{
    JSCell_header.changeParent(&JSCellHeaderAndNamedProperties);
    properties.atAnyNumber().changeParent(&JSCellHeaderAndNamedProperties);

    // Make sure that our explicit assumptions about the TypeInfoBlob match reality.
    RELEASE_ASSERT(!(JSCell_indexingTypeAndMisc.offset() & (sizeof(int32_t) - 1)));
    RELEASE_ASSERT(JSCell_indexingTypeAndMisc.offset() + 1 == JSCell_typeInfoType.offset());
    RELEASE_ASSERT(JSCell_indexingTypeAndMisc.offset() + 2 == JSCell_typeInfoFlags.offset());
    RELEASE_ASSERT(JSCell_indexingTypeAndMisc.offset() + 3 == JSCell_cellState.offset());

    JSCell_structureID.changeParent(&JSCell_header);
    JSCell_usefulBytes.changeParent(&JSCell_header);
    JSCell_indexingTypeAndMisc.changeParent(&JSCell_usefulBytes);
    JSCell_typeInfoType.changeParent(&JSCell_usefulBytes);
    JSCell_typeInfoFlags.changeParent(&JSCell_usefulBytes);
    JSCell_cellState.changeParent(&JSCell_usefulBytes);
    JSRopeString_flags.changeParent(&JSRopeString_fiber0);
    JSRopeString_length.changeParent(&JSRopeString_fiber1);

    VM_heap_barrierThreshold.changeParent(&VM_heapState);
    VM_heap_mutatorShouldBeFenced.changeParent(&VM_heapState);

    RELEASE_ASSERT(!JSCell_freeListNext.offset());

#define RETAIN_INDEXED_ABSTRACT_HEAPS(name, offset, size) name.retainCurrentHeaps();
    FOR_EACH_INDEXED_ABSTRACT_HEAP(RETAIN_INDEXED_ABSTRACT_HEAPS)
#undef RETAIN_INDEXED_ABSTRACT_HEAPS
#define RETAIN_NUMBERED_ABSTRACT_HEAPS(name) name.retainCurrentHeaps();
    FOR_EACH_NUMBERED_ABSTRACT_HEAP(RETAIN_NUMBERED_ABSTRACT_HEAPS)
#undef RETAIN_NUMBERED_ABSTRACT_HEAPS
    absolute.retainCurrentHeaps();
}

AbstractHeapRepository::~AbstractHeapRepository() = default;

void AbstractHeapRepository::attachProcedure(Procedure& procedure)
{
    RELEASE_ASSERT(!m_procedure);
    m_procedure = &procedure;
}

void AbstractHeapRepository::detachProcedure()
{
    m_procedure = nullptr;
    removeTemporaryClientHeaps();
#define REMOVE_TEMPORARY_INDEXED_ABSTRACT_HEAPS(name, offset, size) name.removeTemporaryHeaps();
    FOR_EACH_INDEXED_ABSTRACT_HEAP(REMOVE_TEMPORARY_INDEXED_ABSTRACT_HEAPS)
#undef REMOVE_TEMPORARY_INDEXED_ABSTRACT_HEAPS
#define REMOVE_TEMPORARY_NUMBERED_ABSTRACT_HEAPS(name) name.removeTemporaryHeaps();
    FOR_EACH_NUMBERED_ABSTRACT_HEAP(REMOVE_TEMPORARY_NUMBERED_ABSTRACT_HEAPS)
#undef REMOVE_TEMPORARY_NUMBERED_ABSTRACT_HEAPS
    absolute.removeTemporaryHeaps();
#define CLEAR_DECORATION_VECTOR(elementType, name) name.shrink(0);
    FOR_EACH_DECORATION_VECTOR(CLEAR_DECORATION_VECTOR)
#undef CLEAR_DECORATION_VECTOR
    m_grownHeaps.shrink(0);
}

void AbstractHeapRepository::keepRangesAcrossProcedures()
{
    root.compute();
    root.markAsStayingAcrossProcedures();
    m_keepsRangesAcrossProcedures = true;
}

size_t AbstractHeapRepository::firstGrownHeapEndingAtOrBehind(unsigned position) const
{
    return std::ranges::lower_bound(m_grownHeaps, position, { }, &GrownHeap::end) - m_grownHeaps.begin();
}

unsigned AbstractHeapRepository::growthBefore(unsigned position) const
{
    size_t index = firstGrownHeapEndingAtOrBehind(position);
    return index ? m_grownHeaps[index - 1].growthUpToHere : 0;
}

HeapRange AbstractHeapRepository::rangeOf(const AbstractHeap& heap) const
{
    if (!heap.staysAcrossProcedures())
        return heap.range();
    HeapRange range = heap.m_range;
    if (m_grownHeaps.isEmpty())
        return range;
    unsigned begin = static_cast<unsigned>(range.begin());
    unsigned end = static_cast<unsigned>(range.end());
    size_t index = firstGrownHeapEndingAtOrBehind(end);
    unsigned growthInFrontOfEnd = index ? m_grownHeaps[index - 1].growthUpToHere : 0;
    for (; index < m_grownHeaps.size() && m_grownHeaps[index].end == end; ++index) {
        if (m_grownHeaps[index].heap->isWithin(heap))
            growthInFrontOfEnd += m_grownHeaps[index].growth;
    }
    return HeapRange(begin + growthBefore(begin + 1), end + growthInFrontOfEnd);
}

void AbstractHeapRepository::placeTemporaryHeaps()
{
    m_grownHeaps.shrink(0);
    auto appendIfGrown = [&](AbstractHeap& heap, size_t numberOfRetainedChildren) {
        if (heap.numberOfChildren() != numberOfRetainedChildren)
            m_grownHeaps.append({ &heap, numberOfRetainedChildren });
    };
    appendGrownClientHeaps(m_grownHeaps);
    appendIfGrown(absolute.atAnyAddress(), absolute.numberOfRetainedHeaps());
#define APPEND_GROWN_INDEXED_ABSTRACT_HEAP(name, offset, size) appendIfGrown(name.atAnyIndex(), name.numberOfRetainedHeaps());
    FOR_EACH_INDEXED_ABSTRACT_HEAP(APPEND_GROWN_INDEXED_ABSTRACT_HEAP)
#undef APPEND_GROWN_INDEXED_ABSTRACT_HEAP
#define APPEND_GROWN_NUMBERED_ABSTRACT_HEAP(name) appendIfGrown(name.atAnyNumber(), name.numberOfRetainedHeaps());
    FOR_EACH_NUMBERED_ABSTRACT_HEAP(APPEND_GROWN_NUMBERED_ABSTRACT_HEAP)
#undef APPEND_GROWN_NUMBERED_ABSTRACT_HEAP
    if (m_grownHeaps.isEmpty())
        return;

    for (GrownHeap& grown : m_grownHeaps)
        grown.end = static_cast<unsigned>(grown.heap->m_range.end());
    std::ranges::sort(m_grownHeaps, { }, &GrownHeap::end);
    unsigned growthSoFar = 0;
    for (GrownHeap& grown : m_grownHeaps) {
        unsigned numberOfNewLeaves = 0;
        for (const AbstractHeap* child : grown.heap->children().span().subspan(grown.numberOfRetainedChildren))
            numberOfNewLeaves += child->numberOfLeaves();
        grown.growth = grown.numberOfRetainedChildren ? numberOfNewLeaves : numberOfNewLeaves - 1;
        growthSoFar += grown.growth;
        grown.growthUpToHere = growthSoFar;
    }
    for (const GrownHeap& grown : m_grownHeaps) {
        const Vector<AbstractHeap*>& children = grown.heap->children();
        uint64_t position = grown.numberOfRetainedChildren ? rangeOf(*children[grown.numberOfRetainedChildren - 1]).end() : rangeOf(*grown.heap).begin();
        for (AbstractHeap* child : children.span().subspan(grown.numberOfRetainedChildren)) {
            child->compute(static_cast<unsigned>(position));
            position = child->range().end();
        }
    }
}

void AbstractHeapRepository::decorateMemory(const AbstractHeap* heap, Value* value)
{
    m_heapForMemory.append(HeapForValue(heap, value));
}

static std::span<const AbstractHeap* const> asHeapList(const AbstractHeap* const& heap LIFETIME_BOUND)
{
    if (!heap)
        return { };
    return singleElementSpan(heap);
}

void AbstractHeapRepository::decorateCCallRead(const AbstractHeap* heap, Value* value)
{
    decorateCCallRead(asHeapList(heap), value);
}

void AbstractHeapRepository::decorateCCallWrite(const AbstractHeap* heap, Value* value)
{
    decorateCCallWrite(asHeapList(heap), value);
}

void AbstractHeapRepository::decoratePatchpointRead(const AbstractHeap* heap, Value* value)
{
    decoratePatchpointRead(asHeapList(heap), value);
}

void AbstractHeapRepository::decoratePatchpointWrite(const AbstractHeap* heap, Value* value)
{
    decoratePatchpointWrite(asHeapList(heap), value);
}

auto AbstractHeapRepository::createHeapList(std::span<const AbstractHeap* const> heaps, Value* value) -> HeapListForValue
{
    ASSERT(std::ranges::all_of(heaps, [](const AbstractHeap* heap) { return !!heap; }));
    unsigned begin = m_listedHeaps.size();
    m_listedHeaps.append(heaps);
    unsigned end = m_listedHeaps.size();
    return { begin, end, value };
}

void AbstractHeapRepository::decorateCCallRead(std::span<const AbstractHeap* const> heaps, Value* value)
{
    m_heapListForCCallRead.append(createHeapList(heaps, value));
}

void AbstractHeapRepository::decorateCCallWrite(std::span<const AbstractHeap* const> heaps, Value* value)
{
    m_heapListForCCallWrite.append(createHeapList(heaps, value));
}

void AbstractHeapRepository::decoratePatchpointRead(std::span<const AbstractHeap* const> heaps, Value* value)
{
    m_heapListForPatchpointRead.append(createHeapList(heaps, value));
}

void AbstractHeapRepository::decoratePatchpointWrite(std::span<const AbstractHeap* const> heaps, Value* value)
{
    m_heapListForPatchpointWrite.append(createHeapList(heaps, value));
}

void AbstractHeapRepository::decorateFenceRead(const AbstractHeap* heap, Value* value)
{
    m_heapForFenceRead.append(HeapForValue(heap, value));
}

void AbstractHeapRepository::decorateFenceWrite(const AbstractHeap* heap, Value* value)
{
    m_heapForFenceWrite.append(HeapForValue(heap, value));
}

void AbstractHeapRepository::decorateFencedAccess(const AbstractHeap* heap, Value* value)
{
    m_heapForFencedAccess.append(HeapForValue(heap, value));
}

void AbstractHeapRepository::decorateBulkMemoryRead(const AbstractHeap* heap, Value* value)
{
    m_heapForBulkMemoryRead.append(HeapForValue(heap, value));
}

void AbstractHeapRepository::decorateBulkMemoryWrite(const AbstractHeap* heap, Value* value)
{
    m_heapForBulkMemoryWrite.append(HeapForValue(heap, value));
}

#if ENABLE(WEBASSEMBLY)
void AbstractHeapRepository::decorateWasmStructGet(const AbstractHeap* heap, Value* value)
{
    m_heapForWasmStructGet.append(HeapForValue(heap, value));
}

void AbstractHeapRepository::decorateWasmStructSet(const AbstractHeap* heap, Value* value)
{
    m_heapForWasmStructSet.append(HeapForValue(heap, value));
}

void AbstractHeapRepository::decorateWasmStructNew(const AbstractHeap* heap, Value* value)
{
    m_heapForWasmStructNew.append(HeapForValue(heap, value));
}

void AbstractHeapRepository::decorateWasmArrayGet(const AbstractHeap* heap, Value* value)
{
    m_heapForWasmArrayGet.append(HeapForValue(heap, value));
}

void AbstractHeapRepository::decorateWasmArraySet(const AbstractHeap* heap, Value* value)
{
    m_heapForWasmArraySet.append(HeapForValue(heap, value));
}

void AbstractHeapRepository::decorateWasmArrayNew(const AbstractHeap* heap, Value* value)
{
    m_heapForWasmArrayNew.append(HeapForValue(heap, value));
}

void AbstractHeapRepository::decorateWasmArrayLength(const AbstractHeap* heap, Value* value)
{
    m_heapForWasmArrayLength.append(HeapForValue(heap, value));
}
#endif // ENABLE(WEBASSEMBLY)

void AbstractHeapRepository::computeRanges()
{
    if (m_keepsRangesAcrossProcedures) {
        placeTemporaryHeaps();
        return;
    }

    bool hasTemporaryHeaps = absolute.hasTemporaryHeaps() || hasTemporaryClientHeaps();
#define CHECK_FOR_TEMPORARY_INDEXED_ABSTRACT_HEAPS(name, offset, size) hasTemporaryHeaps |= name.hasTemporaryHeaps();
    FOR_EACH_INDEXED_ABSTRACT_HEAP(CHECK_FOR_TEMPORARY_INDEXED_ABSTRACT_HEAPS)
#undef CHECK_FOR_TEMPORARY_INDEXED_ABSTRACT_HEAPS
#define CHECK_FOR_TEMPORARY_NUMBERED_ABSTRACT_HEAPS(name) hasTemporaryHeaps |= name.hasTemporaryHeaps();
    FOR_EACH_NUMBERED_ABSTRACT_HEAP(CHECK_FOR_TEMPORARY_NUMBERED_ABSTRACT_HEAPS)
#undef CHECK_FOR_TEMPORARY_NUMBERED_ABSTRACT_HEAPS
    if (hasTemporaryHeaps || !m_rangesExcludeTemporaryHeaps)
        root.compute();
    m_rangesExcludeTemporaryHeaps = !hasTemporaryHeaps;
}

void AbstractHeapRepository::computeRangesAndDecorateInstructions()
{
    computeRanges();

    if (Options::verboseCompilation() || Options::verboseFTLCompilation()) [[unlikely]] {
        WTF::dataFile().atomically([&](auto&) {
            dataLogLn("Abstract Heap Repository:");
            root.deepDump(WTF::dataFile());
        });
    }

    auto rangeFor = [&] (const AbstractHeap* heap) -> HeapRange {
        if (heap)
            return rangeOf(*heap);
        return HeapRange();
    };

    for (HeapForValue entry : m_heapForMemory) {
        auto* memoryValue = entry.value->as<MemoryValue>();
        memoryValue->setRange(rangeFor(entry.heap));
        if (memoryValue->isLoad() && entry.heap->mutability() == B3::Mutability::Immutable)
            memoryValue->setReadsMutability(B3::Mutability::Immutable);
    }
    auto effectsOf = [](Value* value) -> Effects* {
        if (auto* patchpoint = value->as<PatchpointValue>())
            return &patchpoint->effects;
        if (auto* call = value->as<CCallValue>())
            return &call->effects;
        return nullptr;
    };
    struct CachedHeapSet {
        std::span<const AbstractHeap* const> heaps;
        HeapSet set;
    };
    std::array<CachedHeapSet, 64> heapSetCache { };
    Vector<HeapRange, 8> ranges;
    auto heapSetFor = [&](HeapSetTable& heapSets, std::span<const AbstractHeap* const> heaps) -> HeapSet {
        uintptr_t hash = heaps.size();
        for (const AbstractHeap* heap : heaps)
            hash = hash * 31 + (std::bit_cast<uintptr_t>(heap) >> 4);
        CachedHeapSet& cached = heapSetCache[(hash ^ (hash >> 16)) % heapSetCache.size()];
        if (!std::ranges::equal(cached.heaps, heaps)) {
            ranges.shrink(0);
            for (const AbstractHeap* heap : heaps)
                ranges.append(rangeOf(*heap));
            cached = { heaps, heapSets.create(ranges.span()) };
        }
        return cached.set;
    };
    auto decorateWithSets = [&](const Vector<HeapListForValue>& heapLists, HeapSet Effects::* set) {
        if (heapLists.isEmpty())
            return;
        RELEASE_ASSERT(m_procedure);
        HeapSetTable& heapSets = m_procedure->heapSets();
        for (const HeapListForValue& entry : heapLists) {
            if (Effects* effects = effectsOf(entry.value))
                effects->*set = HeapSet();
        }
        for (const HeapListForValue& entry : heapLists) {
            if (Effects* effects = effectsOf(entry.value))
                effects->*set = heapSets.unite(effects->*set, heapSetFor(heapSets, m_listedHeaps.subspan(entry.begin, entry.end - entry.begin)));
        }
    };
    decorateWithSets(m_heapListForCCallRead, &Effects::reads);
    decorateWithSets(m_heapListForCCallWrite, &Effects::writes);
    decorateWithSets(m_heapListForPatchpointRead, &Effects::reads);
    decorateWithSets(m_heapListForPatchpointWrite, &Effects::writes);
    for (HeapForValue entry : m_heapForFenceRead)
        entry.value->as<FenceValue>()->read = rangeFor(entry.heap);
    for (HeapForValue entry : m_heapForFenceWrite)
        entry.value->as<FenceValue>()->write = rangeFor(entry.heap);
    for (HeapForValue entry : m_heapForFencedAccess)
        entry.value->as<MemoryValue>()->setFenceRange(rangeFor(entry.heap));
    for (HeapForValue entry : m_heapForBulkMemoryRead)
        entry.value->as<BulkMemoryValue>()->setReadRange(rangeFor(entry.heap));
    for (HeapForValue entry : m_heapForBulkMemoryWrite)
        entry.value->as<BulkMemoryValue>()->setWriteRange(rangeFor(entry.heap));
#if ENABLE(WEBASSEMBLY)
    for (HeapForValue entry : m_heapForWasmStructGet) {
        auto* wasmStructGet = entry.value->as<WasmStructGetValue>();
        wasmStructGet->setRange(rangeFor(entry.heap));
    }
    for (HeapForValue entry : m_heapForWasmStructSet) {
        auto* wasmStructSet = entry.value->as<WasmStructSetValue>();
        wasmStructSet->setRange(rangeFor(entry.heap));
    }
    for (HeapForValue entry : m_heapForWasmStructNew) {
        auto* wasmStructNew = entry.value->as<WasmStructNewValue>();
        wasmStructNew->setRange(rangeFor(entry.heap));
    }
    for (HeapForValue entry : m_heapForWasmArrayGet) {
        auto* wasmArrayGet = entry.value->as<WasmArrayGetValue>();
        wasmArrayGet->setRange(rangeFor(entry.heap));
    }
    for (HeapForValue entry : m_heapForWasmArraySet) {
        auto* wasmArraySet = entry.value->as<WasmArraySetValue>();
        wasmArraySet->setRange(rangeFor(entry.heap));
    }
    for (HeapForValue entry : m_heapForWasmArrayNew) {
        auto* wasmArrayNew = entry.value->as<WasmArrayNewValue>();
        wasmArrayNew->setRange(rangeFor(entry.heap));
    }
    for (HeapForValue entry : m_heapForWasmArrayLength) {
        auto* wasmArrayLength = entry.value->as<WasmArrayLengthValue>();
        wasmArrayLength->setRange(rangeFor(entry.heap));
    }
#endif // ENABLE(WEBASSEMBLY)
}

} // namespace JSC::B3

#endif // ENABLE(B3_JIT)
