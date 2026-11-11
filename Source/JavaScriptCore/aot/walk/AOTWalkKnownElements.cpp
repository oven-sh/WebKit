/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkKnown.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static Reference recordNotedInKnownElements(std::optional<uint64_t> noted)
{
    return noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
}

static bool isAssumedInBoundsInKnownElements(Walker& walker, Reference position)
{
    TypeID type = position && walker.shouldAssumeIndexesInBounds() ? walker.typeOf(position) : TypeID();
    return type && walker.types().at(type).isInteger() && walker.types().at(type).integers.minimum >= 0;
}

std::optional<PassedTest> Walker::findWhatHoldsPositionInsideList(Reference list, Reference position)
{
    if (isAssumedInBoundsInKnownElements(*this, position)) {
        PassedTest taken;
        taken.m_groundsOfPosition = GroundsOfPosition { .view = list && list.isRecord() ? list.id() : ValueID(), .position = position.id() };
        return taken;
    }
    if (!position || !has(Mechanism::TypedOperations) || m_state.answers().isEmpty())
        return std::nullopt;
    if (std::optional<PassedTest> found = showElementPresentBehindTest(list, position, findPassedTest(recordNotedInKnownElements(findKnown(*this, KnownNote::PositionFoundInsideList, list, position)))))
        return found;
    Reference length = recordNotedInKnownElements(findKnown(*this, KnownNote::LengthOfList, list));
    std::optional<PassedTest> fromZeroUp = length ? findNonNegativeProof(position) : std::nullopt;
    std::optional<PassedTest> below = fromZeroUp ? findUpperBoundProof(position, length) : std::nullopt;
    if (below)
        return boundsProofFromChecks(list, position, length, *fromZeroUp, *below);
    return fromZeroUp ? showPositionInsideBehindTestOfSum(list, position, length) : std::nullopt;
}

static bool isTermTestedForPosition(const Code& code, const Operation& narrowed, bool isWholePosition)
{
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    if (!narrowed.testOperand())
        return false;
    const Operation& test = code.at(narrowed.testOperand());
    if (!allRelations.contains(test.opcode) || test.form() != OperatorForm::Integers || narrowed.operandIndex() > 1)
        return false;
    if (!isWholePosition)
        return true;
    ValueID bound = test.operand(1 - narrowed.operandIndex());
    while (valuePreservingOpcodes.contains(code.at(bound).opcode))
        bound = code.at(bound).operand(0);
    return sums.contains(code.at(bound).opcode);
}

std::optional<PassedTest> Walker::showPositionInsideBehindTestOfSum(Reference list, Reference position, Reference length)
{
    if (!list.isRecord() || !position.isValue() || !length.isRecord())
        return std::nullopt;
    for (ValueID term = position.id(); term && !m_code.isConstant(term);) {
        const Operation& record = m_code.at(term);
        bool isTermTested = record.opcode == Opcode::Narrowed && !record.narrowsTermOfOperand() && isTermTestedForPosition(m_code, record, term == position.id());
        if (isTermTested || (record.opcode == Opcode::Narrowed && record.narrowsTermOfOperand())) {
            GroundsOfPosition grounds { .view = list.id(), .position = position.id(), .length = length.id(), .isBelow = record.testOperand(), .isBelowResult = record.isOnTakenEdge() };
            std::optional<PassedTest> shown = m_inputs.options.shouldSkipBoundsProof ? findPassedTest(referenceTo(grounds.isBelow), grounds.isBelowResult) : showPositionInside(grounds);
            if (shown && m_inputs.options.shouldSkipBoundsProof)
                shown->m_groundsOfPosition = grounds;
            if (shown && isTermTested)
                count(StatisticID::PositionsShownByTestedTerms);
            if (shown)
                return shown;
        }
        bool stepsDown = record.opcode == Opcode::Decrement || (record.opcode == Opcode::Sub && m_code.isConstant(record.operand(1)));
        if (valuePreservingOpcodes.contains(record.opcode) || record.opcode == Opcode::Increment || stepsDown) {
            term = record.operand(0);
            continue;
        }
        if (record.opcode != Opcode::Add)
            return std::nullopt;
        bool firstIsWritten = m_code.isConstant(record.operand(0));
        if (firstIsWritten == m_code.isConstant(record.operand(1)))
            return std::nullopt;
        term = record.operand(firstIsWritten);
    }
    return std::nullopt;
}

bool isKnownToLieInsideList(Walker& walker, Reference list, Reference position)
{
    return !!walker.findWhatHoldsPositionInsideList(list, position);
}

std::optional<Effects> effectsOfAppendToListAsMade(Walker& walker, const Draft& append)
{
    static constexpr OpcodeSet appendsThatRunNoCode { Opcode::AppendElements, Opcode::AppendMembers, Opcode::AppendValuesOfMap };
    if (!appendsThatRunNoCode.contains(append.opcode) || !walker.has(Mechanism::TypedOperations) || !isFreshPristineArray(walker, append.arrayOperand()))
        return std::nullopt;
    Effects effects = declaredEffectsOfOperation(walker.program(), append.opcode, append);
    if (effects.mayRunOtherCode())
        return std::nullopt;
    effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}

static constexpr AbstractHeapSet heapsUnderStorageOfListInKnownElements {
    AbstractHeap::ArrayStorage, AbstractHeap::Elements, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::ShapeWords
};
static_assert(perObjectHeaps.containsAll(heapsUnderStorageOfListInKnownElements));
static constexpr unsigned shiftOfLengthInKnownElements = 32;

void noteLengthThatListIsBornWith(Walker& walker, Reference list)
{
    if (!walker.has(Mechanism::TypedOperations) || !list || !list.isValue() || walker.code().at(list.id()).opcode != Opcode::NewArrayLike)
        return;
    ValueID length = walker.code().at(list.id()).lengthOperand();
    Reference born = walker.code().isConstant(length) ? Reference::constant(length) : Reference::value(length);
    const Type& type = walker.types().at(walker.typeOf(born));
    if (!type.isInteger() || !type.integers.isSubsetOf(Range { 0, UINT32_MAX }))
        return;
    noteKnown(walker, KnownNote::LengthOfList, list, { }, 0, placesUnderLengthsOfLists, born.bits());
}

void noteStorageOfListMade(Walker& walker, Reference list)
{
    if (!walker.has(Mechanism::TypedOperations) || !list || !list.isValue())
        return;
    noteLengthThatListIsBornWith(walker, list);
    const Code& code = walker.code();
    const Operation& made = code.at(list.id());
    ArrayStorage storage = ArrayStorage::Other;
    size_t length = 0;
    if (made.opcode == Opcode::NewArray) {
        storage = storageThatLiteralIsSurelyBornWith(walker.program(), walker.types(), code, made);
        length = made.elementsOperands().size();
    } else if (made.opcode == Opcode::Data && made.encoding() == DataEncoding::CopyOnWriteArray && made.data()) {
        const auto* constant = walker.program().at(made.data()).root.tryAs<DataNode::Array>();
        bool hasHole = !constant || std::ranges::any_of(constant->elements, [](const DataNode& element) {
            return element.is<DataNode::Hole>();
        });
        if (hasHole)
            return;
        storage = arrayStorageOfData(walker.program(), *constant);
        length = constant->elements.size();
    }
    if (!hasArrayStorageForm(storage))
        return;
    uint64_t noted = static_cast<uint64_t>(storage) | static_cast<uint64_t>(length) << shiftOfLengthInKnownElements;
    noteKnown(walker, KnownNote::StorageOfListMadeInSight, list, { }, 0, LocationMasks(heapsUnderStorageOfListInKnownElements), noted);
    walker.count(StatisticID::ArrayIndexingTypesRecorded);
}

WrittenListOfVariable writtenListThatVariableHolds(Walker& walker, VariableID variable)
{
    const Program& program = walker.program();
    if (!walker.has(Mechanism::TypedOperations) || (!numberReadOutOfVariableStays(walker, variable) && !objectOfVariablePutOffStays(walker, variable)))
        return { };
    FunctionID body = program.at(program.at(variable).module).body;
    if (!body)
        return { };
    const ThinSummary& summary = walker.facts().summaries().at(body);
    const SummaryWrittenListOfVariable* written = summary.findWrittenListOf(variable);
    if (!written || summary.constantListIsStoredIntoInLoop(written->made))
        return { };
    return { written->data, static_cast<ArrayStorage>(written->storage), written->length };
}

bool isReadOutOfVariableOfFileThatStays(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID source = index.stripCasts(input);
    if (source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::CheckInitialized)
        source = index.stripCasts(index.value(source).operand(0));
    if (!source || index.isParameter(source) || index.opcodeOf(source) != Opcode::GetModuleVariable)
        return false;
    VariableID variable = index.value(source).variable();
    return numberReadOutOfVariableStays(walker, variable) || objectOfVariablePutOffStays(walker, variable);
}

void noteStorageOfWrittenList(Walker& walker, Reference list, const WrittenListOfVariable& written)
{
    uint64_t noted = static_cast<uint64_t>(written.storage) | static_cast<uint64_t>(written.length) << shiftOfLengthInKnownElements;
    noteKnown(walker, KnownNote::StorageOfListMadeInSight, list, { }, 0, LocationMasks(heapsUnderStorageOfListInKnownElements), noted);
    walker.count(StatisticID::ConstantArrayGuardsHoisted);
}

std::optional<PassedTest> Walker::findWhatHoldsPositionInsideListMadeInSight(Reference list, Reference position)
{
    std::optional<uint64_t> noted = findKnown(*this, KnownNote::StorageOfListMadeInSight, list);
    std::optional<PassedTest> fromZeroUp = noted ? findNonNegativeProof(position) : std::nullopt;
    if (!fromZeroUp)
        return std::nullopt;
    Draft length(Opcode::Integer);
    length.setInteger(static_cast<int64_t>(*noted >> shiftOfLengthInKnownElements));
    Reference lengthAsMade = constant(WTF::move(length));
    std::optional<PassedTest> below = findUpperBoundProof(position, lengthAsMade);
    return below ? boundsProofFromChecks(list, position, lengthAsMade, *fromZeroUp, *below) : std::nullopt;
}

WhatListWasMadeWith whatListWasMadeWith(Walker& walker, Reference list)
{
    const Code& code = walker.code();
    const Operation& making = code.at(underlyingCellOf(code, list.id()));
    if (making.opcode == Opcode::Data) {
        const auto* constant = making.data() ? walker.program().at(making.data()).root.tryAs<DataNode::Array>() : nullptr;
        if (!constant)
            return { };
        uint16_t constants = 0;
        for (const DataNode& element : constant->elements) {
            switch (element.kind()) {
            case DataNode::Kind::Undefined:
                constants |= Primitive::Undefined;
                break;
            case DataNode::Kind::Null:
                constants |= Primitive::Null;
                break;
            case DataNode::Kind::Boolean:
                constants |= Primitive::Boolean;
                break;
            case DataNode::Kind::Number:
                constants |= Primitive::Number;
                break;
            case DataNode::Kind::String:
                constants |= Primitive::String;
                break;
            default:
                return { };
            }
        }
        return { TypeArena::fromPrimitives(constants), TypeArena::fromPrimitives(constants) };
    }
    if (making.opcode != Opcode::NewArray)
        return { };
    WhatListWasMadeWith held { TypeTable::never, TypeTable::never };
    for (ValueID element : making.elementsOperands()) {
        held.proven = walker.types().unionOf(held.proven, code.at(element).type);
        held.shown = walker.types().unionOf(held.shown, walker.typeShown(code.isConstant(element) ? Reference::constant(element) : Reference::value(element)));
    }
    const Type& made = walker.types().at(held.proven);
    if (made.classes || made.unnamedClasses)
        return { };
    return held;
}

static void checkTestOfListInKnownElements(Walker& walker, Draft&& test)
{
    Origin origin = test.origin();
    Reference holds = walker.buildTestOfCheck(WTF::move(test));
    if (holds && holds.isValue() && !walker.isUnreachable())
        walker.checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);
}

std::optional<uint32_t> lengthOfListMadeInSight(Walker& walker, Reference list)
{
    std::optional<uint64_t> noted = list && list.isValue() ? findKnown(walker, KnownNote::StorageOfListMadeInSight, list) : std::nullopt;
    if (!noted)
        return std::nullopt;
    return static_cast<uint32_t>(*noted >> shiftOfLengthInKnownElements);
}

Reference elementWrittenInListMadeInSight(Walker& walker, Reference list, uint32_t position)
{
    if (!list || !list.isValue() || !findKnown(walker, KnownNote::StorageOfListMadeInSight, list))
        return { };
    const Operation& made = walker.code().at(list.id());
    const auto* constant = made.opcode == Opcode::Data && made.data() ? walker.program().at(made.data()).root.tryAs<DataNode::Array>() : nullptr;
    if (!constant || position >= constant->elements.size())
        return { };
    const DataNode& element = constant->elements[position];
    switch (element.kind()) {
    case DataNode::Kind::Undefined:
        return walker.constant(Draft(Opcode::Undefined));
    case DataNode::Kind::Null:
        return walker.constant(Draft(Opcode::Null));
    case DataNode::Kind::Boolean: {
        Draft written(Opcode::Boolean);
        written.setBoolean(element.as<DataNode::Boolean>().value);
        return walker.constant(WTF::move(written));
    }
    case DataNode::Kind::Number: {
        double number = element.as<DataNode::Number>().value;
        if (number != number)
            return { };
        Draft written(isTrackedInteger(number) ? Opcode::Integer : Opcode::Double);
        if (written.opcode == Opcode::Integer)
            written.setInteger(static_cast<int64_t>(number));
        else
            written.setNumber(number);
        return walker.constant(WTF::move(written));
    }
    case DataNode::Kind::String: {
        Draft written(Opcode::String);
        written.setString(element.as<DataNode::String>().value);
        return walker.constant(WTF::move(written));
    }
    default:
        return { };
    }
}

void checkLengthOfListMadeInSight(Walker& walker, Reference list, uint32_t length, Origin origin)
{
    if (!walker.checksProofs() || walker.isUnreachable())
        return;
    origin.isTailCall = false;
    Draft believed(Opcode::Integer, origin);
    believed.setInteger(length);
    Draft readAgain(Opcode::GetArrayLength, origin);
    walker.chooseConservativeFields(readAgain);
    readAgain.setArrayOperand(list);
    walker.checkThatMemoryAnswersAsBelieved(walker.constant(WTF::move(believed)), WTF::move(readAgain));
}

bool nameStorageOfListMadeInSight(Walker& walker, Draft& read)
{
    Reference list = read.arrayOperand();
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::StorageOfListMadeInSight, list);
    if (!noted)
        return false;
    ArrayStorage storage = static_cast<ArrayStorage>(*noted & 0xff);
    Origin origin = read.origin();
    origin.isTailCall = false;
    if (walker.checksProofs() && !walker.isUnreachable()) {
        Draft test(Opcode::HasArrayStorage, origin);
        walker.chooseConservativeFields(test);
        test.setArrayOperand(list);
        test.setArrayStorage(storage);
        checkTestOfListInKnownElements(walker, WTF::move(test));
    }
    read.setArrayStorage(storage);
    walker.count(StatisticID::ElementReadsGivenIndexingType);

    std::optional<PassedTest> inside = read.isInBounds() ? read.testsPassed : walker.findWhatHoldsPositionInsideListMadeInSight(list, read.positionOperand());
    if (!inside)
        return true;
    read.setIsInBounds(*inside);
    if (walker.checksProofs() && !walker.isUnreachable()) {
        Draft test(Opcode::IsElementPresent, origin);
        walker.chooseConservativeFields(test);
        test.setArrayOperand(list);
        test.setPositionOperand(read.positionOperand());
        test.setArrayStorage(storage);
        test.setKeyIsIntegral(true);
        checkTestOfListInKnownElements(walker, WTF::move(test));
    }
    read.setIsPresent(*inside);
    walker.count(StatisticID::ElementReadsProvenPresent);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
