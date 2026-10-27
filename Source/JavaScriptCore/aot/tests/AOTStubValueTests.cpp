/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTStubRunner.h"
#include "AOTStubTestSupport.h"
#include "AOTStubsStrings.h"
#include "AOTStubsValues.h"
#include "FreeList.h"
#include "JSString.h"
#include "PureNaN.h"
#include <bit>
#include <limits>
#include <span>
#include <wtf/text/StringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> stubValueTests();

namespace {

constexpr uint64_t stringHeader = 0x0102030400000011ULL;
constexpr size_t sizeOfRope = 4 * sizeof(uint64_t);
constexpr uint64_t secretOfFreeList = 0x1234567890abcdefULL;

struct alignas(16) FakeFlatString {
    explicit FakeFlatString(StringImpl& impl)
        : fiber(std::bit_cast<uintptr_t>(&impl))
    {
    }

    uint64_t address() const { return std::bit_cast<uintptr_t>(this); }

    uint32_t structureID { 0 };
    uint8_t indexingTypeAndMisc { 0 };
    uint8_t type { StringType };
    uint8_t flags { 0 };
    uint8_t cellState { 0 };
    uintptr_t fiber;
};
static_assert(OBJECT_OFFSETOF(FakeFlatString, type) == JSCell::typeInfoTypeOffset() && OBJECT_OFFSETOF(FakeFlatString, fiber) == JSString::offsetOfValue());

struct RopeAllocation {
    RopeAllocation()
    {
        allocator.at<uint64_t>(LocalAllocator::offsetOfFreeList() + FreeList::offsetOfSecret()) = secretOfFreeList;
        runner.instance().prefix().ropeStringAllocator = &allocator;
        runner.instance().prefix().stringHeader = stringHeader;
    }

    void leaveOnlyNextInterval()
    {
        room.bytes.fill(0);
        room.at<uint64_t>(FreeCell::offsetOfScrambledBits()) = (static_cast<uint64_t>(sizeof(room.bytes)) << 32 | FakeAllocator::noNextInterval) ^ secretOfFreeList;
        allocator.makeRoomAt(0, 0);
        allocator.nextInterval() = room.address();
    }

    bool tookFirstCellOfNextInterval()
    {
        return allocator.intervalStart() == room.address() + sizeOfRope && allocator.intervalEnd() == room.address() + sizeof(room.bytes) && allocator.nextInterval() == room.address() + FakeAllocator::noNextInterval;
    }

    uint64_t word(unsigned index) { return room.at<uint64_t>(index * sizeof(uint64_t)); }

    StubRunner runner;
    FakeAllocator allocator { sizeOfRope };
    FakeObject<2 * sizeOfRope> room;
};

uint64_t bitsOfDouble(double number) { return static_cast<uint64_t>(JSValue::encode(JSValue(JSValue::EncodeAsDouble, number))); }

} // anonymous namespace

static void testRopesFromEveryStateOfTheFreeList()
{
    using CompactFibers = JSRopeString::CompactFibers;
    RopeAllocation ropes;
    Ref<StringImpl> firstText = StringImpl::create("ab"_span);
    Ref<StringImpl> secondText = StringImpl::create("cde"_span);
    FakeFlatString first(firstText.get());
    FakeFlatString second(secondText.get());
    auto isRopeOfBoth = [&] {
        return ropes.word(0) == stringHeader && ropes.word(1) == (first.address() | JSString::isRopeInPointer | JSRopeString::is8BitInPointer)
            && ropes.word(2) == (second.address() << CompactFibers::shiftOfFiber1LowerInLengthWord | 5) && ropes.word(3) == second.address() >> CompactFibers::widthOfFiber1LowerInLengthWord;
    };

    ropes.allocator.makeRoomAt(ropes.room.address(), sizeof(ropes.room.bytes));
    StubRun run = ropes.runner.run(concatenate(2), { first.address(), second.address() });
    AOT_TEST_CHECK(!run.calledOperation && run.result == ropes.room.address() && isRopeOfBoth());
    AOT_TEST_CHECK(ropes.allocator.intervalStart() == ropes.room.address() + sizeOfRope);

    ropes.leaveOnlyNextInterval();
    run = ropes.runner.run(concatenate(2), { first.address(), second.address() });
    AOT_TEST_CHECK(!run.calledOperation && run.result == ropes.room.address() && isRopeOfBoth() && ropes.tookFirstCellOfNextInterval());

    ropes.allocator.makeRoomAt(0, 0);
    ropes.allocator.nextInterval() = FakeAllocator::noNextInterval;
    ropes.runner.recordCallsOf(Entry::operationAOTConcat);
    run = ropes.runner.run(concatenate(2), { first.address(), second.address() });
    ropes.runner.countCallsOf(Entry::operationAOTConcat);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == first.address() && run.recordedArguments[1] == second.address());
}

static void testSubstringsFromEveryStateOfTheFreeList()
{
    using CompactFibers = JSRopeString::CompactFibers;
    RopeAllocation ropes;
    Ref<StringImpl> text = StringImpl::create("abcdef"_span);
    FakeFlatString string(text.get());
    auto isSubstring = [&] {
        return ropes.word(0) == stringHeader && ropes.word(1) == (JSString::isRopeInPointer | JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer)
            && ropes.word(2) == (string.address() << CompactFibers::shiftOfFiber1LowerInLengthWord | 3)
            && ropes.word(3) == (string.address() >> CompactFibers::widthOfFiber1LowerInLengthWord | 2ull << CompactFibers::shiftOfFiber2InUpperWord);
    };

    ropes.allocator.makeRoomAt(ropes.room.address(), sizeof(ropes.room.bytes));
    StubRun run = ropes.runner.run(substring(), { string.address(), 2, 5 });
    AOT_TEST_CHECK(!run.calledOperation && run.result == ropes.room.address() && isSubstring());

    ropes.leaveOnlyNextInterval();
    run = ropes.runner.run(substring(), { string.address(), 2, 5 });
    AOT_TEST_CHECK(!run.calledOperation && run.result == ropes.room.address() && isSubstring() && ropes.tookFirstCellOfNextInterval());

    ropes.allocator.makeRoomAt(0, 0);
    ropes.allocator.nextInterval() = FakeAllocator::noNextInterval;
    AOT_TEST_CHECK(ropes.runner.run(substring(), { string.address(), 2, 5 }).calledOperation);
}

static void testTruthinessOfDoubles()
{
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    for (StubKey key : { toBoolean(), valueIsTruthy() }) {
        for (FPRReg marked : markedFPRs)
            AOT_TEST_CHECK(!containsRegister(catalog.summaryOf(key).clobbered, marked));
    }

    StubRunner runner;
    constexpr double infinity = std::numeric_limits<double>::infinity();
    for (double truthy : { 0.5, -1.5, 1e300, std::numeric_limits<double>::denorm_min(), -std::numeric_limits<double>::denorm_min(), infinity, -infinity }) {
        StubRun run = runner.run(toBoolean(), { bitsOfDouble(truthy) });
        AOT_TEST_CHECK(!run.calledOperation && run.result == 1);
    }
    for (double falsy : { 0.0, -0.0, PNaN, -PNaN }) {
        StubRun run = runner.run(toBoolean(), { bitsOfDouble(falsy) });
        AOT_TEST_CHECK(!run.calledOperation && !run.result);
    }
}

std::span<const NativeTest> stubValueTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/concatenate/ropes-from-every-state-of-the-free-list"_s, ImageTests::whenExecutable<testRopesFromEveryStateOfTheFreeList> },
        { "stubs/substring/ropes-from-every-state-of-the-free-list"_s, ImageTests::whenExecutable<testSubstringsFromEveryStateOfTheFreeList> },
        { "stubs/to-boolean/doubles"_s, ImageTests::whenExecutable<testTruthinessOfDoubles> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
