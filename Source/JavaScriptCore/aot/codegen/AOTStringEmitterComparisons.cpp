/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringEmitter.h"

#if ENABLE(AOT)

#include "AOTRuntimeCallEmitter.h"
#include "AOTStubsStrings.h"
#include "AOTVector.h"
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

LValue loadBytes(Output& out, unsigned width, TypedPointer pointer)
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

// The text as it lies in a 16-bit string.
static Vector<uint8_t, 64> bytesInWideString(const String& text)
{
    Vector<uint8_t, 64> bytes;
    for (unsigned i = 0; i < text.length(); ++i) {
        char16_t unit = text[i];
        bytes.append(static_cast<uint8_t>(unit));
        bytes.append(static_cast<uint8_t>(unit >> 8));
    }
    return bytes;
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

Condition StringEmitter::areEqual(const OperationContext& context, CellPointer a, CellPointer b, StringIdentity identity)
{
    StubKey stub = identity == StringIdentity::Distinct ? distinctStringsAreEqual() : stringsAreEqual();
    return Condition(m_out.castToInt32(callStub(context, stub, { a.value(), b.value() }).first));
}

Condition StringEmitter::areBoth8Bit(CellPointer a, CellPointer b)
{
    auto flagsOf = [&](CellPointer string) {
        return Word32(m_out.load32(loadField<Pointer>(string, m_heaps.JSString_value).value(), m_heaps.StringImpl_hashAndFlags));
    };
    return isNotZero(bitAnd(bitAnd(flagsOf(a), flagsOf(b)), word32(StringImpl::flagIs8Bit())));
}

Word32 StringEmitter::order(const OperationContext& context, CellPointer a, CellPointer b)
{
    LValue result = callStub(context, compareStrings(), { a.value(), b.value() }).first;
    m_proc.setKnownRange(result, B3::IntRange(-1, 1));
    return Word32(m_out.castToInt32(result));
}

void StringEmitter::generateStringOrder(const ValueContext& value)
{
    CellPointer left = in<CellPointer>(value->leftOperand());
    define(value, order(value, left, in<CellPointer>(value->rightOperand())));
}

Word32 StringEmitter::orderToCharacter(const CharacterView& view, char16_t character)
{
    Word32 codeUnit = word32(static_cast<int32_t>(character));
    return selectByBranch<Word32>(isZero(view.length), [&] {
        return word32(-1);
    }, [&] {
        Word32 first = codeUnitAt(view, word32(0));
        Word32 orderAfterSameUnit = select<Word32>(equal(view.length, 1), 0, 1);
        Condition comesFirst = below(first, codeUnit);
        Word32 before = word32(-1);
        return select(comesFirst, before, select(above(first, codeUnit), 1, orderAfterSameUnit));
    });
}

void StringEmitter::generateStringOrderToCharacter(const ValueContext& value)
{
    define(value, orderToCharacter(viewOf(value, value->stringOperand()), static_cast<char16_t>(value->codeUnit())));
}

bool StringEmitter::comparesLiteralInPlace(const OperationContext& context, const String& text) const
{
    return context.shape() == CodeShape::Fast && text.is8Bit() && !text.isEmpty() && text.length() <= m_generator.build().limit(LimitID::MaxInlineStringCompareLength);
}

void StringEmitter::branchOnEqualsLiteralByStub(const OperationContext& context, ComparedOperand operand, LValue subject, StringID literal, BranchTargets targets)
{
    String text = textOf(literal);
    if (!fitsInRegister(text)) {
        branch(Condition(m_out.castToInt32(callStub(context, AOT::equalsLiteral(operand), { subject }, { .immediate = literal.index() }).first)), targets);
        return;
    }
    LValue operands[] = { subject, m_out.constInt64(static_cast<int64_t>(packStringIntoRegister(text))) };
    emitter<RuntimeCallEmitter>().callStubAndBranch(context, equalsLiteralOfLength(operand, text.length()), operands, { }, targets);
}

void StringEmitter::branchOnEqualsLiteral(const OperationContext& context, ComparedOperand operand, LValue subject, StringID literal, BranchTargets targets, bool subjectIsFlat, bool everyCellIsString)
{
    String text = textOf(literal);
    if (text.isEmpty()) {
        branch(equal(Boxed(subject), boxCell(emptyString())), targets);
        return;
    }
    if (!comparesLiteralInPlace(context, text)) {
        branchOnEqualsLiteralByStub(context, operand, subject, literal, targets);
        return;
    }

    build().statistics().count(StatisticID::StringLiteralsComparedInPlace);
    CellPointer string(subject);
    if (operand == ComparedOperand::Value) {
        Boxed boxed(subject);
        continueIf(isCell(boxed), targets.notTaken);
        string = unboxCell(boxed);
        if (!everyCellIsString)
            continueIf(hasCellType(string, StringType), targets.notTaken);
    }

    // A rope of the same length and a 16-bit string without a unit above 0xFF may equal the text: the stub answers for them without flattening.
    // What nobody has read yet is read where it is asked for, so that it becomes the memory operand of the compare that uses it.
    LBasicBlock slow = newBlock();
    auto openedString = subjectIsFlat ? m_openedStrings.find(string.value()) : m_openedStrings.end();
    const CharacterSource* source = openedString != m_openedStrings.end() ? &openedString->value.source : nullptr;
    Pointer impl = source ? openedString->value.impl : subjectIsFlat ? implOfFlatString(string) : loadField<Pointer>(string, m_heaps.JSString_value);
    if (!subjectIsFlat)
        continueIf(hasNoBits(impl, JSString::isRopeInPointer), unlikelyEdge(slow));
    continueIf(equal(source ? source->length : loadField<Word32>(impl, m_heaps.StringImpl_length), text.length()), targets.notTaken);
    continueIf(hasBits(source ? source->implFlags : flagsInImpl(impl, subjectIsFlat), StringImpl::flagIs8Bit()), unlikelyEdge(slow));
    Pointer characters = source ? source->characters : loadField<Pointer>(impl, m_heaps.StringImpl_data);
    branchOnBytesAt(characters, asBytes(text.span8()), m_heaps.characters8.atAnyIndex(), string, targets);

    m_out.appendTo(slow);
    branchOnEqualsLiteralByStub(context, ComparedOperand::String, string.value(), literal, targets);
}

Condition StringEmitter::equalsLiteral(const OperationContext& context, ComparedOperand operand, LValue subject, StringID literal, bool subjectIsFlat, bool everyCellIsString)
{
    String text = textOf(literal);
    if (text.isEmpty())
        return equal(Boxed(subject), boxCell(emptyString()));
    if (fitsInRegister(text) || comparesLiteralInPlace(context, text)) {
        return asBoolean([&](BranchTargets targets) {
            branchOnEqualsLiteral(context, operand, subject, literal, targets, subjectIsFlat, everyCellIsString);
        });
    }
    return Condition(m_out.castToInt32(callStub(context, AOT::equalsLiteral(operand), { subject }, { .immediate = literal.index() }).first));
}

void StringEmitter::generateEqualsLiteral(const ValueContext& value, ComparedOperand operand, LValue subject, bool subjectIsFlat, bool everyCellIsString)
{
    if (value.isBranchCondition()) {
        branchOnEqualsLiteral(value, operand, subject, value->literal(), value.branchTargets(), subjectIsFlat, everyCellIsString);
        return;
    }
    define(value, equalsLiteral(value, operand, subject, value->literal(), subjectIsFlat, everyCellIsString));
}

void StringEmitter::generateStringEqualsLiteral(const ValueContext& value)
{
    ValueID string = value->stringOperand();
    generateEqualsLiteral(value, ComparedOperand::String, in<CellPointer>(string).value(), typeOf(string).isOpaque(OpaqueKind::FlatString));
}

void StringEmitter::generateValueEqualsLiteral(const ValueContext& value)
{
    const Type& type = typeOf(value->valueOperand());
    bool everyCellIsString = !type.hasObjects() && !(type.primitives & cellPrimitives & ~Primitive::AnyString);
    generateEqualsLiteral(value, ComparedOperand::Value, in<Boxed>(value->valueOperand()).value(), false, everyCellIsString);
}

void StringEmitter::generateValueLooselyEqualsLiteral(const ValueContext& value)
{
    // The stub answers false for every number, boolean and BigInt without converting the text.
    RELEASE_ASSERT(std::isnan(jsToNumber(textOf(value->literal()))));
    Boxed subject = in<Boxed>(value->valueOperand());
    defineTest(value, Condition(m_out.castToInt32(callStub(value, looseEqualsLiteral(), { subject.value() }, { .immediate = value->literal().index() }).first)));
}

bool StringEmitter::canCompareInline(StringID literal) const
{
    return textOf(literal).length() <= m_generator.build().limit(LimitID::MaxInlineStringCompareLength);
}

// Each piece is loaded in the block of the branch on it: B3 makes a load the memory operand of a compare only within one block.
void StringEmitter::branchOnBytesAt(Pointer address, std::span<const uint8_t> bytes, const B3::AbstractHeap& heap, CellPointer owner, BranchTargets targets)
{
    RELEASE_ASSERT(!bytes.empty());
    struct Piece {
        size_t offset;
        unsigned width;
    };
    Vector<Piece, 8> pieces;
    size_t size = bytes.size();
    unsigned width = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
    size_t end = 0;
    for (; end + width <= size; end += width)
        pieces.append(Piece { end, width });
    if (size_t remaining = size - end) {
        unsigned last = remaining > 4 ? 8 : remaining > 2 ? 4 : static_cast<unsigned>(remaining);
        pieces.append(Piece { size - last, last });
    }

    size_t remainingPieces = pieces.size();
    for (Piece piece : pieces) {
        uint64_t expected = 0;
        for (unsigned i = 0; i < piece.width; ++i)
            expected |= static_cast<uint64_t>(bytes[piece.offset + i]) << (8 * i);
        LValue loaded = loadBytes(m_out, piece.width, m_out.address(heap, address.value(), piece.offset));
        loaded->as<B3::MemoryValue>()->setReadsMutability(B3::Mutability::Immutable);
        Condition isSame = piece.width == 8 ? equal(Integer(loaded), integer(static_cast<int64_t>(expected))) : equal(Word32(loaded), word32(static_cast<int32_t>(expected)));
        if (!--remainingPieces) {
            ensureStillAliveHere(owner);
            branch(isSame, targets);
            return;
        }
        continueIf(isSame, targets.notTaken);
    }
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
        if (!is8Bit) {
            branchOnBytesAt(offsetBy(view.characters, zeroExtended(offset), 1), bytesInWideString(text).span(), m_heaps.characters16.atAnyIndex(), view.owner, targets);
            return;
        }
        // A text with a unit above 0xFF equals no 8-bit string.
        if (!text.is8Bit()) {
            m_out.jump(targets.notTaken.block());
            return;
        }
        branchOnBytesAt(offsetBy(view.characters, zeroExtended(offset), 0), asBytes(text.span8()), m_heaps.characters8.atAnyIndex(), view.owner, targets);
    };
    if (view.is8Bit.value()->hasInt()) {
        compare(view.is8Bit.value()->asInt());
        return;
    }

    LBasicBlock narrow = newBlock();
    LBasicBlock wide = newBlock();
    m_out.branch(view.is8Bit.value(), normalEdge(narrow), unlikelyEdge(wide));
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
        continueIf(aboveOrEqual(view.length, length), targets.notTaken);
        branchOnTextAt(view, isAtStart ? word32(0) : sub(view.length, length), literal, targets);
    });
}

Word32 StringEmitter::findCodeUnit(const CharacterView& subject, Word32 codeUnit, Word32 position, SearchDirection direction)
{
    bool isForward = direction == SearchDirection::Forward;
    auto search = [&](RuntimeOperation leaf) {
        return low32(callLeaf<Integer>(leaf, { subject.characters, subject.length, codeUnit, position }));
    };
    Word32 found = selectByWidth<Word32>(subject.is8Bit, [&](bool is8Bit) {
        if (!is8Bit)
            return search(isForward ? RuntimeOperation::StringFindCharacter16 : RuntimeOperation::StringReverseFindCharacter16);
        if (!isForward)
            return search(RuntimeOperation::StringReverseFindCharacter8);
        if (codeUnit.value()->hasInt32() && static_cast<uint32_t>(codeUnit.value()->asInt32()) > 0xFF)
            return word32(-1);
        // The stub walks a short string itself and hands a long one to the leaf. Like the leaf it reads 32 bits of all but the first operand.
        LValue operands[] = { subject.characters.value(), subject.length.value(), codeUnit.value(), position.value() };
        return low32(Integer(emitter<RuntimeCallEmitter>().callStubWithoutSite(findCharacter8(), operands).first));
    });
    ensureStillAliveHere(subject.owner);
    return found;
}

Word32 StringEmitter::find(const CharacterView& subject, const CharacterView& sought, Word32 position, SearchDirection direction)
{
    bool isForward = direction == SearchDirection::Forward;
    Word32 found = selectByWidth<Word32>(subject.is8Bit, [&](bool is8Bit) {
        RuntimeOperation forward = is8Bit ? RuntimeOperation::StringFind8 : RuntimeOperation::StringFind16;
        RuntimeOperation backward = is8Bit ? RuntimeOperation::StringReverseFind8 : RuntimeOperation::StringReverseFind16;
        return low32(callLeaf<Integer>(isForward ? forward : backward, { subject.characters, subject.length, sought.characters, sought.length, position }));
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
    // Backward from position 0 the text is found at the start or not at all.
    if (!isForward && position.value()->hasInt32() && !position.value()->asInt32() && canCompareInline(value->literal())) {
        CharacterView view = viewOf(value, string);
        Word32 length = word32(textOf(value->literal()).length());
        Condition isAtStart = asBoolean([&](BranchTargets targets) {
            continueIf(aboveOrEqual(view.length, length), targets.notTaken);
            branchOnTextAt(view, word32(0), value->literal(), targets);
        });
        define(value, select<Word32>(isAtStart, 0, -1));
        return;
    }
    auto findInCell = [&] {
        RuntimeOperation operation = isForward ? RuntimeOperation::StringIndexOfLiteral : RuntimeOperation::StringLastIndexOfLiteral;
        return low32(callOperation<Integer>(value, operation, { in<CellPointer>(string), literal, position }));
    };
    if (!textOf(value->literal()).is8Bit()) {
        define(value, findInCell());
        return;
    }

    CharacterView view = viewOf(value, string);
    if (String text = textOf(value->literal()); !isForward && text.length() == 1) {
        // A position in front of the string stands for its start, and the leaf finds nothing from there.
        Word32 codeUnit = word32(text[0]);
        define(value, findCodeUnit(view, codeUnit, select(lessThan(position, 0), 0, position), direction));
        return;
    }

    Word32 found = selectByWidth<Word32>(view.is8Bit, [&](bool is8Bit) {
        if (!is8Bit)
            return findInCell();
        RuntimeOperation operation = isForward ? RuntimeOperation::StringFindLiteral8 : RuntimeOperation::StringReverseFindLiteral8;
        return low32(callWithoutSite<Integer>(operation, { view.characters, view.length, literal, position }));
    });
    ensureStillAliveHere(view.owner);
    define(value, found);
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
