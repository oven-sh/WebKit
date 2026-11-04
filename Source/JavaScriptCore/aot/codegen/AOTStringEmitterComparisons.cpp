/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringEmitter.h"

#if ENABLE(AOT)

#include "AOTRuntimeCallEmitter.h"
#include "AOTStringTable.h"
#include "AOTStubsStrings.h"
#include "AOTVector.h"
#include <wtf/text/ASCIIFastPath.h>

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT { namespace Typed {

static_assert(descriptorOf(StubFamily::StringEqualsLiteralOfLength).maxCount == sizeof(uint64_t));
static_assert(descriptorOf(StubFamily::ValueEqualsLiteralOfLength).maxCount == sizeof(uint64_t));

static bool comparedTextFitsInRegister(const String& text)
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
static Vector<uint8_t, 64> bytesOfTextInWideString(const String& text)
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

Word32 StringEmitter::order(const OperationContext& context, CellPointer a, CellPointer b)
{
    return low32(Integer(callStub(context, compareStrings(), { a.value(), b.value() }).first));
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

void StringEmitter::generateStringOrder(const ValueContext& value)
{
    CellPointer left = in<CellPointer>(value->leftOperand());
    define(value, order(value, left, in<CellPointer>(value->rightOperand())));
}

void StringEmitter::generateStringOrderToCharacter(const ValueContext& value)
{
    define(value, orderToCharacter(viewOf(value, value->stringOperand()), static_cast<char16_t>(value->codeUnit())));
}

bool StringEmitter::comparesLiteralInPlace(const String& text) const
{
    return text.is8Bit() && !text.isEmpty() && text.length() <= maxLengthComparedInPlace;
}

void StringEmitter::branchOnEqualsLiteralByStub(const OperationContext& context, ComparedOperand operand, LValue subject, StringID literal, BranchTargets targets)
{
    String text = textOf(literal);
    if (!comparedTextFitsInRegister(text)) {
        // Only a string that is mentioned has a record in the image for the stub to read.
        branch(Condition(m_out.castToInt32(callStub(context, AOT::equalsLiteral(operand), { subject }, { .numberedImmediate = NumberedImmediate { m_mentions.add(NumberSpace::String, literal) } }).first)), targets);
        return;
    }
    LValue operands[] = { subject, m_out.constInt64(static_cast<int64_t>(packStringIntoRegister(text))) };
    emitter<RuntimeCallEmitter>().callStubAndBranch(context, equalsLiteralOfLength(operand, text.length()), operands, { }, targets);
}

// ProgramIdentifiers::uid(). Image numbers the name: what comes in is the mention of it, in NumberSpace::Name, NameOfText or AtomOfComparedText.
StringEmitter::AtomOfName StringEmitter::atomOfName(uint32_t mentionOfName)
{
    Integer name = zeroExtended(numberOf(mentionOfName));
    if (build().recordedOptions().form() != ImageForm::PositionIndependent) {
        Pointer atoms = loadImmutablePointer(AOT_INSTANCE_OFFSET(image.programIdentifiers));
        return { offsetBy(atoms, mul(name, static_cast<int64_t>(sizeof(UniquedStringImpl))), 0), std::nullopt };
    }
    // The header of the atom is made when the text first becomes an atom. Until then its offset is zero, where no header lies.
    Pointer offsets = loadImmutablePointer(AOT_INSTANCE_OFFSET(image.lazyStringHeaderOffsets));
    Word32 offset(m_out.load32(m_out.baseIndex(m_heaps.AOTInstance_fieldsFilledOnce, offsets.value(), name.value(), ScaleFour)));
    return { offsetBy(loadImmutablePointer(AOT_INSTANCE_OFFSET(lazyStrings.headers)), zeroExtended(offset), 0), isNotZero(offset) };
}

// The type of a cell and its flags lie side by side, and the strings have one Structure: one comparison says that the cell is a string and holds an atom.
// Of the two bytes only the flags change in a string.
static_assert(JSCell::typeInfoFlagsOffset() == JSCell::typeInfoTypeOffset() + 1);
static constexpr uint32_t typeAndFlagsOfAtom = StringType | (TypeInfo(StringType, JSString::StructureFlags).inlineTypeFlags() | TypeInfoPerCellBit) << 8;

static constexpr OpcodeSet comparisonsWithLiteral { Opcode::StringEqualsLiteral, Opcode::ValueEqualsLiteral };

void StringEmitter::makeCellsHoldAtomOf(StringID literal)
{
    // The stub tells a longer text by the header that the image has of it, and an atom of such a text is another object. The cells of the VM are atoms already.
    if (comparesLiteralInPlace(textOf(literal)) && !hasCellOfVM(types().bytesOf(literal)))
        m_mentions.add(NumberSpace::AtomOfString, literal);
}

bool StringEmitter::readsCharactersInLine(const OperationContext& context)
{
    static_assert(formDependsOnLoopOf(Opcode::ValueEqualsLiteral) && formDependsOnLoopOf(Opcode::StringEqualsLiteral));
    static_assert(formDependsOnLoopOf(Opcode::StrictEq) && formDependsOnLoopOf(Opcode::Eq) && formDependsOnLoopOf(TerminatorKind::SwitchString));
    static_assert(formByLoopAlone(Opcode::StringEqualsLiteral, StateOfKnowledge::Always, true).form == formByLoopAlone(Opcode::ValueEqualsLiteral, StateOfKnowledge::Always, true).form);
    static_assert(formByLoopAlone(Opcode::StringEqualsLiteral, StateOfKnowledge::Always, false).form == formByLoopAlone(Opcode::ValueEqualsLiteral, StateOfKnowledge::Always, false).form);
    FormAtPlace named = formByLoopAlone(Opcode::ValueEqualsLiteral, StateOfKnowledge::Always, context.block() && generator().isInLoop(context.block()));
    m_out.noteFormOf(comparisonsWithLiteral, named.form, StateOfKnowledge::Always, named.place);
    return named.form == Form::Open;
}

// A short string out of JSON.parse, a key and a text that the program compares are atoms. The mention makes the text a name of the image, and a name of the image has one atom (ProgramIdentifiers::findAtom()),
// so an atom is that text if and only if it is that pointer. A cell whose bit is clear may still hold an atom, so nothing but the characters says no to it.
// Nothing is looked up under the text here, which the space says: a text that is only compared comes behind every name that is looked up.
CellPointer StringEmitter::branchOnAtomOfLiteral(ComparedOperand operand, LValue subject, StringID literal, BranchTargets targets, bool isString, Machine::EdgeClass frequencyWithoutAtom)
{
    BlockEdge withoutAtom(newBlock(), frequencyWithoutAtom);
    CellPointer string(subject);
    if (operand == ComparedOperand::Value) {
        Boxed boxed(subject);
        continueIf(isCell(boxed), targets.notTaken);
        string = unboxCell(boxed);
    }
    if (isString)
        continueIf(hasBits(loadByte(string, m_heaps.JSCell_typeInfoFlags), TypeInfoPerCellBit), withoutAtom);
    else {
        Word32 typeAndFlags(m_out.load16ZeroExt32(m_out.address(m_heaps.JSCell_typeInfoFlags, string.value(), JSCell::typeInfoTypeOffset())));
        continueIf(equal(typeAndFlags, static_cast<int32_t>(typeAndFlagsOfAtom)), withoutAtom);
    }
    AtomOfName atom = atomOfName(m_mentions.add(NumberSpace::AtomOfComparedText, literal));
    if (atom.exists)
        continueIf(*atom.exists, withoutAtom);
    Condition isAtomOfText = equal(loadField<Pointer>(string, m_heaps.JSString_value), atom.address);
    ensureStillAliveHere(string);
    branch(isAtomOfText, targets);

    m_out.appendTo(withoutAtom.block());
    return string;
}

void StringEmitter::branchOnEqualsLiteral(const OperationContext& context, ComparedOperand operand, LValue subject, StringID literal, BranchTargets targets, std::optional<FlatStringCell> flatSubject, bool everyCellIsString, std::optional<Word32> lengthOfSubject)
{
    RELEASE_ASSERT(!flatSubject || (operand == ComparedOperand::String && flatSubject->cell().value() == subject));
    bool subjectIsFlat = !!flatSubject;
    String text = textOf(literal);
    if (text.isEmpty()) {
        m_out.noteFormOf(comparisonsWithLiteral, Form::Open, StateOfKnowledge::EmptyLiteral);
        branch(equal(Boxed(subject), boxCell(emptyString())), targets);
        return;
    }
    // Nothing but a string is as long as a text.
    if (lengthOfSubject) {
        continueIf(equal(*lengthOfSubject, text.length()), targets.notTaken);
        if (operand == ComparedOperand::Value) {
            subject = unboxCell(Boxed(subject)).value();
            operand = ComparedOperand::String;
        }
    }
    StateOfKnowledge known = subjectIsFlat ? StateOfKnowledge::FlatSubject : lengthOfSubject ? StateOfKnowledge::LengthOfSubject : StateOfKnowledge::Always;
    if (!comparesLiteralInPlace(text)) {
        m_out.noteFormOf(comparisonsWithLiteral, Form::Closed, StateOfKnowledge::LiteralComparedByStub);
        branchOnEqualsLiteralByStub(context, operand, subject, literal, targets);
        return;
    }

    CellPointer string(subject);
    if (known == StateOfKnowledge::Always) {
        if (!readsCharactersInLine(context)) {
            branchOnEqualsLiteralByStub(context, operand, subject, literal, targets);
            return;
        }
        bool isString = operand == ComparedOperand::String || everyCellIsString;
        string = branchOnAtomOfLiteral(operand, subject, literal, targets, isString, Machine::EdgeClass::Normal);
        if (!isString)
            continueIf(hasCellType(string, StringType), targets.notTaken);
    } else {
        // Few subjects are as long as the text, and for those the characters answer in as many steps as the atom.
        m_out.noteFormOf(comparisonsWithLiteral, Form::Open, known);
        makeCellsHoldAtomOf(literal);
    }

    // A rope of the same length and a 16-bit string without a unit above 0xFF may equal the text: the stub answers for them without flattening.
    // What nobody has read yet is read where it is asked for, so that it becomes the memory operand of the compare that uses it.
    LBasicBlock slow = newBlock();
    auto openedString = subjectIsFlat ? m_openedStrings.find(string.value()) : m_openedStrings.end();
    const CharacterSource* source = openedString != m_openedStrings.end() ? &openedString->value.source : nullptr;
    Pointer impl = source ? openedString->value.impl : flatSubject ? implOfFlatString(*flatSubject) : loadField<Pointer>(string, m_heaps.JSString_value);
    if (!subjectIsFlat)
        continueIf(hasNoBits(impl, JSString::isRopeInPointer), unlikelyEdge(slow));
    if (!lengthOfSubject)
        continueIf(equal(source ? source->length : loadField<Word32>(impl, m_heaps.StringImpl_length), text.length()), targets.notTaken);
    continueIf(hasBits(source ? source->implFlags : flagsInImpl(impl), StringImpl::flagIs8Bit()), unlikelyEdge(slow));
    Pointer characters = source ? source->characters : loadField<Pointer>(impl, m_heaps.StringImpl_data);
    branchOnBytesAt(characters, asBytes(text.span8()), m_heaps.characters8.atAnyIndex(), string, targets);

    m_out.appendTo(slow);
    branchOnEqualsLiteralByStub(context, ComparedOperand::String, string.value(), literal, targets);
}

Condition StringEmitter::equalsLiteral(const OperationContext& context, ComparedOperand operand, LValue subject, StringID literal, std::optional<FlatStringCell> flatSubject, bool everyCellIsString, std::optional<Word32> lengthOfSubject)
{
    String text = textOf(literal);
    if (text.isEmpty()) {
        m_out.noteFormOf(comparisonsWithLiteral, Form::Open, StateOfKnowledge::EmptyLiteral);
        return equal(Boxed(subject), boxCell(emptyString()));
    }
    if (lengthOfSubject || comparedTextFitsInRegister(text) || comparesLiteralInPlace(text)) {
        return asBoolean([&](BranchTargets targets) {
            branchOnEqualsLiteral(context, operand, subject, literal, targets, flatSubject, everyCellIsString, lengthOfSubject);
        });
    }
    m_out.noteFormOf(comparisonsWithLiteral, Form::Closed, StateOfKnowledge::LiteralComparedByStub);
    return Condition(m_out.castToInt32(callStub(context, AOT::equalsLiteral(operand), { subject }, { .numberedImmediate = NumberedImmediate { m_mentions.add(NumberSpace::String, literal) } }).first));
}

void StringEmitter::generateEqualsLiteral(const ValueContext& value, ComparedOperand operand, LValue subject, std::optional<FlatStringCell> flatSubject, bool everyCellIsString)
{
    std::optional<Word32> lengthOfSubject;
    if (ValueID length = value->lengthOperand())
        lengthOfSubject = word32Of(length);
    if (value.isBranchCondition()) {
        branchOnEqualsLiteral(value, operand, subject, value->literal(), value.branchTargets(), flatSubject, everyCellIsString, lengthOfSubject);
        return;
    }
    define(value, equalsLiteral(value, operand, subject, value->literal(), flatSubject, everyCellIsString, lengthOfSubject));
}

void StringEmitter::generateStringEqualsLiteral(const ValueContext& value)
{
    ValueID string = value->stringOperand();
    std::optional<FlatStringCell> flat;
    if (ValueID flatString = value->flatOperand())
        flat = flatStringByType(flatString);
    generateEqualsLiteral(value, ComparedOperand::String, (flat ? flat->cell() : in<CellPointer>(string)).value(), flat);
}

void StringEmitter::generateValueEqualsLiteral(const ValueContext& value)
{
    const Type& type = typeOf(value->valueOperand());
    bool everyCellIsString = !type.hasObjects() && !(type.primitives & cellPrimitives & ~Primitive::AnyString);
    generateEqualsLiteral(value, ComparedOperand::Value, in<Boxed>(value->valueOperand()).value(), std::nullopt, everyCellIsString);
}

void StringEmitter::generateValueLooselyEqualsLiteral(const ValueContext& value)
{
    Boxed subject = in<Boxed>(value->valueOperand());
    Integer text = zeroExtended(numberOf(m_mentions.add(NumberSpace::String, value->literal())));
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::CompareEqToString, { subject, text }));
}

void StringEmitter::branchOnBytesAt(Pointer address, std::span<const uint8_t> bytes, const Machine::AbstractHeap& heap, CellPointer owner, BranchTargets targets)
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
        Condition isSame = piece.width == 8 ? equal(Integer(loaded), integer(static_cast<int64_t>(expected))) : equal(Word32(loaded), word32(static_cast<int32_t>(expected)));
        if (!--remainingPieces) {
            ensureStillAliveHere(owner);
            branch(isSame, targets);
            return;
        }
        continueIf(isSame, targets.notTaken);
    }
}

bool StringEmitter::canCompareInline(StringID literal) const
{
    return textOf(literal).length() <= maxLengthComparedInPlace;
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
        m_out.jump(targets.taken);
        return;
    }

    auto compare = [&](bool is8Bit) {
        if (!is8Bit) {
            branchOnBytesAt(offsetBy(view.characters, zeroExtended(offset), 1), bytesOfTextInWideString(text).span(), m_heaps.characters16.atAnyIndex(), view.owner, targets);
            return;
        }
        // A text with a unit above 0xFF equals no 8-bit string.
        if (!text.is8Bit()) {
            m_out.jump(targets.notTaken);
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

void StringEmitter::generateEndsLiteral(const ValueContext& value, LiteralEnd end)
{
    StringID literal = value->literal();
    if (!canCompareInline(literal)) {
        fail(value, InternalErrorKind::PayloadOutOfRange);
        defineTest(value, boolean(false));
        return;
    }

    CharacterView view = viewOf(value, value->stringOperand());
    Word32 length = word32(textOf(literal).length());
    defineTestByBranching(value, [&](BranchTargets targets) {
        continueIf(aboveOrEqual(view.length, length), targets.notTaken);
        branchOnTextAt(view, end == LiteralEnd::Start ? word32(0) : sub(view.length, length), literal, targets);
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
        if (codeUnit.value()->hasInt() && static_cast<uint64_t>(codeUnit.value()->asInt()) > 0xFF)
            return word32(-1);
        // The stub walks a short string itself and hands a long one to the leaf. Like the leaf it reads 32 bits of all but the first operand.
        LValue operands[] = { subject.characters.value(), subject.length.value(), codeUnit.value(), position.value() };
        return low32(Integer(emitter<RuntimeCallEmitter>().callStubWithoutSite(findCharacter8(), operands).first));
    });
    ensureStillAliveHere(subject.owner);
    return found;
}

void StringEmitter::generateFindLiteral(const ValueContext& value, SearchDirection direction)
{
    bool isForward = direction == SearchDirection::Forward;
    ValueID string = value->stringOperand();
    StringID literal = value->literal();
    String text = textOf(literal);
    Word32 position = word32Of(value->positionOperand());
    // Backward from position 0 the text is found at the start or not at all.
    if (!isForward && position.value()->isInt32(0) && canCompareInline(literal)) {
        CharacterView view = viewOf(value, string);
        Word32 length = word32(text.length());
        Condition isAtStart = asBoolean([&](BranchTargets targets) {
            continueIf(aboveOrEqual(view.length, length), targets.notTaken);
            branchOnTextAt(view, word32(0), literal, targets);
        });
        define(value, select<Word32>(isAtStart, 0, -1));
        return;
    }

    // Only a string that is mentioned has a record in the image for the functions to read.
    Word32 numberOfText = numberOf(m_mentions.add(NumberSpace::String, literal));
    auto findInCell = [&] {
        RuntimeOperation operation = isForward ? RuntimeOperation::StringIndexOfLiteral : RuntimeOperation::StringLastIndexOfLiteral;
        return low32(callOperation<Integer>(value, operation, { in<CellPointer>(string), numberOfText, position }));
    };
    if (!text.is8Bit()) {
        define(value, findInCell());
        return;
    }

    CharacterView view = viewOf(value, string);
    if (!isForward && text.length() == 1) {
        // A position in front of the string stands for its start, and the leaf finds nothing from there.
        Word32 codeUnit = word32(text[0]);
        define(value, findCodeUnit(view, codeUnit, select(lessThan(position, 0), 0, position), direction));
        return;
    }

    Word32 found = selectByWidth<Word32>(view.is8Bit, [&](bool is8Bit) {
        if (!is8Bit)
            return findInCell();
        RuntimeOperation operation = isForward ? RuntimeOperation::StringFindLiteral8 : RuntimeOperation::StringReverseFindLiteral8;
        return low32(callWithoutSite<Integer>(operation, { view.characters, view.length, numberOfText, position }));
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
    Condition isRope = isRopeString(cell);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isRope.value(), unlikelyEdge(rope), normalEdge(flat));

    m_out.appendToArm(flat, fork);
    join.add(findCodeUnit(viewOfFlatString(cell), codeUnit, position, SearchDirection::Forward));

    // The operation searches a long rope without flattening it.
    m_out.appendToArm(rope, fork);
    join.add(low32(callOperation<Integer>(value, RuntimeOperation::StringIndexOfCharacter, { cell, codeUnit, position })));
    define(value, join.finishBehind(fork));
}

} } } // namespace JSC::AOT::Typed

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
