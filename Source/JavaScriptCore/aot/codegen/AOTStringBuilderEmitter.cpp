/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringBuilderStorage.h"
#include "B3StackmapGenerationParams.h"
#include "CCallHelpers.h"
#include <algorithm>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

using Storage = StringBuilderStorage;

static LValue loadBytes(Output& out, unsigned width, TypedPointer pointer)
{
    switch (width) {
    case 8:
        return out.load64(pointer);
    case 4:
        return out.load32(pointer);
    case 2:
        return out.load16ZeroExt32(pointer);
    case 1:
        return out.load8ZeroExt32(pointer);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void storeBytes(Output& out, unsigned width, LValue value, TypedPointer pointer)
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

StringBuilderEmitter::StringBuilderEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void StringBuilderEmitter::generateStringBuilderBegin(const ValueContext& value)
{
    if (value.shape() == CodeShape::Compact) {
        define(value, callOperation<CellPointer>(value, RuntimeOperation::StringBuilderBegin, { }));
        return;
    }
    // Every collection clears the word.
    TypedPointer pool = instanceField(m_heaps.collectorState, AOT_INSTANCE_OFFSET(stringBuilder.freeStorage));
    Integer head(m_out.load64(pool));
    define(value, either<CellPointer>(isNotZero(head), [&] {
        CellPointer builder(head.value());
        TypedPointer nextFree = storageField(builder, Storage::offsetOfNextFree());
        m_out.storePtr(m_out.loadPtr(nextFree), pool);
        m_out.storePtr(m_out.constIntPtr(0), nextFree);
        return builder;
    }, [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::StringBuilderBegin, { }, { .isOnRarePath = true });
    }));
}

void StringBuilderEmitter::generateStringBuilderAppendBuiltin(const ValueContext& value)
{
    const BuiltinOverload& overload = m_builtins.at(value->overload());
    const BuiltinForms* forms = m_builtins.formsOf(overload.builtin);
    if (!forms || !forms->hasHostAppendFunction) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Boxed thisValue = inOptional<Boxed>(value->receiverOperand()).value_or(undefined());
    Vector<Boxed, 8> arguments;
    for (ValueID argument : value->argumentsOperands())
        arguments.append(in<Boxed>(argument));
    Word32 count = word32(static_cast<int32_t>(arguments.size()));
    callVoidOperation(value, RuntimeOperation::StringBuilderAppendByHost, { builder, word32(overload.builtin.index()), thisValue, count, argumentBuffer(arguments.span()) });
}

void StringBuilderEmitter::generateStringBuilderAppendJSON(const ValueContext& value)
{
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::StringBuilderAppendJSON, { in<CellPointer>(value->builderOperand()), in<Boxed>(value->valueOperand()), in<Boxed>(value->propertyOperand()) }));
}

void StringBuilderEmitter::generateStringBuilderLength(const ValueContext& value)
{
    define(value, lengthOf(in<CellPointer>(value->builderOperand())));
}

void StringBuilderEmitter::generateStringBuilderToString(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::StringBuilderToString, { in<CellPointer>(value->builderOperand()), word32(0) }));
}

void StringBuilderEmitter::generateStringBuilderFinish(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::StringBuilderFinish, { in<CellPointer>(value->builderOperand()), word32(value->isLikelyKey()) }));
}

void StringBuilderEmitter::generateStringBuilderDiscard(const ValueContext& value)
{
    // The operation returns the builder to the pool, whose word is collector state, and it does not collect.
    callVoidOperation(value, RuntimeOperation::StringBuilderDiscard, { in<CellPointer>(value->builderOperand()) }, { .accessesCollectorState = true });
}

void StringBuilderEmitter::generateStringBuilderCheckpoint(const ValueContext& value)
{
    define(value, lengthOf(in<CellPointer>(value->builderOperand())));
}

void StringBuilderEmitter::generateStringBuilderRollBack(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::StringBuilderRollBack, { in<CellPointer>(value->builderOperand()), emitter<StringEmitter>().word32Of(value->checkpointOperand()) });
}

TypedPointer StringBuilderEmitter::storageField(CellPointer builder, ptrdiff_t offset)
{
    return TypedPointer(m_heaps.entire(AbstractHeap::StringBuilderContents), builder.value(), offset);
}

TypedPointer StringBuilderEmitter::bytesAt(Pointer cursor, ptrdiff_t offset)
{
    return TypedPointer(m_heaps.entire(AbstractHeap::StringBuilderContents), cursor.value(), offset);
}

Word32 StringBuilderEmitter::lengthOf(CellPointer builder)
{
    Word32 position(m_out.load32(storageField(builder, Storage::offsetOfPosition())));
    Word32 lengthOutsideBuffer(m_out.load32(storageField(builder, Storage::offsetOfLengthOutsideBuffer())));
    return add(position, lengthOutsideBuffer);
}

Pointer StringBuilderEmitter::tryReserve(CellPointer builder, Word32 size, LBasicBlock whenThereIsNoRoom)
{
    Integer position = zeroExtended(Word32(m_out.load32(storageField(builder, Storage::offsetOfPosition()))));
    Integer limit = zeroExtended(Word32(m_out.load32(storageField(builder, Storage::offsetOfLimit()))));
    branchUnless(belowOrEqual(add(position, zeroExtended(size)), limit), whenThereIsNoRoom);
    return offsetBy(Pointer(m_out.loadPtr(storageField(builder, Storage::offsetOfCharacters()))), position, 0);
}

Pointer StringBuilderEmitter::reserve(const OperationContext& context, CellPointer builder, Word32 size)
{
    LBasicBlock slow = newBlock();
    Join<Pointer> join(m_out);
    join.add(tryReserve(builder, size, slow));

    m_out.appendTo(slow);
    join.add(callOperation<Pointer>(context, RuntimeOperation::StringBuilderReserve, { builder, zeroExtended(size) }, { .isOnRarePath = true }));
    return join.finish();
}

void StringBuilderEmitter::commit(CellPointer builder, Pointer cursor)
{
    LValue characters = m_out.loadPtr(storageField(builder, Storage::offsetOfCharacters()));
    m_out.store32(m_out.castToInt32(m_out.sub(cursor.value(), characters)), storageField(builder, Storage::offsetOfPosition()));
}

Pointer StringBuilderEmitter::writeBytes(Pointer cursor, std::span<const Latin1Character> bytes)
{
    for (size_t offset = 0; offset < bytes.size();) {
        size_t remaining = bytes.size() - offset;
        unsigned width = remaining > 4 ? 8 : remaining > 2 ? 4 : remaining;
        uint64_t bits = 0;
        for (unsigned i = 0; i < width && i < remaining; ++i)
            bits |= static_cast<uint64_t>(bytes[offset + i].value) << (8 * i);
        LValue constant = width == 8 ? m_out.constInt64(static_cast<int64_t>(bits)) : m_out.constInt32(static_cast<int32_t>(bits));
        storeBytes(m_out, width, constant, bytesAt(cursor, offset));
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
    m_out.branch(m_out.below(number, constant(10)), normalEdge(oneDigit), normalEdge(severalDigits));

    m_out.appendTo(oneDigit);
    ValueFromBlock oneDigitText = m_out.anchor(m_out.add(number, constant('0')));
    ValueFromBlock oneDigitCount = m_out.anchor(constant(1));
    m_out.jump(store);

    m_out.appendTo(severalDigits);
    LValue upperHalf = m_out.lShr(m_out.mul(number, constant(0xd1b71759ull)), m_out.constInt32(45));
    LValue lowerHalf = m_out.sub(number, m_out.mul(upperHalf, constant(10000)));
    LValue halves = m_out.bitOr(upperHalf, m_out.shl(lowerHalf, m_out.constInt32(32)));
    LValue upperPairs = m_out.bitAnd(m_out.lShr(m_out.mul(halves, constant(10486)), m_out.constInt32(20)), constant(0x7full << 32 | 0x7f));
    LValue lowerPairs = m_out.sub(halves, m_out.mul(upperPairs, constant(100)));
    LValue pairs = m_out.add(m_out.shl(lowerPairs, m_out.constInt32(16)), upperPairs);
    LValue tens = m_out.bitAnd(m_out.lShr(m_out.mul(pairs, constant(103)), m_out.constInt32(10)), constant(0x000f000f000f000full));
    LValue digits = m_out.add(tens, m_out.shl(m_out.sub(pairs, m_out.mul(tens, constant(10))), m_out.constInt32(8)));
    B3::PatchpointValue* trailingZeros = m_out.patchpoint(B3::Int64);
    trailingZeros->appendSomeRegister(digits);
    trailingZeros->effects = B3::Effects::none();
    trailingZeros->setRunsWithoutFrame(true);
    trailingZeros->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        jit.countTrailingZeros64WithoutNullCheck(params[1].gpr(), params[0].gpr());
    });
    LValue leadingZeroBits = m_out.bitAnd(trailingZeros, constant(0x38));
    ValueFromBlock severalDigitsText = m_out.anchor(m_out.lShr(m_out.add(digits, constant(asciiZeros)), m_out.castToInt32(leadingZeroBits)));
    ValueFromBlock severalDigitsCount = m_out.anchor(m_out.sub(constant(maxInlineDigits), m_out.lShr(leadingZeroBits, m_out.constInt32(3))));
    m_out.jump(store);

    m_out.appendTo(store);
    LValue text = m_out.phi(B3::Int64, oneDigitText, severalDigitsText);
    Integer count(m_out.phi(B3::Int64, oneDigitCount, severalDigitsCount));
    m_out.store64(text, bytesAt(cursor));
    return offsetBy(cursor, count, 0);
}

Pointer StringBuilderEmitter::copyLatin1Characters(Pointer cursor, Pointer source, Word32 length)
{
    constexpr uint64_t bytesPerMove = 2 * sizeof(uint64_t);
    // The short copy covers two words at most, and the last move of the loop reaches one whole move back from the end.
    uint64_t maxLengthOfShortCopy = std::clamp<uint64_t>(build().limit(LimitID::MaxLengthCopiedInline), bytesPerMove - 1, bytesPerMove);
    LBasicBlock shortCopy = newBlock();
    LBasicBlock loop = newBlock();
    LBasicBlock lastMove = newBlock();
    Join<Pointer> end(m_out);
    Integer size = zeroExtended(length);
    auto moveAt = [&](Integer offset) {
        const B3::AbstractHeap& characters = m_heaps.characters8.atAnyIndex();
        LValue from = offsetBy(source, offset, 0).value();
        Pointer to = offsetBy(cursor, offset, 0);
        LValue firstWord = m_out.load64(TypedPointer(characters, from));
        LValue secondWord = m_out.load64(TypedPointer(characters, from, sizeof(uint64_t)));
        m_out.store64(firstWord, bytesAt(to));
        m_out.store64(secondWord, bytesAt(to, sizeof(uint64_t)));
    };
    ValueFromBlock firstOffset = m_out.anchor(m_out.constInt64(0));
    m_out.branch(belowOrEqual(length, word32(static_cast<int32_t>(maxLengthOfShortCopy))).value(), normalEdge(shortCopy), normalEdge(loop));

    m_out.appendTo(shortCopy);
    end.add(copyShortLatin1Characters(cursor, source, length));

    m_out.appendTo(loop);
    Integer offset(m_out.phi(B3::Int64, firstOffset));
    moveAt(offset);
    Integer nextOffset = add(offset, integer(bytesPerMove));
    m_out.addIncomingToPhi(offset.value(), m_out.anchor(nextOffset.value()));
    m_out.branch(belowOrEqual(add(nextOffset, integer(bytesPerMove)), size).value(), normalEdge(loop), normalEdge(lastMove));

    m_out.appendTo(lastMove);
    moveAt(sub(size, integer(bytesPerMove)));
    end.add(offsetBy(cursor, size, 0));
    return end.finish();
}

Pointer StringBuilderEmitter::copyShortLatin1Characters(Pointer cursor, Pointer source, Word32 length)
{
    const B3::AbstractHeap& characters = m_heaps.characters8.atAnyIndex();
    LBasicBlock done = newBlock();
    Integer size = zeroExtended(length);
    for (unsigned width : { 8u, 4u, 2u }) {
        LBasicBlock copy = newBlock();
        LBasicBlock shorter = newBlock();
        m_out.branch(aboveOrEqual(length, word32(width)).value(), normalEdge(copy), normalEdge(shorter));

        m_out.appendTo(copy);
        Integer lastOffset = sub(size, integer(width));
        LValue first = loadBytes(m_out, width, TypedPointer(characters, source.value()));
        LValue last = loadBytes(m_out, width, TypedPointer(characters, offsetBy(source, lastOffset, 0).value()));
        storeBytes(m_out, width, first, bytesAt(cursor));
        storeBytes(m_out, width, last, bytesAt(offsetBy(cursor, lastOffset, 0)));
        m_out.jump(done);

        m_out.appendTo(shorter);
    }
    LBasicBlock oneCharacter = newBlock();
    m_out.branch(isNotZero(length).value(), normalEdge(oneCharacter), normalEdge(done));

    m_out.appendTo(oneCharacter);
    storeBytes(m_out, 1, loadBytes(m_out, 1, TypedPointer(characters, source.value())), bytesAt(cursor));
    m_out.jump(done);

    m_out.appendTo(done);
    return offsetBy(cursor, size, 0);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
