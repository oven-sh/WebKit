/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeaps.h"

#if ENABLE(AOT)

#include "AOTClassToken.h"
#include "AOTCompiledStructure.h"
#include "CompleteSubspace.h"
#include "DateInstance.h"
#include "DirectArguments.h"
#include "GetterSetter.h"
#include "JSArrayBufferView.h"
#include "JSBigInt.h"
#include "JSBoundFunction.h"
#include "JSCellButterfly.h"
#include "JSMap.h"
#include "JSPromise.h"
#include "JSSet.h"
#include "JSWeakMap.h"
#include "JSWrapperObject.h"
#include "RegExpObject.h"
#include "ScopedArguments.h"
#include "StructureArrayStorageInlines.h"
#include "StructureRareDataInlines.h"
#include <bit>

namespace JSC { namespace AOT {

using Typed::ElementBase;
using Typed::LocationMasks;
using Typed::MemoryLocation;
using Typed::Program;
using IRHeap = Typed::AbstractHeap;

struct SharedHeap {
    Machine::AbstractHeap AOTHeaps::* heap;
    Typed::AbstractHeapSet otherOwners;
};

static constexpr SharedHeap sharedHeaps[] = {
    { &AOTHeaps::JSCell_header, { IRHeap::NamedProperties, IRHeap::PrototypeLinks, IRHeap::ArrayStorage } },
    { &AOTHeaps::JSObject_butterfly, { IRHeap::NamedProperties } },
    { &AOTHeaps::Butterfly_publicLength, { IRHeap::ArrayLengths } },
    { &AOTHeaps::AOTObject_orderWord, { IRHeap::NamedProperties } },
    { &AOTHeaps::AOTObject_shapeWord, { IRHeap::NamedProperties } },
    { &AOTHeaps::AOTSlimClosure_word, { IRHeap::NamedProperties, IRHeap::PrototypeLinks } },
    { &AOTHeaps::Structure_bitField, { IRHeap::NamedProperties, IRHeap::PrototypeLinks } },
    { &AOTHeaps::JSCell_typeInfoFlags, { IRHeap::StringStorage } },
    { &AOTHeaps::weakTableStorage, { IRHeap::CollectionStorage } },
    { &AOTHeaps::weakTableEntries, { IRHeap::CollectionEntries } },
};

static unsigned bucketIndexOf(ElementBase base)
{
    LocationMasks masks;
    masks.add(MemoryLocation::elements(base));
    uint64_t mask = masks.maskOf(IRHeap::Elements) & ~Typed::markOfElementsThatMayHoldCells;
    RELEASE_ASSERT(std::popcount(mask) == 1);
    return std::countr_zero(mask);
}

AOTHeaps::AOTHeaps()
    : root(Machine::rootHeapIndex)
{
    for (IRHeap heap : Typed::allHeaps)
        m_entireHeaps[static_cast<size_t>(heap)] = addHeap(root, nameOf(heap), 0, Traits::Mutable);
    for (IRHeap heap : Typed::indexedHeaps) {
        for (Machine::AbstractHeap& bucket : m_buckets[Typed::maskIndexOf(heap)])
            bucket = addHeap(entire(heap), "bucket"_s, 0, Traits::Mutable);
    }
    elementsOfArrays = bucketOf(IRHeap::Elements, bucketIndexOf(ElementBase::Array));
    elementsOfArguments = bucketOf(IRHeap::Elements, bucketIndexOf(ElementBase::Arguments));

    auto recordIfBelowEntireHeap = [&](const Machine::AbstractHeap& parent, const Machine::AbstractHeap& member) {
        for (IRHeap heap : Typed::allHeaps) {
            if (parent.index() == entire(heap).index())
                m_fieldHeapsBelow[static_cast<size_t>(heap)].append(&member);
        }
    };
    auto addFieldHeap = [&](Machine::AbstractHeap& member, const Machine::AbstractHeap& parent, ASCIILiteral name, ptrdiff_t offset, Traits::Set traits) {
        member = addHeap(parent, name, offset, traits);
        recordIfBelowEntireHeap(parent, member);
    };
    auto addIndexedHeap = [&](Machine::IndexedAbstractHeap& member, const Machine::AbstractHeap& parent, ASCIILiteral name, ptrdiff_t offset, size_t elementSize, Traits::Set traits) {
        RELEASE_ASSERT(parent.isInitialized());
        member = Machine::IndexedAbstractHeap(m_tree, parent.index(), name, offset, elementSize, traits);
        recordIfBelowEntireHeap(parent, member.atAnyIndex());
    };

#define AOT_ADD_HEAP_OUTSIDE_IR(name, traits) addFieldHeap(name, root, #name ""_s, 0, Traits::traits);
#define AOT_ADD_FIELD_HEAP_UNDER_ROOT(name, offset, traits) addFieldHeap(name, root, #name ""_s, offset, Traits::traits);
#define AOT_ADD_FIELD_HEAP_UNDER_IR_HEAP(name, offset, traits, parent) addFieldHeap(name, entire(IRHeap::parent), #name ""_s, offset, Traits::traits);
#define AOT_ADD_FIELD_HEAP_UNDER_FIELD_HEAP(name, offset, traits, parent) addFieldHeap(name, parent, #name ""_s, offset, Traits::traits);
#define AOT_ADD_INDEXED_HEAP(name, offset, elementSize, traits, parent) addIndexedHeap(name, parent, #name ""_s, offset, elementSize, Traits::traits);
    FOR_EACH_AOT_HEAP_OUTSIDE_IR(AOT_ADD_HEAP_OUTSIDE_IR)
    FOR_EACH_AOT_FIELD_HEAP_UNDER_ROOT(AOT_ADD_FIELD_HEAP_UNDER_ROOT)
    FOR_EACH_AOT_FIELD_HEAP_UNDER_IR_HEAP(AOT_ADD_FIELD_HEAP_UNDER_IR_HEAP)
    FOR_EACH_JSC_FIELD_HEAP_UNDER_ROOT(AOT_ADD_FIELD_HEAP_UNDER_ROOT)
    FOR_EACH_JSC_FIELD_HEAP_UNDER_IR_HEAP(AOT_ADD_FIELD_HEAP_UNDER_IR_HEAP)
    FOR_EACH_AOT_FIELD_HEAP_UNDER_FIELD_HEAP(AOT_ADD_FIELD_HEAP_UNDER_FIELD_HEAP)
    FOR_EACH_JSC_FIELD_HEAP_UNDER_FIELD_HEAP(AOT_ADD_FIELD_HEAP_UNDER_FIELD_HEAP)
    FOR_EACH_AOT_FIELD_HEAP_UNDER_JSC_FIELD_HEAP(AOT_ADD_FIELD_HEAP_UNDER_FIELD_HEAP)
    FOR_EACH_AOT_INDEXED_HEAP(AOT_ADD_INDEXED_HEAP)
#undef AOT_ADD_INDEXED_HEAP
#undef AOT_ADD_FIELD_HEAP_UNDER_FIELD_HEAP
#undef AOT_ADD_FIELD_HEAP_UNDER_IR_HEAP
#undef AOT_ADD_FIELD_HEAP_UNDER_ROOT
#undef AOT_ADD_HEAP_OUTSIDE_IR

    properties = Machine::NumberedAbstractHeap(m_tree, entire(IRHeap::NamedProperties).index(), "properties"_s);
    recordIfBelowEntireHeap(entire(IRHeap::NamedProperties), properties.atAnyNumber());

    JSCell_freeListNext = JSCell_header;
    ArrayStorage_publicLength = Butterfly_publicLength;
    ArrayStorage_vectorLength = Butterfly_vectorLength;
    JSString_value = JSRopeString_fiber0;
    JSWrapperObject_internalValue = JSInternalFieldObjectImpl_internalFields[static_cast<unsigned>(JSWrapperObject::Field::WrappedValue)];

    RELEASE_ASSERT(!(JSCell_indexingTypeAndMisc.offset() & (sizeof(int32_t) - 1)));
    RELEASE_ASSERT(JSCell_indexingTypeAndMisc.offset() + 1 == JSCell_typeInfoType.offset());
    RELEASE_ASSERT(JSCell_indexingTypeAndMisc.offset() + 2 == JSCell_typeInfoFlags.offset());
    RELEASE_ASSERT(JSCell_indexingTypeAndMisc.offset() + 3 == JSCell_cellState.offset());
    RELEASE_ASSERT(!JSCell_freeListNext.offset());
    RELEASE_ASSERT(forIndexingType(ArrayWithInt32) == forIndexingType(ArrayWithContiguous));
}

Machine::AbstractHeap AOTHeaps::addHeap(const Machine::AbstractHeap& parent, ASCIILiteral name, ptrdiff_t offset, Traits::Set traits)
{
    RELEASE_ASSERT(parent.isInitialized());
    return Machine::AbstractHeap(m_tree.add(parent.index(), name), offset, traits);
}

const Machine::IndexedAbstractHeap* AOTHeaps::forIndexingType(IndexingType indexingType) const
{
    switch (indexingType) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
        return nullptr;

    case ALL_INT32_INDEXING_TYPES:
        return &indexedContiguousProperties;

    case ALL_DOUBLE_INDEXING_TYPES:
        return &indexedDoubleProperties;

    case ALL_CONTIGUOUS_INDEXING_TYPES:
        return &indexedContiguousProperties;

    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        return &ArrayStorage_vector;

    default:
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }
}

const Machine::AbstractHeap& AOTHeaps::at(const Program& program, const MemoryLocation& location) const
{
    IRHeap heap = location.heap();
    if (location.isEntireHeap())
        return entire(heap);

    uint64_t mask = Typed::maskOf(program, location);
    if (heap == IRHeap::Elements)
        mask &= ~Typed::markOfElementsThatMayHoldCells;
    if (std::popcount(mask) != 1)
        return entire(heap);

    return bucketOf(heap, std::countr_zero(mask));
}

void AOTHeaps::appendHeaps(const LocationMasks& masks, HeapList& list) const
{
    if (masks.isTop()) {
        list.append(&root);
        return;
    }

    masks.forEach([&](IRHeap heap, uint64_t mask) {
        if (mask == LocationMasks::entireHeap) {
            list.append(&entire(heap));
            return;
        }
        if (heap == IRHeap::Elements)
            mask &= ~Typed::markOfElementsThatMayHoldCells;
        for (uint64_t remainingBits = mask; remainingBits; remainingBits &= remainingBits - 1)
            list.append(&bucketOf(heap, std::countr_zero(remainingBits)));
        for (const Machine::AbstractHeap* fieldHeap : m_fieldHeapsBelow[static_cast<size_t>(heap)])
            list.append(fieldHeap);
    });

    for (const SharedHeap& shared : sharedHeaps) {
        if (masks.containsAny(shared.otherOwners))
            list.append(&(this->*shared.heap));
    }
}

bool AOTHeaps::mayConvertCells(const Typed::Effects& effectsOfCall)
{
    constexpr Typed::AbstractHeapSet heapsOfObjects { IRHeap::NamedProperties, IRHeap::ObjectFields, IRHeap::ShapeWords, IRHeap::OrderWords, IRHeap::PrototypeLinks };
    return !effectsOfCall.writes.isEmpty() || effectsOfCall.mayCollect || effectsOfCall.mayExitSideways() || effectsOfCall.reads.containsAny(heapsOfObjects);
}

bool AOTHeaps::mayReplaceStorageOfArrays(const Typed::Effects& effectsOfCall)
{
    constexpr Typed::AbstractHeapSet heapsWrittenWithStorage { IRHeap::ArrayStorage, IRHeap::Elements, IRHeap::NamedProperties, IRHeap::PrototypeLinks };
    return effectsOfCall.mayRunOtherCode() || effectsOfCall.writes.containsAny(heapsWrittenWithStorage) || effectsOfCall.reads.contains(IRHeap::NamedProperties);
}

void AOTHeaps::appendHeapsOfConvertedCells(HeapList& list, ConvertedCells cells) const
{
    if (cells == ConvertedCells::All) {
        list.append(&JSCell_header);
        list.append(&JSObject_butterfly);
    } else {
        list.append(&JSCell_structureID);
        list.append(&JSNonArray_indexingTypeAndMisc);
        list.append(&JSCell_typeInfoFlags);
        list.append(&JSNonArray_butterfly);
    }
    list.append(&AOTSlimClosure_word);
    list.append(&AOTObject_shapeWord);
}

void AOTHeaps::appendHeapsWrittenByCollection(HeapList& list) const
{
    list.append(&collectorState);
    list.append(&weakTableStorage);
    list.append(&weakTableEntries);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
