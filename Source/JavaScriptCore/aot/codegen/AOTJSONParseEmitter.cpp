/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONParseEmitter.h"

#if ENABLE(AOT)

#include "AOTFrameMaps.h"
#include "AOTJSONScanner.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTVector.h"
#include "B3StackmapGenerationParams.h"
#include "StackManager.h"
#include <algorithm>
#include <wtf/ASCIICType.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(maxExactMantissaDigits == 15 && maxJSONInteger == 9007199254740991, "The result type of JSONParseInteger is int[-9007199254740991, 9007199254740991]");

void JSONParseEmitter::generateJSONSkipWhitespace(const ValueContext& value)
{
    static constexpr int64_t whitespace = 1LL << ' ' | 1LL << '\n' | 1LL << '\r' | 1LL << '\t';
    Pointer cursor = cursorOf(value);
    auto byteAt = [&](LValue position) {
        return m_out.load8ZeroExt32(at(Pointer(position)));
    };
    LBasicBlock continuation = newBlock();
    LBasicBlock mayBeWhitespace = newBlock();
    LBasicBlock isWhitespace = newBlock();
    Vector<ValueFromBlock, 3> ends;
    LValue first = byteAt(cursor.value());
    ValueFromBlock start = m_out.anchor(cursor.value());
    ValueFromBlock firstByte = m_out.anchor(first);
    ends.append(m_out.anchor(cursor.value()));
    m_out.branch(m_out.above(first, m_out.constInt32(' ')), normalEdge(continuation), unlikelyEdge(mayBeWhitespace));

    m_out.appendTo(mayBeWhitespace);
    LValue position = m_out.phi(B3::Int64, start);
    LValue byte = m_out.phi(B3::Int32, firstByte);
    ends.append(m_out.anchor(position));
    m_out.branch(m_out.testNonZero64(m_out.lShr(m_out.constInt64(whitespace), byte), 1), normalEdge(isWhitespace), normalEdge(continuation));

    m_out.appendTo(isWhitespace);
    LValue next = m_out.add(position, m_out.constInt64(1));
    LValue nextByte = byteAt(next);
    ends.append(m_out.anchor(next));
    m_out.addIncomingToPhi(position, m_out.anchor(next));
    m_out.addIncomingToPhi(byte, m_out.anchor(nextByte));
    m_out.branch(m_out.above(nextByte, m_out.constInt32(' ')), normalEdge(continuation), normalEdge(mayBeWhitespace));

    m_out.appendTo(continuation);
    define(value, Pointer(m_out.phi(B3::Int64, ends)));
}

Condition JSONParseEmitter::startsWith(Pointer cursor, std::span<const uint8_t> text, bool isLikely)
{
    if (text.empty())
        return boolean(true);
    bool canCompareBytes = std::ranges::all_of(text, [](uint8_t byte) {
        return byte && isASCII(byte);
    });
    if (!canCompareBytes)
        return boolean(false);

    LBasicBlock fails = nullptr;
    auto continueIf = [&](Condition condition) {
        if (!fails)
            fails = newBlock();
        LBasicBlock next = newBlock();
        m_out.branch(condition.value(), normalEdge(next), isLikely ? rareEdge(fails) : normalEdge(fails));
        m_out.appendTo(next);
    };
    size_t size = text.size();
    if (size > jsonSourcePadding)
        continueIf(Condition(m_out.aboveOrEqual(m_out.sub(sourceEnd().value(), cursor.value()), size)));
    Condition isSame;
    for (size_t offset = 0; offset < size;) {
        if (isSame)
            continueIf(isSame);
        size_t remaining = size - offset;
        unsigned width = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
        if (remaining > 4 && remaining < 8 && size >= 8) {
            width = 8;
            offset = size - 8;
        } else if (remaining == 3 && size >= 4) {
            width = 4;
            offset = size - 4;
        }
        uint64_t bits = 0;
        for (unsigned i = 0; i < width; ++i)
            bits |= static_cast<uint64_t>(text[offset + i]) << (8 * i);
        TypedPointer pointer = at(cursor, offset);
        if (width == 8)
            isSame = equal(Integer(m_out.load64(pointer)), static_cast<int64_t>(bits));
        else
            isSame = equal(Word32(width == 4 ? m_out.load32(pointer) : width == 2 ? m_out.load16ZeroExt32(pointer) : m_out.load8ZeroExt32(pointer)), static_cast<int32_t>(bits));
        offset += width;
    }
    if (!fails)
        return isSame;

    Join<Condition> join(m_out);
    join.add(isSame);
    m_out.appendTo(fails);
    join.add(boolean(false));
    return join.finish();
}

StubCallResult JSONParseEmitter::callReturningPair(const ValueContext& value, RuntimeOperation operation, std::initializer_list<Operand> arguments)
{
    return emitter<RuntimeCallEmitter>().callOperationReturningPair(value, operation, std::span<const Operand>(arguments.begin(), arguments.size()));
}

void JSONParseEmitter::defineParsed(const ValueContext& value, Operand parsed, LValue next)
{
    Condition isParsed(m_out.notZero64(next));
    defineResults(value, { isParsed, parsed, cursorOrSourceEnd(isParsed, next) });
}

void JSONParseEmitter::defineParsedByOperation(const ValueContext& value, RuntimeOperation operation, Representation representation)
{
    StubCallResult parsed = callReturningPair(value, operation, { cursorOf(value) });
    defineParsed(value, Operand(representation, parsed.first), parsed.second);
}

void JSONParseEmitter::parseDigits(const ValueContext& value, bool isInteger)
{
    Pointer start = cursorOf(value);
    auto parseByOperation = [&] {
        StubCallResult parsed = callReturningPair(value, isInteger ? RuntimeOperation::JSONParseInteger : RuntimeOperation::JSONParseNumber, { start });
        return StubCallResult { isInteger ? parsed.first : m_out.bitCast(parsed.first, B3::Double), parsed.second };
    };
    auto defineNumber = [&](LValue parsed, LValue next) {
        defineParsed(value, isInteger ? Operand(Integer(parsed)) : Operand(Number(parsed)), next);
    };
    if (value.shape() == CodeShape::Compact) {
        StubCallResult parsed = parseByOperation();
        defineNumber(parsed.first, parsed.second);
        return;
    }
    auto digitAt = [&](LValue position) {
        return m_out.sub(m_out.load8ZeroExt32(at(Pointer(position))), '0');
    };
    LBasicBlock continuation = newBlock();
    LBasicBlock nextDigit = newBlock();
    LBasicBlock hasDigit = newBlock();
    LBasicBlock hasNoMoreDigits = newBlock();
    LBasicBlock isIntegralNumber = newBlock();
    LBasicBlock genericPath = newBlock();
    LValue first = digitAt(start.value());
    ValueFromBlock firstCursor = m_out.anchor(start.value());
    ValueFromBlock firstDigits = m_out.anchor(m_out.zeroExt(first, B3::Int64));
    m_out.branch(m_out.below(m_out.sub(first, 1), 9), normalEdge(nextDigit), normalEdge(genericPath));

    m_out.appendTo(nextDigit);
    LValue previous = m_out.phi(B3::Int64, firstCursor);
    LValue digits = m_out.phi(B3::Int64, firstDigits);
    LValue position = m_out.add(previous, m_out.constInt64(1));
    LValue digit = digitAt(position);
    m_out.branch(m_out.below(digit, m_out.constInt32(10)), normalEdge(hasDigit), normalEdge(hasNoMoreDigits));

    m_out.appendTo(hasDigit);
    m_out.addIncomingToPhi(previous, m_out.anchor(position));
    LValue shiftedDigits = m_out.mul(digits, 10);
    m_out.addIncomingToPhi(digits, m_out.anchor(m_out.add(shiftedDigits, m_out.zeroExt(digit, B3::Int64))));
    m_out.jump(nextDigit);

    m_out.appendTo(hasNoMoreDigits);
    for (LValue continuesNumber : { m_out.equal(digit, '.' - '0'), m_out.equal(m_out.bitOr(digit, 0x20), 'e' - '0') }) {
        LBasicBlock next = newBlock();
        m_out.branch(continuesNumber, normalEdge(genericPath), normalEdge(next));
        m_out.appendTo(next);
    }
    m_out.branch(m_out.above(m_out.sub(position, start.value()), maxExactMantissaDigits), rareEdge(genericPath), normalEdge(isIntegralNumber));

    m_out.appendTo(isIntegralNumber);
    ValueFromBlock inlineNumber = m_out.anchor(isInteger ? digits : m_out.intToDouble(digits));
    ValueFromBlock inlineNext = m_out.anchor(position);
    m_out.jump(continuation);

    m_out.appendTo(genericPath);
    StubCallResult parsed = parseByOperation();
    ValueFromBlock parsedNumber = m_out.anchor(parsed.first);
    ValueFromBlock parsedNext = m_out.anchor(parsed.second);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    LValue joinedNumber = m_out.phi(isInteger ? B3::Int64 : B3::Double, inlineNumber, parsedNumber);
    defineNumber(joinedNumber, m_out.phi(B3::Int64, inlineNext, parsedNext));
}

// B3 gives all FramePointer values of a function one value, and a frame that was saved goes on at another address. A suspension writes every heap, so nothing moves this read across one.
Pointer JSONParseEmitter::framePointerAtThisPoint()
{
    if (!generator().inputs().frameMaps.numberOfSuspensions() && m_function.role != FunctionRole::CoroutineBody)
        return Pointer(m_out.framePointer());

    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::pointerType());
    patchpoint->effects = B3::Effects::none();
    patchpoint->effects.reads = B3::HeapRange::top();
    patchpoint->setNeedsFrameRecord(true);
    patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        jit.move(GPRInfo::callFrameRegister, params[0].gpr());
    });
    return Pointer(patchpoint);
}

void JSONParseEmitter::generateJSONSourceOpen(const ValueContext& value)
{
    Integer isOpen = callOperation<Integer>(value, RuntimeOperation::JSONSourceOpen, { boxed(value->textOperand()), framePointerAtThisPoint() });
    defineTest(value, isNotZero(isOpen));
}

void JSONParseEmitter::generateJSONSourceClose(const ValueContext& value)
{
    Pointer cursor = cursorOf(value);
    auto closeByOperation = [&] {
        return isNotZero(callOperation<Integer>(value, RuntimeOperation::JSONSourceClose, { cursor }));
    };
    if (value.shape() == CodeShape::Compact) {
        defineTest(value, closeByOperation());
        return;
    }

    LBasicBlock endsInline = newBlock();
    LBasicBlock endsByOperation = newBlock();
    Join<Condition> isComplete(m_out);
    TypedPointer canEndInline = word(AOT_INSTANCE_OFFSET(json.canEndSourceInline));
    Condition isAtEnd = equal(cursor, sourceEnd());
    branch(logicalAnd(isAtEnd, isNotZero(Word32(m_out.load8ZeroExt32(canEndInline)))), { normalEdge(endsInline), unlikelyEdge(endsByOperation) });

    m_out.appendTo(endsInline);
    m_out.store32As8(m_out.constInt32(0), canEndInline);
    LValue noOwner = m_out.constIntPtr(0);
    m_out.storePtr(noOwner, word(AOT_INSTANCE_OFFSET(json.sourceOwnerStackPointer)));
    isComplete.add(boolean(true));

    m_out.appendTo(endsByOperation);
    isComplete.add(closeByOperation());
    defineTest(value, isComplete.finish());
}

void JSONParseEmitter::generateJSONSkipByte(const ValueContext& value)
{
    Pointer cursor = cursorOf(value);
    Condition isAtEnd = equal(cursor, sourceEnd());
    define(value, select(isAtEnd, cursor, offsetBy(cursor, 1)));
}

void JSONParseEmitter::generateJSONFindMember(const ValueContext& value)
{
    Word32 name = word32(static_cast<int32_t>(value->string().index()));
    StubCallResult found = callReturningPair(value, RuntimeOperation::JSONFindMember, { cursorOf(value), name });
    Word32 search = low32(Integer(found.first));
    Condition isFound = equal(search, word32(static_cast<int32_t>(JSONMemberSearch::Found)));
    defineResults(value, { search, cursorOrSourceEnd(isFound, found.second) });
}

void JSONParseEmitter::generateJSONMemberRepeats(const ValueContext& value)
{
    Word32 name = word32(static_cast<int32_t>(value->string().index()));
    Integer mayRepeat = callOperation<Integer>(value, RuntimeOperation::JSONMemberRepeats, { in<Pointer>(value->valueStartOperand()), name });
    defineTest(value, isNotZero(mayRepeat));
}

void JSONParseEmitter::generateJSONKeyHasEscapes(const ValueContext& value)
{
    Pointer cursor = cursorOf(value);
    LBasicBlock continuation = newBlock();
    LBasicBlock nextCharacter = newBlock();
    LBasicBlock isNotClosingQuote = newBlock();
    ValueFromBlock hasNoOpeningQuote = m_out.anchor(m_out.constInt32(1));
    ValueFromBlock openingQuote = m_out.anchor(cursor.value());
    m_out.branch(m_out.equal(m_out.load8ZeroExt32(at(cursor)), '"'), normalEdge(nextCharacter), rareEdge(continuation));

    m_out.appendTo(nextCharacter);
    LValue previous = m_out.phi(B3::Int64, openingQuote);
    LValue position = m_out.add(previous, m_out.constInt64(1));
    LValue byte = m_out.load8ZeroExt32(at(Pointer(position)));
    ValueFromBlock hasOnlyPlainCharacters = m_out.anchor(m_out.constInt32(0));
    m_out.branch(m_out.equal(byte, m_out.constInt32('"')), normalEdge(continuation), normalEdge(isNotClosingQuote));

    m_out.appendTo(isNotClosingQuote);
    ValueFromBlock hasSpecialCharacter = m_out.anchor(m_out.constInt32(1));
    m_out.addIncomingToPhi(previous, m_out.anchor(position));
    LValue isBackslash = m_out.equal(byte, '\\');
    m_out.branch(m_out.bitOr(isBackslash, m_out.below(byte, ' ')), rareEdge(continuation), normalEdge(nextCharacter));

    m_out.appendTo(continuation);
    defineTest(value, Condition(m_out.phi(B3::Int32, hasNoOpeningQuote, hasOnlyPlainCharacters, hasSpecialCharacter)));
}

void JSONParseEmitter::generateJSONHasStackSpace(const ValueContext& value)
{
    ptrdiff_t offsetOfMirror = value->failsAtWatchedLevels() ? AOT_INSTANCE_OFFSET(jsonLevels.limit) : AOT_INSTANCE_OFFSET(stackMirror);
    Integer softStackLimit = loadMutable64(offsetOfMirror + StackManager::Mirror::offsetOfSoftStackLimit());
    Integer lowestAddressNeeded(m_out.sub(framePointerAtThisPoint().value(), stackReservedForJSONParsers));
    defineTest(value, aboveOrEqual(lowestAddressNeeded, softStackLimit));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
