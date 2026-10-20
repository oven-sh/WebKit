/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTHeaps.h"
#include "AOTIRTestSupport.h"
#include "AOTNativeTest.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlockInlines.h"
#include "B3CompilationScratch.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include "GPRInfo.h"
#include <bit>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT {

using Typed::ElementBase;
using Typed::LayoutID;
using Typed::LayoutSetID;
using Typed::LocationMasks;
using Typed::MemoryLocation;
using Typed::PropertyKeyID;
using Typed::VariableID;
using IRHeap = Typed::AbstractHeap;

namespace {

struct HeapFixture {
    uint64_t maskOf(const MemoryLocation& location) const { return Typed::maskOf(program.program, location); }
    const B3::AbstractHeap& at(const MemoryLocation& location) { return heaps.at(program.program, location); }

    const B3::AbstractHeap& bucket(IRHeap heap, unsigned bit) const
    {
        LocationMasks masks;
        masks.add(heap, uint64_t { 1 } << bit);
        B3HeapList list;
        heaps.appendHeaps(masks, list);
        AOT_TEST_CHECK(!list.isEmpty());
        return *list[0];
    }

    const B3::AbstractHeap& bucketOf(const MemoryLocation& location) const
    {
        uint64_t mask = maskOf(location);
        AOT_TEST_CHECK(std::popcount(mask) == 1);
        return bucket(location.heap(), std::countr_zero(mask));
    }

    LocationMasks masksOf(std::initializer_list<MemoryLocation> locations) const
    {
        LocationMasks masks;
        for (const MemoryLocation& location : locations)
            masks.add(program.program, location);
        return masks;
    }

    LayoutSetID layoutSet(std::initializer_list<uint32_t> members, bool isComplete = true)
    {
        Typed::LayoutSet set;
        for (uint32_t member : members)
            set.members.append(LayoutID(member));
        set.isComplete = isComplete;
        return program.program.types.intern(WTF::move(set));
    }

    B3::MemoryValue* appendLoad(const B3::AbstractHeap& heap)
    {
        B3::Value* base = root->appendNew<B3::ArgumentRegValue>(proc, B3::Origin(), GPRInfo::argumentGPR0);
        B3::MemoryValue* load = root->appendNew<B3::MemoryValue>(proc, B3::Load, B3::Int64, B3::Origin(), base, 0);
        heaps.decorateMemory(&heap, load);
        return load;
    }

    B3::PatchpointValue* appendPatchpointWriting(const LocationMasks& masks)
    {
        B3::PatchpointValue* patchpoint = root->appendNew<B3::PatchpointValue>(proc, B3::Void, B3::Origin());
        B3HeapList list;
        heaps.appendHeaps(masks, list);
        heaps.decoratePatchpointWrite(list.span(), patchpoint);
        return patchpoint;
    }

    Typed::TestProgram program;
    B3::CompilationScratch scratch { createAOTHeaps };
    B3::Procedure proc { B3::Policy::fromProcessOptions(), &scratch };
    AOTHeaps& heaps { AOTHeaps::of(proc) };
    B3::BasicBlock* root { proc.addBlock() };
};

using HeapPointers = std::initializer_list<const B3::AbstractHeap*>;

struct KeyPair {
    PropertyKeyID first;
    PropertyKeyID second;
};

} // anonymous namespace

static bool isWithin(const B3::AbstractHeap& heap, const B3::AbstractHeap& ancestor)
{
    for (const B3::AbstractHeap* current = &heap; current; current = current->parent()) {
        if (current == &ancestor)
            return true;
    }
    return false;
}

static bool covers(const AOTHeaps& heaps, const B3::AbstractHeap& heap, IRHeap expectedHeap, uint64_t expectedMask)
{
    std::optional<CoveredLocations> locations = heaps.locationsOf(heap);
    return locations && locations->heap == expectedHeap && locations->mask == expectedMask;
}

static bool contains(B3::HeapRange outer, B3::HeapRange inner)
{
    return outer.begin() <= inner.begin() && inner.end() <= outer.end();
}

static KeyPair findKeys(const HeapFixture& fixture, bool inOneBucket)
{
    PropertyKeyID first(1u);
    uint64_t mask = fixture.maskOf(MemoryLocation::objectField(first));
    for (uint32_t index = 2; index < 4096; ++index) {
        PropertyKeyID second(index);
        if ((fixture.maskOf(MemoryLocation::objectField(second)) == mask) == inOneBucket)
            return { first, second };
    }
    AOT_TEST_CHECK(false);
    return { };
}

static uint32_t findLayout(HeapFixture& fixture, uint32_t first, bool inOneBucket)
{
    uint64_t mask = fixture.maskOf(MemoryLocation::shapeWords(fixture.layoutSet({ first })));
    for (uint32_t index = first + 1; index < 4096; ++index) {
        if ((fixture.maskOf(MemoryLocation::shapeWords(fixture.layoutSet({ index }))) == mask) == inOneBucket)
            return index;
    }
    AOT_TEST_CHECK(false);
    return 0;
}

static void testEveryIRHeapHasAHeap()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;

    UncheckedKeyHashSet<const B3::AbstractHeap*> seen;
    for (IRHeap heap : Typed::allHeaps) {
        const B3::AbstractHeap& entire = heaps.entire(heap);
        AOT_TEST_CHECK(entire.isInitialized() && entire.parent() == &heaps.root);
        AOT_TEST_CHECK(entire.mutability() == B3::Mutability::Mutable);
        AOT_TEST_CHECK(seen.add(&entire).isNewEntry);
        AOT_TEST_CHECK(covers(heaps, entire, heap, LocationMasks::entireHeap));
        AOT_TEST_CHECK(&fixture.at(MemoryLocation(heap)) == &entire);
    }
    AOT_TEST_CHECK(seen.size() == Typed::numberOfHeaps);
    AOT_TEST_CHECK(!heaps.locationsOf(heaps.root));
}

static void testBucketsNestUnderTheirHeap()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;

    UncheckedKeyHashSet<const B3::AbstractHeap*> seen;
    for (IRHeap heap : Typed::indexedHeaps) {
        for (unsigned bit = 0; bit < 64; ++bit) {
            const B3::AbstractHeap& bucket = fixture.bucket(heap, bit);
            AOT_TEST_CHECK(bucket.parent() == &heaps.entire(heap));
            AOT_TEST_CHECK(seen.add(&bucket).isNewEntry);
            AOT_TEST_CHECK(covers(heaps, bucket, heap, uint64_t { 1 } << bit));
        }
    }
    AOT_TEST_CHECK(seen.size() == 64 * Typed::numberOfIndexedHeaps);

    for (IRHeap heap : Typed::allHeaps - Typed::indexedHeaps)
        AOT_TEST_CHECK(&fixture.bucket(heap, 5) == &heaps.entire(heap));
}

static void testExactLocationNestsUnderItsBucket()
{
    HeapFixture fixture;
    for (uint32_t index : { 0u, 1u, 2u, 63u, 64u, 1000u, 70000u }) {
        for (MemoryLocation location : { MemoryLocation::objectField(PropertyKeyID(index)), MemoryLocation::namedProperty(PropertyKeyID(index)), MemoryLocation::moduleVariable(VariableID(index)), MemoryLocation::memberGuards(LayoutID(index)), MemoryLocation::environmentField(LayoutID(index), 0), MemoryLocation::environmentField(LayoutID(3u), index) }) {
            const B3::AbstractHeap& exact = fixture.at(location);
            AOT_TEST_CHECK(exact.parent() == &fixture.bucketOf(location));
            AOT_TEST_CHECK(&fixture.at(location) == &exact);
            AOT_TEST_CHECK(covers(fixture.heaps, exact, location.heap(), fixture.maskOf(location)));
        }
    }

    AOT_TEST_CHECK(&fixture.at(MemoryLocation::objectField(PropertyKeyID(7u))) != &fixture.at(MemoryLocation::namedProperty(PropertyKeyID(7u))));
    AOT_TEST_CHECK(&fixture.at(MemoryLocation::environmentField(LayoutID(3u), 1)) != &fixture.at(MemoryLocation::environmentField(LayoutID(3u), 2)));
    AOT_TEST_CHECK(&fixture.at(MemoryLocation::environmentField(LayoutID(1u), 3)) != &fixture.at(MemoryLocation::environmentField(LayoutID(3u), 1)));
}

static void testTwoFieldsOfOneBucketAreDisjoint()
{
    HeapFixture fixture;
    KeyPair keys = findKeys(fixture, true);
    const B3::AbstractHeap& first = fixture.at(MemoryLocation::objectField(keys.first));
    const B3::AbstractHeap& second = fixture.at(MemoryLocation::objectField(keys.second));
    AOT_TEST_CHECK(&first != &second && first.parent() == second.parent());

    fixture.heaps.computeRangesAndDecorateInstructions();
    AOT_TEST_CHECK(first.range() && second.range());
    AOT_TEST_CHECK(!first.range().overlaps(second.range()));
    AOT_TEST_CHECK(contains(first.parent()->range(), first.range()) && contains(first.parent()->range(), second.range()));
    AOT_TEST_CHECK(first.range().overlaps(fixture.at(MemoryLocation::objectField(keys.first)).range()));
}

static void testOneFieldInTwoLayoutsIsDisjoint()
{
    HeapFixture fixture;
    PropertyKeyID key(7u);
    MemoryLocation inEveryLayout = MemoryLocation::objectField(key);
    MemoryLocation inFirst = MemoryLocation::objectField(key, fixture.layoutSet({ 1 }));
    MemoryLocation inSecond = MemoryLocation::objectField(key, fixture.layoutSet({ 2 }));
    MemoryLocation inBoth = MemoryLocation::objectField(key, fixture.layoutSet({ 1, 2 }));
    MemoryLocation inFirstAndOthers = MemoryLocation::objectField(key, fixture.layoutSet({ 1 }, false));

    const B3::AbstractHeap& everyLayout = fixture.at(inEveryLayout);
    const B3::AbstractHeap& first = fixture.at(inFirst);
    const B3::AbstractHeap& second = fixture.at(inSecond);
    AOT_TEST_CHECK(everyLayout.parent() == &fixture.bucketOf(inEveryLayout));
    AOT_TEST_CHECK(&first != &second && first.parent() == &everyLayout && second.parent() == &everyLayout);
    AOT_TEST_CHECK(&fixture.at(inFirst) == &first);
    AOT_TEST_CHECK(&fixture.at(inBoth) == &everyLayout && &fixture.at(inFirstAndOthers) == &everyLayout);
    AOT_TEST_CHECK(&fixture.at(MemoryLocation::objectField(PropertyKeyID(8u), fixture.layoutSet({ 1 }))) != &first);
    for (const B3::AbstractHeap* heap : { &first, &second })
        AOT_TEST_CHECK(covers(fixture.heaps, *heap, IRHeap::ObjectFields, fixture.maskOf(inEveryLayout)));
    AOT_TEST_CHECK(fixture.heaps.isCoveredBy(first, fixture.masksOf({ inEveryLayout })));

    B3::MemoryValue* loadFromFirst = fixture.appendLoad(first);
    B3::MemoryValue* loadFromSecond = fixture.appendLoad(second);
    B3::PatchpointValue* writesField = fixture.appendPatchpointWriting(fixture.masksOf({ inEveryLayout }));
    fixture.heaps.computeRangesAndDecorateInstructions();
    AOT_TEST_CHECK(!first.range().overlaps(second.range()));
    AOT_TEST_CHECK(contains(everyLayout.range(), first.range()) && contains(everyLayout.range(), second.range()));
    AOT_TEST_CHECK(writesField->effects.writes.overlaps(loadFromFirst->range()) && writesField->effects.writes.overlaps(loadFromSecond->range()));

    fixture.heaps.detachProcedure();
    AOT_TEST_CHECK(!fixture.bucketOf(inEveryLayout).numberOfChildren());
    fixture.heaps.attachProcedure(fixture.proc);
    AOT_TEST_CHECK(fixture.at(inFirst).parent() == &fixture.at(inEveryLayout) && fixture.bucketOf(inEveryLayout).numberOfChildren() == 1);
}

static void testFieldLocationsOverlapByReceivers()
{
    using Typed::LocationOverlap;
    HeapFixture fixture;
    const Typed::Program& program = fixture.program.program;
    PropertyKeyID key(7u);
    MemoryLocation inEveryLayout = MemoryLocation::objectField(key);
    MemoryLocation inFirst = MemoryLocation::objectField(key, fixture.layoutSet({ 1 }));
    MemoryLocation inSecond = MemoryLocation::objectField(key, fixture.layoutSet({ 2 }));
    MemoryLocation inBoth = MemoryLocation::objectField(key, fixture.layoutSet({ 1, 2 }));
    MemoryLocation inSecondAndOthers = MemoryLocation::objectField(key, fixture.layoutSet({ 2 }, false));

    AOT_TEST_CHECK(inEveryLayout == MemoryLocation::at(IRHeap::ObjectFields, key.index()) && !inEveryLayout.receivers());
    AOT_TEST_CHECK(inFirst.key() == key && inFirst.receivers() == fixture.layoutSet({ 1 }));
    AOT_TEST_CHECK(fixture.maskOf(inFirst) == fixture.maskOf(inEveryLayout));
    AOT_TEST_CHECK(overlapOf(program, inFirst, inFirst) == LocationOverlap::Exact && overlapOf(program, inEveryLayout, inEveryLayout) == LocationOverlap::Exact);
    AOT_TEST_CHECK(overlapOf(program, inFirst, inSecond) == LocationOverlap::None);
    for (const MemoryLocation& other : { inEveryLayout, inBoth, inSecondAndOthers })
        AOT_TEST_CHECK(overlapOf(program, inFirst, other) == LocationOverlap::May && overlapOf(program, other, inFirst) == LocationOverlap::May);
    AOT_TEST_CHECK(overlapOf(program, inFirst, MemoryLocation::objectField(PropertyKeyID(8u), fixture.layoutSet({ 1 }))) == LocationOverlap::None);
}

static void testLayoutSetOverSeveralBucketsIsTheEntireHeap()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;
    uint32_t inOtherBucket = findLayout(fixture, 1, false);
    uint32_t inSameBucket = findLayout(fixture, 1, true);

    MemoryLocation one = MemoryLocation::shapeWords(fixture.layoutSet({ 1 }));
    MemoryLocation acrossBuckets = MemoryLocation::shapeWords(fixture.layoutSet({ 1, inOtherBucket }));
    MemoryLocation withinBucket = MemoryLocation::shapeWords(fixture.layoutSet({ 1, inSameBucket }));
    MemoryLocation incomplete = MemoryLocation::shapeWords(fixture.layoutSet({ 1 }, false));
    MemoryLocation other = MemoryLocation::shapeWords(fixture.layoutSet({ inOtherBucket }));

    AOT_TEST_CHECK(std::popcount(fixture.maskOf(acrossBuckets)) == 2);
    AOT_TEST_CHECK(&fixture.at(acrossBuckets) == &heaps.entire(IRHeap::ShapeWords));
    AOT_TEST_CHECK(&fixture.at(incomplete) == &heaps.entire(IRHeap::ShapeWords));

    AOT_TEST_CHECK(fixture.at(one).parent() == &fixture.bucketOf(one));
    AOT_TEST_CHECK(isWithin(fixture.at(withinBucket), fixture.bucketOf(one)));
    AOT_TEST_CHECK(covers(heaps, fixture.at(withinBucket), IRHeap::ShapeWords, fixture.maskOf(one)));

    const B3::AbstractHeap& heapOfOne = fixture.at(one);
    const B3::AbstractHeap& heapOfOther = fixture.at(other);
    const B3::AbstractHeap& heapWithinBucket = fixture.at(withinBucket);
    const B3::AbstractHeap& heapAcrossBuckets = fixture.at(acrossBuckets);
    heaps.computeRangesAndDecorateInstructions();
    AOT_TEST_CHECK(heapOfOne.range().overlaps(heapWithinBucket.range()));
    AOT_TEST_CHECK(heapOfOne.range().overlaps(heapAcrossBuckets.range()));
    AOT_TEST_CHECK(heapOfOther.range().overlaps(heapAcrossBuckets.range()));
    AOT_TEST_CHECK(!heapOfOne.range().overlaps(heapOfOther.range()));
    AOT_TEST_CHECK(!heapWithinBucket.range().overlaps(heapOfOther.range()));
}

static void testElementBases()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;
    const B3::AbstractHeap& arrayElement = heaps.indexedContiguousProperties[2];
    const B3::AbstractHeap& argument = heaps.DirectArguments_storage[2];

    AOT_TEST_CHECK(&fixture.at(MemoryLocation::elements(ElementBase::Unknown)) == &heaps.entire(IRHeap::Elements));
    for (ElementBase base : { ElementBase::Array, ElementBase::Arguments, ElementBase::Object })
        AOT_TEST_CHECK(covers(heaps, fixture.at(MemoryLocation::elements(base)), IRHeap::Elements, uint64_t { 1 } << static_cast<unsigned>(base)));

    heaps.computeRangesAndDecorateInstructions();
    B3::HeapRange arrays = fixture.at(MemoryLocation::elements(ElementBase::Array)).range();
    B3::HeapRange arguments = fixture.at(MemoryLocation::elements(ElementBase::Arguments)).range();
    B3::HeapRange objects = fixture.at(MemoryLocation::elements(ElementBase::Object)).range();
    AOT_TEST_CHECK(arrays.overlaps(arrayElement.range()) && !arrays.overlaps(argument.range()));
    AOT_TEST_CHECK(arguments.overlaps(argument.range()) && !arguments.overlaps(arrayElement.range()));
    AOT_TEST_CHECK(!objects.overlaps(arrayElement.range()) && !objects.overlaps(argument.range()));
    AOT_TEST_CHECK(!arrays.overlaps(arguments) && !arrays.overlaps(objects) && !arguments.overlaps(objects));
}

static void checkFieldHeaps(const AOTHeaps& heaps, IRHeap expected, HeapPointers fieldHeaps)
{
    for (const B3::AbstractHeap* heap : fieldHeaps) {
        AOT_TEST_CHECK(isWithin(*heap, heaps.entire(expected)));
        AOT_TEST_CHECK(covers(heaps, *heap, expected, LocationMasks::entireHeap));
    }
}

static void checkElementHeaps(const AOTHeaps& heaps, ElementBase expected, HeapPointers elementHeaps)
{
    for (const B3::AbstractHeap* heap : elementHeaps)
        AOT_TEST_CHECK(covers(heaps, *heap, IRHeap::Elements, uint64_t { 1 } << static_cast<unsigned>(expected)));
}

static void testUpstreamHeapsAreNested()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;

    checkFieldHeaps(heaps, IRHeap::ShapeWords, { &heaps.JSCell_header, &heaps.JSCell_structureID, &heaps.JSCell_usefulBytes, &heaps.JSCell_indexingTypeAndMisc, &heaps.JSCell_typeInfoFlags });
    checkFieldHeaps(heaps, IRHeap::ArrayStorage, { &heaps.JSObject_butterfly, &heaps.Butterfly_publicLength, &heaps.Butterfly_vectorLength });
    checkFieldHeaps(heaps, IRHeap::NamedProperties, { &heaps.properties.atAnyNumber(), &heaps.properties[4] });
    checkFieldHeaps(heaps, IRHeap::PrototypeLinks, { &heaps.Structure_prototype });
    checkFieldHeaps(heaps, IRHeap::TypedArrayStorage, { &heaps.JSArrayBufferView_byteOffset, &heaps.JSArrayBufferView_length, &heaps.JSArrayBufferView_mode, &heaps.JSArrayBufferView_vector });
    checkFieldHeaps(heaps, IRHeap::TypedArrayElements, { &heaps.TypedArrayProperties });
    checkFieldHeaps(heaps, IRHeap::StringStorage, { &heaps.JSString_value, &heaps.JSRopeString_flags, &heaps.JSRopeString_length, &heaps.JSRopeString_fiber0, &heaps.JSRopeString_fiber1, &heaps.JSRopeString_fiber2 });
    checkFieldHeaps(heaps, IRHeap::StringStorage, { &heaps.StringImpl_data, &heaps.StringImpl_hashAndFlags, &heaps.StringImpl_length, &heaps.characters8.atAnyIndex(), &heaps.characters16.atAnyIndex(), &heaps.characters8[1] });
    checkFieldHeaps(heaps, IRHeap::PromiseState, { &heaps.JSPromise_packed, &heaps.JSPromise_slot });
    checkFieldHeaps(heaps, IRHeap::InternalFields, { &heaps.JSInternalFieldObjectImpl_internalFields.atAnyIndex(), &heaps.JSInternalFieldObjectImpl_internalFields[1], &heaps.JSWrapperObject_internalValue });
    checkFieldHeaps(heaps, IRHeap::CollectionStorage, { &heaps.JSMap_storage, &heaps.JSSet_storage });
    checkFieldHeaps(heaps, IRHeap::RegExpState, { &heaps.RegExpObject_regExpAndFlags, &heaps.RegExpObject_lastIndex });
    checkFieldHeaps(heaps, IRHeap::RegExpState, { &heaps.JSGlobalObject_regExpGlobalData_cachedResult_lastRegExp, &heaps.JSGlobalObject_regExpGlobalData_cachedResult_lastInput, &heaps.JSGlobalObject_regExpGlobalData_cachedResult_result_start, &heaps.JSGlobalObject_regExpGlobalData_cachedResult_result_end, &heaps.JSGlobalObject_regExpGlobalData_cachedResult_reified });

    checkElementHeaps(heaps, ElementBase::Array, { &heaps.indexedInt32Properties.atAnyIndex(), &heaps.indexedDoubleProperties.atAnyIndex(), &heaps.indexedContiguousProperties.atAnyIndex(), &heaps.ArrayStorage_vector.atAnyIndex(), &heaps.indexedInt32Properties[3] });
    checkElementHeaps(heaps, ElementBase::Arguments, { &heaps.DirectArguments_callee, &heaps.DirectArguments_length, &heaps.DirectArguments_minCapacity, &heaps.DirectArguments_mappedArguments, &heaps.DirectArguments_modifiedArgumentsDescriptor, &heaps.DirectArguments_storage.atAnyIndex() });
    checkElementHeaps(heaps, ElementBase::Arguments, { &heaps.ScopedArguments_overrodeThings, &heaps.ScopedArguments_scope, &heaps.ScopedArguments_storage, &heaps.ScopedArguments_table, &heaps.ScopedArguments_totalLength, &heaps.ScopedArguments_Storage_storage.atAnyIndex() });

    AOT_TEST_CHECK(isWithin(heaps.JSCell_cellState, heaps.collectorState) && !heaps.locationsOf(heaps.JSCell_cellState));
    for (const B3::AbstractHeap* heap : HeapPointers { &heaps.JSCell_typeInfoType, &heaps.Structure_classInfo, &heaps.ClassInfo_parentClass, &heaps.Structure_realm, &heaps.singleCharacterStrings.atAnyIndex() }) {
        AOT_TEST_CHECK(heap->parent() == &heaps.root && heap->mutability() == B3::Mutability::Immutable);
        AOT_TEST_CHECK(!heaps.locationsOf(*heap));
    }
}

static void testOwnFieldHeapsAreNested()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;

#define AOT_CHECK_MUTABLE_FIELD_HEAP(name, offset, irHeap) \
    AOT_TEST_CHECK(heaps.name.parent() == &heaps.entire(IRHeap::irHeap) && heaps.name.mutability() == B3::Mutability::Mutable);
    FOR_EACH_AOT_MUTABLE_FIELD_HEAP(AOT_CHECK_MUTABLE_FIELD_HEAP)
#undef AOT_CHECK_MUTABLE_FIELD_HEAP

#define AOT_CHECK_IMMUTABLE_FIELD_HEAP(name, offset) \
    AOT_TEST_CHECK(heaps.name.parent() == &heaps.root && heaps.name.mutability() == B3::Mutability::Immutable && !heaps.locationsOf(heaps.name));
    FOR_EACH_AOT_IMMUTABLE_FIELD_HEAP(AOT_CHECK_IMMUTABLE_FIELD_HEAP)
#undef AOT_CHECK_IMMUTABLE_FIELD_HEAP

#define AOT_CHECK_HEAP_OUTSIDE_IR(name) \
    AOT_TEST_CHECK(heaps.name.mutability() == B3::Mutability::Mutable && !heaps.locationsOf(heaps.name));
    FOR_EACH_AOT_HEAP_OUTSIDE_IR(AOT_CHECK_HEAP_OUTSIDE_IR)
#undef AOT_CHECK_HEAP_OUTSIDE_IR

    for (B3::IndexedAbstractHeap* captures : { &heaps.AOTClosure_captures, &heaps.AOTSlimClosure_captures }) {
        AOT_TEST_CHECK(captures->atAnyIndex().parent() == &heaps.entire(IRHeap::EnvironmentFields));
        AOT_TEST_CHECK(captures->atAnyIndex().mutability() == B3::Mutability::Immutable);
    }
}

static void testUnclaimedHeapsLieUnderRuntimeState()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;
    const B3::AbstractHeap& runtimeState = heaps.entire(IRHeap::RuntimeState);

    for (const B3::AbstractHeap* heap : HeapPointers { &heaps.JSBoundFunction_boundThis, &heaps.GetterSetter_getter, &heaps.VM_exception, &heaps.JSFunction_executableOrRareData, &heaps.Structure_bitField, &heaps.variables.atAnyIndex(), &heaps.absolute.atAnyAddress() })
        AOT_TEST_CHECK(heap->parent() == &runtimeState);

    UncheckedKeyHashSet<const B3::AbstractHeap*> mutableHeapsUnderRoot;
    for (IRHeap heap : Typed::allHeaps)
        mutableHeapsUnderRoot.add(&heaps.entire(heap));
#define AOT_ADD_HEAP_OUTSIDE_IR(name) mutableHeapsUnderRoot.add(&heaps.name);
    FOR_EACH_AOT_HEAP_OUTSIDE_IR(AOT_ADD_HEAP_OUTSIDE_IR)
#undef AOT_ADD_HEAP_OUTSIDE_IR

    for (const B3::AbstractHeap* child : heaps.root.children())
        AOT_TEST_CHECK(child->mutability() == B3::Mutability::Immutable || mutableHeapsUnderRoot.contains(child));
}

static void testAppendHeaps()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;

    auto listOf = [&](const LocationMasks& masks) {
        B3HeapList list;
        heaps.appendHeaps(masks, list);
        return list;
    };

    LocationMasks twoBuckets;
    twoBuckets.add(IRHeap::ObjectFields, uint64_t { 1 } << 3 | uint64_t { 1 } << 9);
    B3HeapList objectFields = listOf(twoBuckets);
    AOT_TEST_CHECK(objectFields.size() == 2 && objectFields[0] == &fixture.bucket(IRHeap::ObjectFields, 3) && objectFields[1] == &fixture.bucket(IRHeap::ObjectFields, 9));

    LocationMasks twoEnvironmentBuckets;
    twoEnvironmentBuckets.add(IRHeap::EnvironmentFields, uint64_t { 1 } << 0 | uint64_t { 1 } << 63);
    B3HeapList environmentFields = listOf(twoEnvironmentBuckets);
    AOT_TEST_CHECK(environmentFields.size() == 4);
    AOT_TEST_CHECK(environmentFields.contains(&fixture.bucket(IRHeap::EnvironmentFields, 0)) && environmentFields.contains(&fixture.bucket(IRHeap::EnvironmentFields, 63)));
    AOT_TEST_CHECK(environmentFields.contains(&heaps.AOTClosure_captures.atAnyIndex()) && environmentFields.contains(&heaps.AOTSlimClosure_captures.atAnyIndex()));
    AOT_TEST_CHECK(!environmentFields.contains(&fixture.bucket(IRHeap::EnvironmentFields, 1)) && !environmentFields.contains(&heaps.entire(IRHeap::EnvironmentFields)));

    LocationMasks oneNamedProperty;
    oneNamedProperty.add(IRHeap::NamedProperties, uint64_t { 1 } << 5);
    B3HeapList namedProperties = listOf(oneNamedProperty);
    AOT_TEST_CHECK(namedProperties.contains(&fixture.bucket(IRHeap::NamedProperties, 5)) && namedProperties.contains(&heaps.properties.atAnyNumber()));
    AOT_TEST_CHECK(!namedProperties.contains(&fixture.bucket(IRHeap::NamedProperties, 6)) && !namedProperties.contains(&heaps.entire(IRHeap::NamedProperties)));

    B3HeapList entireIndexedHeap = listOf(LocationMasks(Typed::AbstractHeapSet { IRHeap::ObjectFields }));
    AOT_TEST_CHECK(entireIndexedHeap.size() == 1 && entireIndexedHeap[0] == &heaps.entire(IRHeap::ObjectFields));

    B3HeapList twoHeaps = listOf(LocationMasks(Typed::AbstractHeapSet { IRHeap::PromiseState, IRHeap::RegExpState }));
    AOT_TEST_CHECK(twoHeaps.size() == 2 && twoHeaps.contains(&heaps.entire(IRHeap::PromiseState)) && twoHeaps.contains(&heaps.entire(IRHeap::RegExpState)));

    B3HeapList top = listOf(LocationMasks::top());
    AOT_TEST_CHECK(top.size() == 1 && top[0] == &heaps.root);
    AOT_TEST_CHECK(listOf(LocationMasks()).isEmpty());
}

static void testIsCoveredBy()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;
    KeyPair otherBucket = findKeys(fixture, false);
    KeyPair sameBucket = findKeys(fixture, true);
    MemoryLocation field = MemoryLocation::objectField(otherBucket.first);
    MemoryLocation fieldInOtherBucket = MemoryLocation::objectField(otherBucket.second);
    MemoryLocation fieldInSameBucket = MemoryLocation::objectField(sameBucket.second);
    LocationMasks nothing;
    LocationMasks entireObjectFields(Typed::AbstractHeapSet { IRHeap::ObjectFields });

    const B3::AbstractHeap& exact = fixture.at(field);
    AOT_TEST_CHECK(heaps.isCoveredBy(exact, fixture.masksOf({ field })));
    AOT_TEST_CHECK(heaps.isCoveredBy(exact, fixture.masksOf({ fieldInSameBucket })));
    AOT_TEST_CHECK(heaps.isCoveredBy(exact, fixture.masksOf({ field, fieldInOtherBucket })));
    AOT_TEST_CHECK(heaps.isCoveredBy(exact, entireObjectFields));
    AOT_TEST_CHECK(heaps.isCoveredBy(exact, LocationMasks::top()));
    AOT_TEST_CHECK(!heaps.isCoveredBy(exact, fixture.masksOf({ fieldInOtherBucket })));
    AOT_TEST_CHECK(!heaps.isCoveredBy(exact, fixture.masksOf({ MemoryLocation::moduleVariable(VariableID(otherBucket.first.index())) })));
    AOT_TEST_CHECK(!heaps.isCoveredBy(exact, LocationMasks::top().without({ IRHeap::ObjectFields })));
    AOT_TEST_CHECK(!heaps.isCoveredBy(exact, nothing));

    const B3::AbstractHeap& bucket = fixture.bucketOf(field);
    AOT_TEST_CHECK(heaps.isCoveredBy(bucket, fixture.masksOf({ field })));
    AOT_TEST_CHECK(heaps.isCoveredBy(bucket, entireObjectFields));
    AOT_TEST_CHECK(!heaps.isCoveredBy(bucket, fixture.masksOf({ fieldInOtherBucket })));
    AOT_TEST_CHECK(!heaps.isCoveredBy(bucket, nothing));

    const B3::AbstractHeap& entire = heaps.entire(IRHeap::ObjectFields);
    AOT_TEST_CHECK(heaps.isCoveredBy(entire, entireObjectFields));
    AOT_TEST_CHECK(heaps.isCoveredBy(entire, LocationMasks::top()));
    AOT_TEST_CHECK(!heaps.isCoveredBy(entire, fixture.masksOf({ field, fieldInOtherBucket })));
    AOT_TEST_CHECK(!heaps.isCoveredBy(entire, LocationMasks(Typed::AbstractHeapSet { IRHeap::EnvironmentFields })));

    AOT_TEST_CHECK(heaps.isCoveredBy(heaps.JSPromise_slot, LocationMasks(Typed::AbstractHeapSet { IRHeap::PromiseState })));
    AOT_TEST_CHECK(!heaps.isCoveredBy(heaps.JSPromise_slot, LocationMasks(Typed::AbstractHeapSet { IRHeap::RegExpState })));
    AOT_TEST_CHECK(!heaps.isCoveredBy(heaps.JSPromise_slot, nothing));
    AOT_TEST_CHECK(heaps.isCoveredBy(heaps.JSBoundFunction_boundThis, LocationMasks(Typed::AbstractHeapSet { IRHeap::RuntimeState })));
    AOT_TEST_CHECK(!heaps.isCoveredBy(heaps.JSBoundFunction_boundThis, LocationMasks::top().without({ IRHeap::RuntimeState })));

#define AOT_CHECK_HEAP_OUTSIDE_IR(name) AOT_TEST_CHECK(heaps.isCoveredBy(heaps.name, nothing));
    FOR_EACH_AOT_HEAP_OUTSIDE_IR(AOT_CHECK_HEAP_OUTSIDE_IR)
#undef AOT_CHECK_HEAP_OUTSIDE_IR
    AOT_TEST_CHECK(heaps.isCoveredBy(heaps.JSCell_cellState, nothing));

    for (const B3::AbstractHeap* immutableHeap : HeapPointers { &heaps.AOTClosure_entry, &heaps.AOTInstance_vm, &heaps.AOTImage_tables, &heaps.JSCell_typeInfoType, &heaps.Structure_classInfo }) {
        AOT_TEST_CHECK(!heaps.isCoveredBy(*immutableHeap, nothing));
        AOT_TEST_CHECK(!heaps.isCoveredBy(*immutableHeap, LocationMasks::top()));
    }
}

static void testTemporaryHeapsAreRemoved()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;
    KeyPair keys = findKeys(fixture, true);
    MemoryLocation first = MemoryLocation::objectField(keys.first);
    MemoryLocation second = MemoryLocation::objectField(keys.second);
    const B3::AbstractHeap& bucket = fixture.bucketOf(first);
    const B3::AbstractHeap& arrayBucket = fixture.bucketOf(MemoryLocation::elements(ElementBase::Array));
    size_t retainedChildren = bucket.numberOfChildren();
    size_t retainedArrayChildren = arrayBucket.numberOfChildren();
    AOT_TEST_CHECK(!retainedChildren && retainedArrayChildren);

    heaps.computeRangesAndDecorateInstructions();
    B3::HeapRange rangeWithoutTemporaryHeaps = bucket.range();

    const B3::AbstractHeap& exact = fixture.at(first);
    fixture.at(second);
    AOT_TEST_CHECK(bucket.numberOfChildren() == 2 && !exact.hasRange());
    heaps.computeRangesAndDecorateInstructions();
    AOT_TEST_CHECK(exact.hasRange() && contains(bucket.range(), exact.range()));

    heaps.detachProcedure();
    AOT_TEST_CHECK(bucket.numberOfChildren() == retainedChildren);
    AOT_TEST_CHECK(arrayBucket.numberOfChildren() == retainedArrayChildren);
    AOT_TEST_CHECK(covers(heaps, bucket, IRHeap::ObjectFields, fixture.maskOf(first)));

    heaps.attachProcedure(fixture.proc);
    heaps.computeRangesAndDecorateInstructions();
    AOT_TEST_CHECK(bucket.range() == rangeWithoutTemporaryHeaps);

    const B3::AbstractHeap& exactAgain = fixture.at(first);
    AOT_TEST_CHECK(bucket.numberOfChildren() == 1 && exactAgain.parent() == &bucket && !exactAgain.hasRange());
    AOT_TEST_CHECK(covers(heaps, exactAgain, IRHeap::ObjectFields, fixture.maskOf(first)));
}

static void testWriteSetsExcludeOtherHeaps()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;
    KeyPair otherBucket = findKeys(fixture, false);
    KeyPair sameBucket = findKeys(fixture, true);
    MemoryLocation written = MemoryLocation::objectField(otherBucket.first);

    B3::MemoryValue* loadOfWrittenField = fixture.appendLoad(fixture.at(written));
    B3::MemoryValue* loadInSameBucket = fixture.appendLoad(fixture.at(MemoryLocation::objectField(sameBucket.second)));
    B3::MemoryValue* loadInOtherBucket = fixture.appendLoad(fixture.at(MemoryLocation::objectField(otherBucket.second)));
    B3::MemoryValue* loadOfNamedProperty = fixture.appendLoad(fixture.at(MemoryLocation::namedProperty(otherBucket.first)));
    B3::MemoryValue* loadOfPromise = fixture.appendLoad(heaps.JSPromise_slot);
    B3::MemoryValue* loadOfCellType = fixture.appendLoad(heaps.JSCell_typeInfoType);
    B3::MemoryValue* loadOfCellState = fixture.appendLoad(heaps.JSCell_cellState);

    B3::PatchpointValue* writesOneField = fixture.appendPatchpointWriting(fixture.masksOf({ written }));
    LocationMasks fieldAndRuntimeState = fixture.masksOf({ written });
    fieldAndRuntimeState.add(IRHeap::RuntimeState);
    B3::PatchpointValue* writesFieldAndRuntimeState = fixture.appendPatchpointWriting(fieldAndRuntimeState);
    B3::PatchpointValue* writesNothing = fixture.appendPatchpointWriting(LocationMasks());
    B3::PatchpointValue* writesEverything = fixture.appendPatchpointWriting(LocationMasks::top());
    heaps.computeRangesAndDecorateInstructions();

    for (B3::PatchpointValue* patchpoint : { writesOneField, writesFieldAndRuntimeState }) {
        const B3::HeapSet& writes = patchpoint->effects.writes;
        AOT_TEST_CHECK(writes.overlaps(loadOfWrittenField->range()));
        AOT_TEST_CHECK(writes.overlaps(loadInSameBucket->range()));
        for (B3::MemoryValue* load : { loadInOtherBucket, loadOfNamedProperty, loadOfPromise, loadOfCellType, loadOfCellState })
            AOT_TEST_CHECK(!writes.overlaps(load->range()));
        for (IRHeap heap : Typed::allHeaps - Typed::AbstractHeapSet { IRHeap::ObjectFields, IRHeap::RuntimeState })
            AOT_TEST_CHECK(!writes.overlaps(heaps.entire(heap).range()));
    }
    AOT_TEST_CHECK(!writesOneField->effects.writes.overlaps(heaps.entire(IRHeap::RuntimeState).range()));
    AOT_TEST_CHECK(writesFieldAndRuntimeState->effects.writes.overlaps(heaps.JSBoundFunction_boundThis.range()));

    for (B3::MemoryValue* load : { loadOfWrittenField, loadInSameBucket, loadInOtherBucket, loadOfNamedProperty, loadOfPromise, loadOfCellType, loadOfCellState }) {
        AOT_TEST_CHECK(!writesNothing->effects.writes.overlaps(load->range()));
        AOT_TEST_CHECK(writesEverything->effects.writes.overlaps(load->range()));
    }
    AOT_TEST_CHECK(loadOfCellType->readsMutability() == B3::Mutability::Immutable && loadOfPromise->readsMutability() == B3::Mutability::Mutable);
}

std::span<const NativeTest> b3HeapTests()
{
    static constexpr NativeTest tests[] = {
        { "b3-heaps/every-ir-heap-has-a-heap"_s, testEveryIRHeapHasAHeap },
        { "b3-heaps/buckets-nest-under-their-heap"_s, testBucketsNestUnderTheirHeap },
        { "b3-heaps/exact-location-nests-under-its-bucket"_s, testExactLocationNestsUnderItsBucket },
        { "b3-heaps/two-fields-of-one-bucket-are-disjoint"_s, testTwoFieldsOfOneBucketAreDisjoint },
        { "b3-heaps/one-field-in-two-layouts-is-disjoint"_s, testOneFieldInTwoLayoutsIsDisjoint },
        { "b3-heaps/field-locations-overlap-by-receivers"_s, testFieldLocationsOverlapByReceivers },
        { "b3-heaps/layout-set-over-several-buckets-is-the-entire-heap"_s, testLayoutSetOverSeveralBucketsIsTheEntireHeap },
        { "b3-heaps/element-bases"_s, testElementBases },
        { "b3-heaps/upstream-heaps-are-nested"_s, testUpstreamHeapsAreNested },
        { "b3-heaps/own-field-heaps-are-nested"_s, testOwnFieldHeapsAreNested },
        { "b3-heaps/unclaimed-heaps-lie-under-runtime-state"_s, testUnclaimedHeapsLieUnderRuntimeState },
        { "b3-heaps/append-heaps"_s, testAppendHeaps },
        { "b3-heaps/is-covered-by"_s, testIsCoveredBy },
        { "b3-heaps/temporary-heaps-are-removed"_s, testTemporaryHeapsAreRemoved },
        { "b3-heaps/write-sets-exclude-other-heaps"_s, testWriteSetsExcludeOtherHeaps },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
