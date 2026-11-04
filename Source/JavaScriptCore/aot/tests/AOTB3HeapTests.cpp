/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTClaimChecks.h"
#include "AOTHeaps.h"
#include "AOTIRTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTVector.h"
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
#include <wtf/text/StringBuilder.h>

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
        Vector<LayoutID> list;
        for (uint32_t member : members)
            list.append(LayoutID(member));
        return program.program.types.intern(isComplete ? Typed::LayoutSet::complete(WTF::move(list)) : Typed::LayoutSet::open(WTF::move(list)));
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
    B3::HeapRange rangeOfParent = fixture.heaps.rangeOf(*first.parent());
    AOT_TEST_CHECK(first.range() && second.range());
    AOT_TEST_CHECK(!first.range().overlaps(second.range()));
    AOT_TEST_CHECK(contains(rangeOfParent, first.range()) && contains(rangeOfParent, second.range()));
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
    AOT_TEST_CHECK(heaps.rangeOf(heapOfOne).overlaps(heaps.rangeOf(heapWithinBucket)));
    AOT_TEST_CHECK(heaps.rangeOf(heapOfOne).overlaps(heaps.rangeOf(heapAcrossBuckets)));
    AOT_TEST_CHECK(heaps.rangeOf(heapOfOther).overlaps(heaps.rangeOf(heapAcrossBuckets)));
    AOT_TEST_CHECK(!heaps.rangeOf(heapOfOne).overlaps(heaps.rangeOf(heapOfOther)));
    AOT_TEST_CHECK(!heaps.rangeOf(heapWithinBucket).overlaps(heaps.rangeOf(heapOfOther)));
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
    B3::HeapRange arrays = heaps.rangeOf(fixture.at(MemoryLocation::elements(ElementBase::Array)));
    B3::HeapRange arguments = heaps.rangeOf(fixture.at(MemoryLocation::elements(ElementBase::Arguments)));
    B3::HeapRange objects = heaps.rangeOf(fixture.at(MemoryLocation::elements(ElementBase::Object)));
    AOT_TEST_CHECK(arrays.overlaps(heaps.rangeOf(arrayElement)) && !arrays.overlaps(heaps.rangeOf(argument)));
    AOT_TEST_CHECK(arguments.overlaps(heaps.rangeOf(argument)) && !arguments.overlaps(heaps.rangeOf(arrayElement)));
    AOT_TEST_CHECK(!objects.overlaps(heaps.rangeOf(arrayElement)) && !objects.overlaps(heaps.rangeOf(argument)));
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
    AOT_TEST_CHECK(heaps.locationsOf(heaps.name) && heaps.locationsOf(heaps.name)->heap == IRHeap::irHeap && heaps.name.mutability() == B3::Mutability::Mutable);
    FOR_EACH_AOT_MUTABLE_FIELD_HEAP(AOT_CHECK_MUTABLE_FIELD_HEAP)
#undef AOT_CHECK_MUTABLE_FIELD_HEAP
    AOT_TEST_CHECK(heaps.JSArray_butterfly.parent() == &heaps.JSObject_butterfly && heaps.JSNonArray_butterfly.parent() == &heaps.JSObject_butterfly);

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
    AOT_TEST_CHECK(objectFields.size() == 3 && objectFields[0] == &fixture.bucket(IRHeap::ObjectFields, 3) && objectFields[1] == &fixture.bucket(IRHeap::ObjectFields, 9));
    AOT_TEST_CHECK(objectFields[2] == &heaps.AOTExtension_granuleBits);

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

#define AOT_CHECK_HEAP_OUTSIDE_IR(name) AOT_TEST_CHECK(heaps.isCoveredBy(heaps.name, nothing) == (&heaps.name != &heaps.weakTableStorage && &heaps.name != &heaps.weakTableEntries));
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
    B3::HeapRange rangeWithoutTemporaryHeaps = heaps.rangeOf(bucket);

    const B3::AbstractHeap& exact = fixture.at(first);
    fixture.at(second);
    AOT_TEST_CHECK(bucket.numberOfChildren() == 2 && !exact.hasRange());
    heaps.computeRangesAndDecorateInstructions();
    AOT_TEST_CHECK(exact.hasRange() && contains(heaps.rangeOf(bucket), exact.range()));

    heaps.detachProcedure();
    AOT_TEST_CHECK(bucket.numberOfChildren() == retainedChildren);
    AOT_TEST_CHECK(arrayBucket.numberOfChildren() == retainedArrayChildren);
    AOT_TEST_CHECK(covers(heaps, bucket, IRHeap::ObjectFields, fixture.maskOf(first)));

    heaps.attachProcedure(fixture.proc);
    heaps.computeRangesAndDecorateInstructions();
    AOT_TEST_CHECK(heaps.rangeOf(bucket) == rangeWithoutTemporaryHeaps);

    const B3::AbstractHeap& exactAgain = fixture.at(first);
    AOT_TEST_CHECK(bucket.numberOfChildren() == 1 && exactAgain.parent() == &bucket && !exactAgain.hasRange());
    AOT_TEST_CHECK(covers(heaps, exactAgain, IRHeap::ObjectFields, fixture.maskOf(first)));
}

static unsigned checkNumberingBelow(const B3::AbstractHeapRepository& heaps, const B3::AbstractHeap& heap, uint64_t begin)
{
    B3::HeapRange range = heaps.rangeOf(heap);
    AOT_TEST_CHECK(range.begin() == begin);
    if (!heap.numberOfChildren()) {
        AOT_TEST_CHECK(range.end() == begin + 1);
        return 1;
    }
    unsigned numberOfHeaps = 1;
    uint64_t position = begin;
    for (const B3::AbstractHeap* child : heap.children()) {
        numberOfHeaps += checkNumberingBelow(heaps, *child, position);
        position = heaps.rangeOf(*child).end();
    }
    AOT_TEST_CHECK(range.end() == position);
    return numberOfHeaps;
}

static void testRangesNumberTheLeavesInOrder()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;
    heaps.computeRangesAndDecorateInstructions();
    unsigned numberOfKeptHeaps = checkNumberingBelow(heaps, heaps.root, 0);
    B3::HeapRange rangeOfRoot = heaps.rangeOf(heaps.root);

    KeyPair sameBucket = findKeys(fixture, true);
    KeyPair otherBucket = findKeys(fixture, false);
    unsigned numberOfTemporaryHeaps = 0;
    auto make = [&](const B3::AbstractHeap& heap) {
        numberOfTemporaryHeaps += !heap.hasRange();
    };
    make(fixture.at(MemoryLocation::objectField(sameBucket.first)));
    make(fixture.at(MemoryLocation::objectField(sameBucket.second)));
    make(fixture.at(MemoryLocation::objectField(otherBucket.second)));
    make(fixture.at(MemoryLocation::namedProperty(otherBucket.first)));
    make(fixture.at(MemoryLocation::objectField(PropertyKeyID(7u), fixture.layoutSet({ 1 }))));
    make(fixture.at(MemoryLocation::objectField(PropertyKeyID(7u), fixture.layoutSet({ 2 }))));
    make(heaps.indexedContiguousProperties[2]);
    make(heaps.indexedContiguousProperties[100000]);
    make(heaps.DirectArguments_storage[3]);
    make(heaps.AOTClosure_captures[1]);
    make(heaps.AOTSlimClosure_captures[0]);
    make(heaps.AOTSlimClosure_captures[5]);
    AOT_TEST_CHECK(numberOfTemporaryHeaps >= 10);

    heaps.computeRangesAndDecorateInstructions();
    AOT_TEST_CHECK(checkNumberingBelow(heaps, heaps.root, 0) > numberOfKeptHeaps + numberOfTemporaryHeaps - 1);
    AOT_TEST_CHECK(heaps.rangeOf(heaps.root).end() > rangeOfRoot.end());

    heaps.detachProcedure();
    heaps.attachProcedure(fixture.proc);
    heaps.computeRangesAndDecorateInstructions();
    AOT_TEST_CHECK(checkNumberingBelow(heaps, heaps.root, 0) == numberOfKeptHeaps);
    AOT_TEST_CHECK(heaps.rangeOf(heaps.root) == rangeOfRoot);
}

namespace {

class HeapsWithKeptRanges final : public B3::AbstractHeapRepository {
public:
    HeapsWithKeptRanges()
    {
        keepRangesAcrossProcedures();
    }

    void grow(B3::AbstractHeap& parent, unsigned numberOfHeaps)
    {
        m_grown.append({ &parent, parent.numberOfChildren() });
        for (unsigned index = 0; index < numberOfHeaps; ++index)
            m_temporaryHeaps.append(makeUnique<B3::AbstractHeap>(&parent, "temporary"));
    }

    B3::AbstractHeap first { &root, "first" };
    B3::AbstractHeap firstOfFirst { &first, "firstOfFirst" };
    B3::AbstractHeap lastOfFirst { &first, "lastOfFirst" };
    B3::AbstractHeap leaf { &root, "leaf" };
    B3::AbstractHeap chain { &root, "chain" };
    B3::AbstractHeap middleOfChain { &chain, "middleOfChain" };
    B3::AbstractHeap endOfChain { &middleOfChain, "endOfChain" };

private:
    void appendGrownClientHeaps(WTF::Vector<GrownHeap, 16>& grownHeaps) final
    {
        for (auto [heap, numberOfKeptChildren] : m_grown)
            grownHeaps.append({ heap, numberOfKeptChildren });
    }

    void removeTemporaryClientHeaps() final
    {
        for (auto [heap, numberOfKeptChildren] : m_grown)
            heap->removeChildrenFrom(numberOfKeptChildren);
        m_grown.clear();
        m_temporaryHeaps.clear();
    }

    Vector<std::pair<B3::AbstractHeap*, size_t>> m_grown;
    Vector<std::unique_ptr<B3::AbstractHeap>> m_temporaryHeaps;
};

} // anonymous namespace

static void testRangesOfHeapsThatGrowInsideOneAnother()
{
    HeapsWithKeptRanges heaps;
    B3::Procedure proc;
    using Members = std::initializer_list<B3::AbstractHeap HeapsWithKeptRanges::*>;
    for (Members grown : std::initializer_list<Members> {
        { },
        { &HeapsWithKeptRanges::first },
        { &HeapsWithKeptRanges::lastOfFirst },
        { &HeapsWithKeptRanges::first, &HeapsWithKeptRanges::lastOfFirst },
        { &HeapsWithKeptRanges::lastOfFirst, &HeapsWithKeptRanges::first, &HeapsWithKeptRanges::firstOfFirst },
        { &HeapsWithKeptRanges::leaf },
        { &HeapsWithKeptRanges::endOfChain },
        { &HeapsWithKeptRanges::middleOfChain },
        { &HeapsWithKeptRanges::chain },
        { &HeapsWithKeptRanges::chain, &HeapsWithKeptRanges::endOfChain },
        { &HeapsWithKeptRanges::middleOfChain, &HeapsWithKeptRanges::chain, &HeapsWithKeptRanges::endOfChain },
        { &HeapsWithKeptRanges::endOfChain, &HeapsWithKeptRanges::leaf, &HeapsWithKeptRanges::first, &HeapsWithKeptRanges::middleOfChain, &HeapsWithKeptRanges::lastOfFirst },
    }) {
        heaps.attachProcedure(proc);
        unsigned numberOfHeaps = 1;
        for (B3::AbstractHeap HeapsWithKeptRanges::* member : grown)
            heaps.grow(heaps.*member, numberOfHeaps++);
        heaps.computeRangesAndDecorateInstructions();
        checkNumberingBelow(heaps, heaps.root, 0);
        heaps.detachProcedure();
    }
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
            AOT_TEST_CHECK(!writes.overlaps(heaps.rangeOf(heaps.entire(heap))));
    }
    AOT_TEST_CHECK(!writesOneField->effects.writes.overlaps(heaps.rangeOf(heaps.entire(IRHeap::RuntimeState))));
    AOT_TEST_CHECK(writesFieldAndRuntimeState->effects.writes.overlaps(heaps.rangeOf(heaps.JSBoundFunction_boundThis)));

    for (B3::MemoryValue* load : { loadOfWrittenField, loadInSameBucket, loadInOtherBucket, loadOfNamedProperty, loadOfPromise, loadOfCellType, loadOfCellState }) {
        AOT_TEST_CHECK(!writesNothing->effects.writes.overlaps(load->range()));
        AOT_TEST_CHECK(writesEverything->effects.writes.overlaps(load->range()));
    }
    AOT_TEST_CHECK(loadOfCellType->readsMutability() == B3::Mutability::Immutable && loadOfPromise->readsMutability() == B3::Mutability::Mutable);
}

static void testCallsConvertCells()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;

    auto convertsCells = [](const Typed::Effects& effects) {
        return AOTHeaps::mayConvertCells(effects);
    };
    auto reading = [](IRHeap heap) {
        Typed::Effects effects = Typed::Effects::none();
        effects.reads.add(heap);
        return effects;
    };

    for (IRHeap heap : { IRHeap::NamedProperties, IRHeap::ObjectFields, IRHeap::ShapeWords, IRHeap::OrderWords, IRHeap::PrototypeLinks })
        AOT_TEST_CHECK(convertsCells(reading(heap)));
    for (IRHeap heap : { IRHeap::StringStorage, IRHeap::CollectionEntries, IRHeap::TypedArrayElements, IRHeap::ParserState, IRHeap::EnvironmentFields })
        AOT_TEST_CHECK(!convertsCells(reading(heap)));
    AOT_TEST_CHECK(!convertsCells(Typed::Effects::none()));
    AOT_TEST_CHECK(convertsCells(Typed::Effects::unknownCode()));

    Typed::Effects createsConstant = Typed::Effects::none();
    createsConstant.writes.add(IRHeap::AllocatorState);
    AOT_TEST_CHECK(convertsCells(createsConstant));
    Typed::Effects collects = Typed::Effects::none();
    collects.mayCollect = true;
    AOT_TEST_CHECK(convertsCells(collects));
    Typed::Effects throws = Typed::Effects::none();
    throws.mayThrow = true;
    AOT_TEST_CHECK(convertsCells(throws));

    auto replacesStorageOfArrays = [](const Typed::Effects& effects) {
        return AOTHeaps::mayReplaceStorageOfArrays(effects);
    };
    auto writing = [](IRHeap heap) {
        Typed::Effects effects = Typed::Effects::none();
        effects.writes.add(heap);
        return effects;
    };
    for (IRHeap heap : { IRHeap::ArrayStorage, IRHeap::Elements, IRHeap::NamedProperties, IRHeap::PrototypeLinks })
        AOT_TEST_CHECK(replacesStorageOfArrays(writing(heap)));
    for (IRHeap heap : { IRHeap::ModuleVariables, IRHeap::EnvironmentFields, IRHeap::ObjectFields, IRHeap::ShapeWords, IRHeap::StringStorage, IRHeap::CollectionStorage, IRHeap::CollectionEntries, IRHeap::TypedArrayElements, IRHeap::AllocatorState, IRHeap::RuntimeState })
        AOT_TEST_CHECK(!replacesStorageOfArrays(writing(heap)));
    AOT_TEST_CHECK(replacesStorageOfArrays(reading(IRHeap::NamedProperties)));
    for (IRHeap heap : { IRHeap::ObjectFields, IRHeap::ShapeWords, IRHeap::ArrayStorage, IRHeap::Elements, IRHeap::StringStorage })
        AOT_TEST_CHECK(!replacesStorageOfArrays(reading(heap)));
    AOT_TEST_CHECK(!replacesStorageOfArrays(createsConstant) && !replacesStorageOfArrays(collects) && !replacesStorageOfArrays(throws));
    AOT_TEST_CHECK(replacesStorageOfArrays(Typed::Effects::unknownCode()));
    Typed::Effects suspends = Typed::Effects::none();
    suspends.maySuspend = true;
    AOT_TEST_CHECK(replacesStorageOfArrays(suspends));

    B3::MemoryValue* loadOfStructureID = fixture.appendLoad(heaps.JSCell_structureID);
    B3::MemoryValue* loadOfUsefulBytes = fixture.appendLoad(heaps.JSCell_usefulBytes);
    B3::MemoryValue* loadOfButterfly = fixture.appendLoad(heaps.JSObject_butterfly);
    B3::MemoryValue* loadOfButterflyOfArray = fixture.appendLoad(heaps.JSArray_butterfly);
    B3::MemoryValue* loadOfButterflyOfNonArray = fixture.appendLoad(heaps.JSNonArray_butterfly);
    B3::MemoryValue* loadOfSlimClosureWord = fixture.appendLoad(heaps.AOTSlimClosure_word);
    B3::MemoryValue* loadOfClosureEntry = fixture.appendLoad(heaps.AOTClosure_entry);
    B3::MemoryValue* loadOfShapeWord = fixture.appendLoad(heaps.AOTObject_shapeWord);
    B3::PatchpointValue* lookup = fixture.root->appendNew<B3::PatchpointValue>(fixture.proc, B3::Void, B3::Origin());
    B3HeapList written;
    heaps.appendHeapsOfConvertedCells(written);
    AOT_TEST_CHECK(written.contains(&heaps.JSCell_header) && written.contains(&heaps.JSObject_butterfly) && written.contains(&heaps.AOTSlimClosure_word));
    heaps.decoratePatchpointWrite(written.span(), lookup);
    B3::PatchpointValue* allocation = fixture.root->appendNew<B3::PatchpointValue>(fixture.proc, B3::Void, B3::Origin());
    B3HeapList writtenOutsideArrays;
    heaps.appendHeapsOfConvertedCells(writtenOutsideArrays, AOTHeaps::ConvertedCells::AllButArrays);
    heaps.decoratePatchpointWrite(writtenOutsideArrays.span(), allocation);
    B3::PatchpointValue* growth = fixture.root->appendNew<B3::PatchpointValue>(fixture.proc, B3::Void, B3::Origin());
    B3HeapList writtenByGrowth;
    heaps.appendHeaps(writing(IRHeap::ArrayStorage).writes, writtenByGrowth);
    heaps.decoratePatchpointWrite(writtenByGrowth.span(), growth);
    B3::PatchpointValue* additionOfProperty = fixture.root->appendNew<B3::PatchpointValue>(fixture.proc, B3::Void, B3::Origin());
    B3HeapList writtenByAdditionOfProperty;
    heaps.appendHeaps(writing(IRHeap::NamedProperties).writes, writtenByAdditionOfProperty);
    heaps.decoratePatchpointWrite(writtenByAdditionOfProperty.span(), additionOfProperty);
    heaps.computeRangesAndDecorateInstructions();

    for (B3::MemoryValue* load : { loadOfStructureID, loadOfUsefulBytes, loadOfButterfly, loadOfButterflyOfArray, loadOfButterflyOfNonArray, loadOfSlimClosureWord }) {
        AOT_TEST_CHECK(load->readsMutability() == B3::Mutability::Mutable);
        AOT_TEST_CHECK(lookup->effects.writes.overlaps(load->range()));
        AOT_TEST_CHECK(allocation->effects.writes.overlaps(load->range()) == (load != loadOfButterflyOfArray));
    }
    for (B3::MemoryValue* load : { loadOfButterfly, loadOfButterflyOfArray, loadOfButterflyOfNonArray }) {
        AOT_TEST_CHECK(growth->effects.writes.overlaps(load->range()));
        AOT_TEST_CHECK(additionOfProperty->effects.writes.overlaps(load->range()));
    }
    AOT_TEST_CHECK(!loadOfButterflyOfArray->range().overlaps(loadOfButterflyOfNonArray->range()));
    AOT_TEST_CHECK(loadOfButterfly->range().overlaps(loadOfButterflyOfArray->range()) && loadOfButterfly->range().overlaps(loadOfButterflyOfNonArray->range()));
    for (B3::MemoryValue* load : { loadOfClosureEntry, loadOfShapeWord })
        AOT_TEST_CHECK(load->readsMutability() == B3::Mutability::Immutable);
    for (IRHeap heap : { IRHeap::ObjectFields, IRHeap::Elements, IRHeap::EnvironmentFields, IRHeap::StringStorage })
        AOT_TEST_CHECK(!lookup->effects.writes.overlaps(heaps.rangeOf(heaps.entire(heap))));
}

static void testCollectionsWriteOnlyWeakTables()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;

    B3::MemoryValue* loadOfTable = fixture.appendLoad(heaps.entire(IRHeap::CollectionStorage));
    B3::MemoryValue* loadOfTableOfMap = fixture.appendLoad(heaps.JSMap_storage);
    B3::MemoryValue* loadOfEntry = fixture.appendLoad(heaps.entire(IRHeap::CollectionEntries));
    B3::MemoryValue* loadOfWeakBuffer = fixture.appendLoad(heaps.WeakMapImpl_buffer);
    B3::MemoryValue* loadOfWeakKey = fixture.appendLoad(heaps.WeakMapBucket_key);
    auto appendPatchpointWriting = [&](const B3HeapList& written) {
        B3::PatchpointValue* patchpoint = fixture.root->appendNew<B3::PatchpointValue>(fixture.proc, B3::Void, B3::Origin());
        patchpoint->effects = B3::Effects::none();
        heaps.decoratePatchpointWrite(written.span(), patchpoint);
        return patchpoint;
    };
    auto heapsOf = [&](IRHeap heap) {
        Typed::LocationMasks masks;
        masks.add(heap);
        B3HeapList list;
        heaps.appendHeaps(masks, list);
        return list;
    };
    B3HeapList writtenByCollection;
    heaps.appendHeapsWrittenByCollection(writtenByCollection);
    B3::PatchpointValue* collection = appendPatchpointWriting(writtenByCollection);
    B3::PatchpointValue* growth = appendPatchpointWriting(heapsOf(IRHeap::CollectionStorage));
    B3::PatchpointValue* insertion = appendPatchpointWriting(heapsOf(IRHeap::CollectionEntries));
    heaps.computeRangesAndDecorateInstructions();

    for (B3::MemoryValue* load : { loadOfTable, loadOfTableOfMap, loadOfEntry })
        AOT_TEST_CHECK(!collection->effects.writes.overlaps(load->range()));
    for (B3::MemoryValue* load : { loadOfWeakBuffer, loadOfWeakKey })
        AOT_TEST_CHECK(collection->effects.writes.overlaps(load->range()));
    for (B3::MemoryValue* load : { loadOfTable, loadOfTableOfMap, loadOfWeakBuffer }) {
        AOT_TEST_CHECK(growth->effects.writes.overlaps(load->range()));
        AOT_TEST_CHECK(!insertion->effects.writes.overlaps(load->range()));
    }
    for (B3::MemoryValue* load : { loadOfEntry, loadOfWeakKey }) {
        AOT_TEST_CHECK(insertion->effects.writes.overlaps(load->range()));
        AOT_TEST_CHECK(!growth->effects.writes.overlaps(load->range()));
    }

    Typed::LocationMasks storage;
    storage.add(IRHeap::CollectionStorage);
    Typed::LocationMasks entries;
    entries.add(IRHeap::CollectionEntries);
    AOT_TEST_CHECK(heaps.isCoveredBy(heaps.WeakMapImpl_buffer, storage) && !heaps.isCoveredBy(heaps.WeakMapImpl_buffer, entries) && !heaps.isCoveredBy(heaps.WeakMapImpl_buffer, { }));
    AOT_TEST_CHECK(heaps.isCoveredBy(heaps.WeakMapBucket_key, entries) && !heaps.isCoveredBy(heaps.WeakMapBucket_key, storage) && !heaps.isCoveredBy(heaps.WeakMapBucket_key, { }));
}

static void testWhatCellsOwnOutsideTheHeap()
{
    HeapFixture fixture;
    AOTHeaps& heaps = fixture.heaps;

    B3::PatchpointValue* collection = fixture.root->appendNew<B3::PatchpointValue>(fixture.proc, B3::Void, B3::Origin());
    collection->effects = B3::Effects::none();
    B3::PatchpointValue* barrier = fixture.root->appendNew<B3::PatchpointValue>(fixture.proc, B3::Void, B3::Origin());
    barrier->effects = B3::Effects::none();
    B3::PatchpointValue* lookup = fixture.root->appendNew<B3::PatchpointValue>(fixture.proc, B3::Void, B3::Origin());
    lookup->effects = B3::Effects::none();
    const B3::AbstractHeap& secondCharacter = heaps.characters8[1];
    B3HeapList writtenByCollection;
    heaps.appendHeapsWrittenByCollection(writtenByCollection);
    heaps.decoratePatchpointWrite(writtenByCollection.span(), collection);
    heaps.decoratePatchpointWrite(&heaps.collectorState, barrier);
    B3HeapList writtenByLookup;
    heaps.appendHeapsOfConvertedCells(writtenByLookup);
    heaps.decoratePatchpointWrite(writtenByLookup.span(), lookup);
    heaps.computeRangesAndDecorateInstructions();

    AOT_TEST_CHECK(heaps.mayCollect(collection->effects) && !heaps.mayCollect(barrier->effects) && !heaps.mayCollect(lookup->effects));
    for (const B3::AbstractHeap* heap : HeapPointers { &heaps.StringImpl_length, &heaps.StringImpl_data, &heaps.StringImpl_hashAndFlags, &secondCharacter, &heaps.characters8.atAnyIndex(), &heaps.characters16.atAnyIndex(), &heaps.ArrayBuffer_shared })
        AOT_TEST_CHECK(heaps.isFreedWithOwner(heaps.rangeOf(*heap)));
    for (const B3::AbstractHeap* heap : HeapPointers { &heaps.JSString_value, &heaps.JSRopeString_length, &heaps.JSCell_typeInfoType, &heaps.AOTObject_shapeWord, &heaps.JSArrayBuffer_impl, &heaps.AOTClosure_entry, &heaps.Structure_classInfo })
        AOT_TEST_CHECK(!heaps.isFreedWithOwner(heaps.rangeOf(*heap)));
}

namespace {

// root: reads, uses, collects, uses -> loop: uses, collects or not, uses -> done: uses
struct ReadsAroundCollections {
    ReadsAroundCollections(HeapFixture& fixture, const B3::AbstractHeap& heap, bool collectsInRoot, bool collectsInLoop)
        : fixture(fixture)
    {
        B3::Procedure& proc = fixture.proc;
        loop = proc.addBlock();
        done = proc.addBlock();
        load = fixture.appendLoad(heap);
        load->setReadsMutability(B3::Mutability::Immutable);
        appendUse(fixture.root);
        if (collectsInRoot)
            appendCollection(fixture.root);
        appendUse(fixture.root);
        fixture.root->appendNew<B3::Value>(proc, B3::Jump, B3::Origin());
        fixture.root->setSuccessors(B3::FrequentedBlock(loop));
        appendUse(loop);
        if (collectsInLoop)
            appendCollection(loop);
        appendUse(loop);
        loop->appendNew<B3::Value>(proc, B3::Branch, B3::Origin(), loop->appendNew<B3::ArgumentRegValue>(proc, B3::Origin(), GPRInfo::argumentGPR1));
        loop->setSuccessors(B3::FrequentedBlock(loop), B3::FrequentedBlock(done));
        appendUse(done);
        done->appendNew<B3::Value>(proc, B3::Return, B3::Origin());
        proc.resetReachability();
        fixture.heaps.computeRangesAndDecorateInstructions();

        B3::Policy policy;
        checkClaimsAtRunTime(policy, Typed::FunctionID(0u), { });
        policy.clientPhases[0].run->run(proc);
    }

    void appendUse(B3::BasicBlock* block)
    {
        B3::PatchpointValue* use = block->appendNew<B3::PatchpointValue>(fixture.proc, B3::Void, B3::Origin());
        use->effects = B3::Effects::none();
        use->effects.writesLocalState = true;
        use->append(load, B3::ValueRep::SomeRegister);
        uses.append(use);
    }

    void appendCollection(B3::BasicBlock* block)
    {
        B3::PatchpointValue* collection = block->appendNew<B3::PatchpointValue>(fixture.proc, B3::Void, B3::Origin());
        collection->effects = B3::Effects::none();
        B3HeapList written;
        fixture.heaps.appendHeapsWrittenByCollection(written);
        fixture.heaps.decoratePatchpointWrite(written.span(), collection);
    }

    // One letter a use, in the order root, root, loop, loop, done.
    String usesWithSecondRead() const
    {
        StringBuilder result;
        for (B3::BasicBlock* block : fixture.proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                if (!uses.contains(block->at(index)))
                    continue;
                B3::Value* before = index ? block->at(index - 1) : nullptr;
                bool isReadAgain = before && before->opcode() == B3::Patchpoint && !uses.contains(before) && before->numChildren() == 2 && before->child(1) == load;
                result.append(isReadAgain ? 'r' : '-');
            }
        }
        return result.toString();
    }

    HeapFixture& fixture;
    B3::BasicBlock* loop;
    B3::BasicBlock* done;
    B3::MemoryValue* load;
    Vector<B3::Value*, 5> uses;
};

} // anonymous namespace

static String usesWithSecondRead(B3::AbstractHeap AOTHeaps::* heap, bool collectsInRoot, bool collectsInLoop)
{
    HeapFixture fixture;
    return ReadsAroundCollections(fixture, fixture.heaps.*heap, collectsInRoot, collectsInLoop).usesWithSecondRead();
}

static void testWordsOutsideTheHeapAreReadAgainInFrontOfCollections()
{
    using Member = B3::AbstractHeap AOTHeaps::*;
    for (Member heap : std::initializer_list<Member> { &AOTHeaps::StringImpl_length, &AOTHeaps::StringImpl_data, &AOTHeaps::ArrayBuffer_shared }) {
        AOT_TEST_CHECK(usesWithSecondRead(heap, false, false) == "rrrrr"_s);
        AOT_TEST_CHECK(usesWithSecondRead(heap, true, false) == "r----"_s);
        AOT_TEST_CHECK(usesWithSecondRead(heap, false, true) == "rr---"_s);
        AOT_TEST_CHECK(usesWithSecondRead(heap, true, true) == "r----"_s);
    }
    for (Member heap : std::initializer_list<Member> { &AOTHeaps::JSCell_typeInfoType, &AOTHeaps::AOTClosure_entry, &AOTHeaps::JSArrayBuffer_impl }) {
        for (bool collectsInRoot : { false, true }) {
            for (bool collectsInLoop : { false, true })
                AOT_TEST_CHECK(usesWithSecondRead(heap, collectsInRoot, collectsInLoop) == "rrrrr"_s);
        }
    }
}

// The bit of a set of guards belongs to no class: its load names the entire heap, and whoever writes one place of the heap writes what it reads. The bit of a class is read at the place of the class alone.
static void testAWriteOfOnePlaceAmongTheGuardsIsSeenByALoadOfAllOfThem()
{
    for (uint32_t numberOfLayouts : { 2u, 40u, 200u }) {
        for (uint32_t numberOfOtherPlaces : { 0u, 300u }) {
            HeapFixture fixture;
            AOTHeaps& heaps = fixture.heaps;
            for (uint32_t index = 0; index < numberOfOtherPlaces; ++index) {
                fixture.appendLoad(fixture.at(MemoryLocation::objectField(PropertyKeyID(index))));
                fixture.appendLoad(fixture.at(MemoryLocation::moduleVariable(VariableID(index))));
            }
            B3::MemoryValue* loadOfSet = fixture.appendLoad(fixture.at(MemoryLocation(IRHeap::MemberGuards)));
            B3::MemoryValue* loadOfFirstClass = fixture.appendLoad(fixture.at(MemoryLocation::memberGuards(LayoutID(0u))));
            Vector<B3::PatchpointValue*> beginnings;
            for (uint32_t layout = 0; layout < numberOfLayouts; ++layout) {
                B3::PatchpointValue* fence = fixture.root->appendNew<B3::PatchpointValue>(fixture.proc, B3::Void, B3::Origin());
                fence->effects = B3::Effects::none();
                heaps.decoratePatchpointWrite(&fixture.at(Typed::beginningOfClass(LayoutID(layout))), fence);
                beginnings.append(fence);
            }
            heaps.computeRangesAndDecorateInstructions();
            for (B3::PatchpointValue* fence : beginnings) {
                AOT_TEST_CHECK(fence->effects.writes.overlaps(loadOfSet->range()));
                AOT_TEST_CHECK(!fence->effects.writes.overlaps(loadOfFirstClass->range()));
            }
        }
    }
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
        { "b3-heaps/ranges-number-the-leaves-in-order"_s, testRangesNumberTheLeavesInOrder },
        { "b3-heaps/ranges-of-heaps-that-grow-inside-one-another"_s, testRangesOfHeapsThatGrowInsideOneAnother },
        { "b3-heaps/write-sets-exclude-other-heaps"_s, testWriteSetsExcludeOtherHeaps },
        { "b3-heaps/a-write-of-one-place-among-the-guards-is-seen-by-a-load-of-all-of-them"_s, testAWriteOfOnePlaceAmongTheGuardsIsSeenByALoadOfAllOfThem },
        { "b3-heaps/calls-convert-cells"_s, testCallsConvertCells },
        { "b3-heaps/collections-write-weak-tables-alone"_s, testCollectionsWriteOnlyWeakTables },
        { "b3-heaps/what-cells-own-outside-the-heap"_s, testWhatCellsOwnOutsideTheHeap },
        { "b3-heaps/words-outside-the-heap-are-read-again-in-front-of-collections"_s, testWordsOutsideTheHeapAreReadAgainInFrontOfCollections },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
