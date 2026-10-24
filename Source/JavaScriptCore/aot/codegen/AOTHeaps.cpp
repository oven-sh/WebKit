/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeaps.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTIR.h"
#include "AOTInstance.h"
#include "AOTVector.h"
#include "B3Procedure.h"
#include "BrandedStructure.h"
#include "JSGlobalObject.h"
#include <algorithm>
#include <bit>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT {

using Typed::ElementBase;
using Typed::LocationMasks;
using Typed::MemoryLocation;
using Typed::Program;
using IRHeap = Typed::AbstractHeap;
using Repository = B3::AbstractHeapRepository;

template<typename Heap, typename Parent>
struct NestedHeap {
    Heap Repository::* heap;
    Parent parent;
};

static constexpr NestedHeap<B3::AbstractHeap, IRHeap> nestedFieldHeaps[] = {
    { &Repository::JSCell_header, IRHeap::ShapeWords },
    { &Repository::Structure_prototype, IRHeap::PrototypeLinks },
    { &Repository::JSObject_butterfly, IRHeap::ArrayStorage },
    { &Repository::Butterfly_publicLength, IRHeap::ArrayStorage },
    { &Repository::Butterfly_vectorLength, IRHeap::ArrayStorage },
    { &Repository::ArrayStorage_numValuesInVector, IRHeap::ArrayStorage },
    { &Repository::JSArrayBufferView_byteOffset, IRHeap::TypedArrayStorage },
    { &Repository::JSArrayBufferView_length, IRHeap::TypedArrayStorage },
    { &Repository::JSArrayBufferView_mode, IRHeap::TypedArrayStorage },
    { &Repository::JSArrayBufferView_vector, IRHeap::TypedArrayStorage },
    { &Repository::ArrayBuffer_data, IRHeap::TypedArrayStorage },
    { &Repository::Butterfly_arrayBuffer, IRHeap::TypedArrayStorage },
    { &Repository::TypedArrayProperties, IRHeap::TypedArrayElements },
    { &Repository::JSMap_storage, IRHeap::CollectionStorage },
    { &Repository::JSSet_storage, IRHeap::CollectionStorage },
    { &Repository::DateInstance_internalNumber, IRHeap::InternalFields },
    { &Repository::DateInstance_cachedGregorianDateTime, IRHeap::InternalFields },
    { &Repository::DateInstance_cachedGregorianDateTimeUTC, IRHeap::InternalFields },
    { &Repository::RegExp_flags, IRHeap::RegExpState },
    { &Repository::RegExp_minimumSize, IRHeap::RegExpState },
    { &Repository::RegExpObject_regExpAndFlags, IRHeap::RegExpState },
    { &Repository::RegExpObject_lastIndex, IRHeap::RegExpState },
    { &Repository::JSGlobalObject_regExpGlobalData_cachedResult_lastRegExp, IRHeap::RegExpState },
    { &Repository::JSGlobalObject_regExpGlobalData_cachedResult_lastInput, IRHeap::RegExpState },
    { &Repository::JSGlobalObject_regExpGlobalData_cachedResult_result_start, IRHeap::RegExpState },
    { &Repository::JSGlobalObject_regExpGlobalData_cachedResult_result_end, IRHeap::RegExpState },
    { &Repository::JSGlobalObject_regExpGlobalData_cachedResult_reified, IRHeap::RegExpState },
    { &Repository::JSGlobalObject_regExpGlobalData_cachedResult_oneCharacterMatch, IRHeap::RegExpState },
    { &Repository::JSPromise_packed, IRHeap::PromiseState },
    { &Repository::JSPromise_slot, IRHeap::PromiseState },
    { &Repository::JSRopeString_fiber0, IRHeap::StringStorage },
    { &Repository::JSRopeString_fiber1, IRHeap::StringStorage },
    { &Repository::JSRopeString_fiber2, IRHeap::StringStorage },
    { &Repository::StringImpl_data, IRHeap::StringStorage },
    { &Repository::StringImpl_hashAndFlags, IRHeap::StringStorage },
    { &Repository::StringImpl_length, IRHeap::StringStorage },
};

static constexpr NestedHeap<B3::AbstractHeap, B3::AbstractHeap AOTHeaps::*> weakTableHeaps[] = {
    { &Repository::WeakMapImpl_capacity, &AOTHeaps::weakTableStorage },
    { &Repository::WeakMapImpl_buffer, &AOTHeaps::weakTableStorage },
    { &Repository::WeakMapImpl_keyCount, &AOTHeaps::weakTableStorage },
    { &Repository::WeakMapImpl_deleteCount, &AOTHeaps::weakTableStorage },
    { &Repository::WeakMapBucket_key, &AOTHeaps::weakTableEntries },
    { &Repository::WeakMapBucket_value, &AOTHeaps::weakTableEntries },
};

static constexpr NestedHeap<B3::IndexedAbstractHeap, IRHeap> nestedIndexedHeaps[] = {
    { &Repository::JSInternalFieldObjectImpl_internalFields, IRHeap::InternalFields },
    { &Repository::CompleteSubspace_allocatorForSizeStep, IRHeap::AllocatorState },
    { &Repository::characters8, IRHeap::StringStorage },
    { &Repository::characters16, IRHeap::StringStorage },
};

static constexpr NestedHeap<B3::NumberedAbstractHeap, IRHeap> nestedNumberedHeaps[] = {
    { &Repository::properties, IRHeap::NamedProperties },
};

static constexpr NestedHeap<B3::AbstractHeap, ElementBase> elementFieldHeaps[] = {
    { &Repository::DirectArguments_callee, ElementBase::Arguments },
    { &Repository::DirectArguments_length, ElementBase::Arguments },
    { &Repository::DirectArguments_minCapacity, ElementBase::Arguments },
    { &Repository::DirectArguments_mappedArguments, ElementBase::Arguments },
    { &Repository::DirectArguments_modifiedArgumentsDescriptor, ElementBase::Arguments },
    { &Repository::ScopedArguments_overrodeThings, ElementBase::Arguments },
    { &Repository::ScopedArguments_scope, ElementBase::Arguments },
    { &Repository::ScopedArguments_storage, ElementBase::Arguments },
    { &Repository::ScopedArguments_table, ElementBase::Arguments },
    { &Repository::ScopedArguments_totalLength, ElementBase::Arguments },
    { &Repository::ScopedArgumentsTable_arguments, ElementBase::Arguments },
    { &Repository::ScopedArgumentsTable_length, ElementBase::Arguments },
};

static constexpr NestedHeap<B3::IndexedAbstractHeap, ElementBase> elementIndexedHeaps[] = {
    { &Repository::indexedInt32Properties, ElementBase::Array },
    { &Repository::indexedDoubleProperties, ElementBase::Array },
    { &Repository::indexedContiguousProperties, ElementBase::Array },
    { &Repository::ArrayStorage_vector, ElementBase::Array },
    { &Repository::DirectArguments_storage, ElementBase::Arguments },
    { &Repository::ScopedArguments_Storage_storage, ElementBase::Arguments },
    { &Repository::scopedArgumentsTableArguments, ElementBase::Arguments },
};

static constexpr B3::AbstractHeap Repository::* immutableFieldHeaps[] = {
    &Repository::JSCell_typeInfoType,
    &Repository::Structure_classInfo,
    &Repository::Structure_realm,
    &Repository::ClassInfo_parentClass,
};

// The repository declares these mutable, yet nothing writes them once the enumerator is created.
static constexpr B3::AbstractHeap Repository::* fieldHeapsFixedAtCreation[] = {
    &Repository::JSPropertyNameEnumerator_cachedInlineCapacity,
    &Repository::JSPropertyNameEnumerator_cachedPropertyNamesVector,
    &Repository::JSPropertyNameEnumerator_cachedStructureID,
    &Repository::JSPropertyNameEnumerator_endGenericPropertyIndex,
    &Repository::JSPropertyNameEnumerator_endStructurePropertyIndex,
    &Repository::JSPropertyNameEnumerator_indexLength,
};

static constexpr B3::IndexedAbstractHeap Repository::* immutableIndexedHeaps[] = {
    &Repository::JSPropertyNameEnumerator_cachedPropertyNamesVectorContents,
    &Repository::singleCharacterStrings,
};

struct SharedHeap {
    B3::AbstractHeap AOTHeaps::* heap;
    Typed::AbstractHeapSet otherOwners;
};

// A Structure transition rewrites the cell header, an out-of-line property moves the butterfly, and a bit of the order word says "has extra properties".
static constexpr SharedHeap sharedHeaps[] = {
    { &AOTHeaps::JSCell_header, { IRHeap::NamedProperties, IRHeap::PrototypeLinks, IRHeap::ArrayStorage } },
    { &AOTHeaps::JSObject_butterfly, { IRHeap::NamedProperties } },
    { &AOTHeaps::AOTObject_orderWord, { IRHeap::NamedProperties } },
    { &AOTHeaps::AOTSlimClosure_word, { IRHeap::NamedProperties, IRHeap::PrototypeLinks } },
};

static void nestAsImmutable(B3::AbstractHeap& heap, B3::AbstractHeap& parent)
{
    heap.initialize(&parent, heap.heapName(), heap.offset(), B3::Mutability::Immutable);
}

static bool isWithin(const B3::AbstractHeap& heap, const B3::AbstractHeap& ancestor)
{
    for (const B3::AbstractHeap* current = &heap; current; current = current->parent()) {
        if (current == &ancestor)
            return true;
    }
    return false;
}

static unsigned bucketIndexOf(ElementBase base)
{
    LocationMasks masks;
    masks.add(MemoryLocation::elements(base));
    uint64_t mask = masks.maskOf(IRHeap::Elements);
    RELEASE_ASSERT(std::popcount(mask) == 1);
    return std::countr_zero(mask);
}

// Objects of two layouts are two objects. A set of several layouts, or one with members that have no layout, may meet any other set.
static Typed::LayoutID onlyReceiverOf(const Program& program, const MemoryLocation& field)
{
    if (!field.receivers())
        return { };
    const Typed::LayoutSet& receivers = program.at(field.receivers());
    if (!receivers.isComplete || receivers.hasMembersWithoutLayout || receivers.members.size() != 1)
        return { };
    return receivers.members[0];
}

static std::optional<uint64_t> exactKeyOf(const Program& program, const MemoryLocation& location)
{
    auto keyOf = [](uint32_t first, uint32_t second) {
        return static_cast<uint64_t>(first) << 32 | second;
    };

    switch (location.heap()) {
    case IRHeap::ModuleVariables:
    case IRHeap::EnvironmentFields:
    case IRHeap::MemberGuards:
    case IRHeap::NamedProperties:
        return keyOf(location.firstIndex(), location.secondIndex());
    case IRHeap::ObjectFields:
        return keyOf(location.key().index(), onlyReceiverOf(program, location).index());
    case IRHeap::ShapeWords: {
        const Typed::LayoutSet& layouts = program.at(location.layoutSet());
        if (layouts.members.size() != 1)
            return std::nullopt;
        return keyOf(layouts.members[0].index(), 0);
    }
    case IRHeap::Elements:
        return std::nullopt;
    case IRHeap::OrderWords:
    case IRHeap::PrototypeLinks:
    case IRHeap::ArrayStorage:
    case IRHeap::TypedArrayStorage:
    case IRHeap::TypedArrayElements:
    case IRHeap::CollectionStorage:
    case IRHeap::CollectionEntries:
    case IRHeap::InternalFields:
    case IRHeap::RegExpState:
    case IRHeap::PromiseState:
    case IRHeap::StringStorage:
    case IRHeap::StringBuilderContents:
    case IRHeap::ParserState:
    case IRHeap::ExternalState:
    case IRHeap::AllocatorState:
    case IRHeap::RuntimeState:
    case IRHeap::ValidationState:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

AOTHeaps::AOTHeaps()
    : B3::AbstractHeapRepository()
#define AOT_INITIALIZE_HEAP(name) , name(&root, #name)
    FOR_EACH_AOT_HEAP_OUTSIDE_IR(AOT_INITIALIZE_HEAP)
#undef AOT_INITIALIZE_HEAP
#define AOT_INITIALIZE_HEAP(name, offset) , name(&root, #name, offset, B3::Mutability::Immutable)
    FOR_EACH_AOT_IMMUTABLE_FIELD_HEAP(AOT_INITIALIZE_HEAP)
#undef AOT_INITIALIZE_HEAP
#define AOT_INITIALIZE_HEAP(name, offset, irHeap) , name(&root, #name, offset)
    FOR_EACH_AOT_MUTABLE_FIELD_HEAP(AOT_INITIALIZE_HEAP)
#undef AOT_INITIALIZE_HEAP
    , AOTClosure_captures(&root, "AOTClosure_captures", offsetOfClosureCaptures, sizeof(EncodedJSValue))
    , AOTSlimClosure_captures(&root, "AOTSlimClosure_captures", offsetOfSlimClosureCaptures, sizeof(EncodedJSValue))
{
    initializeEntireHeaps();
    initializeBuckets();
    initializeFieldHeaps();
    nestUpstreamHeaps();
    nestUnclaimedHeaps();
    retainBucketChildren();
    indexHeaps();
    keepRangesAcrossProcedures();
}

AOTHeaps::~AOTHeaps() = default;

AOTHeaps& AOTHeaps::of(B3::Procedure& procedure)
{
    return static_cast<AOTHeaps&>(procedure.heaps());
}

void AOTHeaps::initializeEntireHeaps()
{
    for (IRHeap heap : Typed::allHeaps)
        mutableEntire(heap).initialize(&root, nameOf(heap).characters());
}

void AOTHeaps::initializeBuckets()
{
    for (IRHeap heap : Typed::indexedHeaps) {
        for (Bucket& bucket : m_buckets[Typed::maskIndexOf(heap)])
            bucket.heap.initialize(&mutableEntire(heap), "bucket");
    }
}

void AOTHeaps::initializeFieldHeaps()
{
#define AOT_NEST_FIELD_HEAP(name, offset, irHeap) name.changeParent(&mutableEntire(IRHeap::irHeap));
    FOR_EACH_AOT_MUTABLE_FIELD_HEAP(AOT_NEST_FIELD_HEAP)
#undef AOT_NEST_FIELD_HEAP

    for (B3::IndexedAbstractHeap* captures : { &AOTClosure_captures, &AOTSlimClosure_captures }) {
        nestAsImmutable(captures->atAnyIndex(), mutableEntire(IRHeap::EnvironmentFields));
        captures->retainCurrentHeaps();
    }
}

void AOTHeaps::nestUpstreamHeaps()
{
    for (const auto& row : nestedFieldHeaps)
        (this->*row.heap).changeParent(&mutableEntire(row.parent));
    for (const auto& row : weakTableHeaps)
        (this->*row.heap).changeParent(&(this->*row.parent));
    for (const auto& row : nestedIndexedHeaps)
        (this->*row.heap).atAnyIndex().changeParent(&mutableEntire(row.parent));
    for (const auto& row : nestedNumberedHeaps)
        (this->*row.heap).atAnyNumber().changeParent(&mutableEntire(row.parent));
    for (const auto& row : elementFieldHeaps)
        (this->*row.heap).changeParent(&bucketOf(IRHeap::Elements, bucketIndexOf(row.parent)).heap);
    for (const auto& row : elementIndexedHeaps)
        (this->*row.heap).atAnyIndex().changeParent(&bucketOf(IRHeap::Elements, bucketIndexOf(row.parent)).heap);

    JSCell_cellState.changeParent(&collectorState);
    cards.changeParent(&collectorState);
    VM_heapState.changeParent(&collectorState);
    WriteBarrierBuffer_bufferContents.atAnyIndex().changeParent(&collectorState);

    for (auto heap : immutableFieldHeaps) {
        RELEASE_ASSERT((this->*heap).mutability() == B3::Mutability::Immutable);
        (this->*heap).changeParent(&root);
    }
    for (auto heap : fieldHeapsFixedAtCreation)
        nestAsImmutable(this->*heap, root);
    for (auto heap : immutableIndexedHeaps)
        nestAsImmutable((this->*heap).atAnyIndex(), root);
}

void AOTHeaps::nestUnclaimedHeaps()
{
    UncheckedKeyHashSet<const B3::AbstractHeap*> claimedHeaps;
#define AOT_CLAIM_HEAP(name, ...) claimedHeaps.add(&name);
    FOR_EACH_AOT_HEAP_OUTSIDE_IR(AOT_CLAIM_HEAP)
    FOR_EACH_AOT_IMMUTABLE_FIELD_HEAP(AOT_CLAIM_HEAP)
#undef AOT_CLAIM_HEAP
    for (const B3::AbstractHeap& heap : m_entireHeaps)
        claimedHeaps.add(&heap);
    for (auto heap : immutableFieldHeaps)
        claimedHeaps.add(&(this->*heap));
    for (auto heap : fieldHeapsFixedAtCreation)
        claimedHeaps.add(&(this->*heap));
    for (auto heap : immutableIndexedHeaps)
        claimedHeaps.add(&(this->*heap).atAnyIndex());

    Vector<B3::AbstractHeap*> children = root.children();
    for (B3::AbstractHeap* child : children) {
        if (!claimedHeaps.contains(child))
            child->changeParent(&mutableEntire(IRHeap::RuntimeState));
    }
}

void AOTHeaps::retainBucketChildren()
{
    for (Buckets& buckets : m_buckets) {
        for (Bucket& bucket : buckets)
            bucket.numberOfRetainedChildren = bucket.heap.numberOfChildren();
    }
}

void AOTHeaps::indexHeaps()
{
    for (IRHeap heap : Typed::allHeaps) {
        m_locationsOfHeap.add(&entire(heap), CoveredLocations { heap, LocationMasks::entireHeap });
        if (!Typed::indexedHeaps.contains(heap))
            continue;
        for (unsigned bit = 0; bit < numberOfBuckets; ++bit) {
            const B3::AbstractHeap& bucket = bucketOf(heap, bit).heap;
            RELEASE_ASSERT(entire(heap).children()[bit] == &bucket);
            m_locationsOfHeap.add(&bucket, CoveredLocations { heap, uint64_t { 1 } << bit });
        }
    }
}

auto AOTHeaps::bucketOf(IRHeap heap, unsigned bit) -> Bucket&
{
    return m_buckets[Typed::maskIndexOf(heap)][bit];
}

auto AOTHeaps::bucketOf(IRHeap heap, unsigned bit) const -> const Bucket&
{
    return m_buckets[Typed::maskIndexOf(heap)][bit];
}

const B3::AbstractHeap& AOTHeaps::at(const Program& program, const MemoryLocation& location)
{
    IRHeap heap = location.heap();
    if (location.isEntireHeap())
        return entire(heap);

    uint64_t mask = Typed::maskOf(program, location);
    if (std::popcount(mask) != 1)
        return entire(heap);

    Bucket& bucket = bucketOf(heap, std::countr_zero(mask));
    std::optional<uint64_t> key = exactKeyOf(program, location);
    if (!key)
        return bucket.heap;

    auto ensureHeap = [&](uint64_t exactKey, B3::AbstractHeap& parent) -> B3::AbstractHeap& {
        return *m_exactHeaps[Typed::maskIndexOf(heap)].ensure(exactKey, [&] {
            if (bucket.heap.numberOfChildren() == bucket.numberOfRetainedChildren)
                m_bucketsWithExactHeaps.append(&bucket);
            return makeUnique<B3::AbstractHeap>(&parent, "location");
        }).iterator->value;
    };
    if (heap != IRHeap::ObjectFields || !onlyReceiverOf(program, location))
        return ensureHeap(*key, bucket.heap);
    B3::AbstractHeap& fieldOfEveryLayout = ensureHeap(*exactKeyOf(program, MemoryLocation::objectField(location.key())), bucket.heap);
    return ensureHeap(*key, fieldOfEveryLayout);
}

void AOTHeaps::appendHeaps(const LocationMasks& masks, B3HeapList& list) const
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
        for (uint64_t remainingBits = mask; remainingBits; remainingBits &= remainingBits - 1)
            list.append(&bucketOf(heap, std::countr_zero(remainingBits)).heap);
        for (const B3::AbstractHeap* fieldHeap : entire(heap).children().span().subspan(numberOfBuckets))
            list.append(fieldHeap);
    });

    for (const SharedHeap& shared : sharedHeaps) {
        if (masks.containsAny(shared.otherOwners))
            list.append(&(this->*shared.heap));
    }
}

// The effects of the IR say what a program can tell.
bool AOTHeaps::mayConvertCells(const Typed::Effects& effectsOfCall)
{
    constexpr Typed::AbstractHeapSet heapsOfObjects { IRHeap::NamedProperties, IRHeap::ObjectFields, IRHeap::ShapeWords, IRHeap::OrderWords, IRHeap::PrototypeLinks };
    return !effectsOfCall.writes.isEmpty() || effectsOfCall.mayCollect || effectsOfCall.mayExitSideways() || effectsOfCall.reads.containsAny(heapsOfObjects);
}

void AOTHeaps::appendHeapsOfConvertedCells(B3HeapList& list) const
{
    list.append(&JSCell_header);
    list.append(&JSObject_butterfly);
    list.append(&AOTSlimClosure_word);
}

// The end of a collection removes the dead entries of weak tables, and may replace their buffers.
void AOTHeaps::appendHeapsWrittenByCollection(B3HeapList& list) const
{
    list.append(&collectorState);
    list.append(&weakTableStorage);
    list.append(&weakTableEntries);
}

std::optional<CoveredLocations> AOTHeaps::locationsOf(const B3::AbstractHeap& heap) const
{
    for (const B3::AbstractHeap* current = &heap; current; current = current->parent()) {
        if (std::optional<CoveredLocations> locations = m_locationsOfHeap.getOptional(current))
            return locations;
    }
    return std::nullopt;
}

bool AOTHeaps::isCoveredBy(const B3::AbstractHeap& written, const LocationMasks& declaredWrites) const
{
    for (const SharedHeap& shared : sharedHeaps) {
        if (declaredWrites.containsAny(shared.otherOwners) && isWithin(written, this->*shared.heap))
            return true;
    }

    std::optional<CoveredLocations> locations = locationsOf(written);
    if (!locations) {
#define AOT_RETURN_IF_WITHIN(name) \
        if (isWithin(written, name)) \
            return true;
        FOR_EACH_AOT_HEAP_OUTSIDE_IR(AOT_RETURN_IF_WITHIN)
#undef AOT_RETURN_IF_WITHIN
        return false;
    }

    uint64_t declaredMask = declaredWrites.maskOf(locations->heap);
    bool isFieldHeap = locations->mask == LocationMasks::entireHeap && &written != &entire(locations->heap);
    if (isFieldHeap)
        return !!declaredMask;
    return (declaredMask & locations->mask) == locations->mask;
}

void AOTHeaps::removeTemporaryClientHeaps()
{
    for (unsigned index = 0; index < Typed::numberOfIndexedHeaps; ++index) {
        if (m_exactHeaps[index].isEmpty())
            continue;
        for (auto& exactHeap : m_exactHeaps[index].values())
            exactHeap->removeChildrenFrom(0);
    }
    for (Bucket* bucket : m_bucketsWithExactHeaps)
        bucket->heap.removeChildrenFrom(bucket->numberOfRetainedChildren);
    m_bucketsWithExactHeaps.shrink(0);
    for (ExactHeaps& heaps : m_exactHeaps)
        heaps.clear();
    AOTClosure_captures.removeTemporaryHeaps();
    AOTSlimClosure_captures.removeTemporaryHeaps();
}

bool AOTHeaps::hasTemporaryClientHeaps() const
{
    return AOTClosure_captures.hasTemporaryHeaps()
        || AOTSlimClosure_captures.hasTemporaryHeaps()
        || std::ranges::any_of(m_exactHeaps, [](const ExactHeaps& heaps) { return !heaps.isEmpty(); });
}

void AOTHeaps::appendGrownClientHeaps(WTF::Vector<GrownHeap, 16>& grownHeaps)
{
    for (Bucket* bucket : m_bucketsWithExactHeaps)
        grownHeaps.append({ &bucket->heap, bucket->numberOfRetainedChildren });
    for (B3::IndexedAbstractHeap* captures : { &AOTClosure_captures, &AOTSlimClosure_captures }) {
        if (captures->hasTemporaryHeaps())
            grownHeaps.append({ &captures->atAnyIndex(), captures->numberOfRetainedHeaps() });
    }
}

std::unique_ptr<B3::AbstractHeapRepository> createAOTHeaps()
{
    return makeUnique<AOTHeaps>();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
