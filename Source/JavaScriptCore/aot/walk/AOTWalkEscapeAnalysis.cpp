/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTCellFormat.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTRepresentation.h"
#include "AOTSweep.h"
#include "AOTWalkKnown.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses usesThatKeepFreshObjectHere {
    OperandUse::Inspects,
    OperandUse::ReadsContents,
    OperandUse::RetainsContents,
    OperandUse::WritesContents,
    OperandUse::MovesElements,
    OperandUse::StoresByKey,
    OperandUse::DeletesByKey,
    OperandUse::AssignsFrom,
    OperandUse::ChangesIntegrity,
};

namespace {

struct FreshOperandOfEscapeAnalysis {
    Reference object;
    uint32_t operandIndex { 0 };
    bool isHeldByClosure { false };
};

} // anonymous namespace

static const LinkTables& linkTablesForEscapeAnalysis(const Walker& walker)
{
    return walker.facts().linked();
}

static Reference freshObjectNamedBy(Walker& walker, Reference name)
{
    if (!name || !name.isValue())
        return { };
    const Code& code = walker.code();
    ValueID cell = underlyingCellOf(code, name.id());
    if (code.isConstant(cell))
        return { };
    Reference object = Reference::value(cell);
    return walker.state().isFresh(object) ? object : Reference();
}

static Reference valueHeldByFreshObject(const Code& code, ValueID held)
{
    while (conversionsOfForm.contains(code.at(held).opcode))
        held = code.at(held).operand(0);
    if (code.at(held).representation == Representation::Void)
        return { };
    return code.isConstant(held) ? Reference::constant(held) : Reference::value(held);
}

static void buildByLinksBehindEscapeAnalysis(Walker& walker, const BodyValue& value)
{
    bool isBuilt = (walker.has(Mechanism::BuiltinCalls) && callBuiltin(walker, value))
        || (walker.has(Mechanism::Inlining) && inlineCallee(walker, value))
        || (walker.has(Mechanism::DirectCalls) && callDirectly(walker, value))
        || (walker.has(Mechanism::TypedOperations) && buildTypedOperation(walker, value))
        || replaceOperation(walker, value);
    if (!isBuilt)
        walker.copy(value);
}

static Reference rootOfObjectBuiltFor(Walker& walker, const BodyValue& value, uint32_t numberOfRecordsBefore)
{
    Reference name = walker.referenceOf(value.id());
    if (!name || !name.isValue())
        return { };
    const Code& code = walker.code();
    ValueID cell = underlyingCellOf(code, name.id());
    if (code.isConstant(cell) || cell.index() < numberOfRecordsBefore)
        return { };
    return Reference::value(cell);
}

static ALWAYS_INLINE bool givesObjectThatNothingElseNames(const Operation& made)
{
    Opcode opcode = made.opcode;
    EffectTraits traits = made.traits();
    if (!allocationsOfEscapeAnalysis.contains(opcode) || traits.mayWriteObservableState() || traits.mayRunOtherCode())
        return false;

    if (opcode == Opcode::NewClosure) {
        if (made.isHeldByClosureTable() || made.isMadeOnDemand())
            return false;
        switch (made.closureForm()) {
        case ClosureForm::Full:
        case ClosureForm::Slim:
            return true;
        case ClosureForm::Deferred:
        case ClosureForm::Singleton:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        if (operandUsesOf(opcode, operandIndex).contains(OperandUse::Returns))
            return false;
    }
    return true;
}

static Reference keyOfPropertiesAsListed()
{
    return Reference::postponed(Reference::indexMask - 2);
}

static void noteWhatNewObjectFillsIn(Walker& walker, Reference object, const Operation& made, size_t numberOfFieldsWritten)
{
    const Program& program = walker.program();
    LayoutID layout = made.layout();
    if (!walker.plan().layout(layout).hasSlots())
        return;

    WalkState& state = walker.state();
    std::span<const FieldIndex> fillers = walker.fillersOfAllocation(object);
    std::optional<FieldIndex> environmentOfClosures = walker.plan().layout(layout).environmentOfDeferredClosures;
    size_t numberOfNamesListed = made.fields().size() - (environmentOfClosures && !made.fields().empty() && made.fields().back() == *environmentOfClosures);
    bool isAsAllocated = fillers.empty() && (numberOfFieldsWritten == SIZE_MAX || numberOfNamesListed == numberOfFieldsWritten);
    if (made.prototypeOperand() && program.prototypeMaySelectOtherClass(layout)) {
        std::optional<PlacedAllocation> placed = isAsAllocated ? walker.plan().allocation(program, layout, made.fields()) : std::nullopt;
        if (placed && placed->orderWord == unorderedOrderWord)
            state.noteOrderWord(object, unorderedOrderWord);
        return;
    }

    bool isEnvironment = program.at(layout).isEnvironment();
    if (isEnvironment)
        walker.noteThatUnitMakesScope();
    state.noteShape(object, isEnvironment ? walker.types().forEnvironment(layout) : walker.types().forLayout(layout));
    if (!isEnvironment) {
        OptionSet<CellFact> facts { CellFact::NotFrozen };
        if (!made.prototypeOperand())
            facts.add(CellFact::HasLayoutPrototype);
        if (isAsAllocated)
            facts.add(CellFact::NoFieldOutsideSlot);
        state.recordCellFacts(object, facts);
        std::optional<PlacedAllocation> placed = isAsAllocated ? walker.plan().allocation(program, layout, made.fields()) : std::nullopt;
        if (placed)
            state.noteOrderWord(object, placed->orderWord);
    }

    auto fields = made.fields();
    std::span<const ValueID> values = made.valuesOperands();
    if (fields.size() != values.size())
        return;
    const Code& code = walker.code();
    for (size_t index = 0; index < fields.size(); ++index) {
        if (!values[index] || !walker.plan().slot(layout, fields[index]).hasStorage() || std::ranges::find(fillers, fields[index]) != fillers.end()
            || walker.plan().layout(layout).environmentOfDeferredClosures == fields[index])
            continue;
        bool defersClosure = walker.plan().slot(layout, fields[index]).representation == SlotRepresentation::DeferredClosure;
        if (defersClosure && !isClosureLeftUnmade(walker, Reference::value(values[index])))
            continue;
        FieldReference field { layout, fields[index] };
        Reference value = defersClosure ? Reference::value(values[index]) : valueHeldByFreshObject(code, values[index]);
        if (isEnvironment)
            walker.noteEnvironmentField(object, field, value);
        else
            state.noteSlot(object, field, value);
    }
    if (!isEnvironment && fields.size() == numberOfFieldsWritten && fillers.empty())
        state.noteElement(object, keyOfPropertiesAsListed(), object);
}

static Reference positionInFreshList(Walker& walker, uint32_t position)
{
    Draft index(Opcode::Integer);
    index.setInteger(static_cast<int64_t>(position));
    return walker.constant(WTF::move(index));
}

static Reference keyOfLengthOfFreshList(Walker& walker)
{
    Draft key(Opcode::String);
    key.setString(walker.program().stringOf(BuiltinNames::length));
    return walker.constant(WTF::move(key));
}

static void noteWhatNewArrayFillsIn(Walker& walker, Reference object, const Operation& made)
{
    WalkState& state = walker.state();
    std::span<const uint32_t> holes = made.holes();
    uint32_t position = 0;
    for (ValueID element : made.elementsOperands()) {
        for (; !holes.empty() && holes.front() == position; holes = holes.subspan(1))
            ++position;
        if (Reference key = positionInFreshList(walker, position++))
            state.noteElement(object, key, valueHeldByFreshObject(walker.code(), element));
    }
    state.noteElement(object, keyOfLengthOfFreshList(walker), positionInFreshList(walker, position + static_cast<uint32_t>(holes.size())));
    state.noteShape(object, made.type);
}

static Reference variableInMappedArguments(uint32_t parameter)
{
    return Reference::postponed(parameter);
}

static Reference keyOfListBehindParameters()
{
    return Reference::postponed(Reference::indexMask - 1);
}

static void noteWhatArgumentsObjectHolds(Walker& walker, Reference object, const Operation& made)
{
    WalkState& state = walker.state();
    const Code& code = walker.code();
    bool isMapped = made.opcode == Opcode::CreateMappedArguments;
    uint32_t position = 0;
    for (ValueID parameter : made.parametersOperands()) {
        state.noteElement(object, isMapped ? variableInMappedArguments(position) : positionInFreshList(walker, position), valueHeldByFreshObject(code, parameter));
        ++position;
    }

    Reference list = valueHeldByFreshObject(code, made.restOperand());
    if (std::optional<Vector<Reference, 8>> others = elementsOfFreshList(walker, list)) {
        for (Reference other : *others)
            state.noteElement(object, positionInFreshList(walker, position++), other);
    }
    FunctionID function = code.inlineFrames()[made.origin().inlineFrame].callee;
    if (function && walker.program().at(function).has(FunctionFlags::HasHiddenRest))
        state.noteElement(object, keyOfListBehindParameters(), list);
    state.noteElement(object, keyOfLengthOfFreshList(walker), valueHeldByFreshObject(code, made.countOperand()));
    state.noteShape(object, made.type);
}

static size_t numberOfFieldsWrittenForEscapeAnalysis(const Walker& walker, const BodyValue& allocation, const Operation& made)
{
    if (allocation.opcode() == Opcode::NewObject)
        return allocation.as<Opcode::NewObject>().fields().size() + walker.numberOfValuesThatBirthListsFromBehind();
    if (allocation.opcode() != Opcode::Data)
        return SIZE_MAX;
    DataID data = allocation.as<Opcode::Data>().data();
    const DataNode::Object* constant = data ? walker.program().at(data).root.tryAs<DataNode::Object>() : nullptr;
    return constant && constant->members.size() != made.fields().size() ? constant->members.size() : SIZE_MAX;
}

static_assert(allocationsOfEscapeAnalysis.contains(Opcode::NewArrayLike));

static bool noteWhatListMadeWithLengthHolds(Walker& walker, Reference object, const Operation& made)
{
    const Code& code = walker.code();
    ValueID length = made.lengthOperand();
    if (made.sourceOperand() || !length || !code.isConstant(length) || code.at(length).opcode != Opcode::Integer)
        return false;
    int64_t known = code.at(length).integer();
    if (known < 0 || known > static_cast<int64_t>(UINT32_MAX))
        return false;
    WalkState& state = walker.state();
    state.noteElement(object, keyOfLengthOfFreshList(walker), positionInFreshList(walker, static_cast<uint32_t>(known)));
    state.noteShape(object, made.type);
    return true;
}

static bool buildAllocationOfFreshObject(Walker& walker, const BodyValue& allocation)
{
    uint32_t numberOfRecordsBefore = walker.code().numberOfValues();
    buildByLinksBehindEscapeAnalysis(walker, allocation);

    Reference object = rootOfObjectBuiltFor(walker, allocation, numberOfRecordsBefore);
    if (!object)
        return true;
    const Operation& made = walker.code().at(object.id());
    if (!givesObjectThatNothingElseNames(made))
        return true;

    if (made.opcode == Opcode::NewObject)
        noteWhatNewObjectFillsIn(walker, object, made, numberOfFieldsWrittenForEscapeAnalysis(walker, allocation, made));
    else if (made.opcode == Opcode::NewArray)
        noteWhatNewArrayFillsIn(walker, object, made);
    else if (argumentsObjectsOfEscapeAnalysis.contains(made.opcode))
        noteWhatArgumentsObjectHolds(walker, object, made);
    else if ((made.opcode == Opcode::Data && hasKind(walker.types(), made.type, OperandKind::Array)) || made.opcode == Opcode::NewRegExp)
        walker.state().noteShape(object, made.type);
    walker.state().noteFresh(object);
    noteStorageOfListMade(walker, object);
    walker.bind(allocation.id(), object);
    return true;
}

void noteFreshList(Walker& walker, Reference list)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !list || !list.isValue())
        return;
    const Operation& made = walker.code().at(list.id());
    if ((made.opcode != Opcode::NewArray && made.opcode != Opcode::NewArrayLike) || !givesObjectThatNothingElseNames(made))
        return;
    if (made.opcode == Opcode::NewArrayLike) {
        if (noteWhatListMadeWithLengthHolds(walker, list, made))
            walker.state().noteFresh(list);
        return;
    }
    noteWhatNewArrayFillsIn(walker, list, made);
    walker.state().noteFresh(list);
    noteStorageOfListMade(walker, list);
}

void noteFreshObject(Walker& walker, Reference name, size_t numberOfFieldsWritten)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !name || !name.isValue())
        return;
    ValueID cell = underlyingCellOf(walker.code(), name.id());
    const Operation& made = walker.code().at(cell);
    if (walker.code().isConstant(cell) || made.opcode != Opcode::NewObject || !givesObjectThatNothingElseNames(made))
        return;
    noteWhatNewObjectFillsIn(walker, Reference::value(cell), made, numberOfFieldsWritten);
    walker.state().noteFresh(Reference::value(cell));
}

static std::optional<std::span<const FieldIndex>> fieldsListedByOrderWordInEscapeAnalysis(const Walker& walker, LayoutID layout, uint32_t orderWord)
{
    const KeyOrders& orders = walker.plan().keyOrders();
    uint32_t count = countOf(orderWord);
    if (orderWord & ~orderAndCountMask || isOrderStoredOutOfLine(orderWord) || !orderWordTellsOwnKeysOf(walker, layout))
        return std::nullopt;
    if (!count)
        return std::span<const FieldIndex>();
    if (orderOf(orderWord) >= orders.numberOfOrdersOf(layout) || count > orders.order(layout, orderOf(orderWord)).size())
        return std::nullopt;
    return orders.order(layout, orderOf(orderWord)).first(count);
}

std::optional<Vector<PropertyOfFreshObject, 8>> propertiesOfFreshObject(Walker& walker, Reference name)
{
    Reference object = freshObjectNamedBy(walker, name);
    WalkState& state = walker.state();
    if (!object || !state.shapeOf(object) || !state.element(object, keyOfPropertiesAsListed()))
        return std::nullopt;
    const Operation& made = walker.code().at(object.id());
    const Program& program = walker.program();
    LayoutID layout = made.layout();
    if (made.opcode != Opcode::NewObject || program.at(layout).kind != LayoutKind::Record || !walker.plan().slots().layout(layout).appendedKeys.isEmpty())
        return std::nullopt;
    std::optional<PlacedAllocation> placed = walker.plan().allocation(program, layout, made.fields());
    std::optional<uint32_t> orderWord = state.orderWordOf(object);
    if (!placed || !orderWord)
        return std::nullopt;
    std::span<const FieldIndex> created = made.fields();
    if (*orderWord != placed->orderWord) {
        std::optional<std::span<const FieldIndex>> createdSince = fieldsListedByOrderWordInEscapeAnalysis(walker, layout, *orderWord);
        if (!createdSince || !std::ranges::all_of(created, [&](FieldIndex listed) { return std::ranges::contains(*createdSince, listed); }))
            return std::nullopt;
        created = *createdSince;
    }

    Vector<PropertyOfFreshObject, 8> properties;
    for (FieldIndex listed : created) {
        FieldReference field { layout, listed };
        Reference value = state.slot(object, field);
        TypeID type = value && !isClosureLeftUnmade(walker, value) ? walker.typeOf(value) : TypeID();
        if (!type || (walker.types().at(type).primitives & Primitive::Absent))
            return std::nullopt;
        properties.append({ field, value });
    }
    return properties;
}

static constexpr OpcodeSet readsThatProveNoSlotFilledInEscapeAnalysis { Opcode::GetSlotOrUndefined, Opcode::GetDeferredClosureSlotOrUndefined };

std::optional<ObjectsThatMeet> findObjectsThatMeetAsOne(Walker& walker, std::span<const WalkState* const> ways, std::span<const Reference> arguments, uint32_t firstRecordBehindParting)
{
    const Code& code = walker.code();
    ObjectsThatMeet met;
    LayoutID layout;
    Vector<Reference, 4> cells;
    for (size_t way = 0; way < ways.size(); ++way) {
        if (!arguments[way] || !arguments[way].isValue())
            return std::nullopt;
        ValueID cell = underlyingCellOf(code, arguments[way].id());
        if (code.isConstant(cell) || cell.index() < firstRecordBehindParting)
            return std::nullopt;
        const Operation& made = code.at(cell);
        Reference object = Reference::value(cell);
        const WalkState& state = *ways[way];
        if (made.opcode != Opcode::NewObject || made.prototypeOperand() || !state.isFresh(object))
            return std::nullopt;
        TypeID shape = state.shapeOf(object);
        std::optional<uint32_t> orderWord = state.orderWordOf(object);
        if (!shape || !orderWord || walker.program().at(made.layout()).isEnvironment())
            return std::nullopt;
        if (!way) {
            layout = made.layout();
            met.shape = shape;
            met.orderWord = *orderWord;
            met.facts = state.cellFacts(object);
        } else if (made.layout() != layout || shape != met.shape || *orderWord != met.orderWord)
            return std::nullopt;
        else
            met.facts = met.facts & state.cellFacts(object);
        cells.append(object);
    }

    for (const WalkState::SlotOfObject& first : ways[0]->listSlotsOf(cells[0])) {
        ObjectsThatMeet::Slot slot { first.field, first.value, { } };
        bool everyWayKnowsIt = true;
        for (size_t way = 0; way < ways.size() && everyWayKnowsIt; ++way) {
            Reference value = ways[way]->slot(cells[way], first.field);
            TypeID type = value && value.isRecord() && !isClosureLeftUnmade(walker, value) ? walker.typeOf(value) : TypeID();
            everyWayKnowsIt = type && !(walker.types().at(type).primitives & Primitive::Absent)
                && !(value.isValue() && (readsThatProveNoSlotFilledInEscapeAnalysis.contains(code.at(value.id()).opcode) || underlyingCellOf(code, value.id()) == cells[way].id()));
            if (value != first.value)
                slot.valueOnEveryWay = { };
            slot.valuesOfWays.append(value);
        }
        if (everyWayKnowsIt)
            met.slots.append(WTF::move(slot));
    }
    return met;
}

void noteObjectsThatHaveMetAsOne(Walker& walker, Reference parameter, const ObjectsThatMeet& met, std::span<const Reference> parametersOfSlots)
{
    WalkState& state = walker.state();
    state.noteShape(parameter, met.shape);
    state.recordCellFacts(parameter, met.facts);
    state.noteOrderWord(parameter, met.orderWord);
    size_t numberOfParametersTaken = 0;
    for (const ObjectsThatMeet::Slot& slot : met.slots)
        state.noteSlot(parameter, slot.field, slot.valueOnEveryWay ? slot.valueOnEveryWay : parametersOfSlots[numberOfParametersTaken++]);
    state.noteFresh(parameter);
    walker.recordMergedFreshObject(parameter.id());
}

void noteListMadeByCaller(Walker& walker, Reference list)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !list || !list.isValue())
        return;
    walker.state().noteShape(list, walker.code().at(list.id()).type);
    walker.state().noteFresh(list);
}

static Reference rootOfListInEscapeAnalysis(Walker& walker, Reference name)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !name || !name.isValue())
        return { };
    ValueID cell = underlyingCellOf(walker.code(), name.id());
    return walker.code().isConstant(cell) ? Reference() : Reference::value(cell);
}

void markObjectPristine(Walker& walker, Reference name)
{
    if (Reference list = rootOfListInEscapeAnalysis(walker, name))
        walker.state().noteShape(list, walker.code().at(list.id()).type);
}

bool isKnownPristine(Walker& walker, Reference name)
{
    Reference list = rootOfListInEscapeAnalysis(walker, name);
    return list && walker.state().shapeOf(list);
}

std::optional<uint32_t> lengthOfFreshList(Walker& walker, Reference name)
{
    Reference list = freshObjectNamedBy(walker, name);
    Reference length = list ? walker.state().element(list, keyOfLengthOfFreshList(walker)) : Reference();
    if (!length || !length.isConstant() || walker.code().at(length.id()).opcode != Opcode::Integer)
        return std::nullopt;
    return static_cast<uint32_t>(walker.code().at(length.id()).integer());
}

Reference elementOfFreshList(Walker& walker, Reference name, uint32_t position)
{
    Reference list = freshObjectNamedBy(walker, name);
    return list ? walker.state().element(list, positionInFreshList(walker, position)) : Reference();
}

std::optional<Vector<Reference, 8>> elementsOfFreshList(Walker& walker, Reference name)
{
    std::optional<uint32_t> length = lengthOfFreshList(walker, name);
    if (!length)
        return std::nullopt;
    Vector<Reference, 8> elements;
    for (uint32_t position = 0; position < *length; ++position) {
        Reference element = elementOfFreshList(walker, name, position);
        if (!element)
            return std::nullopt;
        elements.append(element);
    }
    return elements;
}

std::optional<uint32_t> lengthOfListInSight(Walker& walker, Reference name)
{
    Reference list = rootOfListInEscapeAnalysis(walker, name);
    Reference length = list ? walker.state().element(list, keyOfLengthOfFreshList(walker)) : Reference();
    if (!length || !length.isConstant() || walker.code().at(length.id()).opcode != Opcode::Integer)
        return std::nullopt;
    return static_cast<uint32_t>(walker.code().at(length.id()).integer());
}

Reference elementOfListInSight(Walker& walker, Reference name, uint32_t position)
{
    Reference list = rootOfListInEscapeAnalysis(walker, name);
    return list ? walker.state().element(list, positionInFreshList(walker, position)) : Reference();
}

bool isFreshPristineArray(Walker& walker, Reference name)
{
    Reference list = freshObjectNamedBy(walker, name);
    return list && walker.state().shapeOf(list) && hasKind(walker.types(), walker.typeOf(list), OperandKind::Array);
}

static Reference freshArgumentsNamedBy(Walker& walker, Reference name)
{
    Reference object = freshObjectNamedBy(walker, name);
    return object && argumentsObjectsOfEscapeAnalysis.contains(walker.code().at(object.id()).opcode) ? object : Reference();
}

static bool mayBeAbsentInEscapeAnalysis(Walker& walker, Reference value)
{
    TypeID type = walker.typeOf(value);
    return !type || (walker.types().at(type).primitives & Primitive::Absent);
}

Reference lengthOfFreshArguments(Walker& walker, Reference name)
{
    Reference object = freshArgumentsNamedBy(walker, name);
    return object ? walker.state().element(object, keyOfLengthOfFreshList(walker)) : Reference();
}

static bool countOfFreshArgumentsIsAbove(Walker& walker, Reference object, uint32_t position)
{
    Reference count = walker.state().element(object, keyOfLengthOfFreshList(walker));
    TypeID type = count ? walker.typeOf(count) : TypeID();
    return type && walker.types().at(type).isInteger() && walker.types().at(type).integers.minimum > position;
}

Reference elementOfFreshArguments(Walker& walker, Reference name, uint32_t position)
{
    Reference object = freshArgumentsNamedBy(walker, name);
    if (!object)
        return { };
    WalkState& state = walker.state();
    const Operation& made = walker.code().at(object.id());
    Reference held = state.element(object, positionInFreshList(walker, position));
    if (made.opcode == Opcode::CreateMappedArguments && position < made.parametersOperands().size()) {
        held = state.shapeOf(object) ? state.element(object, variableInMappedArguments(position)) : Reference();
        bool isAsAtEntry = held == valueHeldByFreshObject(walker.code(), made.parametersOperands()[position]);
        if (!isAsAtEntry && !countOfFreshArgumentsIsAbove(walker, object, position))
            return { };
    }
    return held && !mayBeAbsentInEscapeAnalysis(walker, held) ? held : Reference();
}

std::optional<ListBehindParametersOfArguments> listBehindParametersOfFreshArguments(Walker& walker, Reference name, bool forItsOwnElementsAlone)
{
    Reference object = freshArgumentsNamedBy(walker, name);
    Reference list = object && walker.state().shapeOf(object) ? walker.state().element(object, keyOfListBehindParameters()) : Reference();
    if (!list || !(forItsOwnElementsAlone ? !!freshObjectNamedBy(walker, list) : isFreshPristineArray(walker, list)))
        return std::nullopt;
    return ListBehindParametersOfArguments { list, static_cast<uint32_t>(walker.code().at(object.id()).parametersOperands().size()) };
}

bool positionsOfFreshArgumentsAreInSight(Walker& walker, Reference name, int64_t lowest)
{
    std::optional<ListBehindParametersOfArguments> behind = listBehindParametersOfFreshArguments(walker, name);
    if (!behind || lowest < 0)
        return false;
    if (lowest >= behind->firstPosition)
        return true;
    if (behind->firstPosition - lowest > mostParametersOfArgumentsAskedOneByOne)
        return false;
    for (uint32_t position = static_cast<uint32_t>(lowest); position < behind->firstPosition; ++position) {
        if (!elementOfFreshArguments(walker, name, position))
            return false;
    }
    return true;
}

Reference listStandingForFreshArguments(Walker& walker, Reference name)
{
    std::optional<ListBehindParametersOfArguments> behind = listBehindParametersOfFreshArguments(walker, name);
    if (!behind || behind->firstPosition || !walker.state().shapeOf(freshArgumentsNamedBy(walker, name)))
        return { };
    return behind->list;
}

std::optional<Vector<Reference, 8>> elementsOfFreshArguments(Walker& walker, Reference name)
{
    Reference object = freshArgumentsNamedBy(walker, name);
    std::optional<uint32_t> count = object && walker.state().shapeOf(object) ? lengthOfFreshList(walker, object) : std::nullopt;
    if (!count)
        return std::nullopt;
    Vector<Reference, 8> elements;
    for (uint32_t position = 0; position < *count; ++position) {
        Reference element = elementOfFreshArguments(walker, object, position);
        if (!element)
            return std::nullopt;
        elements.append(element);
    }
    return elements;
}

static bool answerMappedArgumentFromState(Walker& walker, const BodyValue& read)
{
    Reference object = freshArgumentsNamedBy(walker, walker.referenceOf(read.as<Opcode::GetMappedArgument>().argumentsOperand()));
    Reference held = object ? walker.state().element(object, variableInMappedArguments(read.as<Opcode::GetMappedArgument>().index())) : Reference();
    if (!held || mayBeAbsentInEscapeAnalysis(walker, held))
        return false;
    walker.bind(read.id(), held);
    return true;
}

static bool bindNameToFreshObjectItself(Walker& walker, const BodyValue& name)
{
    Reference object = freshObjectNamedBy(walker, walker.referenceOf(name.operand(0)));
    if (!object)
        return false;
    walker.bind(name.id(), object);
    return true;
}

static bool wasThereBeforeFreshObject(const Code& code, Reference other, Reference object)
{
    if (!other || !other.isRecord())
        return false;
    ValueID root = underlyingCellOf(code, other.id());
    return code.isConstant(root) || root.index() < object.id().index();
}

static bool foldIdentityOfFreshObject(Walker& walker, const BodyValue& comparison)
{
    Reference left = walker.referenceOf(comparison.operand(OperandIndex::StrictEq::left));
    Reference right = walker.referenceOf(comparison.operand(OperandIndex::StrictEq::right));
    Reference freshLeft = freshObjectNamedBy(walker, left);
    Reference freshRight = freshObjectNamedBy(walker, right);
    if (!freshLeft && !freshRight)
        return false;

    bool isSameObject = freshLeft == freshRight;
    if (!freshLeft || !freshRight) {
        if (!wasThereBeforeFreshObject(walker.code(), freshLeft ? right : left, freshLeft ? freshLeft : freshRight))
            return false;
    }

    Draft answer = walker.draftOf(comparison);
    answer.become(Opcode::Boolean);
    answer.setBoolean(isSameObject);
    walker.bind(comparison.id(), walker.constant(WTF::move(answer)));
    return true;
}

static std::optional<FieldReference> fieldThatDeleteClearsInFreshObject(Walker& walker, Reference object, PropertyKeyID key)
{
    LayoutID layout = walker.code().at(object.id()).layout();
    std::optional<FieldReference> field = findField(walker.program(), layout, key);
    if (!field || field->layout != layout)
        return std::nullopt;
    const PlacedSlot& slot = walker.plan().slot(layout, field->field);
    if (!slot.hasStorage() || slot.isOutOfLine() || slot.holdsAccessor || slot.representation == SlotRepresentation::DeferredClosure || !canLeaveSlot(slot.representation))
        return std::nullopt;
    return field;
}

static Reference freshRecordThatHoldsNothingButFields(Walker& walker, Reference name)
{
    Reference object = freshObjectNamedBy(walker, name);
    WalkState& state = walker.state();
    if (!object || !state.shapeOf(object) || !state.orderWordOf(object) || !state.element(object, keyOfPropertiesAsListed()))
        return { };
    const Operation& made = walker.code().at(object.id());
    if (made.opcode != Opcode::NewObject || walker.program().at(made.layout()).kind != LayoutKind::Record || !walker.plan().slots().layout(made.layout()).appendedKeys.isEmpty())
        return { };
    if (!orderWordTellsOwnKeysOf(walker, made.layout()))
        return { };
    return state.cellFacts(object).containsAll({ CellFact::NotFrozen, CellFact::NoFieldOutsideSlot }) ? object : Reference();
}

static bool followDeleteOffFreshObject(Walker& walker, const BodyValue& removal)
{
    if (removal.isFreeName())
        return false;
    Reference object = freshRecordThatHoldsNothingButFields(walker, walker.referenceOf(removal.objectOperand()));
    if (!object)
        return false;

    std::optional<FieldReference> named = findField(walker.program(), walker.code().at(object.id()).layout(), removal.key());
    bool isHeld = !!named;
    if (named) {
        Reference held = walker.state().slot(object, *named);
        TypeID type = held && !isClosureLeftUnmade(walker, held) ? walker.typeOf(held) : TypeID();
        if (!type || (walker.types().at(type).primitives & Primitive::Absent))
            return false;
    }
    if (isHeld) {
        if (!fieldThatDeleteClearsInFreshObject(walker, object, removal.key()))
            return false;
        Draft closed = walker.draftOf(removal);
        walker.chooseConservativeFields(closed);
        Effects effects = Effects::ofReadByNameThatRunsNoCode();
        effects.writes = LocationMasks::top().restrictedTo(perObjectHeaps | unobservableWriteHeaps);
        if (!walker.build(WTF::move(closed), &effects) || walker.isUnreachable())
            return true;
    }
    walker.count(isHeld ? StatisticID::FreshObjectDeletesTracked : StatisticID::FreshObjectDeletesFolded);
    Draft yes = walker.draftOf(removal);
    yes.become(Opcode::Boolean);
    yes.setBoolean(true);
    walker.bind(removal.id(), walker.constant(WTF::move(yes)));
    return true;
}

bool buildFreshObjectOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!opcodesOfEscapeAnalysis.contains(opcode))
        return false;

    if (opcode == Opcode::PropDelete)
        return walker.state().holdsFreshObjects() && followDeleteOffFreshObject(walker, value);
    if (opcode == Opcode::StrictEq)
        return walker.state().holdsFreshObjects() && foldIdentityOfFreshObject(walker, value);
    if (opcode == Opcode::GetMappedArgument)
        return answerMappedArgumentFromState(walker, value);
    if (namesOfOperandInEscapeAnalysis.contains(opcode))
        return walker.state().holdsFreshObjects() && bindNameToFreshObjectItself(walker, value);
    return buildAllocationOfFreshObject(walker, value);
}

static Effects effectsOfDraftForEscapeAnalysis(const Program& program, const Draft& draft, bool doesGenericOperation)
{
    Opcode opcode = draft.opcode;
    Effects effects = declaredEffectsOfOperation(program, opcode, draft);
    if (doesGenericOperation)
        effects.merge(declaredBoundOf(*genericOperationOf(opcode)));
    if (operandsMayRunCode(opcode))
        effects.merge(Effects::unknownCode());
    return effects;
}

static bool useLetsFreshObjectLeave(const OperandInfo& info, bool runsOtherCode)
{
    if (!info.uses.containsOnly(usesThatKeepFreshObjectHere) || !info.protocols.isEmpty())
        return true;
    return runsOtherCode && !info.uses.contains(OperandUse::ReadsContents);
}

static constexpr OpcodeSet opcodesThatMayBorrowScopesInEscapeAnalysis { Opcode::NewClosure, Opcode::Call, Opcode::CallResolved, Opcode::CallDirect };
static constexpr OpcodeSet appendsFollowedByEscapeAnalysis { Opcode::AppendElement, Opcode::AppendHole };
static_assert(!OperandIndex::AppendElement::array && !OperandIndex::AppendHole::array);

static bool followAppendToFreshList(Walker& walker, const Draft& append, const FreshOperandOfEscapeAnalysis& written)
{
    std::optional<uint32_t> length = lengthOfFreshList(walker, written.object);
    if (written.operandIndex || !length || *length == UINT32_MAX || !isKnownPristine(walker, written.object))
        return false;

    WalkState& state = walker.state();
    state.markEscaped(written.object);
    if (append.opcode == Opcode::AppendElement && append.valueOperand().isRecord())
        state.noteElement(written.object, positionInFreshList(walker, *length), valueHeldByFreshObject(walker.code(), append.valueOperand().id()));
    state.noteElement(written.object, keyOfLengthOfFreshList(walker), positionInFreshList(walker, *length + 1));
    state.noteFresh(written.object);
    return true;
}

static bool followStoreUnderKnownIndexIntoFreshList(Walker& walker, const Draft& store, const FreshOperandOfEscapeAnalysis& written)
{
    if (written.operandIndex != OperandIndex::PutArrayElement::array || !store.fillsPlaceOfListMadeHere() || !store.valueOperand().isRecord())
        return false;
    std::optional<uint32_t> length = lengthOfFreshList(walker, written.object);
    Reference position = store.positionOperand();
    if (!length || !isKnownPristine(walker, written.object) || !position || !position.isConstant() || walker.code().at(position.id()).opcode != Opcode::Integer)
        return false;
    int64_t index = walker.code().at(position.id()).integer();
    if (index < 0 || index >= static_cast<int64_t>(UINT32_MAX))
        return false;
    WalkState& state = walker.state();
    state.markEscaped(written.object);
    state.noteElement(written.object, positionInFreshList(walker, static_cast<uint32_t>(index)), valueHeldByFreshObject(walker.code(), store.valueOperand().id()));
    if (index >= static_cast<int64_t>(*length))
        state.noteElement(written.object, keyOfLengthOfFreshList(walker), positionInFreshList(walker, static_cast<uint32_t>(index) + 1));
    state.noteFresh(written.object);
    walker.count(StatisticID::StoresFollowedIntoFreshLists);
    return true;
}

static bool followStoreToMappedArgument(Walker& walker, const Draft& store, const FreshOperandOfEscapeAnalysis& written)
{
    if (written.operandIndex != OperandIndex::SetMappedArgument::arguments || !store.valueOperand().isRecord())
        return false;
    WalkState& state = walker.state();
    state.markEscaped(written.object);
    state.noteElement(written.object, variableInMappedArguments(store.index()), valueHeldByFreshObject(walker.code(), store.valueOperand().id()));
    state.noteFresh(written.object);
    return true;
}

static bool followStoreIntoFreshObject(Walker& walker, const Draft& store, const FreshOperandOfEscapeAnalysis& written)
{
    Opcode opcode = store.opcode;
    if (appendsFollowedByEscapeAnalysis.contains(opcode))
        return followAppendToFreshList(walker, store, written);
    if (opcode == Opcode::PutArrayElement)
        return followStoreUnderKnownIndexIntoFreshList(walker, store, written);
    if (opcode == Opcode::SetMappedArgument)
        return followStoreToMappedArgument(walker, store, written);
    if (opcode == Opcode::PropDelete) {
        if (written.operandIndex != OperandIndex::PropDelete::object || freshRecordThatHoldsNothingButFields(walker, written.object) != written.object)
            return false;
        std::optional<FieldReference> cleared = fieldThatDeleteClearsInFreshObject(walker, written.object, store.key());
        if (!cleared)
            return false;
        WalkState& state = walker.state();
        state.noteSlot(written.object, *cleared, Reference());
        state.noteOrderWord(written.object, std::nullopt);
        state.noteElement(written.object, keyOfPropertiesAsListed(), Reference());
        state.invalidateCellFacts(written.object, CellFact::BearsNoMark);
        return true;
    }
    bool storesIntoEnvironment = opcode == Opcode::PutEnvironmentField;
    bool storesIntoSlot = accessClassOf(opcode) == AccessClass::Slot && accessOf(opcode) == Access::Store;
    if (!storesIntoEnvironment && !storesIntoSlot)
        return false;

    Reference stored;
    for (uint32_t operandIndex = 0; operandIndex < store.numberOfNamedOperands(); ++operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (info.uses.contains(OperandUse::Retains) && info.retainedIn == RetainedIn::Operand && info.retainingOperand && *info.retainingOperand == written.operandIndex)
            stored = store.operand(operandIndex);
    }
    if (!stored || !stored.isRecord())
        return false;

    WalkState& state = walker.state();
    Reference value = valueHeldByFreshObject(walker.code(), stored.id());
    if (storesIntoEnvironment) {
        walker.noteEnvironmentField(written.object, FieldReference { store.layout(), store.field() }, value);
        return true;
    }

    TypeID type = walker.typeOf(written.object);
    if (!type)
        return false;
    TypeArena& types = walker.types();
    LayoutID layout = singleLayoutOf(types, types.at(type));
    if (!layout)
        return false;
    std::optional<FieldReference> field = findField(walker.program(), layout, store.key());
    if (!field)
        return false;
    state.noteSlot(written.object, *field, value);
    if (opcode == Opcode::PutSlotOrAdd)
        state.noteOrderWord(written.object, store.orderWordAfter() ? std::optional<uint32_t>(store.orderWordAfter()->value()) : std::nullopt);
    return true;
}

bool nothingButOwnFieldsAnswersFor(const Program& program, const Operation& made)
{
    if (made.opcode != Opcode::NewObject || made.prototypeOperand())
        return false;
    const Layout& layout = program.at(made.layout());
    return layout.kind == LayoutKind::Record && !layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction) && !layout.has(LayoutFlags::IsOfCreatedObject) && !objectPrototypeMayGainName(program);
}

static bool calleeOnlyLooksAtFreshCell(Walker& walker, const Draft& call, const FreshOperandOfEscapeAnalysis& operand)
{
    uint32_t numberOfNamedOperands = call.numberOfNamedOperands();
    if (call.opcode != Opcode::CallDirect || operand.isHeldByClosure || operand.operandIndex < numberOfNamedOperands || !returnsResultOfFunction(call.role()) || call.numberOfBoundArguments())
        return false;
    return linkTablesForEscapeAnalysis(walker).onlyLooksAtWhatArrivesAt(call.function(), operand.operandIndex - numberOfNamedOperands) && nothingButOwnFieldsAnswersFor(walker.program(), walker.code().at(operand.object.id()));
}

static bool laysNullAboveFreshObject(Walker& walker, const Draft& draft, const FreshOperandOfEscapeAnalysis& operand)
{
    if (draft.opcode != Opcode::SetLiteralPrototype || operand.isHeldByClosure || operand.operandIndex != OperandIndex::SetLiteralPrototype::object)
        return false;
    return walker.typeOf(draft.prototypeOperand()) == TypeTable::null;
}

void markEscapingOperands(Walker& walker, const Draft& draft, const Effects* foundByCaller)
{
    Opcode opcode = draft.opcode;
    if (!walker.state().holdsFreshObjects() || !draft.numberOfOperands() || cellPreservingOpcodes.contains(opcode))
        return;

    bool closuresHoldScopes = walker.closuresHoldScopes();
    static constexpr OpcodeSet appendsToLists { Opcode::AppendElement, Opcode::AppendHole, Opcode::AppendElements, Opcode::AppendMembers };
    bool shouldSkipFreshArrays = walker.shouldIgnoreFreshArrayEscapes() && !appendsToLists.contains(opcode);
    Vector<FreshOperandOfEscapeAnalysis, 4> freshOperands;
    draft.forEachOperand([&](Reference operand, uint32_t operandIndex) {
        if (Reference object = freshObjectNamedBy(walker, operand); object && !(shouldSkipFreshArrays && isFreshPristineArray(walker, operand)))
            freshOperands.append({ object, operandIndex });
        if (!closuresHoldScopes || !operand || !operand.isValue())
            return;
        const Code& code = walker.code();
        const Operation& named = code.at(underlyingCellOf(code, operand.id()));
        if (named.opcode != Opcode::NewClosure)
            return;
        for (ValueID capture : named.capturesOperands()) {
            if (Reference scope = capture && !code.isConstant(capture) ? freshObjectNamedBy(walker, Reference::value(capture)) : Reference())
                freshOperands.append({ scope, operandIndex, true });
        }
    });
    if (freshOperands.isEmpty())
        return;

    WalkState& state = walker.state();
    bool doesGenericOperation = testsItsOwnGuess(draft);
    Effects effects = effectsOfDraftForEscapeAnalysis(walker.program(), draft, doesGenericOperation);
    if (foundByCaller && !doesGenericOperation)
        effects.intersect(*foundByCaller);
    walker.applyUnderstatedEffects(draft, effects);
    bool runsOtherCode = effects.mayRunOtherCode();

    if (opcode == Opcode::CallDirect && returnsResultOfFunction(draft.role()) && !draft.numberOfBoundArguments()) {
        const LinkedFunction& callee = linkTablesForEscapeAnalysis(walker).function(draft.function());
        effects = callee.effectsOnReturn();
        effects.merge(callee.effectsOnThrow());
        walker.applyUnderstatedEffects(draft, effects);
    }

    uint32_t numberOfNamedOperands = draft.numberOfNamedOperands();
    bool someObjectHasLeft = false;
    for (const FreshOperandOfEscapeAnalysis& operand : freshOperands) {
        if (opcode == Opcode::CallDirect && operand.operandIndex >= numberOfNamedOperands && walker.shouldAssumeCalleesRetainNothing())
            continue;
        if (calleeOnlyLooksAtFreshCell(walker, draft, operand) || laysNullAboveFreshObject(walker, draft, operand))
            continue;
        if (walker.unitHasMadeScopes() && opcodesThatMayBorrowScopesInEscapeAnalysis.contains(opcode) && walker.operationOnlyBorrowsScope(draft, operand.object, operand.operandIndex, operand.isHeldByClosure))
            continue;
        bool handsOutWhatClosureHolds = operand.isHeldByClosure && opcode == Opcode::GetCapture;
        if (!doesGenericOperation && !handsOutWhatClosureHolds && !useLetsFreshObjectLeave(operandInfoOf(opcode, std::min(operand.operandIndex, numberOfNamedOperands)), runsOtherCode))
            continue;
        if (walker.unitHasMadeScopes())
            walker.noteThatWalkLetsScopeLeave(operand.object);
        state.markEscaped(operand.object);
        someObjectHasLeft = true;
    }
    if (!effects.mayRunOtherCode() && effects.writes.containsOnly(unobservableWriteHeaps))
        return;

    Vector<Reference, 4> lent;
    for (const FreshOperandOfEscapeAnalysis& operand : freshOperands) {
        if (!state.isFresh(operand.object) || (!operand.isHeldByClosure && followStoreIntoFreshObject(walker, draft, operand)))
            continue;
        state.markEscaped(operand.object);
        lent.append(operand.object);
    }
    if (someObjectHasLeft || !lent.isEmpty()) {
        if (effects.writes.contains(AbstractHeap::MemberGuards) && !effects.mayRunOtherCode() && writerEndsNoGuard(walker, draft))
            effects.writes.remove(AbstractHeapSet { AbstractHeap::MemberGuards });
        std::optional<MemoryLocation> writtenAlone = storeMayLeaveWhatLiesBeside(state, draft) ? locationThatStoreNames(walker.program(), draft, effects) : std::nullopt;
        state.invalidate(effects, writtenAlone ? &*writtenAlone : nullptr);
    }
    for (Reference object : lent)
        state.noteFresh(object);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
