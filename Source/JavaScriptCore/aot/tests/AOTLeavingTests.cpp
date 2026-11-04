/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageFormat.h"
#include "AOTImageTestSupport.h"
#include "AOTLeaveRows.h"
#include "AOTLeaveRowsOfCode.h"
#include "AOTPlanOfArrival.h"
#include "AOTNativeTest.h"
#include "AOTStubRunner.h"
#include "AOTStubsLeaving.h"
#include "CCallHelpers.h"
#include "JSCJSValueInlines.h"
#include "LinkBuffer.h"
#include "RegisterSet.h"
#include "StackAlignment.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> leavingTests();

static uint64_t bitsOf(JSValue value) { return static_cast<uint64_t>(JSValue::encode(value)); }

static bool isSameValue(const ValueInLeaveRow& left, const ValueInLeaveRow& right)
{
    return left.slot == right.slot && left.form == right.form && left.place == right.place && left.where == right.where;
}

static bool isPart(LeaveRowReader::Part part, PartOfLeaveRow kind, uint32_t words)
{
    return part.kind == kind && part.words == words;
}

static void testRowsAreReadAsWritten()
{
    const ValueInLeaveRow inner[] = {
        { 1, FormInLeaveRow::JSValue, PlaceInLeaveRow::Register, 0 },
        { 2, FormInLeaveRow::Int32, PlaceInLeaveRow::Stack, 7 },
        { 9, FormInLeaveRow::Double, PlaceInLeaveRow::Register, Reg::maxIndex() },
        { 300, FormInLeaveRow::Int64, PlaceInLeaveRow::Stack, 40000 },
        { 301, FormInLeaveRow::Int32, PlaceInLeaveRow::Constant, static_cast<uint64_t>(-5) },
        { 70000, FormInLeaveRow::Double, PlaceInLeaveRow::Constant, std::bit_cast<uint64_t>(-1.5) },
    };
    const ValueInLeaveRow outer[] = {
        { 4, FormInLeaveRow::Boolean, PlaceInLeaveRow::Stack, 1 },
        { 5, FormInLeaveRow::Cell, PlaceInLeaveRow::Constant, 0x7fff12345678ULL },
        { 6, FormInLeaveRow::JSValue, PlaceInLeaveRow::Constant, bitsOf(jsUndefined()) },
    };

    Vector<uint32_t> words;
    words.append(0xabababab);
    LeaveRowWriter first(words, 2);
    first.beginFrame(70000, std::size(inner));
    for (const ValueInLeaveRow& value : inner)
        first.add(value);
    first.beginFrame(6, std::size(outer));
    for (const ValueInLeaveRow& value : outer)
        first.add(value);
    uint32_t firstRow = first.finish();
    AOT_TEST_CHECK(firstRow == 1);

    LeaveRowWriter second(words, 1);
    second.beginFrame(0, 0);
    uint32_t secondRow = second.finish();
    AOT_TEST_CHECK(secondRow > firstRow && secondRow + 1 == words.size());

    LeaveRowReader reader(words.span().subspan(firstRow));
    AOT_TEST_CHECK(reader.partsLeft() == 2);
    AOT_TEST_CHECK(isPart(reader.beginPart(), PartOfLeaveRow::Frame, 70000) && reader.valuesLeft() == std::size(inner));
    for (const ValueInLeaveRow& value : inner)
        AOT_TEST_CHECK(isSameValue(reader.next(), value));
    AOT_TEST_CHECK(reader.partsLeft() == 1 && !reader.valuesLeft());
    AOT_TEST_CHECK(isPart(reader.beginPart(), PartOfLeaveRow::Frame, 6) && reader.valuesLeft() == std::size(outer));
    for (const ValueInLeaveRow& value : outer)
        AOT_TEST_CHECK(isSameValue(reader.next(), value));
    AOT_TEST_CHECK(!reader.partsLeft() && !reader.valuesLeft());

    LeaveRowReader emptyFrame(words.span().subspan(secondRow));
    AOT_TEST_CHECK(emptyFrame.partsLeft() == 1 && isPart(emptyFrame.beginPart(), PartOfLeaveRow::Frame, 0) && !emptyFrame.valuesLeft() && !emptyFrame.partsLeft());

    // Heap words, a call that takes the cell in a register and on the stack, and a frame above it.
    const ValueInLeaveRow inWords[] = { { 2, FormInLeaveRow::Cell, PlaceInLeaveRow::Register, 3 }, { 3, FormInLeaveRow::Double, PlaceInLeaveRow::Stack, 8 } };
    const ValueInLeaveRow inRegisters[] = { { 1, FormInLeaveRow::JSValue, PlaceInLeaveRow::Constant, bitsOf(jsNumber(12)) }, { Reg::maxIndex() + 1, FormInLeaveRow::Cell, PlaceInLeaveRow::HeapWords, 0 }, { firstSlotOfStackWordInLeaveRow + 1, FormInLeaveRow::Cell, PlaceInLeaveRow::HeapWords, 0 } };
    const ValueInLeaveRow onStack[] = { { 2, FormInLeaveRow::Int32, PlaceInLeaveRow::Constant, 9 } };
    LeaveRowWriter third(words, 3);
    third.beginHeapWords(3, std::size(inWords));
    for (const ValueInLeaveRow& value : inWords)
        third.add(value);
    third.beginCall(std::size(inRegisters));
    for (const ValueInLeaveRow& value : inRegisters)
        third.add(value);
    third.beginFrame(2, std::size(onStack));
    for (const ValueInLeaveRow& value : onStack)
        third.add(value);
    uint32_t thirdRow = third.finish();
    LeaveRowReader parts(words.span().subspan(thirdRow));
    AOT_TEST_CHECK(parts.partsLeft() == 3 && isPart(parts.beginPart(), PartOfLeaveRow::HeapWords, 3));
    for (const ValueInLeaveRow& value : inWords)
        AOT_TEST_CHECK(isSameValue(parts.next(), value));
    AOT_TEST_CHECK(isPart(parts.beginPart(), PartOfLeaveRow::Call, 0));
    for (const ValueInLeaveRow& value : inRegisters)
        AOT_TEST_CHECK(isSameValue(parts.next(), value));
    AOT_TEST_CHECK(isPart(parts.beginPart(), PartOfLeaveRow::Frame, 2));
    for (const ValueInLeaveRow& value : onStack)
        AOT_TEST_CHECK(isSameValue(parts.next(), value));
    AOT_TEST_CHECK(!parts.partsLeft() && !parts.valuesLeft());

    // A chain that comes back: each frame and call has its own heap words in front, and a frame holds an address.
    const ValueInLeaveRow inFrame[] = { { 1, FormInLeaveRow::JSValue, PlaceInLeaveRow::Address, 0 }, { 2, FormInLeaveRow::Cell, PlaceInLeaveRow::HeapWords, 0 } };
    LeaveRowWriter fourth(words, 4, EndOfLeave::ComesBack);
    fourth.beginHeapWords(1, 0);
    fourth.beginCall(0);
    fourth.beginHeapWords(2, 0);
    fourth.beginFrame(2, std::size(inFrame));
    for (const ValueInLeaveRow& value : inFrame)
        fourth.add(value);
    uint32_t fourthRow = fourth.finish();
    LeaveRowReader chain(words.span().subspan(fourthRow));
    AOT_TEST_CHECK(chain.end() == EndOfLeave::ComesBack && chain.partsLeft() == 4 && parts.end() == EndOfLeave::ForGood);
    AOT_TEST_CHECK(isPart(chain.beginPart(), PartOfLeaveRow::HeapWords, 1) && isPart(chain.beginPart(), PartOfLeaveRow::Call, 0) && isPart(chain.beginPart(), PartOfLeaveRow::HeapWords, 2) && isPart(chain.beginPart(), PartOfLeaveRow::Frame, 2));
    for (const ValueInLeaveRow& value : inFrame)
        AOT_TEST_CHECK(isSameValue(chain.next(), value));
    AOT_TEST_CHECK(!chain.partsLeft() && !chain.valuesLeft());

    // The usual value: it follows the last slot and lies in a register.
    Vector<uint32_t> usual;
    LeaveRowWriter row(usual, 1);
    row.beginFrame(8, 8);
    for (uint32_t slot = 1; slot <= 8; ++slot)
        row.add({ slot, FormInLeaveRow::JSValue, PlaceInLeaveRow::Register, slot });
    row.finish();
    AOT_TEST_CHECK(usual.size() * sizeof(uint32_t) == WTF::roundUpToMultipleOf<sizeof(uint32_t)>(3 + 8 * 2));
}

// The registers that were laid down, with room for the addresses of three frames.
// The stack pointer of a fast body is a multiple of 16, and what it pushes hangs below: on x86-64 an odd number of addresses, as in every row of these tests that comes back, begins 8 bytes off.
struct alignas(16) LaidDownForTest {
    uint64_t padding[isX86_64() ? 1 : 2] { };
    RegistersLaidDownAtLeave registers { };
    const void* moreAddresses[2] { };
};
static_assert(OBJECT_OFFSETOF(LaidDownForTest, moreAddresses) == OBJECT_OFFSETOF(LaidDownForTest, registers) + OBJECT_OFFSETOF(RegistersLaidDownAtLeave, addresses) + sizeof(void*));

static uint64_t wordStoredAt(PlanOfArrival& plan, const uint64_t* address)
{
    std::optional<uint64_t> found;
    for (StoreOfArrival& store : unsafeMakeSpan(plan.stores(), plan.numberOfStores)) {
        if (store.address != address)
            continue;
        AOT_TEST_CHECK(!found);
        found = store.word;
    }
    AOT_TEST_CHECK(found);
    return *found;
}

static void testFramesAreLaidOut()
{
    alignas(16) static uint64_t stack[256];
    uint64_t* fastFrame = &stack[200];
    const void* noLimit = &stack[0];

    Reg firstSaved = GPRInfo::regCS0;
    Reg secondSaved = GPRInfo::regCS1;
    AOT_TEST_CHECK(firstSaved.index() < secondSaved.index());
    Reg plainGPR = GPRInfo::regT1;
    Reg plainFPR = FPRInfo::fpRegT1;
    // Two callee saves, which begin three words below the record: [-3] the first, [-2] the second.
    ImageFrame imageFrame { ImageFrame::packRegisters(1ull << firstSaved.index() | 1ull << secondSaved.index()), 3, 4 };
    AOT_TEST_CHECK(imageFrame.hasFrameRecord() && imageFrame.frameSizeInBytes() == 64 && imageFrame.startOfCalleeSaves() == 3);
    fastFrame[-3] = 0xca11e501;
    fastFrame[-2] = 0xca11e502;
    fastFrame[-5] = static_cast<uint64_t>(1) << 40;
    reinterpret_cast<uint32_t*>(fastFrame)[-13] = 1;
    reinterpret_cast<uint32_t*>(fastFrame)[-14] = 0x55555555;

    LaidDownForTest laidDown;
    for (unsigned index = 0; index < RegistersLaidDownAtLeave::numberOfWords; ++index)
        laidDown.registers.byIndex[index] = 0x1000 + index;
    laidDown.registers.framePointerOfFastBody = fastFrame;
    laidDown.registers.byIndex[firstSaved.index()] = bitsOf(jsNull());
    laidDown.registers.byIndex[plainGPR.index()] = 0xdeadbeef00000007ULL;
    laidDown.registers.byIndex[plainFPR.index()] = std::bit_cast<uint64_t>(1.5);
    laidDown.registers.addresses[0] = std::bit_cast<const void*>(static_cast<uintptr_t>(0xe0));
    laidDown.moreAddresses[0] = std::bit_cast<const void*>(static_cast<uintptr_t>(0xe1));
    laidDown.moreAddresses[1] = std::bit_cast<const void*>(static_cast<uintptr_t>(0xe2));

    Vector<uint64_t> memory;
    auto makesNoHeapWords = [](uint32_t) -> HeapWordsOfArrival {
        AOT_TEST_CHECK(false);
        return { };
    };
    {
        Vector<uint32_t> words;
        LeaveRowWriter row(words, 1);
        row.beginFrame(10, 7);
        row.add({ 1, FormInLeaveRow::JSValue, PlaceInLeaveRow::Register, firstSaved.index() });
        row.add({ 2, FormInLeaveRow::Int32, PlaceInLeaveRow::Register, plainGPR.index() });
        row.add({ 3, FormInLeaveRow::Double, PlaceInLeaveRow::Register, plainFPR.index() });
        row.add({ 5, FormInLeaveRow::Int64, PlaceInLeaveRow::Stack, 10 });
        row.add({ 6, FormInLeaveRow::Boolean, PlaceInLeaveRow::Stack, 13 });
        row.add({ 7, FormInLeaveRow::Double, PlaceInLeaveRow::Constant, std::bit_cast<uint64_t>(3.0) });
        row.add({ 10, FormInLeaveRow::Int64, PlaceInLeaveRow::Constant, static_cast<uint64_t>(-9) });
        uint32_t first = row.finish();
        PlanOfArrival* plan = layOutSlowFrames(memory, laidDown.registers, imageFrame, words.span().subspan(first), noLimit, makesNoHeapWords);
        AOT_TEST_CHECK(plan);
        AOT_TEST_CHECK(plan->target == laidDown.registers.addresses[0] && plan->framePointer == fastFrame && plan->stackPointer == fastFrame - 10 && plan->numberOfStores == 7);
        AOT_TEST_CHECK(wordStoredAt(*plan, fastFrame - 1) == bitsOf(jsNull()));
        AOT_TEST_CHECK(wordStoredAt(*plan, fastFrame - 2) == bitsOf(jsNumber(7)));
        AOT_TEST_CHECK(wordStoredAt(*plan, fastFrame - 3) == bitsOf(jsDoubleNumber(1.5)));
        AOT_TEST_CHECK(wordStoredAt(*plan, fastFrame - 5) == bitsOf(jsDoubleNumber(static_cast<double>(1ull << 40))));
        AOT_TEST_CHECK(wordStoredAt(*plan, fastFrame - 6) == bitsOf(jsBoolean(true)));
        AOT_TEST_CHECK(wordStoredAt(*plan, fastFrame - 7) == bitsOf(jsNumber(3)) && JSValue::decode(wordStoredAt(*plan, fastFrame - 7)).isInt32());
        AOT_TEST_CHECK(wordStoredAt(*plan, fastFrame - 10) == bitsOf(jsNumber(-9)));
        // What the fast body has saved comes out of its frame, whatever the register held at the point. Every other register is as it came.
        AOT_TEST_CHECK(plan->registers[firstSaved.index()] == 0xca11e501 && plan->registers[secondSaved.index()] == 0xca11e502);
        AOT_TEST_CHECK(plan->registers[plainGPR.index()] == 0xdeadbeef00000007ULL);
    }

    {
        Vector<uint32_t> words;
        LeaveRowWriter row(words, 3);
        row.beginFrame(4, 1);
        row.add({ 4, FormInLeaveRow::Int32, PlaceInLeaveRow::Constant, 40 });
        row.beginFrame(2, 1);
        row.add({ 1, FormInLeaveRow::Int32, PlaceInLeaveRow::Constant, 41 });
        row.beginFrame(6, 2);
        row.add({ 1, FormInLeaveRow::Int32, PlaceInLeaveRow::Constant, 42 });
        row.add({ 6, FormInLeaveRow::Int32, PlaceInLeaveRow::Constant, 43 });
        uint32_t first = row.finish();
        PlanOfArrival* plan = layOutSlowFrames(memory, laidDown.registers, imageFrame, words.span().subspan(first), noLimit, makesNoHeapWords);
        AOT_TEST_CHECK(plan);
        uint64_t* outermost = fastFrame;
        uint64_t* middle = outermost - 6 - 2;
        uint64_t* innermost = middle - 2 - 2;
        AOT_TEST_CHECK(plan->numberOfStores == 4 + 2 * 2 && plan->target == laidDown.registers.addresses[0]);
        AOT_TEST_CHECK(plan->framePointer == innermost && plan->stackPointer == innermost - 4);
        AOT_TEST_CHECK(wordStoredAt(*plan, outermost - 1) == bitsOf(jsNumber(42)) && wordStoredAt(*plan, outermost - 6) == bitsOf(jsNumber(43)));
        AOT_TEST_CHECK(wordStoredAt(*plan, middle - 1) == bitsOf(jsNumber(41)) && wordStoredAt(*plan, innermost - 4) == bitsOf(jsNumber(40)));
        // Each frame below the outermost is as if the one above had called it.
        AOT_TEST_CHECK(wordStoredAt(*plan, middle) == std::bit_cast<uint64_t>(outermost) && wordStoredAt(*plan, middle + 1) == 0xe2);
        AOT_TEST_CHECK(wordStoredAt(*plan, innermost) == std::bit_cast<uint64_t>(middle) && wordStoredAt(*plan, innermost + 1) == 0xe1);

        // There is room for exactly these frames, and for not one word less.
        AOT_TEST_CHECK(layOutSlowFrames(memory, laidDown.registers, imageFrame, words.span().subspan(first), innermost - 4, makesNoHeapWords));
        AOT_TEST_CHECK(!layOutSlowFrames(memory, laidDown.registers, imageFrame, words.span().subspan(first), innermost - 3, makesNoHeapWords));
        AOT_TEST_CHECK(!layOutSlowFrames(memory, laidDown.registers, imageFrame, words.span().subspan(first), fastFrame + 1, makesNoHeapWords));
    }

    // A unit is entered at its head, by a call, under a frame, whose lowest words are what it takes on the stack. The live list goes into heap words.
    static uint64_t heapWords[4];
    constexpr uint64_t cell = 0xce11;
    unsigned numberOfCellsMade = 0;
    auto makeHeapWords = [&](uint32_t numberOfWords) {
        AOT_TEST_CHECK(numberOfWords == 3);
        ++numberOfCellsMade;
        return HeapWordsOfArrival { cell, heapWords };
    };
    Reg firstParameter = GPRInfo::argumentGPR1;
    Reg secondParameter = GPRInfo::argumentGPR0;
    if (firstParameter.index() > secondParameter.index())
        std::swap(firstParameter, secondParameter);
    auto addHeapWordsAndCall = [&](LeaveRowWriter& row) {
        row.beginHeapWords(3, 2);
        row.add({ 1, FormInLeaveRow::Int32, PlaceInLeaveRow::Register, plainGPR.index() });
        row.add({ 3, FormInLeaveRow::JSValue, PlaceInLeaveRow::Register, firstSaved.index() });
        row.beginCall(4);
        row.add({ firstParameter.index() + 1, FormInLeaveRow::JSValue, PlaceInLeaveRow::Constant, bitsOf(jsNumber(24)) });
        row.add({ secondParameter.index() + 1, FormInLeaveRow::Cell, PlaceInLeaveRow::HeapWords, 0 });
        row.add({ firstSlotOfStackWordInLeaveRow, FormInLeaveRow::Cell, PlaceInLeaveRow::HeapWords, 0 });
        row.add({ firstSlotOfStackWordInLeaveRow + 1, FormInLeaveRow::Double, PlaceInLeaveRow::Register, plainFPR.index() });
    };
    auto checkHeapWordsAndRegisters = [&](PlanOfArrival& plan) {
        AOT_TEST_CHECK(heapWords[0] == bitsOf(jsNumber(7)) && !heapWords[1] && heapWords[2] == bitsOf(jsNull()));
        AOT_TEST_CHECK(plan.target == laidDown.registers.addresses[0]);
        AOT_TEST_CHECK(plan.registers[firstParameter.index()] == bitsOf(jsNumber(24)) && plan.registers[secondParameter.index()] == cell);
        AOT_TEST_CHECK(plan.registers[firstSaved.index()] == 0xca11e501);
    };
    {
        Vector<uint32_t> words;
        LeaveRowWriter row(words, 3);
        addHeapWordsAndCall(row);
        row.beginFrame(4, 1);
        row.add({ 1, FormInLeaveRow::Int32, PlaceInLeaveRow::Constant, 44 });
        uint32_t first = row.finish();
        uint64_t* bottom = fastFrame - 4 - isX86_64();
        AOT_TEST_CHECK(!layOutSlowFrames(memory, laidDown.registers, imageFrame, words.span().subspan(first), bottom + 1, makeHeapWords) && !numberOfCellsMade);
        PlanOfArrival* plan = layOutSlowFrames(memory, laidDown.registers, imageFrame, words.span().subspan(first), bottom, makeHeapWords);
        AOT_TEST_CHECK(plan && numberOfCellsMade == 1);
        checkHeapWordsAndRegisters(*plan);
        AOT_TEST_CHECK(plan->framePointer == fastFrame && plan->stackPointer == bottom);
        AOT_TEST_CHECK(wordStoredAt(*plan, fastFrame - 1) == bitsOf(jsNumber(44)) && wordStoredAt(*plan, fastFrame - 4) == cell && wordStoredAt(*plan, fastFrame - 3) == bitsOf(jsDoubleNumber(1.5)));
#if CPU(X86_64)
        AOT_TEST_CHECK(plan->numberOfStores == 4 && wordStoredAt(*plan, bottom) == 0xe1);
#else
        AOT_TEST_CHECK(plan->numberOfStores == 3 && plan->registers[Reg(CCallHelpers::linkRegister).index()] == 0xe1);
#endif
    }

    // Under no frame the call takes the place of the one that entered the fast body: the record and the return address of the fast frame are taken down, and the stack words are those that its caller left room for.
    {
        fastFrame[0] = 0xf0;
        fastFrame[1] = 0xf1;
        Vector<uint32_t> words;
        LeaveRowWriter row(words, 2);
        addHeapWordsAndCall(row);
        uint32_t first = row.finish();
        PlanOfArrival* plan = layOutSlowFrames(memory, laidDown.registers, imageFrame, words.span().subspan(first), fastFrame, makeHeapWords);
        AOT_TEST_CHECK(plan && numberOfCellsMade == 2 && plan->numberOfStores == 2);
        checkHeapWordsAndRegisters(*plan);
        AOT_TEST_CHECK(wordStoredAt(*plan, fastFrame + 2) == cell && wordStoredAt(*plan, fastFrame + 3) == bitsOf(jsDoubleNumber(1.5)));
        AOT_TEST_CHECK(plan->framePointer == std::bit_cast<void*>(static_cast<uintptr_t>(0xf0)) && plan->stackPointer == fastFrame + (isX86_64() ? 1 : 2));
#if !CPU(X86_64)
        AOT_TEST_CHECK(plan->registers[Reg(CCallHelpers::linkRegister).index()] == 0xf1);
#endif
    }

    // It comes back: the fast frame stays, with what it has saved, and the call stands below it as one of its own: it returns to where the stub was called from.
    uint64_t* stackPointerOfFastBody = std::bit_cast<uint64_t*>(&laidDown.registers.addresses[0]);
    laidDown.registers.returnAddress = std::bit_cast<const void*>(static_cast<uintptr_t>(0xbac));
    {
        Vector<uint32_t> words;
        LeaveRowWriter row(words, 2, EndOfLeave::ComesBack);
        addHeapWordsAndCall(row);
        uint32_t first = row.finish();
        uint64_t* top = stackPointerOfFastBody + (isX86_64() ? 1 : 2);
        PlanOfArrival* plan = layOutSlowFrames(memory, laidDown.registers, imageFrame, words.span().subspan(first), top - isX86_64(), makeHeapWords);
        AOT_TEST_CHECK(plan && plan->framePointer == fastFrame && plan->stackPointer == top - isX86_64());
        AOT_TEST_CHECK(!layOutSlowFrames(memory, laidDown.registers, imageFrame, words.span().subspan(first), top + 1, makeHeapWords));
        AOT_TEST_CHECK(plan->registers[firstSaved.index()] == bitsOf(jsNull()) && plan->registers[secondParameter.index()] == cell);
        AOT_TEST_CHECK(wordStoredAt(*plan, top) == cell && wordStoredAt(*plan, top + 1) == bitsOf(jsDoubleNumber(1.5)));
#if CPU(X86_64)
        AOT_TEST_CHECK(plan->numberOfStores == 3 && wordStoredAt(*plan, top - 1) == 0xbac);
#else
        AOT_TEST_CHECK(plan->numberOfStores == 2 && plan->registers[Reg(CCallHelpers::linkRegister).index()] == 0xbac);
#endif
    }

    // A chain: the frame above the call has heap words of its own and holds an address. The addresses are taken in the order in which the row is read.
    for (EndOfLeave end : { EndOfLeave::ForGood, EndOfLeave::ComesBack }) {
        static uint64_t wordsOfCells[2][4];
        unsigned made = 0;
        auto makeCellsOfChain = [&](uint32_t numberOfWords) {
            AOT_TEST_CHECK(numberOfWords == 2 + made && made < 2);
            ++made;
            return HeapWordsOfArrival { 0xce10 + made, wordsOfCells[made - 1] };
        };
        Vector<uint32_t> words;
        LeaveRowWriter row(words, 4, end);
        row.beginHeapWords(2, 1);
        row.add({ 2, FormInLeaveRow::Int32, PlaceInLeaveRow::Constant, 61 });
        row.beginCall(1);
        row.add({ firstParameter.index() + 1, FormInLeaveRow::Cell, PlaceInLeaveRow::HeapWords, 0 });
        row.beginHeapWords(3, 1);
        row.add({ 1, FormInLeaveRow::Int32, PlaceInLeaveRow::Constant, 62 });
        row.beginFrame(4, 2);
        row.add({ 1, FormInLeaveRow::JSValue, PlaceInLeaveRow::Address, 0 });
        row.add({ 3, FormInLeaveRow::Cell, PlaceInLeaveRow::HeapWords, 0 });
        uint32_t first = row.finish();
        PlanOfArrival* plan = layOutSlowFrames(memory, laidDown.registers, imageFrame, words.span().subspan(first), noLimit, makeCellsOfChain);
        AOT_TEST_CHECK(plan && made == 2 && wordsOfCells[0][1] == bitsOf(jsNumber(61)) && wordsOfCells[1][0] == bitsOf(jsNumber(62)));
        // Three addresses: the entry of the call, the frame's own, the one that the frame holds.
        uint64_t* frame = end == EndOfLeave::ForGood ? fastFrame : stackPointerOfFastBody + (isX86_64() ? 3 : 4) - 2;
        AOT_TEST_CHECK(plan->target == laidDown.registers.addresses[0] && plan->framePointer == frame && plan->stackPointer == frame - 4 - isX86_64());
        AOT_TEST_CHECK(plan->registers[firstParameter.index()] == 0xce11 && wordStoredAt(*plan, frame - 1) == 0xe2 && wordStoredAt(*plan, frame - 3) == 0xce12);
#if CPU(X86_64)
        AOT_TEST_CHECK(wordStoredAt(*plan, frame - 5) == 0xe1);
#else
        AOT_TEST_CHECK(plan->registers[Reg(CCallHelpers::linkRegister).index()] == 0xe1);
#endif
        if (end == EndOfLeave::ComesBack)
            AOT_TEST_CHECK(wordStoredAt(*plan, frame + 1) == 0xbac && wordStoredAt(*plan, frame) == std::bit_cast<uint64_t>(fastFrame));
    }
}

// Its header is that of a cell that a collection has marked: whoever stores into it owes a barrier.
struct alignas(16) CellOfWordsInTest {
    std::array<uint8_t, 64> header { };
    std::array<uint64_t, 4> words { };
};

// What the operation that stands in for operationAOTLayOutSlowFrames is told, and what the code of the test leaves behind.
struct LeavingInTest {
    void* stub { nullptr };
    Vector<uint64_t> memoryOfPlan;
    Vector<uint32_t> words;
    ImageFrame fastFrame { };
    const void* stackLimit { nullptr };
    unsigned numberOfLeaves { 0 };
    std::array<uint64_t, 16> seenInInnerFrame { };
    std::array<uint64_t, 16> seenInOuterFrame { };
    std::array<uint64_t, 16> savedAtEntry { };
    std::array<uint64_t, 16> savedInInnerFrame { };
    uint64_t framePointerOfFastBody { 0 };
    uint64_t framePointerOfCaller { 0 };
    uint64_t framePointerInInnerFrame { 0 };
    uint64_t stackPointerInInnerFrame { 0 };
    uint64_t framePointerInOuterFrame { 0 };
    uint64_t stackPointerInOuterFrame { 0 };
    uint64_t returnedIntoOuterFrame { 0 };
    std::array<CellOfWordsInTest, 2> cells { };
    unsigned numberOfCellsMade { 0 };
    uint64_t returnedIntoFastBody { 0 };
    uint64_t stackPointerBehindCall { 0 };
    uint64_t savedRegisterBehindCall { 0 };
};
static LeavingInTest* s_leaving;

// The stub goes back to where it was called from, as if it had returned.
static void* JIT_OPERATION_ATTRIBUTES planReturnToCaller(Instance* instance, uint64_t* registersLaidDown)
{
    auto& laidDown = *reinterpret_cast<RegistersLaidDownAtLeave*>(registersLaidDown);
    LeavingInTest& leaving = *s_leaving;
    ++leaving.numberOfLeaves;
    leaving.stub = StubEnvironment::of(instance).stub;
    AOT_TEST_CHECK(laidDown.byIndex[Reg(instanceGPR).index()] == std::bit_cast<uint64_t>(instance));
    // On ARM64 the caller that the runner makes reaches its own words through this register once it has loaded it.
    AOT_TEST_CHECK(!isX86_64() || static_cast<uint32_t>(laidDown.byIndex[Reg(rowOfLeaveGPR).index()]) == 0x1234);
    for (size_t i = 0; i < markedFPRs.size(); ++i)
        AOT_TEST_CHECK(laidDown.byIndex[Reg(markedFPRs[i]).index()] == StubEnvironment::of(instance).numbers[i]);
    leaving.memoryOfPlan.grow(PlanOfArrival::wordsFor(0));
    auto* plan = reinterpret_cast<PlanOfArrival*>(leaving.memoryOfPlan.mutableSpan().data());
    plan->target = laidDown.returnAddress;
    plan->stackPointer = &laidDown.addresses[0];
    plan->framePointer = laidDown.framePointerOfFastBody;
    plan->numberOfStores = 0;
    memcpy(plan->registers, laidDown.byIndex, sizeof(plan->registers));
    return plan;
}

static void* JIT_OPERATION_ATTRIBUTES layOutFramesOfTest(Instance*, uint64_t* registersLaidDown)
{
    auto& laidDown = *reinterpret_cast<RegistersLaidDownAtLeave*>(registersLaidDown);
    LeavingInTest& leaving = *s_leaving;
    ++leaving.numberOfLeaves;
    uint32_t row = static_cast<uint32_t>(laidDown.byIndex[Reg(rowOfLeaveGPR).index()]);
    auto makeHeapWords = [&](uint32_t numberOfWords) {
        AOT_TEST_CHECK(leaving.numberOfCellsMade < leaving.cells.size() && numberOfWords <= leaving.cells[0].words.size());
        CellOfWordsInTest& cell = leaving.cells[leaving.numberOfCellsMade++];
        return HeapWordsOfArrival { std::bit_cast<uint64_t>(&cell), cell.words.data() };
    };
    return layOutSlowFrames(leaving.memoryOfPlan, laidDown, leaving.fastFrame, leaving.words.span().subspan(row), leaving.stackLimit, makeHeapWords);
}

// No unit saves a register that is pinned: it holds the same in all of them.
static Vector<GPRReg> registersSavedByFastBodyOfTest(bool withPinnedRegisters = false)
{
    Vector<GPRReg> saved;
    RegisterSet::vmCalleeSaveRegisters().forEach([&](Reg reg) {
        if (reg.isGPR() && (withPinnedRegisters || (reg.gpr() != instanceGPR && reg.gpr() != cellCageBaseGPR)))
            saved.append(reg.gpr());
    });
    AOT_TEST_CHECK(saved.size() <= std::tuple_size_v<decltype(LeavingInTest::savedAtEntry)> && saved.contains(instanceGPR) == withPinnedRegisters);
    return saved;
}

template<typename Functor>
static MacroAssemblerCodeRef<JSEntryPtrTag> assembleForLeavingTest(const Functor& emit)
{
    CCallHelpers jit;
    emit(jit);
    LinkBuffer linkBuffer(jit, nullptr);
    return FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "AOT code of a test of leaving");
}

static void* addressOfCodeForLeavingTest(const MacroAssemblerCodeRef<JSEntryPtrTag>& code)
{
    return untagCFunctionPtr<JSEntryPtrTag>(code.code().taggedPtr());
}

static void storeSlots(CCallHelpers& jit, unsigned numberOfSlots, std::array<uint64_t, 16>& seen)
{
    for (unsigned slot = 1; slot <= numberOfSlots; ++slot) {
        jit.load64(CCallHelpers::Address(GPRInfo::callFrameRegister, -static_cast<int32_t>(slot * sizeof(uint64_t))), GPRInfo::regT0);
        jit.store64(GPRInfo::regT0, &seen[slot]);
    }
}

static void storeFrameRegisters(CCallHelpers& jit, uint64_t& framePointer, uint64_t& stackPointer)
{
    jit.store64(GPRInfo::callFrameRegister, &framePointer);
    jit.move(CCallHelpers::stackPointerRegister, GPRInfo::regT0);
    jit.store64(GPRInfo::regT0, &stackPointer);
}

static void testStubLeavesAndArrives()
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
    constexpr GPRReg addressOfStub = gprOf(StubRegister::T14);
    constexpr GPRReg valueInSavedRegister = GPRInfo::regCS0;
    constexpr GPRReg valueInPlainRegister = gprOf(StubRegister::T10);
    constexpr FPRReg numberInRegister = FPRInfo::fpRegT1;
    static_assert(noOverlap(addressOfStub, valueInSavedRegister, valueInPlainRegister, rowOfLeaveGPR, instanceGPR, GPRInfo::regT0));

    LeavingInTest leaving;
    s_leaving = &leaving;
    StubRunner runner;

    runner.install(Entry::operationAOTLayOutSlowFrames, planReturnToCaller);
    runner.run(leaveForSlowBody(), { 0x1234 });
    AOT_TEST_CHECK(leaving.numberOfLeaves == 1 && leaving.stub);

    runner.install(Entry::operationAOTLayOutSlowFrames, layOutFramesOfTest);
    Vector<GPRReg> saved = registersSavedByFastBodyOfTest();
    // The frame of the fast body: the callee saves in one run right below the record, ascending, then six words.
    unsigned wordsOfSaves = saved.size();
    unsigned wordsOfFastFrame = WTF::roundUpToMultipleOf<2>(wordsOfSaves + 6);
    uint64_t maskOfSaves = 0;
    for (GPRReg reg : saved)
        maskOfSaves |= 1ull << Reg(reg).index();
    leaving.fastFrame = { ImageFrame::packRegisters(maskOfSaves), static_cast<uint16_t>(wordsOfSaves), static_cast<uint16_t>(wordsOfFastFrame / 2) };
    unsigned wordOfInteger = wordsOfSaves + 1;
    unsigned halfWordOfBoolean = 2 * (wordsOfSaves + 2) + 1;
    uint64_t stackOfThisFunction;
    leaving.stackLimit = reinterpret_cast<const uint8_t*>(&stackOfThisFunction) - 64 * KB;

    constexpr uint64_t resultOfInnerFrame = 0x7e57;
    constexpr uint64_t resultOfOuterFrame = 0x7e58;
    constexpr uint64_t resultOfFastBody = 0x7e5a;
    constexpr unsigned wordsOfInnerFrame = 8;
    // Larger than the frame of the fast body and what the stub lays down together, so that the new frames reach over both.
    constexpr unsigned wordsOfOuterFrame = 2 * RegistersLaidDownAtLeave::numberOfWords;

    auto entryOfInnerFrame = assembleForLeavingTest([&](CCallHelpers& jit) {
        storeSlots(jit, 7, leaving.seenInInnerFrame);
        for (size_t i = 0; i < saved.size(); ++i)
            jit.store64(saved[i], &leaving.savedInInnerFrame[i]);
        storeFrameRegisters(jit, leaving.framePointerInInnerFrame, leaving.stackPointerInInnerFrame);
        jit.move(CCallHelpers::TrustedImm64(resultOfInnerFrame), GPRInfo::returnValueGPR);
        jit.emitFunctionEpilogue();
        jit.ret();
    });
    auto returnIntoOuterFrame = assembleForLeavingTest([&](CCallHelpers& jit) {
        jit.store64(GPRInfo::returnValueGPR, &leaving.returnedIntoOuterFrame);
        storeSlots(jit, 2, leaving.seenInOuterFrame);
        storeFrameRegisters(jit, leaving.framePointerInOuterFrame, leaving.stackPointerInOuterFrame);
        jit.move(CCallHelpers::TrustedImm64(resultOfOuterFrame), GPRInfo::returnValueGPR);
        jit.emitFunctionEpilogue();
        jit.ret();
    });

    auto addValuesOfInnerFrame = [&](LeaveRowWriter& row) {
        row.beginFrame(wordsOfInnerFrame, 7);
        row.add({ 1, FormInLeaveRow::JSValue, PlaceInLeaveRow::Register, Reg(valueInSavedRegister).index() });
        row.add({ 2, FormInLeaveRow::Int32, PlaceInLeaveRow::Register, Reg(valueInPlainRegister).index() });
        row.add({ 3, FormInLeaveRow::Double, PlaceInLeaveRow::Register, Reg(numberInRegister).index() });
        row.add({ 4, FormInLeaveRow::Int64, PlaceInLeaveRow::Stack, 2 * wordOfInteger });
        row.add({ 5, FormInLeaveRow::Boolean, PlaceInLeaveRow::Stack, halfWordOfBoolean });
        row.add({ 6, FormInLeaveRow::JSValue, PlaceInLeaveRow::Constant, bitsOf(jsUndefined()) });
        row.add({ 7, FormInLeaveRow::Int32, PlaceInLeaveRow::Constant, static_cast<uint64_t>(-5) });
    };
    LeaveRowWriter rowOfOneFrame(leaving.words, 1);
    addValuesOfInnerFrame(rowOfOneFrame);
    uint32_t oneFrame = rowOfOneFrame.finish();
    LeaveRowWriter rowOfTwoFrames(leaving.words, 2);
    addValuesOfInnerFrame(rowOfTwoFrames);
    rowOfTwoFrames.beginFrame(wordsOfOuterFrame, 2);
    rowOfTwoFrames.add({ 1, FormInLeaveRow::Int32, PlaceInLeaveRow::Register, Reg(valueInPlainRegister).index() });
    rowOfTwoFrames.add({ 2, FormInLeaveRow::Cell, PlaceInLeaveRow::Constant, 0x123456789000ULL });
    uint32_t twoFrames = rowOfTwoFrames.finish();

    // The border to the code of the test: it keeps what that code counts on, and pins the instance.
    // As every caller of a unit does, it leaves room for what any unit of the function takes on the stack.
    auto callerOfFastBody = assembleForLeavingTest([&](CCallHelpers& jit) {
        Vector<GPRReg> kept = registersSavedByFastBodyOfTest(true);
        jit.emitFunctionPrologue();
        jit.subPtr(CCallHelpers::TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>((kept.size() + 4) * sizeof(uint64_t))), sp);
        for (size_t i = 0; i < kept.size(); ++i)
            jit.storePtr(kept[i], CCallHelpers::Address(fp, -static_cast<int32_t>((i + 1) * sizeof(uint64_t))));
        jit.move(GPRInfo::argumentGPR0, instanceGPR);
        jit.call(GPRInfo::argumentGPR1, JSEntryPtrTag);
        for (size_t i = 0; i < kept.size(); ++i)
            jit.loadPtr(CCallHelpers::Address(fp, -static_cast<int32_t>((i + 1) * sizeof(uint64_t))), kept[i]);
        jit.emitFunctionEpilogue();
        jit.ret();
    });
    auto runFastBody = [&](uint32_t row, std::initializer_list<void*> addressesOutermostFirst, EndOfLeave end = EndOfLeave::ForGood) {
        auto fastBody = assembleForLeavingTest([&](CCallHelpers& jit) {
            jit.emitFunctionPrologue();
            jit.subPtr(CCallHelpers::TrustedImm32(wordsOfFastFrame * sizeof(uint64_t)), sp);
            for (size_t i = 0; i < saved.size(); ++i) {
                jit.storePtr(saved[i], CCallHelpers::Address(fp, -static_cast<int32_t>((wordsOfSaves - i) * sizeof(uint64_t))));
                jit.store64(saved[i], &leaving.savedAtEntry[i]);
            }
            jit.store64(fp, &leaving.framePointerOfFastBody);
            jit.load64(CCallHelpers::Address(fp), GPRInfo::regT0);
            jit.store64(GPRInfo::regT0, &leaving.framePointerOfCaller);
            // Whatever the fast body has saved it may use.
            for (GPRReg reg : saved)
                jit.move(CCallHelpers::TrustedImm64(0x0badbadbadbadbadLL), reg);
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(1) << 40), GPRInfo::regT0);
            jit.store64(GPRInfo::regT0, CCallHelpers::Address(fp, -static_cast<int32_t>(wordOfInteger * sizeof(uint64_t))));
            jit.move(CCallHelpers::TrustedImm64(0x5555555555555555LL), GPRInfo::regT0);
            jit.store64(GPRInfo::regT0, CCallHelpers::Address(fp, -static_cast<int32_t>((halfWordOfBoolean + 1) / 2 * sizeof(uint64_t))));
            jit.store32(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(fp, -static_cast<int32_t>(halfWordOfBoolean * sizeof(uint32_t))));
            jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(2.5)), GPRInfo::regT0);
            jit.move64ToDouble(GPRInfo::regT0, numberInRegister);
            jit.move(CCallHelpers::TrustedImm64(bitsOf(jsNull())), valueInSavedRegister);
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(0xdeadbeef00000007ULL)), valueInPlainRegister);
            jit.move(CCallHelpers::TrustedImmPtr(end == EndOfLeave::ForGood ? leaving.stub : runner.entryOf(enterSlowBodiesAndComeBack())), addressOfStub);

            // As pushes leave them. On x86-64 an odd number of them leaves the stack pointer off a multiple of 16.
            unsigned bytesOfAddresses = addressesOutermostFirst.size() * sizeof(void*);
            jit.subPtr(CCallHelpers::TrustedImm32(isX86() ? bytesOfAddresses : WTF::roundUpToMultipleOf<stackAlignmentBytes()>(bytesOfAddresses)), sp);
            unsigned place = addressesOutermostFirst.size();
            for (void* address : addressesOutermostFirst) {
                jit.move(CCallHelpers::TrustedImmPtr(address), rowOfLeaveGPR);
                jit.storePtr(rowOfLeaveGPR, CCallHelpers::Address(sp, --place * sizeof(void*)));
            }
            jit.move(CCallHelpers::TrustedImm32(row), rowOfLeaveGPR);
            jit.call(addressOfStub, JITThunkPtrTag);
            if (end == EndOfLeave::ForGood) {
                jit.breakpoint();
                return;
            }
            // It goes on as behind any call, and returns as it would have.
            jit.store64(GPRInfo::returnValueGPR, &leaving.returnedIntoFastBody);
            jit.store64(valueInSavedRegister, &leaving.savedRegisterBehindCall);
            jit.move(sp, GPRInfo::regT0);
            jit.store64(GPRInfo::regT0, &leaving.stackPointerBehindCall);
            jit.store64(fp, &leaving.framePointerInOuterFrame);
            for (size_t i = 0; i < saved.size(); ++i)
                jit.loadPtr(CCallHelpers::Address(fp, -static_cast<int32_t>((wordsOfSaves - i) * sizeof(uint64_t))), saved[i]);
            jit.move(CCallHelpers::TrustedImm64(resultOfFastBody), GPRInfo::returnValueGPR);
            jit.emitFunctionEpilogue();
            jit.ret();
        });
        leaving.numberOfCellsMade = 0;
        leaving.cells = { };
        leaving.returnedIntoFastBody = 0;
        leaving.seenInInnerFrame = { };
        leaving.seenInOuterFrame = { };
        leaving.savedInInnerFrame = { };
        leaving.returnedIntoOuterFrame = 0;
        return std::bit_cast<uint64_t (SYSV_ABI *)(Instance*, void*)>(addressOfCodeForLeavingTest(callerOfFastBody))(&runner.instance(), addressOfCodeForLeavingTest(fastBody));
    };
    auto checkInnerFrame = [&] {
        AOT_TEST_CHECK(leaving.seenInInnerFrame[1] == bitsOf(jsNull()));
        AOT_TEST_CHECK(leaving.seenInInnerFrame[2] == bitsOf(jsNumber(7)));
        AOT_TEST_CHECK(leaving.seenInInnerFrame[3] == bitsOf(jsDoubleNumber(2.5)));
        AOT_TEST_CHECK(leaving.seenInInnerFrame[4] == bitsOf(jsDoubleNumber(static_cast<double>(1ull << 40))));
        AOT_TEST_CHECK(leaving.seenInInnerFrame[5] == bitsOf(jsBoolean(true)));
        AOT_TEST_CHECK(leaving.seenInInnerFrame[6] == bitsOf(jsUndefined()));
        AOT_TEST_CHECK(leaving.seenInInnerFrame[7] == bitsOf(jsNumber(-5)));
        // The registers that the fast body had saved are those of its caller again.
        for (size_t i = 0; i < saved.size(); ++i)
            AOT_TEST_CHECK(leaving.savedInInnerFrame[i] == leaving.savedAtEntry[i]);
        AOT_TEST_CHECK(leaving.stackPointerInInnerFrame == leaving.framePointerInInnerFrame - wordsOfInnerFrame * sizeof(uint64_t));
    };

    AOT_TEST_CHECK(runFastBody(oneFrame, { addressOfCodeForLeavingTest(entryOfInnerFrame) }) == resultOfInnerFrame);
    AOT_TEST_CHECK(leaving.numberOfLeaves == 2);
    checkInnerFrame();
    AOT_TEST_CHECK(leaving.framePointerInInnerFrame == leaving.framePointerOfFastBody);

    AOT_TEST_CHECK(runFastBody(twoFrames, { addressOfCodeForLeavingTest(returnIntoOuterFrame), addressOfCodeForLeavingTest(entryOfInnerFrame) }) == resultOfOuterFrame);
    AOT_TEST_CHECK(leaving.numberOfLeaves == 3);
    checkInnerFrame();
    AOT_TEST_CHECK(leaving.returnedIntoOuterFrame == resultOfInnerFrame);
    AOT_TEST_CHECK(leaving.seenInOuterFrame[1] == bitsOf(jsNumber(7)) && leaving.seenInOuterFrame[2] == 0x123456789000ULL);
    AOT_TEST_CHECK(leaving.framePointerInOuterFrame == leaving.framePointerOfFastBody);
    AOT_TEST_CHECK(leaving.stackPointerInOuterFrame == leaving.framePointerOfFastBody - wordsOfOuterFrame * sizeof(uint64_t));
    AOT_TEST_CHECK(leaving.framePointerInInnerFrame == leaving.stackPointerInOuterFrame - 2 * sizeof(uint64_t));

    // A construction: between the frame of the constructor and that of its caller stands a frame that holds `this`.
    FakeObject<16> made;
    made.setHeader(1, ObjectType);
    FakeObject<16> returned;
    returned.setHeader(1, ObjectType);
    FakeObject<16> text;
    text.setHeader(1, StringType);
    for (auto [result, selected] : std::initializer_list<std::pair<uint64_t, uint64_t>> { { bitsOf(jsUndefined()), made.address() }, { bitsOf(jsNumber(1)), made.address() }, { text.address(), made.address() }, { returned.address(), returned.address() } }) {
        auto entryOfConstructor = assembleForLeavingTest([&](CCallHelpers& jit) {
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(result)), GPRInfo::returnValueGPR);
            jit.emitFunctionEpilogue();
            jit.ret();
        });
        LeaveRowWriter row(leaving.words, 3);
        row.beginFrame(4, 0);
        row.beginFrame(wordsOfFrameThatSelectsResultOfConstruction, 1);
        row.add({ slotOfThisInFrameThatSelectsResultOfConstruction, FormInLeaveRow::Cell, PlaceInLeaveRow::Constant, made.address() });
        row.beginFrame(wordsOfOuterFrame, 0);
        AOT_TEST_CHECK(runFastBody(row.finish(), { addressOfCodeForLeavingTest(returnIntoOuterFrame), static_cast<uint8_t*>(runner.entryOf(selectResultOfConstruction())) + bytesInFrontOfReturnIntoSelectResultOfConstruction, addressOfCodeForLeavingTest(entryOfConstructor) }) == resultOfOuterFrame);
        AOT_TEST_CHECK(leaving.returnedIntoOuterFrame == selected);
        AOT_TEST_CHECK(leaving.framePointerInOuterFrame == leaving.framePointerOfFastBody && leaving.stackPointerInOuterFrame == leaving.framePointerOfFastBody - wordsOfOuterFrame * sizeof(uint64_t));
    }

    // BY WORDS. The head of a slow body sets up its own frame: two parameters in registers, two on the stack, the live list in heap words.
    const GPRReg numberParameter = directArgumentGPR(1);
    const GPRReg wordsParameter = directArgumentGPR(3);
    AOT_TEST_CHECK(noOverlap(numberParameter, wordsParameter, GPRInfo::regT0, instanceGPR));
    Typed::ArgumentLocations locations;
    locations.parameters.append({ Typed::ArgumentLocation::Kind::GPR, numberParameter, 0, Typed::Representation::JSValue });
    locations.parameters.append({ Typed::ArgumentLocation::Kind::GPR, wordsParameter, 0, Typed::Representation::JSValue });
    locations.parameters.append({ Typed::ArgumentLocation::Kind::Stack, { }, 0, Typed::Representation::JSValue });
    locations.parameters.append({ Typed::ArgumentLocation::Kind::Stack, { }, 1, Typed::Representation::JSValue });
    locations.parameters.append({ Typed::ArgumentLocation::Kind::GPR, directArgumentGPR(0), 0, Typed::Representation::JSValue });
    locations.numberOfStackArguments = 2;
    auto argumentsOfHead = [&](int32_t number) {
        return std::array<std::optional<ValueInLeaveRow>, 5> { {
            ValueInLeaveRow { 0, FormInLeaveRow::JSValue, PlaceInLeaveRow::Constant, bitsOf(jsNumber(number)) },
            ValueInLeaveRow { 0, FormInLeaveRow::Cell, PlaceInLeaveRow::HeapWords, 0 },
            ValueInLeaveRow { 0, FormInLeaveRow::Boolean, PlaceInLeaveRow::Stack, halfWordOfBoolean },
            ValueInLeaveRow { 0, FormInLeaveRow::Double, PlaceInLeaveRow::Register, Reg(numberInRegister).index() },
            std::nullopt,
        } };
    };
    auto assembleHead = [&](std::array<uint64_t, 16>& seen, uint64_t& framePointer, uint64_t& framePointerAbove, uint64_t result, bool returnsInFPR = false) {
        return assembleForLeavingTest([&](CCallHelpers& jit) {
            jit.emitFunctionPrologue();
            jit.subPtr(CCallHelpers::TrustedImm32(64), sp);
            jit.store64(numberParameter, &seen[1]);
            jit.store64(wordsParameter, &seen[2]);
            for (unsigned word = 0; word < 2; ++word) {
                jit.load64(CCallHelpers::Address(fp, static_cast<int32_t>(offsetOfStackWordFromFrame(word))), GPRInfo::regT0);
                jit.store64(GPRInfo::regT0, &seen[3 + word]);
            }
            for (unsigned word = 0; word < 3; ++word) {
                jit.load64(CCallHelpers::Address(wordsParameter, static_cast<int32_t>(OBJECT_OFFSETOF(CellOfWordsInTest, words) + word * sizeof(uint64_t))), GPRInfo::regT0);
                jit.store64(GPRInfo::regT0, &seen[5 + word]);
            }
            for (size_t i = 0; i < saved.size(); ++i)
                jit.store64(saved[i], &leaving.savedInInnerFrame[i]);
            jit.store64(fp, &framePointer);
            jit.load64(CCallHelpers::Address(fp), GPRInfo::regT0);
            jit.store64(GPRInfo::regT0, &framePointerAbove);
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(result)), GPRInfo::returnValueGPR);
            if (returnsInFPR) {
                jit.move64ToDouble(GPRInfo::returnValueGPR, FPRInfo::returnValueFPR);
                jit.move(CCallHelpers::TrustedImm64(0x0badbadbadbadbadLL), GPRInfo::returnValueGPR);
            }
            jit.emitFunctionEpilogue();
            jit.ret();
        });
    };
    auto addWords = [&](LeaveRowWriter& row) {
        row.beginHeapWords(3, 3);
        row.add({ 1, FormInLeaveRow::JSValue, PlaceInLeaveRow::Register, Reg(valueInSavedRegister).index() });
        row.add({ 2, FormInLeaveRow::Int32, PlaceInLeaveRow::Register, Reg(valueInPlainRegister).index() });
        row.add({ 3, FormInLeaveRow::Int64, PlaceInLeaveRow::Stack, 2 * wordOfInteger });
    };
    auto checkHead = [&](const std::array<uint64_t, 16>& seen, int32_t number, const CellOfWordsInTest& cell) {
        AOT_TEST_CHECK(seen[1] == bitsOf(jsNumber(number)) && seen[2] == std::bit_cast<uint64_t>(&cell));
        AOT_TEST_CHECK(seen[3] == bitsOf(jsBoolean(true)) && seen[4] == bitsOf(jsDoubleNumber(2.5)));
        AOT_TEST_CHECK(seen[5] == bitsOf(jsNull()) && seen[7] == bitsOf(jsDoubleNumber(static_cast<double>(1ull << 40))));
    };
    auto checkFastBodyWentOn = [&](uint64_t returned) {
        AOT_TEST_CHECK(leaving.returnedIntoFastBody == returned && leaving.savedRegisterBehindCall == bitsOf(jsNull()));
        AOT_TEST_CHECK(leaving.framePointerInOuterFrame == leaving.framePointerOfFastBody && leaving.stackPointerBehindCall == leaving.framePointerOfFastBody - wordsOfFastFrame * sizeof(uint64_t));
    };
    constexpr uint64_t resultOfHead = 0x7e59;
    uint64_t framePointerAboveInnerHead = 0;
    uint64_t framePointerOfOuterHead = 0;
    uint64_t framePointerAboveOuterHead = 0;
    auto head = assembleHead(leaving.seenInInnerFrame, leaving.framePointerInInnerFrame, framePointerAboveInnerHead, resultOfHead);

    // For good: the head is entered in the place of the fast body.
    {
        LeaveRowWriter row(leaving.words, 2);
        addWords(row);
        Typed::addCallOfHead(row, locations, argumentsOfHead(34));
        AOT_TEST_CHECK(runFastBody(row.finish(), { addressOfCodeForLeavingTest(head) }) == resultOfHead);
        checkHead(leaving.seenInInnerFrame, 34, leaving.cells[0]);
        AOT_TEST_CHECK(leaving.seenInInnerFrame[6] == bitsOf(jsNumber(7)));
        for (size_t i = 0; i < saved.size(); ++i)
            AOT_TEST_CHECK(leaving.savedInInnerFrame[i] == leaving.savedAtEntry[i]);
        AOT_TEST_CHECK(leaving.framePointerInInnerFrame == leaving.framePointerOfFastBody && framePointerAboveInnerHead == leaving.framePointerOfCaller);
    }

    // It comes back: the head is called by the fast body, which goes on with what it had in the registers that a callee saves.
    {
        LeaveRowWriter row(leaving.words, 2, EndOfLeave::ComesBack);
        addWords(row);
        Typed::addCallOfHead(row, locations, argumentsOfHead(35));
        AOT_TEST_CHECK(runFastBody(row.finish(), { addressOfCodeForLeavingTest(head) }, EndOfLeave::ComesBack) == resultOfFastBody);
        checkHead(leaving.seenInInnerFrame, 35, leaving.cells[0]);
        AOT_TEST_CHECK(framePointerAboveInnerHead == leaving.framePointerOfFastBody);
        checkFastBodyWentOn(resultOfHead);
    }

    // A chain: the callee's head runs first. What it returns goes into its word of the caller's cell, with a barrier, and the caller's head is entered.
    constexpr uint64_t resultOfOuterHead = 0x7e5b;
    auto outerHead = assembleHead(leaving.seenInOuterFrame, framePointerOfOuterHead, framePointerAboveOuterHead, resultOfOuterHead);
    void* returnIntoStub = static_cast<uint8_t*>(runner.entryOf(goOnInSlowBodyOfCaller())) + bytesInFrontOfReturnIntoStubOfLeaving;
    runner.recordCallsOf(Entry::operationAOTBarrierSlow);
    StubEnvironment& environment = StubEnvironment::of(&runner.instance());
    struct Debt {
        uint64_t returned;
        bool isConstruction;
        bool isDropped;
        uint64_t valueOfOperation;
        FormInLeaveRow form { FormInLeaveRow::JSValue };
    };
    for (EndOfLeave end : { EndOfLeave::ForGood, EndOfLeave::ComesBack }) {
        for (Debt debt : { Debt { bitsOf(jsNumber(77)), false, false, bitsOf(jsNumber(77)) }, Debt { bitsOf(jsNumber(77)), false, true, bitsOf(jsNumber(7)) }, Debt { bitsOf(jsNumber(77)), true, false, made.address() }, Debt { returned.address(), true, false, returned.address() },
            // A callee whose result is proven returns it raw.
            Debt { 0xdeadbeef00000009ULL, false, false, bitsOf(jsNumber(9)), FormInLeaveRow::Int32 }, Debt { 0xdeadbeefffffffffULL, false, false, bitsOf(jsNumber(-1)), FormInLeaveRow::Int32 },
            Debt { 0xdeadbeef00000001ULL, false, false, bitsOf(jsBoolean(true)), FormInLeaveRow::Boolean }, Debt { 0xdeadbeef00000000ULL, false, false, bitsOf(jsBoolean(false)), FormInLeaveRow::Boolean },
            Debt { std::bit_cast<uint64_t>(0.75), false, false, bitsOf(jsDoubleNumber(0.75)), FormInLeaveRow::Double }, Debt { std::bit_cast<uint64_t>(4.0), false, false, bitsOf(jsNumber(4)), FormInLeaveRow::Double },
            Debt { std::bit_cast<uint64_t>(-0.0), false, false, bitsOf(jsDoubleNumber(-0.0)), FormInLeaveRow::Double }, Debt { 0xfff8dead00000001ULL, false, false, bitsOf(jsNaN()), FormInLeaveRow::Double },
            Debt { static_cast<uint64_t>(-3), false, false, bitsOf(jsNumber(-3)), FormInLeaveRow::Int64 }, Debt { 1ull << 41, false, false, bitsOf(jsDoubleNumber(static_cast<double>(1ull << 41))), FormInLeaveRow::Int64 },
            Debt { returned.address(), false, false, returned.address(), FormInLeaveRow::Cell } }) {
            auto innerHead = assembleHead(leaving.seenInInnerFrame, leaving.framePointerInInnerFrame, framePointerAboveInnerHead, debt.returned, debt.form == FormInLeaveRow::Double);
            LeaveRowWriter row(leaving.words, 4, end);
            addWords(row);
            Typed::addCallOfHead(row, locations, argumentsOfHead(36));
            addWords(row);
            auto arguments = argumentsOfHead(37);
            Typed::addFrameThatGoesOn(row, {
                .locations = locations,
                .arguments = arguments,
                .wordOfOperation = debt.isDropped ? std::nullopt : std::optional<uint32_t>((OBJECT_OFFSETOF(CellOfWordsInTest, words) - SavedFrame::offsetOfWords()) / sizeof(uint64_t) + 1),
                .thisOfConstruction = debt.isConstruction ? std::optional(ValueInLeaveRow { 0, FormInLeaveRow::Cell, PlaceInLeaveRow::Constant, made.address() }) : std::nullopt,
                .formOfResultOfPartBelow = debt.form,
                .stackWordsOfPartBelow = locations.numberOfStackArguments,
            });
            environment.numberOfRecordedCalls = 0;
            uint64_t answer = runFastBody(row.finish(), { addressOfCodeForLeavingTest(outerHead), returnIntoStub, addressOfCodeForLeavingTest(innerHead) }, end);
            checkHead(leaving.seenInInnerFrame, 36, leaving.cells[0]);
            checkHead(leaving.seenInOuterFrame, 37, leaving.cells[1]);
            AOT_TEST_CHECK(leaving.seenInInnerFrame[6] == bitsOf(jsNumber(7)) && leaving.seenInOuterFrame[6] == debt.valueOfOperation);
            AOT_TEST_CHECK(environment.numberOfRecordedCalls == !debt.isDropped && (debt.isDropped || environment.recordedArguments[0] == std::bit_cast<uint64_t>(&leaving.cells[1])));
            // The head of the caller has set its frame up where the frame of the stub stood.
            AOT_TEST_CHECK(framePointerOfOuterHead == framePointerAboveInnerHead);
            if (end == EndOfLeave::ForGood) {
                AOT_TEST_CHECK(answer == resultOfOuterHead && framePointerOfOuterHead == leaving.framePointerOfFastBody && framePointerAboveOuterHead == leaving.framePointerOfCaller);
                for (size_t i = 0; i < saved.size(); ++i)
                    AOT_TEST_CHECK(leaving.savedInInnerFrame[i] == leaving.savedAtEntry[i]);
            } else {
                AOT_TEST_CHECK(answer == resultOfFastBody && framePointerAboveOuterHead == leaving.framePointerOfFastBody);
                checkFastBodyWentOn(resultOfOuterHead);
            }
        }
    }
    s_leaving = nullptr;
}

std::span<const NativeTest> leavingTests()
{
    static constexpr NativeTest tests[] = {
        { "leaving/rows-are-read-as-written"_s, testRowsAreReadAsWritten },
        { "leaving/frames-are-laid-out"_s, testFramesAreLaidOut },
        { "leaving/the-stub-leaves-and-arrives"_s, ImageTests::whenExecutable<testStubLeavesAndArrives> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
