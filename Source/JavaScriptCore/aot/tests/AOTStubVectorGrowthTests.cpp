/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTStubRunner.h"
#include "AOTStubTestSupport.h"
#include "AOTSavedFrame.h"
#include "AOTStubsArrays.h"
#include "AOTStubsCoroutines.h"
#include "AOTStubsElements.h"
#include "AOTStubsPropertyKeys.h"
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

size_t sizeStepOfNextVector(uint32_t fullLength)
{
    return MarkedSpace::sizeClassToIndex(sizeOfVector(std::max<size_t>(BASE_CONTIGUOUS_VECTOR_LEN, nextLength(fullLength + 1))));
}

class GrowingArrays {
public:
    static constexpr size_t roomSize = 1024;
    static constexpr size_t margin = 2 * MarkedSpace::sizeStep;
    static constexpr uint8_t byteNobodyWrote = 0xab;

    GrowingArrays()
    {
        InstancePrefix& prefix = runner.instance().prefix();
        prefix.newArrayWithInt32Header = int32StructureID;
        prefix.newArrayWithDoubleHeader = doubleStructureID;
        prefix.newArrayWithContiguousHeader = contiguousStructureID;
        runner.instance().allocation.auxiliarySpace = m_auxiliarySpace.bytes.data();
        runner.instance().barrierThreshold = blackThreshold;
    }

    void makeRoomFor(FakeAllocator& allocator, size_t cellSize)
    {
        AOT_TEST_CHECK(margin + cellSize + margin <= roomSize);
        room.bytes.fill(byteNobodyWrote);
        allocator.makeRoomAt(addressOfCell(), cellSize);
    }

    uint64_t addressOfCell() { return room.address() + margin; }

    bool isWrittenOnlyInCell(size_t cellSize)
    {
        for (size_t offset = 0; offset < roomSize; ++offset) {
            if ((offset < margin || offset >= margin + cellSize) && room.bytes[offset] != byteNobodyWrote)
                return false;
        }
        return true;
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
    const size_t sizeStep = sizeStepOfNextVector(fullLength);
    const size_t cellSize = (sizeStep + 1) * MarkedSpace::sizeStep;
    const uint32_t newVectorLength = static_cast<uint32_t>((cellSize - sizeof(IndexingHeader)) / sizeof(EncodedJSValue));

    FakeVector<fullLength> full;
    full.fill(kind);
    FakeArray array(kind, full.butterfly());
    FakeAllocator allocator { static_cast<uint32_t>(cellSize) };
    arrays.makeRoomFor(allocator, cellSize);
    arrays.setAllocator(sizeStep, &allocator);

    StubRun run = append(arrays.runner, array, fullLength, kind.appended);
    AOT_TEST_CHECK(!run.calledOperation && !allocator.hasRoom());
    AOT_TEST_CHECK(std::bit_cast<uintptr_t>(array.butterfly) == arrays.addressOfCell() + sizeof(IndexingHeader));
    AOT_TEST_CHECK(arrays.isWrittenOnlyInCell(cellSize));
    AOT_TEST_CHECK(array.structureID == kind.structureID && array.publicLength() == fullLength + 1 && array.vectorLength() == newVectorLength);
    for (uint32_t index = 0; index < newVectorLength; ++index) {
        uint64_t expected = index < fullLength ? kind.element(index) : index == fullLength ? kind.appendedElement : kind.hole;
        AOT_TEST_CHECK(array.butterfly[index] == expected);
    }
    AOT_TEST_CHECK(full.publicLength == fullLength && full.vectorLength == fullLength);
    for (uint32_t index = 0; index < fullLength; ++index)
        AOT_TEST_CHECK(full.elements[index] == kind.element(index));
    arrays.setAllocator(sizeStep, nullptr);
}

static void checkGrowthOfEveryKind(Append append)
{
    GrowingArrays arrays;
    for (const ArrayKind& kind : arrayKinds()) {
        checkGrowth<0>(arrays, append, kind);
        checkGrowth<1>(arrays, append, kind);
        checkGrowth<2>(arrays, append, kind);
        checkGrowth<3>(arrays, append, kind);
        checkGrowth<4>(arrays, append, kind);
        checkGrowth<5>(arrays, append, kind);
        checkGrowth<6>(arrays, append, kind);
        checkGrowth<7>(arrays, append, kind);
        checkGrowth<9>(arrays, append, kind);
        checkGrowth<15>(arrays, append, kind);
        checkGrowth<25>(arrays, append, kind);
        checkGrowth<41>(arrays, append, kind);
        checkGrowth<maxLengthForElementwiseCopy - 1>(arrays, append, kind);
        checkGrowth<maxLengthForElementwiseCopy>(arrays, append, kind);
    }
}

template<uint32_t capacity, uint32_t length, uint32_t appended>
static void checkGrowthOfVectorThatIsNotFull(GrowingArrays& arrays, const ArrayKind& kind)
{
    static_assert(length <= capacity && length + appended > capacity);
    constexpr uint32_t newLength = length + appended;
    const size_t sizeStep = MarkedSpace::sizeClassToIndex(sizeOfVector(std::max<size_t>(BASE_CONTIGUOUS_VECTOR_LEN, nextLength(newLength))));
    const size_t cellSize = (sizeStep + 1) * MarkedSpace::sizeStep;
    const uint32_t newVectorLength = static_cast<uint32_t>((cellSize - sizeof(IndexingHeader)) / sizeof(EncodedJSValue));

    FakeVector<capacity> vector;
    vector.fill(kind);
    vector.publicLength = length;
    for (uint32_t index = length; index < capacity; ++index)
        vector.elements[index] = kind.hole;
    FakeArray array(kind, vector.butterfly());
    FakeVector<appended> others;
    others.fill(kind);
    FakeArray source(kind, others.butterfly());
    FakeAllocator allocator { static_cast<uint32_t>(cellSize) };
    arrays.makeRoomFor(allocator, cellSize);
    arrays.setAllocator(sizeStep, &allocator);

    StubRun run = arrays.runner.run(appendElements(), { array.address(), source.address() });
    AOT_TEST_CHECK(!run.calledOperation && !allocator.hasRoom());
    AOT_TEST_CHECK(std::bit_cast<uintptr_t>(array.butterfly) == arrays.addressOfCell() + sizeof(IndexingHeader));
    AOT_TEST_CHECK(arrays.isWrittenOnlyInCell(cellSize));
    AOT_TEST_CHECK(array.structureID == kind.structureID && array.publicLength() == newLength && array.vectorLength() == newVectorLength);
    for (uint32_t index = 0; index < newVectorLength; ++index) {
        uint64_t expected = index < length ? kind.element(index) : index < newLength ? kind.element(index - length) : kind.hole;
        AOT_TEST_CHECK(array.butterfly[index] == expected);
    }
    AOT_TEST_CHECK(vector.publicLength == length && vector.vectorLength == capacity);
    arrays.setAllocator(sizeStep, nullptr);
}

template<uint32_t capacity, uint32_t length>
static void checkGrowthByReservation(GrowingArrays& arrays, const ArrayKind& kind, uint32_t room)
{
    static_assert(length <= capacity);
    AOT_TEST_CHECK(room > capacity);
    const uint32_t neededLength = std::max(room - (room >> 2) - (room >> 4), capacity + 1);
    const size_t sizeStep = MarkedSpace::sizeClassToIndex(sizeOfVector(std::max<size_t>(BASE_CONTIGUOUS_VECTOR_LEN, nextLength(neededLength))));
    const size_t cellSize = (sizeStep + 1) * MarkedSpace::sizeStep;
    const uint32_t newVectorLength = static_cast<uint32_t>((cellSize - sizeof(IndexingHeader)) / sizeof(EncodedJSValue));

    FakeVector<capacity> vector;
    vector.fill(kind);
    vector.publicLength = length;
    for (uint32_t index = length; index < capacity; ++index)
        vector.elements[index] = kind.hole;
    FakeArray array(kind, vector.butterfly());
    FakeAllocator allocator { static_cast<uint32_t>(cellSize) };
    arrays.makeRoomFor(allocator, cellSize);
    arrays.setAllocator(sizeStep, &allocator);

    StubRun run = arrays.runner.run(reserveArrayCapacity(), { array.address(), std::bit_cast<uint64_t>(static_cast<double>(room)) });
    AOT_TEST_CHECK(!run.calledOperation && !allocator.hasRoom());
    AOT_TEST_CHECK(std::bit_cast<uintptr_t>(array.butterfly) == arrays.addressOfCell() + sizeof(IndexingHeader));
    AOT_TEST_CHECK(arrays.isWrittenOnlyInCell(cellSize));
    AOT_TEST_CHECK(array.structureID == kind.structureID && array.publicLength() == length && array.vectorLength() == newVectorLength);
    for (uint32_t index = 0; index < newVectorLength; ++index)
        AOT_TEST_CHECK(array.butterfly[index] == (index < length ? kind.element(index) : kind.hole));
    arrays.setAllocator(sizeStep, nullptr);
}

static void testReservedRoomIsAllHolesBehindLength()
{
    GrowingArrays arrays;
    for (const ArrayKind& kind : arrayKinds()) {
        checkGrowthByReservation<5, 0>(arrays, kind, 12);
        checkGrowthByReservation<5, 1>(arrays, kind, 12);
        checkGrowthByReservation<5, 2>(arrays, kind, 12);
        checkGrowthByReservation<5, 3>(arrays, kind, 12);
        checkGrowthByReservation<5, 4>(arrays, kind, 12);
        checkGrowthByReservation<5, 5>(arrays, kind, 12);
        checkGrowthByReservation<5, 0>(arrays, kind, 6);
        checkGrowthByReservation<9, 6>(arrays, kind, 40);
        checkGrowthByReservation<0, 0>(arrays, kind, 1);
    }
}

static void testAppendedElementsGrowVectorsThatAreNotFull()
{
    GrowingArrays arrays;
    for (const ArrayKind& kind : arrayKinds()) {
        checkGrowthOfVectorThatIsNotFull<5, 0, 6>(arrays, kind);
        checkGrowthOfVectorThatIsNotFull<5, 1, 5>(arrays, kind);
        checkGrowthOfVectorThatIsNotFull<5, 2, 4>(arrays, kind);
        checkGrowthOfVectorThatIsNotFull<5, 3, 3>(arrays, kind);
        checkGrowthOfVectorThatIsNotFull<5, 4, 2>(arrays, kind);
        checkGrowthOfVectorThatIsNotFull<4, 4, 1>(arrays, kind);
        checkGrowthOfVectorThatIsNotFull<9, 8, 7>(arrays, kind);
        checkGrowthOfVectorThatIsNotFull<0, 0, 1>(arrays, kind);
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

    StubRun run = appendByPush(arrays.runner, array, fullLength, jsNumber(2.25));
    AOT_TEST_CHECK(run.calledOperation && array.publicLength() == fullLength);
    for (uint32_t index = 0; index < fullLength; ++index)
        AOT_TEST_CHECK(array.butterfly[index] == integers.element(index));
    for (uint32_t index = fullLength; index < array.vectorLength(); ++index)
        AOT_TEST_CHECK(array.butterfly[index] == integers.hole);
}

static void testDefinitionUnderIndexIsStoredInPlaceOrLeftWhole()
{
    constexpr uint32_t capacity = 6;
    constexpr uint32_t length = 3;
    GrowingArrays arrays;
    for (const ArrayKind& kind : arrayKinds()) {
        const uint8_t asMade = IsArray | kind.shape;
        auto define = [&](uint8_t indexingTypeAndMisc, uint32_t lengthBefore, uint64_t key, uint64_t value, std::optional<uint64_t> stored) {
            FakeVector<capacity> vector;
            vector.fill(kind);
            vector.publicLength = lengthBefore;
            for (uint32_t index = lengthBefore; index < capacity; ++index)
                vector.elements[index] = kind.hole;
            const std::array<uint64_t, capacity> before = vector.elements;
            FakeArray array(kind, vector.butterfly());
            array.indexingTypeAndMisc = indexingTypeAndMisc;
            StubRun run = arrays.runner.run(defineByValCached(), { array.address(), key, value });
            AOT_TEST_CHECK(run.calledOperation == !stored && array.butterfly == vector.butterfly() && array.vectorLength() == capacity);
            AOT_TEST_CHECK(array.structureID == kind.structureID && array.indexingTypeAndMisc == indexingTypeAndMisc);
            const uint32_t place = stored ? static_cast<uint32_t>(JSValue::decode(static_cast<EncodedJSValue>(key)).asInt32()) : capacity;
            AOT_TEST_CHECK(array.publicLength() == (stored ? std::max(lengthBefore, place + 1) : lengthBefore));
            for (uint32_t index = 0; index < capacity; ++index)
                AOT_TEST_CHECK(vector.elements[index] == (index == place ? *stored : before[index]));
        };
        auto at = [](int32_t place) { return boxed(jsNumber(place)); };
        const uint64_t fits = boxed(kind.appended);

        for (int32_t place : { 0, 1, 2, 3, 4, 5 })
            define(asMade, length, at(place), fits, kind.appendedElement);
        define(asMade, length, at(capacity), fits, std::nullopt);
        define(asMade, length, at(capacity + 40), fits, std::nullopt);
        define(asMade, length, at(-1), fits, std::nullopt);
        define(asMade, length, boxed(jsDoubleNumber(1.0)), fits, std::nullopt);
        for (uint8_t other : { static_cast<uint8_t>(asMade | CopyOnWrite), static_cast<uint8_t>(kind.shape), static_cast<uint8_t>(ArrayWithArrayStorage), static_cast<uint8_t>(ArrayWithSlowPutArrayStorage), static_cast<uint8_t>(ArrayWithUndecided) })
            define(other, length, at(1), fits, std::nullopt);

        const uint64_t whole = boxed(jsNumber(7));
        const uint64_t fraction = boxed(jsDoubleNumber(2.25));
        const uint64_t minusZero = boxed(jsDoubleNumber(-0.0));
        const uint64_t notANumber = boxed(jsNaN());
        const uint64_t nothing = boxed(jsUndefined());
        switch (kind.shape) {
        case DoubleShape:
            define(asMade, capacity, at(capacity), fits, std::nullopt);
            for (int32_t place : { static_cast<int32_t>(capacity), static_cast<int32_t>(capacity + 40), -1 }) {
                for (uint64_t leftWhole : { whole, notANumber, nothing, arrays.room.address() })
                    define(asMade, length, at(place), leftWhole, std::nullopt);
            }
            for (int32_t place : { 1, 3, 4 }) {
                define(asMade, length, at(place), whole, std::bit_cast<uint64_t>(7.0));
                define(asMade, length, at(place), minusZero, std::bit_cast<uint64_t>(-0.0));
                define(asMade, length, at(place), notANumber, std::nullopt);
                define(asMade, length, at(place), nothing, std::nullopt);
                define(asMade, length, at(place), arrays.room.address(), std::nullopt);
            }
            break;
        case Int32Shape:
            for (int32_t place : { 1, 3, 4 }) {
                for (uint64_t noWholeNumber : { fraction, minusZero, notANumber, nothing })
                    define(asMade, length, at(place), noWholeNumber, std::nullopt);
            }
            break;
        case ContiguousShape:
            for (int32_t place : { 1, 3, 4 }) {
                for (uint64_t anyValue : { whole, fraction, minusZero, notANumber, nothing })
                    define(asMade, length, at(place), anyValue, anyValue);
            }
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
}

static void testHeapFrameIsBornWithEveryWordZeroAndNothingWrittenBesideIt()
{
    constexpr size_t roomSize = 512;
    constexpr size_t margin = 4 * MarkedSpace::sizeStep;
    constexpr uint8_t byteNobodyWrote = 0xab;
    constexpr uint64_t header = 0x0102030405060708ULL;
    constexpr uint64_t resumeAddress = 0x7654321000ULL;
    constexpr size_t numberOfSizeSteps = roomSize / MarkedSpace::sizeStep + 1;
    StubRunner runner;
    FakeObject<roomSize> room;
    FakeObject<CompleteSubspace::offsetOfAllocatorForSizeStep() + numberOfSizeSteps * sizeof(void*)> cellSpace;
    runner.instance().allocation.cellSpace = cellSpace.bytes.data();
    runner.instance().coroutines.savedFrameHeader = header;
    for (uint32_t numberOfWords = 1; numberOfWords <= 21; ++numberOfWords) {
        const size_t cellSize = roundUpToMultipleOf<MarkedSpace::sizeStep>(SavedFrame::allocationSize(numberOfWords));
        AOT_TEST_CHECK(margin + cellSize + margin <= roomSize);
        void*& allocatorOfSize = cellSpace.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + cellSize / MarkedSpace::sizeStep * sizeof(void*));
        FakeAllocator allocator { static_cast<uint32_t>(cellSize) };
        room.bytes.fill(byteNobodyWrote);
        allocator.makeRoomAt(room.address() + margin, cellSize);
        allocatorOfSize = &allocator;
        StubRun run = runner.run(newHeapFrame(), { numberOfWords, resumeAddress });
        AOT_TEST_CHECK(!run.calledOperation && run.result == room.address() + margin && !allocator.hasRoom());
        AOT_TEST_CHECK(room.at<uint64_t>(margin) == header && room.at<uint64_t>(margin + SavedFrame::offsetOfResumeAddress()) == resumeAddress);
        AOT_TEST_CHECK(room.at<uint64_t>(margin + SavedFrame::offsetOfNumberOfWords()) == numberOfWords && room.at<uint64_t>(margin + SavedFrame::offsetOfPromiseOrWaiter()) == run.result);
        for (uint32_t word = 0; word < numberOfWords; ++word)
            AOT_TEST_CHECK(!room.at<uint64_t>(margin + SavedFrame::offsetOfWords() + word * sizeof(uint64_t)));
        for (size_t offset = 0; offset < roomSize; ++offset)
            AOT_TEST_CHECK((offset >= margin && offset < margin + cellSize) || room.bytes[offset] == byteNobodyWrote);

        allocatorOfSize = nullptr;
        room.bytes.fill(byteNobodyWrote);
        AOT_TEST_CHECK(runner.run(newHeapFrame(), { numberOfWords, resumeAddress }).calledOperation);
        for (size_t offset = 0; offset < roomSize; ++offset)
            AOT_TEST_CHECK(room.bytes[offset] == byteNobodyWrote);
    }
}

static void checkThatIntegersAreWidened(Append append)
{
    constexpr uint32_t capacity = 4;
    constexpr uint32_t length = 2;
    GrowingArrays arrays;
    const ArrayKind integers = arrayOfIntegers();
    auto appendTo = [&](uint32_t structureID, uint8_t indexingTypeAndMisc) {
        FakeVector<capacity> vector;
        vector.fill(integers);
        vector.publicLength = length;
        vector.elements[2] = vector.elements[3] = integers.hole;
        FakeArray array(integers, vector.butterfly());
        array.structureID = structureID;
        array.indexingTypeAndMisc = indexingTypeAndMisc;
        StubRun run = append(arrays.runner, array, length, jsBoolean(true));
        bool isStored = array.publicLength() == length + 1 && array.butterfly[length] == boxed(jsBoolean(true));
        AOT_TEST_CHECK(array.butterfly[0] == integers.element(0) && array.butterfly[1] == integers.element(1) && array.butterfly[3] == integers.hole);
        AOT_TEST_CHECK(run.calledOperation != isStored);
        AOT_TEST_CHECK(!isStored || (array.structureID == contiguousStructureID && array.indexingTypeAndMisc == (IsArray | ContiguousShape)));
        AOT_TEST_CHECK(isStored || (array.structureID == structureID && array.indexingTypeAndMisc == indexingTypeAndMisc));
        return isStored;
    };
    AOT_TEST_CHECK(appendTo(int32StructureID, IsArray | Int32Shape));
    AOT_TEST_CHECK(!appendTo(structureIDWithNamedProperties, IsArray | Int32Shape));
    AOT_TEST_CHECK(!appendTo(int32StructureID, IsArray | Int32Shape | CopyOnWrite));
    arrays.runner.instance().prefix().newArrayWithContiguousHeader = 0;
    AOT_TEST_CHECK(!appendTo(int32StructureID, IsArray | Int32Shape));
}

static StubRun appendByStoreAtIndex(StubRunner& runner, FakeArray& array, uint32_t length, JSValue value)
{
    return runner.run(arrayPutAtIndex(ArrayStoredValue::Value), { array.address(), boxed(value), length });
}

static StubRun appendByStoreAtIndexInSloppyCode(StubRunner& runner, FakeArray& array, uint32_t length, JSValue value)
{
    return runner.run(arrayPutAtIndex(ArrayStoredValue::ValueInSloppyCode), { array.address(), boxed(value), length });
}

static void testWhatIsNoNumberWidensIntegers()
{
    checkThatIntegersAreWidened(appendByPush);
    checkThatIntegersAreWidened(appendByStoreAtIndex);
    checkThatIntegersAreWidened(appendByStoreAtIndexInSloppyCode);
}

namespace {

class LiteralsBornHoldingLists {
public:
    static constexpr uint32_t longest = maxHoleArrayCapacity + 1;
    static constexpr size_t offsetOfLengths = sizeof(JSArray);
    static constexpr size_t offsetOfElements = offsetOfLengths + sizeof(IndexingHeader);
    static constexpr size_t roomSize = offsetOfElements + maxHoleArrayCapacity * sizeof(EncodedJSValue);
    static constexpr uint64_t answerOfOperation = 0x7770;

    struct Born {
        StubRun run;
        uint32_t capacity;
        bool tookRoom;
    };

    LiteralsBornHoldingLists()
    {
        InstancePrefix& prefix = runner.instance().prefix();
        prefix.newArrayWithInt32Header = int32StructureID;
        prefix.newArrayWithDoubleHeader = doubleStructureID;
        prefix.newArrayWithContiguousHeader = contiguousStructureID;
        runner.instance().allocation.cellSpace = m_cellSpace.bytes.data();
        runner.recordCallsOf(Entry::operationAOTNewArrayOfList);
        runner.setResultOfRecordedOperation(answerOfOperation);
    }

    template<typename Functor>
    Born bear(bool namesValues, IndexingType shape, std::span<const uint64_t> words, uint32_t roomBehind, const Functor& prepare)
    {
        AOT_TEST_CHECK(words.size() <= longest);
        m_given.publicLength = static_cast<uint32_t>(words.size());
        m_given.elements.fill(0xcdcdcdcdcdcdcdcdull);
        std::ranges::copy(words, m_given.elements.begin());
        FakeArray list(ArrayKind { shape, 0x2000, 0, nullptr, JSValue(), 0 }, m_given.butterfly());
        list.flags = IsPristine;

        const uint32_t capacity = (m_given.publicLength + roomBehind) | 1;
        const size_t cellSize = offsetOfElements + capacity * sizeof(EncodedJSValue);
        const size_t sizeStep = MarkedSpace::sizeClassToIndex(cellSize);
        FakeAllocator allocator { static_cast<uint32_t>(cellSize) };
        room.bytes.fill(0xab);
        if (sizeStep < numberOfSizeSteps) {
            allocator.makeRoomAt(room.address(), cellSize);
            m_cellSpace.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + sizeStep * sizeof(void*)) = &allocator;
        }
        uint64_t given = list.address();
        prepare(list, allocator, given);
        if (sizeStep < numberOfSizeSteps && !allocator.intervalEnd() && !allocator.intervalStart())
            m_cellSpace.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + sizeStep * sizeof(void*)) = m_leavesAllocatorIn ? &allocator : nullptr;

        Born born { runner.run(newArrayOfList(namesValues), { given, roomBehind }), capacity, sizeStep < numberOfSizeSteps && allocator.intervalStart() != room.address() };
        if (sizeStep < numberOfSizeSteps)
            m_cellSpace.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + sizeStep * sizeof(void*)) = nullptr;
        AOT_TEST_CHECK(m_given.publicLength == words.size() && std::ranges::equal(words, std::span { m_given.elements }.first(words.size())));
        return born;
    }

    Born bear(bool namesValues, IndexingType shape, std::span<const uint64_t> words, uint32_t roomBehind)
    {
        return bear(namesValues, shape, words, roomBehind, [](FakeArray&, FakeAllocator&, uint64_t&) { });
    }

    void checkCell(uint32_t structureID, uint32_t length, uint32_t capacity)
    {
        AOT_TEST_CHECK(room.at<uint64_t>(0) == structureID && room.at<uint64_t>(JSObject::butterflyOffset()) == room.address() + offsetOfElements);
        AOT_TEST_CHECK(room.at<uint32_t>(offsetOfLengths) == length && room.at<uint32_t>(offsetOfLengths + sizeof(uint32_t)) == capacity);
    }

    void checkBorn(bool namesValues, IndexingType shape, std::span<const uint64_t> words, uint32_t roomBehind, uint32_t structureID, std::span<const uint64_t> expected)
    {
        Born born = bear(namesValues, shape, words, roomBehind);
        AOT_TEST_CHECK(!born.run.numberOfRecordedCalls && born.tookRoom && born.run.result == room.address() && expected.size() == words.size());
        checkCell(structureID, static_cast<uint32_t>(words.size()), born.capacity);
        const uint64_t hole = structureID == doubleStructureID ? std::bit_cast<uint64_t>(PNaN) : 0;
        for (uint32_t index = 0; index < born.capacity; ++index)
            AOT_TEST_CHECK(room.at<uint64_t>(offsetOfElements + index * sizeof(uint64_t)) == (index < expected.size() ? expected[index] : hole));
    }

    void checkBornAsGiven(bool namesValues, IndexingType shape, std::span<const uint64_t> words, uint32_t roomBehind, uint32_t structureID)
    {
        checkBorn(namesValues, shape, words, roomBehind, structureID, words);
    }

    template<typename Functor>
    void checkNotServed(bool namesValues, IndexingType shape, std::span<const uint64_t> words, uint32_t roomBehind, const Functor& prepare)
    {
        Born born = bear(namesValues, shape, words, roomBehind, prepare);
        AOT_TEST_CHECK(!born.run.numberOfRecordedCalls && !born.tookRoom && !born.run.result);
    }

    void checkNotServed(bool namesValues, IndexingType shape, std::span<const uint64_t> words, uint32_t roomBehind)
    {
        checkNotServed(namesValues, shape, words, roomBehind, [](FakeArray&, FakeAllocator&, uint64_t&) { });
    }

    void checkLeftToOperation(bool namesValues, bool leavesAllocatorIn)
    {
        const std::array<uint64_t, 3> words { boxedInteger(0), boxedInteger(1), boxedInteger(2) };
        uint64_t address = 0;
        m_leavesAllocatorIn = leavesAllocatorIn;
        Born born = bear(namesValues, ContiguousShape, words, 2, [&](FakeArray& list, FakeAllocator& allocator, uint64_t&) {
            allocator.makeRoomAt(0, 0);
            address = list.address();
        });
        AOT_TEST_CHECK(born.run.numberOfRecordedCalls == 1 && born.run.result == answerOfOperation);
        AOT_TEST_CHECK(born.run.recordedArguments[0] == address && static_cast<uint32_t>(born.run.recordedArguments[1]) == (born.capacity << 1 | namesValues));
    }

    StubRunner runner;
    FakeObject<roomSize> room;

private:
    static constexpr size_t numberOfSizeSteps = roomSize / MarkedSpace::sizeStep + 1;

    FakeVector<longest> m_given;
    FakeObject<CompleteSubspace::offsetOfAllocatorForSizeStep() + numberOfSizeSteps * sizeof(void*)> m_cellSpace;
    bool m_leavesAllocatorIn { false };
};

} // anonymous namespace

static void testStorageOfLiteralBornHoldingListGoesByWhatItHolds()
{
    LiteralsBornHoldingLists literals;
    const uint64_t a = boxedInteger(0), b = boxedInteger(1), c = boxedInteger(2), nothing = boxed(jsUndefined());
    const std::array<uint64_t, 3> wholeNumbers { a, b, c };
    for (IndexingType shape : { Int32Shape, ContiguousShape }) {
        for (uint32_t roomBehind : { 0u, 1u, 2u, 5u }) {
            literals.checkBornAsGiven(false, shape, wholeNumbers, roomBehind, int32StructureID);
            literals.checkBornAsGiven(true, shape, wholeNumbers, roomBehind, contiguousStructureID);
            literals.checkBornAsGiven(false, shape, { }, roomBehind, int32StructureID);
            literals.checkBornAsGiven(true, shape, { }, roomBehind, contiguousStructureID);
        }
        for (bool namesValues : { false, true }) {
            for (uint32_t place = 0; place < wholeNumbers.size(); ++place) {
                std::array<uint64_t, 3> given = wholeNumbers, expected = wholeNumbers;
                given[place] = 0;
                expected[place] = nothing;
                literals.checkBorn(namesValues, shape, given, 1, contiguousStructureID, expected);
            }
        }
    }
    for (JSValue odd : { jsBoolean(true), jsUndefined(), jsNull(), jsNumber(2.5), jsNumber(-0.0), jsNumber(2147483648.0), JSValue(std::bit_cast<JSCell*>(literals.room.address())) }) {
        for (bool namesValues : { false, true }) {
            for (uint32_t place = 0; place < wholeNumbers.size(); ++place) {
                std::array<uint64_t, 3> given = wholeNumbers;
                given[place] = boxed(odd);
                literals.checkBornAsGiven(namesValues, ContiguousShape, given, 1, contiguousStructureID);
            }
        }
    }
    Vector<uint64_t> most(maxHoleArrayCapacity, [](size_t index) { return boxedInteger(static_cast<uint32_t>(index)); });
    literals.checkBornAsGiven(false, ContiguousShape, most.span(), 0, int32StructureID);
    most.last() = boxed(jsBoolean(false));
    literals.checkBornAsGiven(false, ContiguousShape, most.span(), 0, contiguousStructureID);
}

static void testLiteralBornHoldingFractions()
{
    LiteralsBornHoldingLists literals;
    const std::array<uint64_t, 4> fractions { rawDouble(0), std::bit_cast<uint64_t>(-0.0), std::bit_cast<uint64_t>(std::numeric_limits<double>::infinity()), std::bit_cast<uint64_t>(-std::numeric_limits<double>::infinity()) };
    auto boxedFraction = [](uint64_t bits) { return boxed(JSValue(JSValue::EncodeAsDouble, std::bit_cast<double>(bits))); };
    std::array<uint64_t, 4> boxedFractions;
    std::ranges::transform(fractions, boxedFractions.begin(), boxedFraction);
    for (uint32_t roomBehind : { 0u, 1u, 2u, 5u }) {
        literals.checkBornAsGiven(false, DoubleShape, fractions, roomBehind, doubleStructureID);
        literals.checkBornAsGiven(false, DoubleShape, { }, roomBehind, doubleStructureID);
        literals.checkBorn(true, DoubleShape, fractions, roomBehind, contiguousStructureID, boxedFractions);
        literals.checkBornAsGiven(true, DoubleShape, { }, roomBehind, contiguousStructureID);
    }
    for (uint64_t hole : std::initializer_list<uint64_t> { std::bit_cast<uint64_t>(PNaN), 0xfff8000000000000ull, 0x7ff0000000000001ull, 0xffffffffffffffffull }) {
        for (uint32_t place = 0; place < fractions.size(); ++place) {
            std::array<uint64_t, 4> given = fractions, expected = boxedFractions;
            given[place] = hole;
            expected[place] = boxed(jsUndefined());
            for (uint32_t roomBehind : { 0u, 1u, 2u, 5u }) {
                for (bool namesValues : { false, true }) {
                    literals.checkBorn(namesValues, DoubleShape, given, roomBehind, contiguousStructureID, expected);
                    if (place < 3)
                        literals.checkBorn(namesValues, DoubleShape, std::span { given }.first(3), roomBehind, contiguousStructureID, std::span { expected }.first(3));
                }
            }
        }
    }
    const uint64_t hole = std::bit_cast<uint64_t>(PNaN), nothing = boxed(jsUndefined());
    const std::array<uint64_t, 4> holesAtBothEnds { hole, fractions[1], fractions[2], hole }, undefinedAtBothEnds { nothing, boxedFractions[1], boxedFractions[2], nothing };
    const std::array<uint64_t, 4> holesAlone { hole, hole, hole, hole }, undefinedAlone { nothing, nothing, nothing, nothing };
    const std::array<uint64_t, 3> oddFractions { std::bit_cast<uint64_t>(5e-324), std::bit_cast<uint64_t>(4.0), hole }, oddFractionsBoxed { boxedFraction(oddFractions[0]), boxedFraction(oddFractions[1]), nothing };
    Vector<uint64_t> most(maxHoleArrayCapacity, [](size_t index) { return rawDouble(static_cast<uint32_t>(index)); });
    literals.checkBornAsGiven(false, DoubleShape, most.span(), 0, doubleStructureID);
    Vector<uint64_t> mostBoxed = most.map(boxedFraction);
    most.last() = hole;
    mostBoxed.last() = nothing;
    for (bool namesValues : { false, true }) {
        for (uint32_t roomBehind : { 0u, 1u, 2u, 5u }) {
            literals.checkBorn(namesValues, DoubleShape, holesAtBothEnds, roomBehind, contiguousStructureID, undefinedAtBothEnds);
            literals.checkBorn(namesValues, DoubleShape, holesAlone, roomBehind, contiguousStructureID, undefinedAlone);
            literals.checkBorn(namesValues, DoubleShape, oddFractions, roomBehind, contiguousStructureID, oddFractionsBoxed);
        }
        literals.checkBorn(namesValues, DoubleShape, most.span(), 0, contiguousStructureID, mostBoxed.span());
    }
}

static void testListsThatNoLiteralIsBornHolding()
{
    LiteralsBornHoldingLists literals;
    const std::array<uint64_t, 3> words { boxedInteger(0), boxedInteger(1), boxedInteger(2) };
    Vector<uint64_t> tooMany(LiteralsBornHoldingLists::longest, [](size_t index) { return boxedInteger(static_cast<uint32_t>(index)); });
    for (bool namesValues : { false, true }) {
        literals.checkNotServed(namesValues, ContiguousShape, words, 1, [](FakeArray&, FakeAllocator&, uint64_t& given) { given = boxed(jsNumber(7)); });
        literals.checkNotServed(namesValues, ContiguousShape, words, 1, [](FakeArray&, FakeAllocator&, uint64_t& given) { given = boxed(jsUndefined()); });
        literals.checkNotServed(namesValues, ContiguousShape, words, 1, [](FakeArray& list, FakeAllocator&, uint64_t&) { list.type = ObjectType; });
        literals.checkNotServed(namesValues, ContiguousShape, words, 1, [](FakeArray& list, FakeAllocator&, uint64_t&) { list.type = DerivedArrayType; });
        literals.checkNotServed(namesValues, ContiguousShape, words, 1, [](FakeArray& list, FakeAllocator&, uint64_t&) { list.flags = 0; });
        for (IndexingType shape : { NoIndexingShape, UndecidedShape, ArrayStorageShape, SlowPutArrayStorageShape })
            literals.checkNotServed(namesValues, shape, words, 1);
        literals.checkNotServed(namesValues, ContiguousShape, tooMany.span(), 0);
        literals.checkNotServed(namesValues, ContiguousShape, tooMany.span().first(maxHoleArrayCapacity), 1);
        literals.checkNotServed(namesValues, ContiguousShape, words, maxHoleArrayCapacity);
        literals.checkLeftToOperation(namesValues, false);
        literals.checkLeftToOperation(namesValues, true);
    }
    literals.runner.instance().prefix().newArrayWithContiguousHeader = 0;
    for (bool namesValues : { false, true })
        literals.checkNotServed(namesValues, ContiguousShape, words, 1);
}

std::span<const NativeTest> stubVectorGrowthTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/vector-growth/push-grows-full-vectors"_s, ImageTests::whenExecutable<testPushGrowsFullVectors> },
        { "stubs/vector-growth/store-at-length-grows-full-vectors"_s, ImageTests::whenExecutable<testStoreAtLengthGrowsFullVectors> },
        { "stubs/vector-growth/appended-elements-grow-vectors-that-are-not-full"_s, ImageTests::whenExecutable<testAppendedElementsGrowVectorsThatAreNotFull> },
        { "stubs/vector-growth/reserved-room-is-all-holes-behind-the-length"_s, ImageTests::whenExecutable<testReservedRoomIsAllHolesBehindLength> },
        { "stubs/vector-growth/push-leaves-other-vectors-to-operation"_s, ImageTests::whenExecutable<testPushLeavesOtherVectorsToOperation> },
        { "stubs/vector-growth/store-leaves-other-vectors-to-operation"_s, ImageTests::whenExecutable<testStoreLeavesOtherVectorsToOperation> },
        { "stubs/vector-growth/value-of-other-kind-still-reaches-operation"_s, ImageTests::whenExecutable<testValueOfOtherKindStillReachesOperation> },
        { "stubs/vector-growth/what-is-no-number-widens-integers-where-it-stands"_s, ImageTests::whenExecutable<testWhatIsNoNumberWidensIntegers> },
        { "stubs/definition-under-an-index/is-stored-in-place-or-left-whole-to-the-operation"_s, ImageTests::whenExecutable<testDefinitionUnderIndexIsStoredInPlaceOrLeftWhole> },
        { "stubs/heap-frame/is-born-with-every-word-zero-and-nothing-written-beside-it"_s, ImageTests::whenExecutable<testHeapFrameIsBornWithEveryWordZeroAndNothingWrittenBesideIt> },
        { "stubs/literal-born-holding-list/its-storage-goes-by-what-it-holds"_s, ImageTests::whenExecutable<testStorageOfLiteralBornHoldingListGoesByWhatItHolds> },
        { "stubs/literal-born-holding-list/fractions-are-held-raw-and-as-values-where-a-hole-is-among-them"_s, ImageTests::whenExecutable<testLiteralBornHoldingFractions> },
        { "stubs/literal-born-holding-list/other-lists-are-not-served-and-a-dry-allocator-calls-the-operation"_s, ImageTests::whenExecutable<testListsThatNoLiteralIsBornHolding> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
