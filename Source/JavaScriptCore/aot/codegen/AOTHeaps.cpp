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

struct SharedHeap {
    B3::AbstractHeap AOTHeaps::* heap;
    Typed::AbstractHeapSet otherOwners;
    bool isUnderIRHeap { true };
};

// A Structure transition rewrites the cell header, an out-of-line property moves the butterfly, and a bit of the order word says "has extra properties".
static constexpr SharedHeap sharedHeaps[] = {
    { &AOTHeaps::JSCell_header, { IRHeap::NamedProperties, IRHeap::PrototypeLinks, IRHeap::ArrayStorage } },
    { &AOTHeaps::JSObject_butterfly, { IRHeap::NamedProperties } },
    { &AOTHeaps::AOTObject_orderWord, { IRHeap::NamedProperties } },
    { &AOTHeaps::AOTSlimClosure_word, { IRHeap::NamedProperties, IRHeap::PrototypeLinks } },
    // To the IR a weak table is a collection like any other. A garbage collection only writes the weak ones, so they get a separate heap: values read from a Map or a Set stay valid across it.
    { &AOTHeaps::weakTableStorage, { IRHeap::CollectionStorage }, false },
    { &AOTHeaps::weakTableEntries, { IRHeap::CollectionEntries }, false },
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
    auto receivers = program.at(field.receivers()).all();
    if (!receivers || receivers->value().size() != 1)
        return { };
    return receivers->value().front();
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
        auto layouts = program.at(location.layoutSet()).all();
        if (!layouts || layouts->value().size() != 1)
            return std::nullopt;
        return keyOf(layouts->value().front().index(), 0);
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
    case IRHeap::CalleesInFrames:
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
    for (IRHeap heap : Typed::allHeaps)
        mutableEntire(heap).initialize(&root, nameOf(heap).characters());
    for (IRHeap heap : Typed::indexedHeaps) {
        for (Bucket& bucket : m_buckets[Typed::maskIndexOf(heap)])
            bucket.heap.initialize(&mutableEntire(heap), "bucket");
    }

#define AOT_NEST_FIELD_HEAP(name, offset, irHeap) name.changeParent(&mutableEntire(IRHeap::irHeap));
    FOR_EACH_AOT_MUTABLE_FIELD_HEAP(AOT_NEST_FIELD_HEAP)
#undef AOT_NEST_FIELD_HEAP
    for (B3::IndexedAbstractHeap* captures : { &AOTClosure_captures, &AOTSlimClosure_captures }) {
        nestAsImmutable(captures->atAnyIndex(), mutableEntire(IRHeap::EnvironmentFields));
        captures->retainCurrentHeaps();
    }

    nestUpstreamHeaps();
    for (Buckets& buckets : m_buckets) {
        for (Bucket& bucket : buckets)
            bucket.numberOfRetainedChildren = bucket.heap.numberOfChildren();
    }
    indexHeaps();
    keepRangesAcrossProcedures();
}

AOTHeaps::~AOTHeaps() = default;

AOTHeaps& AOTHeaps::of(B3::Procedure& procedure)
{
    return static_cast<AOTHeaps&>(procedure.heaps());
}

void AOTHeaps::nestUpstreamHeaps()
{
    auto nest = [](B3::AbstractHeap& parent, std::initializer_list<B3::AbstractHeap*> heaps) {
        for (B3::AbstractHeap* heap : heaps)
            heap->changeParent(&parent);
    };
    B3::AbstractHeap& elementsOfArrays = bucketOf(IRHeap::Elements, bucketIndexOf(ElementBase::Array)).heap;
    B3::AbstractHeap& elementsOfArguments = bucketOf(IRHeap::Elements, bucketIndexOf(ElementBase::Arguments)).heap;

    nest(mutableEntire(IRHeap::ShapeWords), { &JSCell_header });
    nest(mutableEntire(IRHeap::PrototypeLinks), { &Structure_prototype });
    nest(mutableEntire(IRHeap::ArrayStorage), { &JSObject_butterfly, &Butterfly_publicLength, &Butterfly_vectorLength, &ArrayStorage_numValuesInVector });
    // An array stays an array, so an array's butterfly word and another object's are separate heaps. JSObject_butterfly is for an object that may be either.
    nest(JSObject_butterfly, { &JSArray_butterfly, &JSNonArray_butterfly });
    nest(mutableEntire(IRHeap::TypedArrayStorage), {
        &JSArrayBufferView_byteOffset, &JSArrayBufferView_length, &JSArrayBufferView_mode, &JSArrayBufferView_vector, &ArrayBuffer_data, &Butterfly_arrayBuffer,
    });
    nest(mutableEntire(IRHeap::TypedArrayElements), { &TypedArrayProperties });
    nest(mutableEntire(IRHeap::CollectionStorage), { &JSMap_storage, &JSSet_storage });
    nest(mutableEntire(IRHeap::InternalFields), { &DateInstance_internalNumber, &DateInstance_cachedGregorianDateTime, &DateInstance_cachedGregorianDateTimeUTC });
    nest(mutableEntire(IRHeap::RegExpState), {
        &RegExp_flags, &RegExp_minimumSize, &RegExpObject_regExpAndFlags, &RegExpObject_lastIndex,
        &JSGlobalObject_regExpGlobalData_cachedResult_lastRegExp, &JSGlobalObject_regExpGlobalData_cachedResult_lastInput,
        &JSGlobalObject_regExpGlobalData_cachedResult_result_start, &JSGlobalObject_regExpGlobalData_cachedResult_result_end,
        &JSGlobalObject_regExpGlobalData_cachedResult_reified, &JSGlobalObject_regExpGlobalData_cachedResult_oneCharacterMatch,
    });
    nest(mutableEntire(IRHeap::PromiseState), { &JSPromise_packed, &JSPromise_slot });
    nest(mutableEntire(IRHeap::StringStorage), { &JSRopeString_fiber0, &JSRopeString_fiber1, &JSRopeString_fiber2, &StringImpl_data, &StringImpl_hashAndFlags, &StringImpl_length });
    nest(weakTableStorage, { &WeakMapImpl_capacity, &WeakMapImpl_buffer, &WeakMapImpl_keyCount, &WeakMapImpl_deleteCount });
    nest(weakTableEntries, { &WeakMapBucket_key, &WeakMapBucket_value });
    nest(mutableEntire(IRHeap::InternalFields), { &JSInternalFieldObjectImpl_internalFields.atAnyIndex() });
    nest(mutableEntire(IRHeap::AllocatorState), { &CompleteSubspace_allocatorForSizeStep.atAnyIndex() });
    nest(mutableEntire(IRHeap::StringStorage), { &characters8.atAnyIndex(), &characters16.atAnyIndex() });
    nest(mutableEntire(IRHeap::NamedProperties), { &properties.atAnyNumber() });
    nest(elementsOfArguments, {
        &DirectArguments_callee, &DirectArguments_length, &DirectArguments_minCapacity, &DirectArguments_mappedArguments, &DirectArguments_modifiedArgumentsDescriptor,
        &ScopedArguments_overrodeThings, &ScopedArguments_scope, &ScopedArguments_storage, &ScopedArguments_table, &ScopedArguments_totalLength,
        &ScopedArgumentsTable_arguments, &ScopedArgumentsTable_length,
    });
    nest(elementsOfArrays, {
        &indexedInt32Properties.atAnyIndex(), &indexedDoubleProperties.atAnyIndex(), &indexedContiguousProperties.atAnyIndex(), &ArrayStorage_vector.atAnyIndex(),
    });
    nest(elementsOfArguments, { &DirectArguments_storage.atAnyIndex(), &ScopedArguments_Storage_storage.atAnyIndex(), &scopedArgumentsTableArguments.atAnyIndex() });
    nest(collectorState, { &JSCell_cellState, &cards, &VM_heapState, &WriteBarrierBuffer_bufferContents.atAnyIndex() });

    UncheckedKeyHashSet<const B3::AbstractHeap*> heapsThatStayUnderRoot;
#define AOT_KEEP_UNDER_ROOT(name, ...) heapsThatStayUnderRoot.add(&name);
    FOR_EACH_AOT_HEAP_OUTSIDE_IR(AOT_KEEP_UNDER_ROOT)
    FOR_EACH_AOT_IMMUTABLE_FIELD_HEAP(AOT_KEEP_UNDER_ROOT)
#undef AOT_KEEP_UNDER_ROOT
    for (const B3::AbstractHeap& heap : m_entireHeaps)
        heapsThatStayUnderRoot.add(&heap);

    // JSCell_typeInfoType does change in place, between the types of a row of cellTypeChangesInPlace(): PrimitiveEmitter::hasCellTypeIn() keeps every test under this heap to ranges that cannot tell.
    for (B3::AbstractHeap* heap : { &JSCell_typeInfoType, &Structure_classInfo, &Structure_realm, &ClassInfo_parentClass }) {
        RELEASE_ASSERT(heap->mutability() == B3::Mutability::Immutable);
        heap->changeParent(&root);
        heapsThatStayUnderRoot.add(heap);
    }
    // The repository declares these mutable, yet nothing writes them once their owner is created.
    B3::AbstractHeap* const heapsFixedAtCreation[] = {
        &JSPropertyNameEnumerator_cachedInlineCapacity, &JSPropertyNameEnumerator_cachedPropertyNamesVector, &JSPropertyNameEnumerator_cachedStructureID,
        &JSPropertyNameEnumerator_endGenericPropertyIndex, &JSPropertyNameEnumerator_endStructurePropertyIndex, &JSPropertyNameEnumerator_indexLength,
        &JSPropertyNameEnumerator_cachedPropertyNamesVectorContents.atAnyIndex(), &singleCharacterStrings.atAnyIndex(),
    };
    for (B3::AbstractHeap* heap : heapsFixedAtCreation) {
        nestAsImmutable(*heap, root);
        heapsThatStayUnderRoot.add(heap);
    }

    Vector<B3::AbstractHeap*> children = root.children();
    for (B3::AbstractHeap* child : children) {
        if (!heapsThatStayUnderRoot.contains(child))
            child->changeParent(&mutableEntire(IRHeap::RuntimeState));
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

// An array's storage is replaced when its elements or length are written, when it gains or loses a property or becomes a prototype, and when a lookup by name reorganizes the properties of an object it visits.
// A garbage collection does not move storage while a register or a stack word points into it.
bool AOTHeaps::mayReplaceStorageOfArrays(const Typed::Effects& effectsOfCall)
{
    constexpr Typed::AbstractHeapSet heapsWrittenWithStorage { IRHeap::ArrayStorage, IRHeap::Elements, IRHeap::NamedProperties, IRHeap::PrototypeLinks };
    return effectsOfCall.mayRunOtherCode() || effectsOfCall.writes.containsAny(heapsWrittenWithStorage) || effectsOfCall.reads.contains(IRHeap::NamedProperties);
}

void AOTHeaps::appendHeapsOfConvertedCells(B3HeapList& list, ConvertedCells cells) const
{
    list.append(&JSCell_header);
    list.append(cells == ConvertedCells::All ? &JSObject_butterfly : &JSNonArray_butterfly);
    list.append(&AOTSlimClosure_word);
}

// The end of a collection removes the dead entries of weak tables, and may replace their buffers.
void AOTHeaps::appendHeapsWrittenByCollection(B3HeapList& list) const
{
    list.append(&collectorState);
    list.append(&weakTableStorage);
    list.append(&weakTableEntries);
}

bool AOTHeaps::mayCollect(const B3::Effects& effects) const
{
    return effects.writes.overlaps(rangeOf(collectorState)) && effects.writes.overlaps(rangeOf(weakTableStorage));
}

B3::HeapRange AOTHeaps::rangeWrittenByWhateverMayCollect() const
{
    return rangeOf(weakTableStorage);
}

bool AOTHeaps::isFreedWithOwner(const B3::HeapRange& range) const
{
    for (const B3::AbstractHeap* heap : { &StringImpl_data, &StringImpl_hashAndFlags, &StringImpl_length, &characters8.atAnyIndex(), &characters16.atAnyIndex(), &ArrayBuffer_shared, &ArrayBuffer_sizeInBytes }) {
        if (range.overlaps(rangeOf(*heap)))
            return true;
    }
    return false;
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
        if (!isWithin(written, this->*shared.heap))
            continue;
        if (declaredWrites.containsAny(shared.otherOwners))
            return true;
        if (!shared.isUnderIRHeap)
            return false;
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
    for (ExactHeaps& heaps : m_exactHeaps) {
        for (auto& exactHeap : heaps.values())
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
