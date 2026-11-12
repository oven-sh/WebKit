/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkKnown.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTSumOverRecords.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTWalkStrings.h"
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
    auto isView = [&] {
        TypeID type = typeOf(list);
        return typedArrayTypeOf({ program(), m_types }, type) != NotTypedArray || kindsOfViewsOfNumbersIn(program(), m_types, type);
    };
    if (std::optional<PassedTest> found = showElementPresentBehindTest(list, position, findPassedTest(recordNotedInKnownElements(findKnown(*this, KnownNote::PositionFoundInsideList, list, position)))); found && !isView())
        return found;
    if (std::optional<PassedTest> behind = findElementFoundAfter(list, position); behind && !isView())
        return behind;
    Reference length = recordNotedInKnownElements(findKnown(*this, KnownNote::LengthOfList, list));
    std::optional<PassedTest> fromZeroUp = length ? findNonNegativeProof(position) : std::nullopt;
    std::optional<PassedTest> below = fromZeroUp ? findUpperBoundProof(position, length) : std::nullopt;
    if (below)
        return boundsProofFromChecks(list, position, length, *fromZeroUp, *below);
    if (std::optional<PassedTest> lowered = length ? findLoweredCounterProof(list, position, length) : std::nullopt)
        return lowered;
    if (std::optional<PassedTest> stepped = fromZeroUp ? findSteppedKeyProof(list, position, length) : std::nullopt)
        return stepped;
    return fromZeroUp ? showPositionInsideBehindTestOfSum(list, position, length) : provePositionAfterCounterStart(list, position, length);
}

std::optional<PassedTest> Walker::findElementFoundAfter(Reference list, Reference position)
{
    if (!list || !list.isRecord() || !position.isValue())
        return std::nullopt;
    for (ValueID term = position.id(); !m_code.isConstant(term);) {
        const Operation& record = m_code.at(term);
        bool stepsDown = record.opcode == Opcode::Decrement || (record.opcode == Opcode::Sub && m_code.isConstant(record.operand(1)));
        if (!stepsDown && !valuePreservingOpcodes.contains(record.opcode))
            return std::nullopt;
        term = record.operand(0);
        if (!stepsDown)
            continue;
        std::optional<PassedTest> found = findPassedTest(recordNotedInKnownElements(findKnown(*this, KnownNote::PositionFoundInsideList, list, referenceTo(term))));
        if (!found || !testsProveElementAtOrAfter(m_code, m_types, list.id(), position.id(), found->sides()))
            continue;
        PassedTest inside = *found;
        inside.m_groundsOfPosition = GroundsOfPosition { .view = list.id(), .position = position.id() };
        return inside;
    }
    return std::nullopt;
}

static bool isSumInKnownElements(const Code& code, ValueID value)
{
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    while (valuePreservingOpcodes.contains(code.at(value).opcode))
        value = code.at(value).operand(0);
    return sums.contains(code.at(value).opcode);
}

static bool isTermTestedAsIndex(const Code& code, const Operation& narrowed, bool isWholePosition, bool lengthIsSum)
{
    if (!narrowed.testOperand())
        return false;
    const Operation& test = code.at(narrowed.testOperand());
    if (!allRelations.contains(test.opcode) || test.form() != OperatorForm::Integers || narrowed.operandIndex() > 1)
        return false;
    return !isWholePosition || lengthIsSum || isSumInKnownElements(code, test.operand(1 - narrowed.operandIndex()));
}

std::optional<PassedTest> Walker::showPositionInsideBehindTestOfSum(Reference list, Reference position, Reference length)
{
    if (!list.isRecord() || !position.isValue() || !length.isRecord())
        return std::nullopt;
    bool lengthIsSum = isSumInKnownElements(m_code, length.id());
    GroundsOfPosition groundsByLength { .view = list.id(), .position = position.id(), .length = length.id() };
    if (lengthIsSum && m_inputs.options.shouldSkipBoundsProof) {
        PassedTest taken;
        taken.m_groundsOfPosition = groundsByLength;
        return taken;
    }
    if (std::optional<PassedTest> shown = lengthIsSum ? showPositionInside(groundsByLength) : std::nullopt)
        return shown;
    for (ValueID term = position.id(); term && !m_code.isConstant(term);) {
        const Operation& record = m_code.at(term);
        bool isTermTested = record.opcode == Opcode::Narrowed && !record.narrowsTermOfOperand() && isTermTestedAsIndex(m_code, record, term == position.id(), lengthIsSum);
        if (isTermTested || (record.opcode == Opcode::Narrowed && record.narrowsTermOfOperand())) {
            GroundsOfPosition grounds { .view = list.id(), .position = position.id(), .length = length.id(), .isBelow = record.testOperand(), .isBelowResult = record.isOnTakenEdge() };
            std::optional<PassedTest> shown = m_inputs.options.shouldSkipBoundsProof ? findPassedTest(referenceTo(grounds.isBelow), grounds.isBelowResult) : showPositionInside(grounds);
            if (shown && m_inputs.options.shouldSkipBoundsProof)
                shown->m_groundsOfPosition = grounds;
            if (shown && isTermTested)
                count(StatisticID::IndexesProvenByTestedTerms);
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

static WrittenListOfVariable literalArrayAt(Walker& walker, ModuleID module, LiteralArrayLocation place)
{
    FunctionID body = walker.program().at(module).body;
    if (!body)
        return { };
    const ThinSummary& summary = walker.facts().summaries().at(body);
    const SummaryWrittenListOfVariable* written = summary.findLiteralArray(place);
    if (!written || summary.constantListIsStoredIntoInLoop(written->made) || walker.facts().summaries().storesElementInto(written->data))
        return { };
    return { written->data, static_cast<ArrayStorage>(written->storage), written->length };
}

static WrittenListOfVariable literalNumericArray(const WrittenListOfVariable& written)
{
    return isNumericStorage(written.storage) ? written : WrittenListOfVariable();
}

static WrittenListOfVariable listThatVariableHolds(Walker& walker, VariableID variable)
{
    const Program& program = walker.program();
    if (!walker.has(Mechanism::TypedOperations) || (!numberReadOutOfVariableStays(walker, variable) && !objectOfVariablePutOffStays(walker, variable)))
        return { };
    if (walker.facts().summaries().storesElementInto(variable))
        return { };
    return literalArrayAt(walker, program.at(variable).module, LiteralArrayLocation::of(variable));
}

WrittenListOfVariable writtenListThatVariableHolds(Walker& walker, VariableID variable) { return literalNumericArray(listThatVariableHolds(walker, variable)); }

static WrittenListOfVariable listInInstance(Walker& walker, FieldReference field)
{
    const Program& program = walker.program();
    if (!walker.has(Mechanism::TypedOperations) || !leavesDeadZoneForGood(program, field) || !fieldAt(program, field).flags.contains(FieldFlags::IsImmutable))
        return { };
    return literalArrayAt(walker, program.at(field.layout).module, LiteralArrayLocation::of(field));
}

WrittenListOfVariable literalArrayInInstance(Walker& walker, FieldReference field) { return literalNumericArray(listInInstance(walker, field)); }

WrittenListOfVariable writtenListReadBy(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID source = index.stripCasts(input);
    if (source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::CheckInitialized)
        source = index.stripCasts(index.value(source).operand(0));
    if (!source || index.isParameter(source))
        return { };
    BodyValue read = index.value(source);
    if (read.opcode() == Opcode::GetModuleVariable)
        return listThatVariableHolds(walker, read.variable());
    return read.opcode() == Opcode::GetEnvironmentField ? listInInstance(walker, { read.layout(), read.field() }) : WrittenListOfVariable();
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
    return lengthOfListMadeInSight(walker, walker.state(), list);
}

std::optional<uint32_t> lengthOfListMadeInSight(const Walker& walker, const WalkState& state, Reference list)
{
    std::optional<uint64_t> noted = list && list.isValue() ? findKnown(walker, state, KnownNote::StorageOfListMadeInSight, list) : std::nullopt;
    if (!noted)
        return std::nullopt;
    return static_cast<uint32_t>(*noted >> shiftOfLengthInKnownElements);
}

Reference elementWrittenInListMadeInSight(Walker& walker, Reference list, uint32_t position)
{
    return elementWrittenInListMadeInSight(walker, walker.state(), list, position);
}

static Reference constantOfElement(Walker& walker, const DataNode& element)
{
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

Reference elementOfConstantArray(Walker& walker, DataID data, uint32_t position)
{
    const auto* array = walker.program().at(data).root.tryAs<DataNode::Array>();
    return array && position < array->elements.size() ? constantOfElement(walker, array->elements[position]) : Reference();
}

Reference elementWrittenInListMadeInSight(Walker& walker, const WalkState& state, Reference list, uint32_t position)
{
    if (!list || !list.isValue() || !findKnown(walker, state, KnownNote::StorageOfListMadeInSight, list))
        return { };
    const Operation& made = walker.code().at(list.id());
    const auto* constant = made.opcode == Opcode::Data && made.data() ? walker.program().at(made.data()).root.tryAs<DataNode::Array>() : nullptr;
    return constant && position < constant->elements.size() ? constantOfElement(walker, constant->elements[position]) : Reference();
}

static constexpr uint32_t mostElementsCompared = 12;

static Reference buildSearchAmong(Walker& walker, std::span<const Reference> elements, Reference sought, AnswerOfSearch answer, Origin origin)
{
    Vector<Walker::JoinArm, mostElementsCompared + 1> arms;
    auto addArm = [&](std::optional<uint32_t> place) {
        if (answer == AnswerOfSearch::Membership) {
            arms.append({ walker.currentBlock(), { walker.constant(PrimitiveKnownToFolding::boolean(place.has_value())) } });
            return;
        }
        Draft number(Opcode::Integer, origin);
        number.setInteger(place ? static_cast<int64_t>(*place) : -1);
        arms.append({ walker.currentBlock(), { walker.constant(WTF::move(number)) } });
    };
    uint32_t place = 0;
    for (Reference element : elements) {
        Reference isEqual = buildOperatorForStrings(walker, Opcode::StrictEq, sought, element, origin);
        if (!isEqual || walker.isUnreachable())
            return { };
        BlockID found = walker.addBlock();
        BlockID next = walker.addBlock();
        walker.branch(isEqual, found, next);
        walker.continueIn(found);
        if (!walker.isUnreachable())
            addArm(place);
        walker.continueIn(next);
        if (walker.isUnreachable())
            break;
        ++place;
    }
    if (!walker.isUnreachable())
        addArm(std::nullopt);
    if (walker.hasFailed() || arms.isEmpty())
        return { };
    Reference met;
    if (!walker.joinArms(arms.span(), walker.addBlock(), std::span<Reference>(&met, 1)))
        return { };
    walker.count(StatisticID::MembershipsComparedInline);
    return met;
}

Reference buildSearchAmongWords(Walker& walker, const DataNode::Array& list, Reference sought, AnswerOfSearch answer, Origin origin)
{
    Vector<StringID, 16> words;
    Vector<uint32_t, 16> places;
    for (size_t place = 0; place < list.elements.size(); ++place) {
        const auto* word = list.elements[place].tryAs<DataNode::String>();
        if (!word)
            return { };
        if (words.contains(word->value))
            continue;
        words.append(word->value);
        places.append(place);
    }
    if (words.isEmpty() || walker.isUnreachable())
        return { };

    auto answerAt = [&](std::optional<uint32_t> place) -> Reference {
        if (answer == AnswerOfSearch::Membership)
            return walker.constant(PrimitiveKnownToFolding::boolean(place.has_value()));
        return integerForStrings(walker, place ? static_cast<int64_t>(*place) : -1);
    };
    Vector<BlockID, 16> foundAt;
    for (size_t place = 0; place < words.size(); ++place)
        foundAt.append(walker.addBlock());
    BlockID missing = walker.addBlock();
    origin.isTailCall = false;
    walker.switchOnString(sought, words.span(), foundAt.span(), missing, origin);
    Vector<Walker::JoinArm, 16> arms;
    auto addArm = [&](BlockID block, std::optional<uint32_t> place) {
        walker.continueIn(block);
        if (!walker.isUnreachable() && !walker.hasFailed())
            arms.append({ walker.currentBlock(), { answerAt(place) } });
    };
    for (size_t place = 0; place < words.size(); ++place)
        addArm(foundAt[place], places[place]);
    addArm(missing, std::nullopt);
    if (walker.hasFailed() || arms.isEmpty())
        return { };
    Reference met;
    if (!walker.joinArms(arms.span(), walker.addBlock(), std::span<Reference>(&met, 1)))
        return { };
    walker.count(StatisticID::SearchesOfWordsSwitched);
    return met;
}

Reference buildSearchInWrittenList(Walker& walker, DataID data, Reference sought, AnswerOfSearch answer, Origin origin)
{
    const auto* list = walker.program().at(data).root.tryAs<DataNode::Array>();
    if (!list || !sought || walker.isUnreachable())
        return { };
    if (isShortStringList(*list))
        return buildSearchAmongWords(walker, *list, sought, answer, origin);
    if (list->elements.size() > mostElementsComparedInLoop)
        return { };
    Vector<Reference, mostElementsComparedInLoop> elements;
    for (const DataNode& node : list->elements) {
        Reference element = constantOfElement(walker, node);
        if (!element)
            return { };
        elements.append(element);
    }
    origin.isTailCall = false;
    return buildSearchAmong(walker, elements.span(), sought, answer, origin);
}

Reference buildSearchInKnownArray(Walker& walker, Reference list, Reference sought, AnswerOfSearch answer, Origin origin)
{
    if (!list || !list.isValue() || !sought || walker.isUnreachable())
        return { };
    list = Reference::value(underlyingCellOf(walker.code(), list.id()));
    std::optional<uint32_t> length = lengthOfListMadeInSight(walker, list);
    if (!length || *length > (walker.isInLoop() ? mostElementsComparedInLoop : mostElementsCompared))
        return { };
    Vector<Reference, mostElementsCompared> elements;
    for (uint32_t position = 0; position < *length; ++position) {
        Reference element = elementWrittenInListMadeInSight(walker, list, position);
        if (!element)
            return { };
        elements.append(element);
    }

    checkLengthOfListMadeInSight(walker, list, *length, origin);
    origin.isTailCall = false;
    return buildSearchAmong(walker, elements.span(), sought, answer, origin);
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
