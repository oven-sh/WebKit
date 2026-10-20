/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringEmitter.h"

#if ENABLE(AOT)

#include "AOTRuntimeCallEmitter.h"
#include "AOTStubsStrings.h"
#include "B3IntRange.h"
#include "B3MemoryValue.h"
#include "JSGlobalObjectFunctions.h"
#include <cmath>
#include <wtf/text/ASCIIFastPath.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(descriptorOf(StubFamily::StringEqualsLiteralOfLength).maxCount == sizeof(uint64_t));
static_assert(descriptorOf(StubFamily::ValueEqualsLiteralOfLength).maxCount == sizeof(uint64_t));

static bool fitsInRegister(const String& text)
{
    const StubDescriptor& descriptor = descriptorOf(StubFamily::StringEqualsLiteralOfLength);
    return text.is8Bit() && text.length() >= descriptor.minCount && text.length() <= descriptor.maxCount;
}

static LValue loadBytes(Output& out, TypedPointer pointer, unsigned width)
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

uint64_t packStringIntoRegister(StringView text)
{
    RELEASE_ASSERT(text.length() <= sizeof(uint64_t));
    RELEASE_ASSERT(text.is8Bit() || WTF::charactersAreAllLatin1(text.span16()));
    uint64_t packed = 0;
    // The stub reads seven units as two words of four that overlap in the fourth unit.
    if (text.length() == 7) {
        for (unsigned i = 0; i < 4; ++i)
            packed |= static_cast<uint64_t>(text[i]) << (8 * i) | static_cast<uint64_t>(text[3 + i]) << (32 + 8 * i);
        return packed;
    }
    for (unsigned i = 0; i < text.length(); ++i)
        packed |= static_cast<uint64_t>(text[i]) << (8 * i);
    return packed;
}

Condition StringEmitter::areEqual(const OperationContext& context, CellPointer a, CellPointer b)
{
    return Condition(m_out.castToInt32(callStub(context, stringsAreEqual(), { a.value(), b.value() }).first));
}

Word32 StringEmitter::order(const OperationContext& context, CellPointer a, CellPointer b)
{
    LValue result = callStub(context, compareStrings(), { a.value(), b.value() }).first;
    m_proc.setKnownRange(result, B3::IntRange(-1, 1));
    return Word32(m_out.castToInt32(result));
}

void StringEmitter::generateStringOrder(const ValueContext& value)
{
    define(value, order(value, in<CellPointer>(value->leftOperand()), in<CellPointer>(value->rightOperand())));
}

void StringEmitter::generateStringOrderToCharacter(const ValueContext& value)
{
    CharacterView view = viewOf(value, value->stringOperand());
    Word32 codeUnit = word32(static_cast<int32_t>(value->codeUnit()));
    define(value, selectByBranch<Word32>(isZero(view.length), [&] {
        return word32(-1);
    }, [&] {
        Word32 first = codeUnitAt(view, word32(0));
        Word32 orderAfterSameUnit = select(equal(view.length, word32(1)), word32(0), word32(1));
        return select(below(first, codeUnit), word32(-1), select(above(first, codeUnit), word32(1), orderAfterSameUnit));
    }));
}

void StringEmitter::branchOnEqualsLiteral(const OperationContext& context, ComparedOperand operand, LValue subject, StringID literal, BranchTargets targets)
{
    String text = textOf(literal);
    if (!fitsInRegister(text)) {
        branch(equalsLiteral(context, operand, subject, literal), targets);
        return;
    }
    LValue operands[] = { subject, m_out.constInt64(static_cast<int64_t>(packStringIntoRegister(text))) };
    emitter<RuntimeCallEmitter>().callStubAndBranch(context, equalsLiteralOfLength(operand, text.length()), operands, { }, targets);
}

Condition StringEmitter::equalsLiteral(const OperationContext& context, ComparedOperand operand, LValue subject, StringID literal)
{
    String text = textOf(literal);
    if (text.isEmpty() && operand == ComparedOperand::String)
        return isZero(lengthOf(CellPointer(subject)));
    if (fitsInRegister(text)) {
        return asBoolean([&](BranchTargets targets) {
            branchOnEqualsLiteral(context, operand, subject, literal, targets);
        });
    }
    return Condition(m_out.castToInt32(callStub(context, AOT::equalsLiteral(operand), { subject }, { .immediate = literal.index() }).first));
}

Condition StringEmitter::looselyEqualsLiteral(const OperationContext& context, Boxed value, StringID literal)
{
    // The stub answers false for every number, boolean and BigInt without converting the text.
    RELEASE_ASSERT(std::isnan(jsToNumber(textOf(literal))));
    return Condition(m_out.castToInt32(callStub(context, looseEqualsLiteral(), { value.value() }, { .immediate = literal.index() }).first));
}

void StringEmitter::generateEqualsLiteral(const ValueContext& value, ComparedOperand operand, LValue subject)
{
    if (value.isBranchCondition()) {
        branchOnEqualsLiteral(value, operand, subject, value->literal(), value.branchTargets());
        return;
    }
    define(value, equalsLiteral(value, operand, subject, value->literal()));
}

void StringEmitter::generateStringEqualsLiteral(const ValueContext& value)
{
    generateEqualsLiteral(value, ComparedOperand::String, in<CellPointer>(value->stringOperand()).value());
}

void StringEmitter::generateValueEqualsLiteral(const ValueContext& value)
{
    generateEqualsLiteral(value, ComparedOperand::Value, in<Boxed>(value->valueOperand()).value());
}

void StringEmitter::generateValueLooselyEqualsLiteral(const ValueContext& value)
{
    defineTest(value, looselyEqualsLiteral(value, in<Boxed>(value->valueOperand()), value->literal()));
}

bool StringEmitter::canCompareInline(StringID literal) const
{
    return textOf(literal).length() <= m_generator.build().limit(LimitID::MaxInlineStringCompareLength);
}

Condition StringEmitter::hasBytesAt(Pointer address, std::span<const uint8_t> bytes, const B3::AbstractHeap& heap)
{
    RELEASE_ASSERT(!bytes.empty());
    Integer wideDifference;
    Word32 narrowDifference;
    auto compare = [&](size_t offset, unsigned width) {
        uint64_t expected = 0;
        for (unsigned i = 0; i < width; ++i)
            expected |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
        LValue loaded = loadBytes(m_out, m_out.address(heap, address.value(), offset), width);
        loaded->as<B3::MemoryValue>()->setReadsMutability(B3::Mutability::Immutable);
        if (width == 8) {
            Integer difference = bitXor(Integer(loaded), integer(static_cast<int64_t>(expected)));
            wideDifference = wideDifference ? bitOr(wideDifference, difference) : difference;
            return;
        }
        Word32 difference = bitXor(Word32(loaded), word32(static_cast<int32_t>(expected)));
        narrowDifference = narrowDifference ? bitOr(narrowDifference, difference) : difference;
    };

    size_t size = bytes.size();
    unsigned width = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
    size_t offset = 0;
    for (; offset + width <= size; offset += width)
        compare(offset, width);
    if (size_t remaining = size - offset) {
        unsigned last = remaining > 4 ? 8 : remaining > 2 ? 4 : static_cast<unsigned>(remaining);
        compare(size - last, last);
    }

    if (!wideDifference)
        return isZero(narrowDifference);
    if (narrowDifference)
        wideDifference = bitOr(wideDifference, zeroExtended(narrowDifference));
    return isZero(wideDifference);
}

Condition StringEmitter::has8BitTextAt(Pointer characters, Word32 offset, StringView text)
{
    if (!text.is8Bit())
        return boolean(false);
    return hasBytesAt(offsetBy(characters, zeroExtended(offset), 0), asBytes(text.span8()), m_heaps.characters8.atAnyIndex());
}

Condition StringEmitter::has16BitTextAt(Pointer characters, Word32 offset, StringView text)
{
    Vector<uint8_t, 64> bytes;
    for (unsigned i = 0; i < text.length(); ++i) {
        char16_t unit = text[i];
        bytes.append(static_cast<uint8_t>(unit));
        bytes.append(static_cast<uint8_t>(unit >> 8));
    }
    return hasBytesAt(offsetBy(characters, zeroExtended(offset), 1), bytes.span(), m_heaps.characters16.atAnyIndex());
}

Condition StringEmitter::hasTextAt(const CharacterView& view, Word32 offset, StringID literal)
{
    return asBoolean([&](BranchTargets targets) {
        branchOnTextAt(view, offset, literal, targets);
    });
}

void StringEmitter::branchOnTextAt(const CharacterView& view, Word32 offset, StringID literal, BranchTargets targets)
{
    RELEASE_ASSERT(canCompareInline(literal));
    String text = textOf(literal);
    if (text.isEmpty()) {
        m_out.jump(targets.taken.block());
        return;
    }

    auto compare = [&](bool is8Bit) {
        Condition hasText = is8Bit ? has8BitTextAt(view.characters, offset, text) : has16BitTextAt(view.characters, offset, text);
        ensureStillAliveHere(view.owner);
        branch(hasText, targets);
    };
    if (view.is8Bit.value()->hasInt()) {
        compare(view.is8Bit.value()->asInt());
        return;
    }

    LBasicBlock narrow = newBlock();
    LBasicBlock wide = newBlock();
    m_out.branch(view.is8Bit.value(), normalEdge(narrow), normalEdge(wide));
    m_out.appendTo(narrow);
    compare(true);
    m_out.appendTo(wide);
    compare(false);
}

void StringEmitter::generateEndsLiteral(const ValueContext& value, LiteralEnd end)
{
    bool isAtStart = end == LiteralEnd::Start;
    ValueID string = value->stringOperand();
    StringID literal = value->literal();
    if (!canCompareInline(literal)) {
        RuntimeOperation operation = isAtStart ? RuntimeOperation::StringStartsWithLiteral : RuntimeOperation::StringEndsWithLiteral;
        defineTest(value, callOperation<Condition>(value, operation, { in<CellPointer>(string), word32(literal.index()) }));
        return;
    }

    CharacterView view = viewOf(value, string);
    Word32 length = word32(textOf(literal).length());
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock isLongEnough = newBlock();
        m_out.branch(aboveOrEqual(view.length, length).value(), normalEdge(isLongEnough), targets.notTaken);
        m_out.appendTo(isLongEnough);
        branchOnTextAt(view, isAtStart ? word32(0) : sub(view.length, length), literal, targets);
    });
}

void StringEmitter::generateStringStartsWithLiteral(const ValueContext& value)
{
    generateEndsLiteral(value, LiteralEnd::Start);
}

void StringEmitter::generateStringEndsWithLiteral(const ValueContext& value)
{
    generateEndsLiteral(value, LiteralEnd::End);
}

Word32 StringEmitter::findCodeUnit(const CharacterView& subject, Word32 codeUnit, Word32 position, SearchDirection direction)
{
    bool isForward = direction == SearchDirection::Forward;
    auto search = [&](RuntimeOperation leaf) {
        return low32(callLeaf<Integer>(leaf, { subject.characters, subject.length, codeUnit, position }));
    };
    Word32 found = selectByBranch<Word32>(subject.is8Bit, [&] {
        return search(isForward ? RuntimeOperation::StringFindCharacter8 : RuntimeOperation::StringReverseFindCharacter8);
    }, [&] {
        return search(isForward ? RuntimeOperation::StringFindCharacter16 : RuntimeOperation::StringReverseFindCharacter16);
    });
    ensureStillAliveHere(subject.owner);
    return found;
}

Word32 StringEmitter::find(const CharacterView& subject, const CharacterView& sought, Word32 position, SearchDirection direction)
{
    bool isForward = direction == SearchDirection::Forward;
    auto search = [&](RuntimeOperation leaf) {
        return low32(callLeaf<Integer>(leaf, { subject.characters, subject.length, sought.characters, sought.length, position }));
    };
    Word32 found = selectByBranch<Word32>(subject.is8Bit, [&] {
        return search(isForward ? RuntimeOperation::StringFind8 : RuntimeOperation::StringReverseFind8);
    }, [&] {
        return search(isForward ? RuntimeOperation::StringFind16 : RuntimeOperation::StringReverseFind16);
    });
    ensureStillAliveHere(subject.owner);
    ensureStillAliveHere(sought.owner);
    return found;
}

void StringEmitter::generateFindLiteral(const ValueContext& value, SearchDirection direction)
{
    bool isForward = direction == SearchDirection::Forward;
    ValueID string = value->stringOperand();
    Word32 literal = word32(value->literal().index());
    Word32 position = word32Of(value->positionOperand());
    auto findInCell = [&] {
        RuntimeOperation operation = isForward ? RuntimeOperation::StringIndexOfLiteral : RuntimeOperation::StringLastIndexOfLiteral;
        return low32(callOperation<Integer>(value, operation, { in<CellPointer>(string), literal, position }));
    };
    if (!typeOf(string).isOpaque(OpaqueKind::FlatString) || !textOf(value->literal()).is8Bit()) {
        define(value, findInCell());
        return;
    }

    CharacterView view = viewOf(value, string);
    if (String text = textOf(value->literal()); !isForward && text.length() == 1) {
        // A position in front of the string stands for its start, and the leaf finds nothing from there.
        define(value, findCodeUnit(view, word32(text[0]), select(lessThan(position, word32(0)), word32(0), position), direction));
        return;
    }

    Word32 found = selectByBranch<Word32>(view.is8Bit, [&] {
        RuntimeOperation operation = isForward ? RuntimeOperation::StringFindLiteral8 : RuntimeOperation::StringReverseFindLiteral8;
        return low32(callWithoutSite<Integer>(operation, { view.characters, view.length, literal, position }));
    }, findInCell);
    ensureStillAliveHere(view.owner);
    define(value, found);
}

void StringEmitter::generateStringIndexOfLiteral(const ValueContext& value)
{
    generateFindLiteral(value, SearchDirection::Forward);
}

void StringEmitter::generateStringLastIndexOfLiteral(const ValueContext& value)
{
    generateFindLiteral(value, SearchDirection::Backward);
}

void StringEmitter::generateStringIndexOfCharacter(const ValueContext& value)
{
    ValueID string = value->stringOperand();
    Word32 codeUnit = word32(static_cast<int32_t>(value->codeUnit()));
    Word32 position = word32Of(value->positionOperand());
    if (typeOf(string).isOpaque(OpaqueKind::FlatString)) {
        define(value, findCodeUnit(viewOf(value, string), codeUnit, position, SearchDirection::Forward));
        return;
    }

    CellPointer cell = in<CellPointer>(string);
    LBasicBlock flat = newBlock();
    LBasicBlock rope = newBlock();
    Join<Word32> join(m_out);
    m_out.branch(isRopeString(cell).value(), unlikelyEdge(rope), normalEdge(flat));

    m_out.appendTo(flat);
    join.add(findCodeUnit(viewOfFlatString(cell), codeUnit, position, SearchDirection::Forward));

    // The operation searches a long rope without flattening it.
    m_out.appendTo(rope);
    join.add(low32(callOperation<Integer>(value, RuntimeOperation::StringIndexOfCharacter, { cell, codeUnit, position })));
    define(value, join.finish());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
