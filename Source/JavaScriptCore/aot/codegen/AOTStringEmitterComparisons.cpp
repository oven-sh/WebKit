/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTControlEmitter.h"
#include "AOTImageText.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringTable.h"
#include "AOTStubsStrings.h"
#include "AOTVector.h"
#include "Identifier.h"
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

static bool comparedByCharacters(const String& text)
{
    const StubDescriptor& descriptor = descriptorOf(StubFamily::StringEqualsCharacters);
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

void StringEmitter::branchOnEqualityOfDistinctStrings(const OperationContext& context, CellPointer a, CellPointer b, BranchTargets targets)
{
    LBasicBlock areFlat = newBlock();
    LBasicBlock haveDifferentImpls = newBlock();
    LBasicBlock haveSameLength = newBlock();
    LBasicBlock comparesCharacters = newBlock();
    Pointer first = loadField<Pointer>(a, m_heaps.JSString_value);
    Pointer second = loadField<Pointer>(b, m_heaps.JSString_value);
    Condition bothAreFlat = hasNoBits(Pointer(m_out.bitOr(first.value(), second.value())), JSString::isRopeInPointer);
    m_out.branch(bothAreFlat.value(), normalEdge(areFlat), unlikelyEdge(comparesCharacters));
    m_out.appendTo(areFlat);
    m_out.branch(equal(first, second).value(), targets.taken, normalEdge(haveDifferentImpls));
    m_out.appendTo(haveDifferentImpls);
    m_out.branch(equal(loadField<Word32>(first, m_heaps.StringImpl_length), loadField<Word32>(second, m_heaps.StringImpl_length)).value(), normalEdge(haveSameLength), targets.notTaken);
    m_out.appendTo(haveSameLength);
    Word32 sharedFlags(m_out.bitAnd(flagsInImpl(first).value(), flagsInImpl(second).value()));
    m_out.branch(hasNoBits(sharedFlags, flagsOfUniqueTexts).value(), normalEdge(comparesCharacters), targets.notTaken);
    m_out.appendTo(comparesCharacters);
    branch(areEqual(context, a, b, StringIdentity::Distinct), targets);
}

Word32 StringEmitter::order(const OperationContext& context, CellPointer a, CellPointer b)
{
    return low32(Integer(callStub(context, compareStrings(), { a.value(), b.value() }).first));
}

static constexpr int64_t noWordOfText = std::numeric_limits<int64_t>::min();
static constexpr int64_t firstBitsOfBytes = 0x0101010101010101;
static constexpr int64_t lastBitsOfBytes = static_cast<int64_t>(0x8080808080808080);

StringEmitter::ShortText StringEmitter::shortTextOf(CellPointer string, BlockEdge otherwise)
{
    constexpr int32_t wordSize = sizeof(uint64_t);
    constexpr ptrdiff_t offsetOfText = StringImpl::headerSize<Latin1Character>();
    static_assert(offsetOfText >= wordSize);
    Pointer impl = loadField<Pointer>(string, m_heaps.JSString_value);
    continueIf(hasNoBits(impl, JSString::isRopeInPointer), otherwise);
    continueIf(hasBits(loadField<Word32>(impl, m_heaps.StringImpl_width), StringImpl::flagIs8Bit()), otherwise);
    continueIf(equal(loadField<Pointer>(impl, m_heaps.StringImpl_data), offsetBy(impl, offsetOfText)), otherwise);
    Word32 length = loadField<Word32>(impl, m_heaps.StringImpl_length);
    continueIf(below(sub(length, word32(1)), word32(wordSize)), otherwise);
    LValue upToEnd = m_out.load64(m_out.baseIndex(m_heaps.wordOfText, impl.value(), zeroExtended(length).value(), ScaleOne, offsetOfText - wordSize));
    LValue bitsInFront = m_out.neg(m_out.shl(length.value(), 3));
    return { Integer(m_out.lShr(upToEnd, bitsInFront)), length };
}

void StringEmitter::generateWordOfText(const ValueContext& value)
{
    LBasicBlock hasNone = newBlock();
    Join<Pointer> join(m_out);
    ShortText text = shortTextOf(in<CellPointer>(value->stringOperand()), unlikelyEdge(hasNone));
    continueIf(hasNoBits(Pointer(text.word.value()), static_cast<uintptr_t>(lastBitsOfBytes)), unlikelyEdge(hasNone));
    LValue zeroBytes = m_out.bitAnd(m_out.sub(text.word.value(), m_out.constInt64(firstBitsOfBytes)), m_out.constInt64(lastBitsOfBytes));
    LValue zeroCharacters = m_out.shl(zeroBytes, m_out.neg(m_out.shl(text.length.value(), 3)));
    continueIf(isZero(Integer(zeroCharacters)), unlikelyEdge(hasNone));
    join.add(Pointer(text.word.value()));

    m_out.appendTo(hasNone);
    join.add(Pointer(m_out.constInt64(noWordOfText)));
    define(value, join.finish());
}

void StringEmitter::generateReverseWordOfText(const ValueContext& value)
{
    define(value, Pointer(reverseBytes(in<Pointer>(value->wordOperand()).value())));
}

void StringEmitter::generateHasTextInCell(const ValueContext& value)
{
    Word32 flags(loadByte(in<CellPointer>(value->stringOperand()), m_heaps.JSString_flags));
    Word32 flag(m_out.bitAnd(flags.value(), m_out.constInt32(TextInCell::flag)));
    Condition hasWord(m_out.greaterThanOrEqual(in<Pointer>(value->wordOperand()).value(), m_out.constInt64(0)));
    define(value, select(hasWord, flag, 0));
}

void StringEmitter::generateLocaleCompareToWord(const ValueContext& value)
{
    build().statistics().count(StatisticID::LocaleComparesToWords);
    CellPointer string = in<CellPointer>(value->stringOperand());
    CellPointer other = in<CellPointer>(value->otherOperand());
    LValue word = in<Pointer>(value->wordOperand()).value();
    LValue reversedWord = in<Pointer>(value->reversedWordOperand()).value();
    LBasicBlock calls = newBlock();
    LBasicBlock isEqual = newBlock();
    LBasicBlock differs = newBlock();
    LBasicBlock mayComeFirst = newBlock();
    LBasicBlock mayComeLast = newBlock();
    LBasicBlock comesFirst = newBlock();
    LBasicBlock comesLast = newBlock();
    LBasicBlock hasNoWeight = newBlock();
    LBasicBlock otherHasNoWeight = newBlock();
    LBasicBlock otherHasZeroByte = newBlock();
    LBasicBlock isBefore = newBlock();
    LBasicBlock isBehind = newBlock();
    Join<Word32> join(m_out);

    LBasicBlock isInCell = newBlock();
    LBasicBlock isElsewhere = newBlock();
    LBasicBlock differsInCell = newBlock();
    Word32 flags(loadByte(other, m_heaps.JSString_flags));
    m_out.branch(m_out.testNonZero32(flags.value(), in<Word32>(value->flagOperand()).value()), normalEdge(isInCell), normalEdge(isElsewhere));
    m_out.appendTo(isInCell);
    LValue textInCell = m_out.load64(m_out.address(m_heaps.wordOfText, other.value(), JSRopeString::offsetOfFiber2()));
    m_out.branch(m_out.equal(reversedWord, textInCell), unlikelyEdge(isEqual), normalEdge(differsInCell));
    m_out.appendTo(differsInCell);
    m_out.branch(m_out.above(textInCell, reversedWord), normalEdge(isBefore), normalEdge(isBehind));

    m_out.appendTo(isElsewhere);
    continueIf(Condition(m_out.greaterThanOrEqual(word, m_out.constInt64(0))), unlikelyEdge(calls));
    ShortText text = shortTextOf(other, unlikelyEdge(calls));
    LValue reversedText = reverseBytes(text.word.value());
    m_out.branch(m_out.equal(reversedWord, reversedText), unlikelyEdge(isEqual), normalEdge(differs));
    m_out.appendTo(differs);
    m_out.branch(m_out.above(reversedText, reversedWord), normalEdge(mayComeFirst), normalEdge(mayComeLast));

    struct FirstDifference {
        LValue firstBit;
        Word32 byte;
        Word32 otherByte;
        Word32 weight;
        Word32 otherWeight;
    };
    auto readFirstDifference = [&] {
        LValue firstBit = m_out.castToInt32(m_out.bitAnd(countTrailingZerosOfNonZero(m_out.bitXor(word, text.word.value())), m_out.constInt64(0x38)));
        auto byteOf = [&](LValue bytes) { return m_out.bitAnd(m_out.castToInt32(m_out.lShr(bytes, firstBit)), m_out.constInt32(0xff)); };
        LValue byte = byteOf(word);
        LValue otherByte = byteOf(text.word.value());
        LValue weights = m_out.loadPtr(mutableInstanceField(AOT_INSTANCE_OFFSET(collation.knownLevel1Weights)));
        auto weightOf = [&](LValue byte) { return Word32(m_out.load8ZeroExt32(m_out.baseIndex(m_heaps.AOTInstance_fixedFields, weights, m_out.zeroExtPtr(byte), ScaleOne))); };
        return FirstDifference { firstBit, Word32(byte), Word32(otherByte), weightOf(byte), weightOf(otherByte) };
    };

    m_out.appendTo(mayComeFirst);
    FirstDifference first = readFirstDifference();
    m_out.branch(below(first.weight, first.otherWeight).value(), normalEdge(comesFirst), unlikelyEdge(calls));
    m_out.appendTo(comesFirst);
    m_out.branch(isNotZero(first.weight).value(), normalEdge(isBefore), unlikelyEdge(hasNoWeight));
    m_out.appendTo(hasNoWeight);
    m_out.branch(isZero(first.byte).value(), normalEdge(isBefore), unlikelyEdge(calls));

    m_out.appendTo(mayComeLast);
    FirstDifference last = readFirstDifference();
    m_out.branch(above(last.weight, last.otherWeight).value(), normalEdge(comesLast), unlikelyEdge(calls));
    m_out.appendTo(comesLast);
    m_out.branch(isNotZero(last.otherWeight).value(), normalEdge(isBehind), unlikelyEdge(otherHasNoWeight));
    m_out.appendTo(otherHasNoWeight);
    m_out.branch(isZero(last.otherByte).value(), normalEdge(otherHasZeroByte), unlikelyEdge(calls));
    m_out.appendTo(otherHasZeroByte);
    m_out.branch(aboveOrEqual(Word32(last.firstBit), Word32(m_out.shl(text.length.value(), 3))).value(), normalEdge(isBehind), unlikelyEdge(calls));

    m_out.appendTo(isBefore);
    join.add(word32(-1));
    m_out.appendTo(isBehind);
    join.add(word32(1));
    m_out.appendTo(isEqual);
    join.add(word32(0));

    m_out.appendTo(calls);
    join.add(low32(callOperation<Integer>(value, RuntimeOperation::StringLocaleCompare, { string, other })));
    define(value, join.finish());
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
    auto codeUnit = static_cast<char16_t>(value->codeUnit());
    recordInlined(codeUnit);
    define(value, orderToCharacter(viewOf(value, value->stringOperand()), codeUnit));
}

bool StringEmitter::comparesLiteralInPlace(const String& text) const
{
    return text.is8Bit() && !text.isEmpty() && text.length() <= maxLengthComparedInPlace;
}

void StringEmitter::branchOnEqualsLiteralByStub(const OperationContext& context, ComparedOperand operand, LValue subject, StringID literal, BranchTargets targets)
{
    branchOnEqualsLiteralByStub(context, operand, subject, literal, textOf(literal), targets);
}

void StringEmitter::branchOnEqualsLiteralByStub(const OperationContext& context, ComparedOperand operand, LValue subject, StringID literal, const String& text, BranchTargets targets)
{
    if (comparedByCharacters(text)) {
        LValue operands[] = { subject };
        emitter<RuntimeCallEmitter>().callStubAndBranch(context, equalsCharacters(operand, text.length()), operands, { .numberedImmediate = NumberedImmediate { m_mentions.add(NumberSpace::StringCharacters, literal) } }, targets);
        return;
    }
    if (!comparedTextFitsInRegister(text)) {
        branch(Condition(m_out.castToInt32(callStub(context, AOT::equalsLiteral(operand), { subject }, { .numberedImmediate = NumberedImmediate { m_mentions.add(NumberSpace::String, literal) } }).first)), targets);
        return;
    }
    recordInlined(literal);
    LValue operands[] = { subject, m_out.constInt64(static_cast<int64_t>(packStringIntoRegister(text))) };
    emitter<RuntimeCallEmitter>().callStubAndBranch(context, equalsLiteralOfLength(operand, text.length()), operands, { }, targets);
}

static constexpr OpcodeSet comparisonsWithLiteral { Opcode::StringEqualsLiteral, Opcode::ValueEqualsLiteral };
static constexpr unsigned maxCharactersNextToName = 4;

void StringEmitter::makeCellsHoldAtomOf(StringID literal)
{
    makeCellsHoldAtomOf(literal, textOf(literal));
}

void StringEmitter::makeCellsHoldAtomOf(StringID literal, const String& text)
{
    if (comparesLiteralInPlace(text) && !hasCellOfVM(types().bytesOf(literal)))
        m_mentions.add(NumberSpace::AtomOfString, literal);
}

bool StringEmitter::mayBeNameInLoop(const OperationContext& context, const String& text)
{
    return context.block() && (generator().usesLoopForms(context.block()) || generator().isInRecursion(context.block())) && !text.isEmpty() && !parseIndex(*text.impl());
}

bool StringEmitter::expectsName(const OperationContext& context, const String& text)
{
    if (!context.valueID() || !mayBeNameInLoop(context, text))
        return false;
    const Operation& comparison = generator().code().at(context.valueID());
    return comparison.opcode == Opcode::StrictEq && comparison.expectsLiteral();
}

Condition StringEmitter::holdsHeaderOf(CellPointer string, StringID literal)
{
    build().statistics().count(StatisticID::LiteralHeadersCompared);
    Integer header(m_out.wideNumber(m_mentions.add(NumberSpace::NameHeader, literal)));
    bool isMadeAtRunTime = m_options.recorded.form() == ImageForm::PositionIndependent;
    Pointer headers = loadImmutablePointer(isMadeAtRunTime ? AOT_INSTANCE_OFFSET(lazyStrings.headers) : AOT_INSTANCE_OFFSET(image.programIdentifiers));
    Integer distance(m_out.sub(m_out.load64(string.value(), m_heaps.JSString_word), headers.value()));
    if (isMadeAtRunTime) {
        Pointer offsets = loadImmutablePointer(AOT_INSTANCE_OFFSET(image.lazyStringHeaderOffsets));
        m_out.forgetLoadsFrom(m_heaps.AOTImage_offsetsOfHeaders);
        header = zeroExtended(Word32(m_out.load32(m_out.address(m_heaps.AOTImage_offsetsOfHeaders, m_out.add(offsets.value(), header.value())))));
    }
    return equal(distance, header);
}

void StringEmitter::branchOnHeaderOf(CellPointer string, StringID literal, BlockEdge isEqual)
{
    LBasicBlock differs = newBlock();
    m_out.branch(holdsHeaderOf(string, literal).value(), isEqual, normalEdge(differs));
    m_out.appendToContinuation(differs);
}

Condition StringEmitter::holdsName(CellPointer string)
{
    return hasBits(loadByte(string, m_heaps.JSCell_typeInfoFlags), ConstantCellWords::flagsOfNamedText);
}

void StringEmitter::continueUnlessHoldsName(CellPointer string, StringID literal, BranchTargets targets, bool isString)
{
    LBasicBlock named = newBlock();
    LBasicBlock unnamed = newBlock();
    ScopeFork fork = m_out.forkScope();
    branch(holdsName(string), { normalEdge(named), normalEdge(unnamed) });
    m_out.appendToArm(named, fork);
    if (isString)
        branch(holdsHeaderOf(string, literal), targets);
    else {
        continueIf(holdsHeaderOf(string, literal), targets.notTaken);
        branch(hasCellType(string, StringType), targets);
    }
    m_out.appendToArm(unnamed, fork);
}

bool StringEmitter::readsCharactersInLine(const OperationContext& context)
{
    static_assert(formDependsOnLoopOf(Opcode::ValueEqualsLiteral) && formDependsOnLoopOf(Opcode::StringEqualsLiteral));
    static_assert(formDependsOnLoopOf(Opcode::StrictEq) && formDependsOnLoopOf(Opcode::Eq) && formDependsOnLoopOf(TerminatorKind::SwitchString));
    static_assert(formByLoopAlone(Opcode::StringEqualsLiteral, StateOfKnowledge::Always, true).form == formByLoopAlone(Opcode::ValueEqualsLiteral, StateOfKnowledge::Always, true).form);
    static_assert(formByLoopAlone(Opcode::StringEqualsLiteral, StateOfKnowledge::Always, false).form == formByLoopAlone(Opcode::ValueEqualsLiteral, StateOfKnowledge::Always, false).form);
    static constexpr FormAtPlace namedInLoops = formByLoopAlone(Opcode::ValueEqualsLiteral, StateOfKnowledge::Always, true);
    static constexpr FormAtPlace namedOutsideLoops = formByLoopAlone(Opcode::ValueEqualsLiteral, StateOfKnowledge::Always, false);
    bool hasLeftLoop = context.valueID() && comparisonsWithLiteral.contains(generator().code().at(context.valueID()).opcode) && generator().code().at(context.valueID()).hasLeftLoop();
    FormAtPlace named = context.block() && (generator().usesLoopForms(context.block()) || generator().isInRecursion(context.block()) || hasLeftLoop) ? namedInLoops : namedOutsideLoops;
    m_out.noteFormOf(comparisonsWithLiteral, named.form, StateOfKnowledge::Always, named.place);
    return named.form == Form::Open;
}

void StringEmitter::branchOnEqualsLiteral(const OperationContext& context, ComparedOperand operand, LValue subject, StringID literal, BranchTargets targets, std::optional<FlatStringCell> flatSubject, bool everyCellIsString, std::optional<Word32> lengthOfSubject)
{
    branchOnEqualsLiteral(context, operand, subject, literal, textOf(literal), targets, flatSubject, everyCellIsString, lengthOfSubject);
}

void StringEmitter::branchOnEqualsLiteral(const OperationContext& context, ComparedOperand operand, LValue subject, StringID literal, const String& text, BranchTargets targets, std::optional<FlatStringCell> flatSubject, bool everyCellIsString, std::optional<Word32> lengthOfSubject)
{
    RELEASE_ASSERT(!flatSubject || (operand == ComparedOperand::String && flatSubject->cell().value() == subject));
    bool subjectIsFlat = !!flatSubject;
    if (text.isEmpty()) {
        m_out.noteFormOf(comparisonsWithLiteral, Form::Open, StateOfKnowledge::EmptyLiteral);
        branch(equal(Boxed(subject), boxCell(emptyString())), targets);
        return;
    }
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
        if (known == StateOfKnowledge::Always && expectsName(context, text)) {
            if (operand == ComparedOperand::Value)
                continueIf(isCell(Boxed(subject)), targets.notTaken);
            continueUnlessHoldsName(operand == ComparedOperand::Value ? unboxCell(Boxed(subject)) : CellPointer(subject), literal, targets, operand == ComparedOperand::String || everyCellIsString);
        }
        branchOnEqualsLiteralByStub(context, operand, subject, literal, text, targets);
        return;
    }

    CellPointer string(subject);
    bool leavesCharactersToStub = false;
    if (known == StateOfKnowledge::Always) {
        if (!readsCharactersInLine(context)) {
            branchOnEqualsLiteralByStub(context, operand, subject, literal, text, targets);
            return;
        }
        bool isString = operand == ComparedOperand::String || everyCellIsString;
        if (operand == ComparedOperand::Value) {
            Boxed boxed(subject);
            continueIf(isCell(boxed), targets.notTaken);
            string = unboxCell(boxed);
        }
        if (expectsName(context, text)) {
            continueUnlessHoldsName(string, literal, targets, isString);
            leavesCharactersToStub = text.length() > maxCharactersNextToName;
        }
        else if (operand == ComparedOperand::String && mayBeNameInLoop(context, text)) {
            if (std::optional<CellPointer> constant = emitter<ConstantEmitter>().stringConstantIfMade(literal)) {
                LBasicBlock isOtherCell = newBlock();
                m_out.branch(equal(string, *constant).value(), targets.taken, normalEdge(isOtherCell));
                m_out.appendToContinuation(isOtherCell);
                build().statistics().count(StatisticID::LiteralCellsCompared);
            }
        }
        if (!isString)
            continueIf(hasCellType(string, StringType), targets.notTaken);
    } else {
        m_out.noteFormOf(comparisonsWithLiteral, Form::Open, known);
        makeCellsHoldAtomOf(literal, text);
    }

    LBasicBlock slow = newBlock();
    auto openedString = subjectIsFlat ? m_openedStrings.find(string.value()) : m_openedStrings.end();
    const CharacterSource* source = openedString != m_openedStrings.end() ? &openedString->value.source : nullptr;
    Pointer impl = source ? openedString->value.impl : flatSubject ? implOfFlatString(*flatSubject) : loadField<Pointer>(string, m_heaps.JSString_value);
    if (!subjectIsFlat)
        continueIf(hasNoBits(impl, JSString::isRopeInPointer), unlikelyEdge(slow));
    if (!lengthOfSubject)
        continueIf(equal(source ? source->length : loadField<Word32>(impl, m_heaps.StringImpl_length), text.length()), targets.notTaken);
    if (leavesCharactersToStub)
        m_out.jump(normalEdge(slow));
    else {
        continueIf(hasBits(source ? source->implFlags : flagsInImpl(impl), StringImpl::flagIs8Bit()), unlikelyEdge(slow));
        Pointer characters = source ? source->characters : loadField<Pointer>(impl, m_heaps.StringImpl_data);
        recordInlined(literal);
        branchOnBytesAt(characters, asBytes(text.span8()), m_heaps.characters8.atAnyIndex(), string, targets);
    }

    m_out.appendTo(slow);
    branchOnEqualsLiteralByStub(context, ComparedOperand::String, string.value(), literal, text, targets);
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
            branchOnEqualsLiteral(context, operand, subject, literal, text, targets, flatSubject, everyCellIsString, lengthOfSubject);
        });
    }
    m_out.noteFormOf(comparisonsWithLiteral, Form::Closed, StateOfKnowledge::LiteralComparedByStub);
    if (comparedByCharacters(text)) {
        return asBoolean([&](BranchTargets targets) {
            branchOnEqualsLiteralByStub(context, operand, subject, literal, text, targets);
        });
    }
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
    if (emitter<ControlEmitter>().tryGenerateLiteralChain(value))
        return;
    ValueID string = value->stringOperand();
    std::optional<FlatStringCell> flat;
    if (ValueID flatString = value->flatOperand())
        flat = flatStringByType(flatString);
    generateEqualsLiteral(value, ComparedOperand::String, (flat ? flat->cell() : in<CellPointer>(string)).value(), flat);
}

void StringEmitter::generateValueEqualsLiteral(const ValueContext& value)
{
    if (emitter<ControlEmitter>().tryGenerateLiteralChain(value))
        return;
    const Type& type = typeOf(value->valueOperand());
    bool everyCellIsString = !type.hasObjects() && !(type.primitives & cellPrimitives & ~Primitive::AnyString);
    generateEqualsLiteral(value, ComparedOperand::Value, in<Boxed>(value->valueOperand()).value(), std::nullopt, everyCellIsString);
}

void StringEmitter::addOrderByFirstDifference(ValueID left, ValueID right, LBasicBlock byCall, Join<Word32>& order)
{
    constexpr unsigned width = sizeof(uint32_t);
    constexpr uint32_t nonASCIIBits = 0x80808080;
    struct Side {
        CellPointer cell;
        Pointer impl;
        const CharacterSource* source;
    };
    auto open = [&](ValueID string) -> Side {
        CellPointer cell = in<CellPointer>(string);
        if (std::optional<FlatStringCell> flat = flatStringByType(string)) {
            auto opened = m_openedStrings.find(cell.value());
            if (opened == m_openedStrings.end() || !opened->value.mayBeRope)
                return { cell, implOfFlatString(*flat), opened == m_openedStrings.end() ? nullptr : &opened->value.source };
        }
        Pointer impl = loadField<Pointer>(cell, m_heaps.JSString_value);
        continueIf(hasNoBits(impl, JSString::isRopeInPointer), unlikelyEdge(byCall));
        return { cell, impl, nullptr };
    };
    Side x = open(left);
    Side y = open(right);
    auto flagsOf = [&](const Side& side) { return side.source ? side.source->implFlags : flagsInImpl(side.impl); };
    auto lengthOf = [&](const Side& side) { return side.source ? side.source->length : loadField<Word32>(side.impl, m_heaps.StringImpl_length); };
    auto charactersOf = [&](const Side& side) { return side.source ? side.source->characters : loadField<Pointer>(side.impl, m_heaps.StringImpl_data); };
    continueIf(hasBits(bitAnd(flagsOf(x), flagsOf(y)), StringImpl::flagIs8Bit()), unlikelyEdge(byCall));
    Word32 xLength = lengthOf(x);
    Word32 yLength = lengthOf(y);
    Word32 length = select(below(xLength, yLength), xLength, yLength);
    continueIf(aboveOrEqual(length, word32(width)), unlikelyEdge(byCall));
    Pointer xCharacters = charactersOf(x);
    Pointer yCharacters = charactersOf(y);
    auto load = [&](Pointer address) { return Word32(loadBytes(m_out, width, TypedPointer(m_heaps.characters8.atAnyIndex(), address.value()))); };

    LBasicBlock firstAreEqual = newBlock();
    LBasicBlock firstDiffer = newBlock();
    LBasicBlock differ = newBlock();
    Vector<ValueFromBlock, 2> differences;
    Vector<ValueFromBlock, 2> xAddresses;
    Vector<ValueFromBlock, 2> yAddresses;
    auto join = [&](Word32 xUnits, Word32 yUnits, Pointer xAddress, Pointer yAddress) {
        differences.append(m_out.anchor(bitXor(xUnits, yUnits).value()));
        xAddresses.append(m_out.anchor(xAddress.value()));
        yAddresses.append(m_out.anchor(yAddress.value()));
        m_out.jump(differ);
    };
    Word32 xFirst = load(xCharacters);
    Word32 yFirst = load(yCharacters);
    m_out.branch(notEqual(xFirst, yFirst).value(), normalEdge(firstDiffer), normalEdge(firstAreEqual));

    m_out.appendTo(firstAreEqual);
    continueIf(belowOrEqual(length, word32(2 * width)), unlikelyEdge(byCall));
    Integer offset = zeroExtended(sub(length, word32(width)));
    Pointer xEnd = offsetBy(xCharacters, offset, 0);
    Pointer yEnd = offsetBy(yCharacters, offset, 0);
    Word32 xLast = load(xEnd);
    Word32 yLast = load(yEnd);
    continueIf(notEqual(xLast, yLast), unlikelyEdge(byCall));
    continueIf(Condition(m_out.testIsZero32(bitOr(xFirst, bitOr(xLast, yLast)).value(), word32(nonASCIIBits).value())), unlikelyEdge(byCall));
    join(xLast, yLast, xEnd, yEnd);

    m_out.appendTo(firstDiffer);
    continueIf(Condition(m_out.testIsZero32(bitOr(xFirst, yFirst).value(), word32(nonASCIIBits).value())), unlikelyEdge(byCall));
    join(xFirst, yFirst, xCharacters, yCharacters);

    m_out.appendTo(differ);
    Word32 difference(m_out.phi(Machine::Int32, differences));
    Pointer xAddress(m_out.phi(machineTypeOf(Pointer::representation), xAddresses));
    Pointer yAddress(m_out.phi(machineTypeOf(Pointer::representation), yAddresses));
    LValue firstByte = m_out.lShr(countTrailingZerosOfNonZero(zeroExtended(difference).value()), 3);
    Pointer weights(m_out.loadPtr(mutableInstanceField(AOT_INSTANCE_OFFSET(collation.knownLevel1Weights))));
    auto byteAt = [&](Pointer bytes, LValue index) { return Word32(m_out.load8ZeroExt32(m_out.baseIndex(m_heaps.characters8, bytes.value(), index))); };
    Word32 xByte = byteAt(xAddress, firstByte);
    Word32 yByte = byteAt(yAddress, firstByte);
    ensureStillAliveHere(x.cell);
    ensureStillAliveHere(y.cell);
    Word32 xWeight = byteAt(weights, zeroExtended(xByte).value());
    Word32 yWeight = byteAt(weights, zeroExtended(yByte).value());
    continueIf(notEqual(xWeight, yWeight), unlikelyEdge(byCall));
    LBasicBlock isBefore = newBlock();
    LBasicBlock isBehind = newBlock();
    m_out.branch(below(xWeight, yWeight).value(), normalEdge(isBefore), normalEdge(isBehind));
    m_out.appendTo(isBefore);
    continueIf(isNotZero(xWeight), unlikelyEdge(byCall));
    order.add(word32(-1));
    m_out.appendTo(isBehind);
    continueIf(isNotZero(yWeight), unlikelyEdge(byCall));
    order.add(word32(1));
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

void StringEmitter::branchOnTextAt(const CharacterView& view, Word32 offset, StringID literal, BranchTargets targets)
{
    String text = textOf(literal);
    RELEASE_ASSERT(canCompareInline(text));
    if (text.isEmpty()) {
        m_out.jump(targets.taken);
        return;
    }
    recordInlined(literal);

    auto compare = [&](bool is8Bit) {
        if (!is8Bit) {
            branchOnBytesAt(offsetBy(view.characters, zeroExtended(offset), 1), bytesOfTextInWideString(text).span(), m_heaps.characters16.atAnyIndex(), view.owner, targets);
            return;
        }
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
        if (is8Bit && isForward) {
            LValue operands[] = { subject.characters.value(), subject.length.value(), sought.characters.value(), sought.length.value(), position.value() };
            return low32(Integer(emitter<RuntimeCallEmitter>().callStubWithoutSite(findText8(), operands).first));
        }
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
    if (isForward && !position.value()->isInt32(0))
        holdRangeInside(value, position, position);
    bool readsRange = !!value->startOperand();
    auto viewOfSubject = [&] {
        return readsRange ? viewOfRange(value) : viewOf(value, string);
    };
    if (!isForward && position.value()->isInt32(0) && canCompareInline(literal)) {
        CharacterView view = viewOfSubject();
        Word32 length = word32(text.length());
        Condition isAtStart = asBoolean([&](BranchTargets targets) {
            continueIf(aboveOrEqual(view.length, length), targets.notTaken);
            branchOnTextAt(view, word32(0), literal, targets);
        });
        define(value, select<Word32>(isAtStart, 0, -1));
        return;
    }

    Word32 numberOfText = numberOf(m_mentions.add(NumberSpace::String, literal));
    auto findInCell = [&] {
        RuntimeOperation operation = isForward ? RuntimeOperation::StringIndexOfLiteral : RuntimeOperation::StringLastIndexOfLiteral;
        return low32(callOperation<Integer>(value, operation, { in<CellPointer>(string), numberOfText, position }));
    };
    if (!text.is8Bit()) {
        RELEASE_ASSERT(!readsRange);
        define(value, findInCell());
        return;
    }

    CharacterView view = viewOfSubject();
    if (!isForward && text.length() == 1) {
        Word32 codeUnit = word32(text[0]);
        recordInlined(literal);
        define(value, findCodeUnit(view, codeUnit, select(lessThan(position, 0), 0, position), direction));
        return;
    }

    Word32 found = selectByWidth<Word32>(view.is8Bit, [&](bool is8Bit) {
        if (!is8Bit && !readsRange)
            return findInCell();
        RuntimeOperation operation = isForward ? (is8Bit ? RuntimeOperation::StringFindLiteral8 : RuntimeOperation::StringFindLiteral16)
            : (is8Bit ? RuntimeOperation::StringReverseFindLiteral8 : RuntimeOperation::StringReverseFindLiteral16);
        return low32(callWithoutSite<Integer>(operation, { view.characters, view.length, numberOfText, position }));
    });
    ensureStillAliveHere(view.owner);
    define(value, found);
}

void StringEmitter::generateStringIndexOfCharacter(const ValueContext& value)
{
    ValueID string = value->stringOperand();
    Word32 codeUnit = word32(static_cast<int32_t>(value->codeUnit()));
    recordInlined(static_cast<char16_t>(value->codeUnit()));
    Word32 position = word32Of(value->positionOperand());
    if (!position.value()->isInt32(0))
        holdRangeInside(value, position, position);
    if (value->startOperand()) {
        define(value, findCodeUnit(viewOfRange(value), codeUnit, position, SearchDirection::Forward));
        return;
    }
    if (typeOf(string).isOpaque(OpaqueKind::FlatString)) {
        define(value, findCodeUnit(viewOf(value, string), codeUnit, position, SearchDirection::Forward));
        return;
    }

    CellPointer cell = in<CellPointer>(string);
    LBasicBlock rope = newBlock();
    Join<Word32> join(m_out);
    ScopeFork fork = m_out.forkScope();
    join.add(findCodeUnit(viewWithoutResolving(value, cell, unlikelyEdge(rope)), codeUnit, position, SearchDirection::Forward));

    m_out.appendTo(rope);
    join.add(low32(callOperation<Integer>(value, RuntimeOperation::StringIndexOfCharacter, { cell, codeUnit, position })));
    define(value, join.finishBehind(fork));
}

} } } // namespace JSC::AOT::Typed

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
