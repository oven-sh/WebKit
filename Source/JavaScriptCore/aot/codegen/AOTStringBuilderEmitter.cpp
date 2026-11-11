/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderEmitter.h"

#if ENABLE(AOT)

#include "AOTStringBuilderStorage.h"
#include "AOTStringEmitter.h"
#include "AOTStubsStrings.h"
#include "JSCJSValueInlines.h"
#include <limits>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(JSString::MaxLength == static_cast<unsigned>(std::numeric_limits<int32_t>::max()), "the length of a builder, and so a checkpoint, is an int32 that is not negative");

static void storeBytesOfStringBuilder(Output& out, unsigned width, LValue value, TypedPointer pointer)
{
    switch (width) {
    case 8:
        out.store64(value, pointer);
        return;
    case 4:
        out.store32(value, pointer);
        return;
    case 2:
        out.store32As16(value, pointer);
        return;
    case 1:
        out.store32As8(value, pointer);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StringBuilderEmitter::generateStringBuilderBegin(const ValueContext& value)
{
    TypedPointer pool = instanceField(m_heaps.collectorState, AOT_INSTANCE_OFFSET(stringBuilder.freeStorage));
    Integer head(m_out.load64(pool));
    define(value, either<CellPointer>(isNotZero(head), [&] {
        CellPointer builder(head.value());
        TypedPointer nextFree = storageField(builder, StringBuilderStorage::offsetOfNextFree());
        m_out.storePtr(m_out.loadPtr(nextFree), pool);
        m_out.storePtr(m_out.constIntPtr(0), nextFree);
        return builder;
    }, [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::StringBuilderBegin, { }, { .isOnRarePath = true });
    }));
}

void StringBuilderEmitter::generateStringBuilderLength(const ValueContext& value)
{
    define(value, lengthOf(in<CellPointer>(value->builderOperand())));
}

void StringBuilderEmitter::generateStringBuilderToString(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Integer lastString(m_out.load64(storageField(builder, StringBuilderStorage::offsetOfLastString())));
    Word32 lengthOfLastString(m_out.load32(storageField(builder, StringBuilderStorage::offsetOfLengthOfLastString())));
    Condition hasLastString = isNotZero(lastString);
    define(value, either<CellPointer>(logicalAnd(hasLastString, equal(lengthOfLastString, lengthOf(builder))), [&] {
        return CellPointer(lastString.value());
    }, [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::StringBuilderToString, { builder, word32(static_cast<int32_t>(StringBuilderStorage::StringUse::Unknown)) });
    }));
}

void StringBuilderEmitter::generateStringBuilderFinish(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    define(value, callOperation<CellPointer>(value, RuntimeOperation::StringBuilderFinish, { builder, word32(static_cast<int32_t>(StringBuilderStorage::StringUse::Unknown)) }));
}

void StringBuilderEmitter::generateStringBuilderDiscard(const ValueContext& value)
{
    callStub(value, stringBuilderDiscard(), { in<CellPointer>(value->builderOperand()).value() }, { .accessesCollectorState = true });
}

void StringBuilderEmitter::generateStringBuilderCheckpoint(const ValueContext& value)
{
    define(value, lengthOf(in<CellPointer>(value->builderOperand())));
}

void StringBuilderEmitter::generateStringBuilderRollBack(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Word32 checkpoint = emitter<StringEmitter>().word32Of(value->checkpointOperand());
    callVoidOperation(value, RuntimeOperation::StringBuilderRollBack, { builder, checkpoint });
}

void StringBuilderEmitter::generateStackStringBuilderBegin(const ValueContext& value)
{
    LValue slot = m_out.lockedStackSlot(sizeof(StackStringBuilder));
    slot->slot()->setIsDeadAtSuspensions(true);
    storeEmptyRecord(Pointer(slot));
    define(value, CellPointer(slot));
}

void StringBuilderEmitter::storeEmptyRecord(Pointer record)
{
    Pointer buffer = offsetBy(record, OBJECT_OFFSETOF(StackStringBuilder, buffer));
    m_out.storePtr(buffer.value(), recordField(record, OBJECT_OFFSETOF(StackStringBuilder, savedCursor)));
    m_out.storePtr(limitOnStack(record).value(), recordField(record, OBJECT_OFFSETOF(StackStringBuilder, limit)));
    m_out.storePtr(buffer.value(), recordField(record, OBJECT_OFFSETOF(StackStringBuilder, lengthOrigin)));
}

void StringBuilderEmitter::generateStackStringBuilderStart(const ValueContext& value)
{
    define(value, offsetBy(recordOf(value), OBJECT_OFFSETOF(StackStringBuilder, buffer)));
}

void StringBuilderEmitter::generateStackStringBuilderCursor(const ValueContext& value)
{
    define(value, Pointer(m_out.loadPtr(recordField(recordOf(value), OBJECT_OFFSETOF(StackStringBuilder, savedCursor)))));
}

void StringBuilderEmitter::defineCursorBehindPart(const ValueContext& value, Pointer record, Pointer cursor, unsigned constantsWrittenAhead)
{
    std::span<const ValueID> behind = value.operationsBehind();
    RELEASE_ASSERT(constantsWrittenAhead <= behind.size());
    ValueID newestCursor = constantsWrittenAhead ? behind[constantsWrittenAhead - 1] : value.id();
    const Operation* next = constantsWrittenAhead < behind.size() ? &operationAt(behind[constantsWrittenAhead]) : nullptr;
    bool nextPartTakesCursor = next && partsOfStackStringBuilder.contains(next->opcode) && next->cursorOperand() == newestCursor;
    if (!nextPartTakesCursor)
        m_out.storePtr(cursor.value(), recordField(record, OBJECT_OFFSETOF(StackStringBuilder, savedCursor)));
    define(value, cursor);
}

void StringBuilderEmitter::generateStackStringBuilderLength(const ValueContext& value)
{
    LValue lengthOrigin = m_out.loadPtr(recordField(recordOf(value), OBJECT_OFFSETOF(StackStringBuilder, lengthOrigin)));
    define(value, Word32(m_out.castToInt32(m_out.sub(in<Pointer>(value->cursorOperand()).value(), lengthOrigin))));
}

void StringBuilderEmitter::generateStackStringBuilderToString(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::StackStringBuilderToString, { recordOf(value), in<Pointer>(value->cursorOperand()) }));
}

void StringBuilderEmitter::generateStackStringBuilderFinish(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::StackStringBuilderFinish, { recordOf(value), in<Pointer>(value->cursorOperand()), word32(static_cast<int32_t>(StringBuilderStorage::StringUse::Unknown)) }));
}

void StringBuilderEmitter::generateStackStringBuilderDiscard(const ValueContext& value)
{
    Pointer record = recordOf(value);
    Pointer limit = limitOf(record);
    ifElse(equal(limit, limitOnStack(record)), [] { }, [&] {
        callVoidOperation(value, RuntimeOperation::StackStringBuilderDiscard, { record }, { .isOnRarePath = true, .accessesCollectorState = true });
    });
    storeEmptyRecord(record);
}

Pointer StringBuilderEmitter::limitOf(Pointer record)
{
    return Pointer(m_out.loadPtr(recordField(record, OBJECT_OFFSETOF(StackStringBuilder, limit))));
}

Pointer StringBuilderEmitter::limitOnStack(Pointer record)
{
    return offsetBy(record, OBJECT_OFFSETOF(StackStringBuilder, buffer) + StackStringBuilder::capacity);
}

Pointer StringBuilderEmitter::advance(const OperationContext& context, Pointer record, Pointer cursor)
{
    return either<Pointer>(belowOrEqual(cursor, limitOf(record)), [&]() -> Pointer {
        return cursor;
    }, [&]() -> Pointer {
        return callOperation<Pointer>(context, RuntimeOperation::StackStringBuilderGrow, { record, cursor }, { .isOnRarePath = true });
    });
}

Word32 StringBuilderEmitter::lengthOf(CellPointer builder)
{
    Word32 position(m_out.load32(storageField(builder, StringBuilderStorage::offsetOfPosition())));
    Word32 lengthOutsideBuffer(m_out.load32(storageField(builder, StringBuilderStorage::offsetOfLengthOutsideBuffer())));
    return add(position, lengthOutsideBuffer);
}

Pointer StringBuilderEmitter::tryReserve(CellPointer builder, Word32 size, LBasicBlock whenThereIsNoRoom)
{
    Integer position = zeroExtended(Word32(m_out.load32(storageField(builder, StringBuilderStorage::offsetOfPosition()))));
    Integer limit = zeroExtended(Word32(m_out.load32(storageField(builder, StringBuilderStorage::offsetOfLimit()))));
    branchUnless(belowOrEqual(add(position, zeroExtended(size)), limit), whenThereIsNoRoom);
    return offsetBy(Pointer(m_out.loadPtr(storageField(builder, StringBuilderStorage::offsetOfCharacters()))), position, 0);
}

Pointer StringBuilderEmitter::reserve(const OperationContext& context, CellPointer builder, uint32_t length)
{
    LBasicBlock slow = newBlock();
    Join<Pointer> join(m_out);
    join.add(tryReserve(builder, word32(static_cast<int32_t>(roundUpToMultipleOf<sizeof(uint64_t)>(length))), slow));

    m_out.appendTo(slow);
    join.add(callOperation<Pointer>(context, RuntimeOperation::StringBuilderReserve, { builder, integer(length) }, { .isOnRarePath = true }));
    return join.finish();
}

void StringBuilderEmitter::commit(CellPointer builder, Pointer cursor)
{
    LValue characters = m_out.loadPtr(storageField(builder, StringBuilderStorage::offsetOfCharacters()));
    LValue position = m_out.castToInt32(m_out.sub(cursor.value(), characters));
    m_out.store32(position, storageField(builder, StringBuilderStorage::offsetOfPosition()));
}

Pointer StringBuilderEmitter::writeBytes(Pointer cursor, std::span<const Latin1Character> characters)
{
    std::span<const uint8_t> bytes = byteCast<uint8_t>(characters);
    for (size_t offset = 0; offset < bytes.size();) {
        size_t remaining = bytes.size() - offset;
        unsigned width = remaining > 4 ? 8 : remaining > 2 ? 4 : static_cast<unsigned>(remaining);
        uint64_t bits = 0;
        for (unsigned i = 0; i < width && i < remaining; ++i)
            bits |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
        LValue constant = width == 8 ? m_out.constInt64(static_cast<int64_t>(bits)) : m_out.constInt32(static_cast<int32_t>(bits));
        storeBytesOfStringBuilder(m_out, width, constant, bytesAt(cursor, static_cast<ptrdiff_t>(offset)));
        offset += width;
    }
    return offsetBy(cursor, static_cast<ptrdiff_t>(bytes.size()));
}

Pointer StringBuilderEmitter::writeDigits(Pointer cursor, Word32 value)
{
    static_assert(maxInlineDigits == sizeof(uint64_t));
    static_assert(firstIntegerWrittenByCall == 100'000'000);
    constexpr uint64_t asciiZeros = 0x3030303030303030ull;
    LBasicBlock oneDigit = newBlock();
    LBasicBlock severalDigits = newBlock();
    LBasicBlock store = newBlock();
    auto constant = [&](uint64_t bits) {
        return m_out.constInt64(static_cast<int64_t>(bits));
    };
    LValue number = zeroExtended(value).value();
    LValue hasOneDigit = m_out.below(number, constant(10));
    ScopeFork fork = m_out.forkScope();
    m_out.branch(hasOneDigit, normalEdge(oneDigit), normalEdge(severalDigits));

    m_out.appendToArm(oneDigit, fork);
    ValueFromBlock oneDigitText = m_out.anchor(m_out.add(number, constant('0')));
    ValueFromBlock oneDigitCount = m_out.anchor(constant(1));
    m_out.jump(store);

    m_out.appendToArm(severalDigits, fork);
    LValue upperHalf = m_out.lShr(m_out.mul(number, constant(0xd1b71759ull)), 45);
    LValue lowerHalf = m_out.sub(number, m_out.mul(upperHalf, constant(10000)));
    LValue halves = m_out.bitOr(upperHalf, m_out.shl(lowerHalf, 32));
    LValue hundredthsOfHalves = m_out.lShr(m_out.mul(halves, constant(10486)), 20);
    LValue upperPairs = m_out.bitAnd(hundredthsOfHalves, constant(0x7full << 32 | 0x7f));
    LValue lowerPairs = m_out.sub(halves, m_out.mul(upperPairs, constant(100)));
    LValue pairs = m_out.add(m_out.shl(lowerPairs, 16), upperPairs);
    LValue tenthsOfPairs = m_out.lShr(m_out.mul(pairs, constant(103)), 10);
    LValue tens = m_out.bitAnd(tenthsOfPairs, constant(0x000f000f000f000full));
    LValue digits = m_out.add(tens, m_out.shl(m_out.sub(pairs, m_out.mul(tens, constant(10))), 8));
    LValue leadingZeroBits = m_out.bitAnd(countTrailingZerosOfNonZero(digits), constant(0x38));
    LValue textBehindZeros = m_out.add(digits, constant(asciiZeros));
    ValueFromBlock severalDigitsText = m_out.anchor(m_out.lShr(textBehindZeros, m_out.castToInt32(leadingZeroBits)));
    LValue mostDigits = constant(maxInlineDigits);
    ValueFromBlock severalDigitsCount = m_out.anchor(m_out.sub(mostDigits, m_out.lShr(leadingZeroBits, 3)));
    m_out.jump(store);

    m_out.appendToJoin(store, fork);
    LValue text = m_out.phi(Machine::Int64, oneDigitText, severalDigitsText);
    Integer count(m_out.phi(Machine::Int64, oneDigitCount, severalDigitsCount));
    m_out.store64(text, bytesAt(cursor));
    return offsetBy(cursor, count, 0);
}

Pointer StringBuilderEmitter::copyLatin1Characters(Pointer cursor, Pointer source, Word32 length)
{
    constexpr int32_t bytesPerMove = 2 * sizeof(uint64_t);
    Condition isShort = belowOrEqual(length, word32(bytesPerMove));
    if (isShort.value()->hasInt() && isShort.value()->asInt())
        return copyShortLatin1Characters(cursor, source, length);

    LBasicBlock shortCopy = newBlock();
    LBasicBlock loop = newBlock();
    LBasicBlock lastMove = newBlock();
    Join<Pointer> end(m_out);
    Integer size = zeroExtended(length);
    auto moveAt = [&](Integer offset) {
        const Machine::AbstractHeap& characters = m_heaps.characters8.atAnyIndex();
        LValue from = offsetBy(source, offset, 0).value();
        Pointer to = offsetBy(cursor, offset, 0);
        LValue firstWord = m_out.load64(m_out.address(characters, from));
        LValue secondWord = m_out.load64(m_out.address(characters, from, sizeof(uint64_t)));
        m_out.store64(firstWord, bytesAt(to));
        m_out.store64(secondWord, bytesAt(to, sizeof(uint64_t)));
    };
    ValueFromBlock firstOffset = m_out.anchor(m_out.constInt64(0));
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isShort.value(), normalEdge(shortCopy), normalEdge(loop));

    m_out.appendToArm(shortCopy, fork);
    end.add(copyShortLatin1Characters(cursor, source, length));

    m_out.appendTo(loop);
    Integer offset(m_out.phi(Machine::Int64, firstOffset));
    moveAt(offset);
    Integer nextOffset = add(offset, integer(bytesPerMove));
    m_out.addIncomingToPhi(offset.value(), m_out.anchor(nextOffset.value()));
    m_out.branch(belowOrEqual(add(nextOffset, integer(bytesPerMove)), size).value(), normalEdge(loop), normalEdge(lastMove));

    m_out.appendTo(lastMove);
    moveAt(sub(size, integer(bytesPerMove)));
    end.add(offsetBy(cursor, size, 0));
    return end.finishBehind(fork);
}

Pointer StringBuilderEmitter::copyShortLatin1Characters(Pointer cursor, Pointer source, Word32 length)
{
    const Machine::AbstractHeap& characters = m_heaps.characters8.atAnyIndex();
    LBasicBlock done = newBlock();
    Integer size = zeroExtended(length);
    for (unsigned width : { 8u, 4u, 2u }) {
        LBasicBlock copy = newBlock();
        LBasicBlock shorter = newBlock();
        m_out.branch(aboveOrEqual(length, word32(static_cast<int32_t>(width))).value(), normalEdge(copy), normalEdge(shorter));

        m_out.appendTo(copy);
        Integer lastOffset = sub(size, integer(width));
        LValue first = loadBytes(m_out, width, m_out.address(characters, source.value()));
        LValue last = loadBytes(m_out, width, m_out.address(characters, offsetBy(source, lastOffset, 0).value()));
        storeBytesOfStringBuilder(m_out, width, first, bytesAt(cursor));
        storeBytesOfStringBuilder(m_out, width, last, bytesAt(offsetBy(cursor, lastOffset, 0)));
        m_out.jump(done);

        m_out.appendTo(shorter);
    }
    LBasicBlock oneCharacter = newBlock();
    m_out.branch(isNotZero(length).value(), normalEdge(oneCharacter), normalEdge(done));

    m_out.appendTo(oneCharacter);
    LValue character = loadBytes(m_out, 1, m_out.address(characters, source.value()));
    storeBytesOfStringBuilder(m_out, 1, character, bytesAt(cursor));
    m_out.jump(done);

    m_out.appendTo(done);
    return offsetBy(cursor, size, 0);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
