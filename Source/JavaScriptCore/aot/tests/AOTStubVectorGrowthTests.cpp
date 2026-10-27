/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTStubRunner.h"
#include "AOTStubTestSupport.h"
#include "AOTStubsArrays.h"
#include "AOTStubsElements.h"
#include "ArrayConventions.h"
#include "ButterflyInlines.h"
#include "CellState.h"
#include "CompleteSubspace.h"
#include "IndexingType.h"
#include "MarkedSpace.h"
#include "PureNaN.h"
#include <algorithm>
#include <array>
#include <bit>
#include <span>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> stubVectorGrowthTests();

namespace {

constexpr uint8_t stateOfNewCell = static_cast<uint8_t>(CellState::DefinitelyWhite);
constexpr uint8_t stateOfOldCell = static_cast<uint8_t>(CellState::PossiblyBlack);

struct ArrayKind {
    IndexingType shape;
    uint32_t structureID;
    uint64_t hole;
    uint64_t (*element)(uint32_t index);
    JSValue appended;
    uint64_t appendedElement;
};

uint64_t boxed(JSValue value) { return static_cast<uint64_t>(JSValue::encode(value)); }
uint64_t boxedInteger(uint32_t index) { return boxed(jsNumber(static_cast<int32_t>(100 + index))); }
uint64_t rawDouble(uint32_t index) { return std::bit_cast<uint64_t>(index + 0.5); }

constexpr uint32_t int32StructureID = 0x1000;
constexpr uint32_t doubleStructureID = 0x1040;
constexpr uint32_t contiguousStructureID = 0x1080;
constexpr uint32_t structureIDWithNamedProperties = 0x10c0;

ArrayKind arrayOfIntegers() { return { Int32Shape, int32StructureID, 0, boxedInteger, jsNumber(7), boxed(jsNumber(7)) }; }

std::array<ArrayKind, 3> arrayKinds()
{
    return {
        arrayOfIntegers(),
        ArrayKind { DoubleShape, doubleStructureID, std::bit_cast<uint64_t>(PNaN), rawDouble, jsNumber(2.25), std::bit_cast<uint64_t>(2.25) },
        ArrayKind { ContiguousShape, contiguousStructureID, 0, boxedInteger, jsBoolean(true), boxed(jsBoolean(true)) },
    };
}

template<uint32_t capacity>
struct alignas(16) FakeVector {
    uint64_t* butterfly() { return reinterpret_cast<uint64_t*>(&vectorLength + 1); }

    void fill(const ArrayKind& kind)
    {
        publicLength = capacity;
        for (uint32_t index = 0; index < capacity; ++index)
            elements[index] = kind.element(index);
    }

    uint64_t unused { 0 };
    uint32_t publicLength { 0 };
    uint32_t vectorLength { capacity };
    std::array<uint64_t, capacity> elements { };
};

struct alignas(16) FakeArray {
    FakeArray(const ArrayKind& kind, uint64_t* butterfly)
        : structureID(kind.structureID)
        , indexingTypeAndMisc(IsArray | kind.shape)
        , butterfly(butterfly)
    {
    }

    uint64_t address() const { return std::bit_cast<uintptr_t>(this); }
    uint32_t publicLength() const { return reinterpret_cast<const uint32_t*>(butterfly)[-2]; }
    uint32_t vectorLength() const { return reinterpret_cast<const uint32_t*>(butterfly)[-1]; }

    uint32_t structureID;
    uint8_t indexingTypeAndMisc;
    uint8_t type { ArrayType };
    uint8_t flags { 0 };
    uint8_t cellState { stateOfNewCell };
    uint64_t* butterfly;
};
static_assert(OBJECT_OFFSETOF(FakeArray, structureID) == JSCell::structureIDOffset() && OBJECT_OFFSETOF(FakeArray, indexingTypeAndMisc) == JSCell::indexingTypeAndMiscOffset());
static_assert(OBJECT_OFFSETOF(FakeArray, type) == JSCell::typeInfoTypeOffset() && OBJECT_OFFSETOF(FakeArray, cellState) == JSCell::cellStateOffset());
static_assert(OBJECT_OFFSETOF(FakeArray, butterfly) == JSObject::butterflyOffset());
static_assert(Butterfly::offsetOfPublicLength() == -2 * static_cast<ptrdiff_t>(sizeof(uint32_t)) && Butterfly::offsetOfVectorLength() == -static_cast<ptrdiff_t>(sizeof(uint32_t)));

size_t sizeOfVector(size_t vectorLength) { return Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)); }

// What JSObject::ensureLengthSlow() asks the auxiliary space for when a full vector of this length gets one more element.
size_t sizeStepOfNextVector(uint32_t fullLength)
{
    return MarkedSpace::sizeClassToIndex(sizeOfVector(std::max<size_t>(BASE_CONTIGUOUS_VECTOR_LEN, nextLength(fullLength + 1))));
}

class GrowingArrays {
public:
    static constexpr size_t roomSize = 1024;

    GrowingArrays()
    {
        InstancePrefix& prefix = runner.instance().prefix();
        prefix.newArrayWithInt32Header = int32StructureID;
        prefix.newArrayWithDoubleHeader = doubleStructureID;
        prefix.newArrayWithContiguousHeader = contiguousStructureID;
        runner.instance().allocation.auxiliarySpace = m_auxiliarySpace.bytes.data();
        runner.instance().barrierThreshold = blackThreshold;
    }

    void setAllocator(size_t sizeStep, FakeAllocator* allocator)
    {
        AOT_TEST_CHECK(sizeStep < numberOfSizeSteps);
        m_auxiliarySpace.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + sizeStep * sizeof(void*)) = allocator;
    }

    StubRunner runner;
    FakeObject<roomSize> room;

private:
    static constexpr size_t numberOfSizeSteps = roomSize / MarkedSpace::sizeStep + 1;

    FakeObject<CompleteSubspace::offsetOfAllocatorForSizeStep() + numberOfSizeSteps * sizeof(void*)> m_auxiliarySpace;
};

using Append = StubRun (*)(StubRunner&, FakeArray&, uint32_t length, JSValue);

StubRun appendByPush(StubRunner& runner, FakeArray& array, uint32_t, JSValue value)
{
    return runner.run(arrayPush(), { array.address(), boxed(value) });
}

StubRun appendByStore(StubRunner& runner, FakeArray& array, uint32_t length, JSValue value)
{
    return runner.run(putByVal(false, ECMAMode::strict()), { array.address(), boxed(jsNumber(static_cast<int32_t>(length))), boxed(value) });
}

} // anonymous namespace

template<uint32_t fullLength>
static void checkGrowth(GrowingArrays& arrays, Append append, const ArrayKind& kind)
{
    // The cell is larger than the request, as a size class may be, and the vector takes all of it.
    const size_t sizeStep = sizeStepOfNextVector(fullLength);
    const size_t cellSize = (sizeStep + 1) * MarkedSpace::sizeStep;
    const uint32_t newVectorLength = static_cast<uint32_t>((cellSize - sizeof(IndexingHeader)) / sizeof(EncodedJSValue));
    AOT_TEST_CHECK(cellSize <= GrowingArrays::roomSize);

    FakeVector<fullLength> full;
    full.fill(kind);
    FakeArray array(kind, full.butterfly());
    FakeAllocator allocator { static_cast<uint32_t>(cellSize) };
    arrays.room.bytes.fill(0xab);
    allocator.makeRoomAt(arrays.room.address(), cellSize);
    arrays.setAllocator(sizeStep, &allocator);

    StubRun run = append(arrays.runner, array, fullLength, kind.appended);
    AOT_TEST_CHECK(!run.calledOperation && !allocator.hasRoom());
    AOT_TEST_CHECK(std::bit_cast<uintptr_t>(array.butterfly) == arrays.room.address() + sizeof(IndexingHeader));
    AOT_TEST_CHECK(array.structureID == kind.structureID && array.publicLength() == fullLength + 1 && array.vectorLength() == newVectorLength);
    for (uint32_t index = 0; index < newVectorLength; ++index) {
        uint64_t expected = index < fullLength ? kind.element(index) : index == fullLength ? kind.appendedElement : kind.hole;
        AOT_TEST_CHECK(array.butterfly[index] == expected);
    }
    AOT_TEST_CHECK(full.publicLength == fullLength && full.vectorLength == fullLength);
    arrays.setAllocator(sizeStep, nullptr);
}

static void checkGrowthOfEveryKind(Append append)
{
    GrowingArrays arrays;
    for (const ArrayKind& kind : arrayKinds()) {
        checkGrowth<0>(arrays, append, kind);
        checkGrowth<1>(arrays, append, kind);
        checkGrowth<3>(arrays, append, kind);
        checkGrowth<4>(arrays, append, kind);
        checkGrowth<7>(arrays, append, kind);
        checkGrowth<maxLengthForElementwiseCopy>(arrays, append, kind);
    }
}

static void testPushGrowsFullVectors() { checkGrowthOfEveryKind(appendByPush); }
static void testStoreAtLengthGrowsFullVectors() { checkGrowthOfEveryKind(appendByStore); }

template<uint32_t fullLength, typename Functor>
static void checkLeftToOperation(GrowingArrays& arrays, Append append, const ArrayKind& kind, const Functor& prepare)
{
    const size_t sizeStep = sizeStepOfNextVector(fullLength);
    const size_t cellSize = sizeStep * MarkedSpace::sizeStep;
    FakeVector<fullLength> full;
    full.fill(kind);
    FakeArray array(kind, full.butterfly());
    FakeAllocator allocator { static_cast<uint32_t>(cellSize) };
    allocator.makeRoomAt(arrays.room.address(), cellSize);
    arrays.setAllocator(sizeStep, &allocator);
    uint32_t index = prepare(array, allocator);

    StubRun run = append(arrays.runner, array, index, kind.appended);
    AOT_TEST_CHECK(run.calledOperation && array.butterfly == full.butterfly() && full.publicLength == fullLength && full.vectorLength == fullLength);
    AOT_TEST_CHECK(allocator.intervalStart() == arrays.room.address() || !allocator.hasRoom());
    for (uint32_t i = 0; i < fullLength; ++i)
        AOT_TEST_CHECK(full.elements[i] == kind.element(i));
    arrays.setAllocator(sizeStep, nullptr);
}

static void checkVectorsLeftToOperation(Append append)
{
    constexpr uint32_t fullLength = 3;
    GrowingArrays arrays;
    for (const ArrayKind& kind : arrayKinds()) {
        checkLeftToOperation<fullLength>(arrays, append, kind, [](FakeArray& array, FakeAllocator&) {
            array.cellState = stateOfOldCell;
            return fullLength;
        });
        checkLeftToOperation<fullLength>(arrays, append, kind, [](FakeArray& array, FakeAllocator&) {
            array.structureID = structureIDWithNamedProperties;
            return fullLength;
        });
        checkLeftToOperation<fullLength>(arrays, append, kind, [](FakeArray&, FakeAllocator& allocator) {
            allocator.makeRoomAt(0, 0);
            return fullLength;
        });
        checkLeftToOperation<fullLength>(arrays, append, kind, [&](FakeArray&, FakeAllocator&) {
            arrays.setAllocator(sizeStepOfNextVector(fullLength), nullptr);
            return fullLength;
        });
        checkLeftToOperation<maxLengthForElementwiseCopy + 1>(arrays, append, kind, [](FakeArray&, FakeAllocator&) {
            return maxLengthForElementwiseCopy + 1;
        });
    }
}

static void testPushLeavesOtherVectorsToOperation() { checkVectorsLeftToOperation(appendByPush); }

static void testStoreLeavesOtherVectorsToOperation()
{
    checkVectorsLeftToOperation(appendByStore);

    constexpr uint32_t fullLength = 3;
    GrowingArrays arrays;
    for (const ArrayKind& kind : arrayKinds()) {
        checkLeftToOperation<fullLength>(arrays, appendByStore, kind, [](FakeArray&, FakeAllocator&) {
            return fullLength + 1;
        });
    }
}

static void testValueOfOtherKindStillReachesOperation()
{
    constexpr uint32_t fullLength = 3;
    GrowingArrays arrays;
    const ArrayKind integers = arrayOfIntegers();
    const size_t sizeStep = sizeStepOfNextVector(fullLength);
    const size_t cellSize = sizeStep * MarkedSpace::sizeStep;
    FakeVector<fullLength> full;
    full.fill(integers);
    FakeArray array(integers, full.butterfly());
    FakeAllocator allocator { static_cast<uint32_t>(cellSize) };
    allocator.makeRoomAt(arrays.room.address(), cellSize);
    arrays.setAllocator(sizeStep, &allocator);

    StubRun run = appendByPush(arrays.runner, array, fullLength, jsBoolean(true));
    AOT_TEST_CHECK(run.calledOperation && array.publicLength() == fullLength);
    for (uint32_t index = 0; index < fullLength; ++index)
        AOT_TEST_CHECK(array.butterfly[index] == integers.element(index));
    for (uint32_t index = fullLength; index < array.vectorLength(); ++index)
        AOT_TEST_CHECK(array.butterfly[index] == integers.hole);
}

std::span<const NativeTest> stubVectorGrowthTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/vector-growth/push-grows-full-vectors"_s, ImageTests::whenExecutable<testPushGrowsFullVectors> },
        { "stubs/vector-growth/store-at-length-grows-full-vectors"_s, ImageTests::whenExecutable<testStoreAtLengthGrowsFullVectors> },
        { "stubs/vector-growth/push-leaves-other-vectors-to-operation"_s, ImageTests::whenExecutable<testPushLeavesOtherVectorsToOperation> },
        { "stubs/vector-growth/store-leaves-other-vectors-to-operation"_s, ImageTests::whenExecutable<testStoreLeavesOtherVectorsToOperation> },
        { "stubs/vector-growth/value-of-other-kind-still-reaches-operation"_s, ImageTests::whenExecutable<testValueOfOtherKindStillReachesOperation> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
