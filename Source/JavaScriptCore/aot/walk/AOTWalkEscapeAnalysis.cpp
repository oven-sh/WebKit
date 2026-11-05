/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
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

static constexpr OpcodeSet allocationsOfEscapeAnalysis = opcodesWhere([](Opcode opcode) {
    if (opcode == Opcode::NewClosure || opcode == Opcode::Data)
        return true;
    if (resultRuleOf(opcode) != ResultRule::Allocation || effectRuleOf(opcode) != EffectRule::Declared || boundToOf(opcode) == BoundTo::Site)
        return false;
    Effects bound = declaredBoundOf(opcode);
    return bound.writes.containsOnly(heapsWhoseWritesNobodySees) && !bound.mayRunOtherCode();
});
static_assert(allocationsOfEscapeAnalysis.contains(Opcode::NewObject) && allocationsOfEscapeAnalysis.contains(Opcode::NewArray) && allocationsOfEscapeAnalysis.contains(Opcode::CreateIteratorResult));
static_assert(!allocationsOfEscapeAnalysis.contains(Opcode::NewTemplateObject) && !allocationsOfEscapeAnalysis.contains(Opcode::TruncateNewArray));

static constexpr OpcodeSet namesOfOperandInEscapeAnalysis = opcodesThatHandOn({ HandedOn::PassedCheck, HandedOn::Mark });
static constexpr OpcodeSet argumentsObjectsOfEscapeAnalysis { Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments };
static_assert(allocationsOfEscapeAnalysis.contains(Opcode::CreateUnmappedArguments) && allocationsOfEscapeAnalysis.contains(Opcode::CreateMappedArguments));
static constexpr OpcodeSet opcodesOfEscapeAnalysis = allocationsOfEscapeAnalysis | namesOfOperandInEscapeAnalysis | OpcodeSet { Opcode::StrictEq, Opcode::GetMappedArgument };

static constexpr OperandUses usesThatKeepFreshObjectHere {
    OperandUse::Inspects,
    OperandUse::ReadsContents,
    OperandUse::RetainsContents,
    OperandUse::WritesContents,
    OperandUse::MovesElements,
    OperandUse::StoresByKey,
    OperandUse::DeletesByKey,
    OperandUse::AssignsFrom,
    OperandUse::DefinesName,
    OperandUse::ChangesIntegrity,
    OperandUse::ChangesPrototype,
};

namespace {

struct FreshOperandOfEscapeAnalysis {
    Reference object;
    uint32_t operandIndex { 0 };
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
    ValueID cell = rootOfCellInSweptCode(code, name.id());
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
    ValueID cell = rootOfCellInSweptCode(code, name.id());
    if (code.isConstant(cell) || cell.index() < numberOfRecordsBefore)
        return { };
    return Reference::value(cell);
}

static bool givesObjectThatNothingElseNames(const Operation& made)
{
    Opcode opcode = made.opcode;
    EffectTraits traits = made.traits();
    if (!allocationsOfEscapeAnalysis.contains(opcode) || traits.mayWriteWhatIsSeen() || traits.mayRunOtherCode())
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
    if (made.prototypeOperand() && program.prototypeMaySelectOtherClass(layout))
        return;
    if (!walker.plan().layout(layout).hasSlots())
        return;

    WalkState& state = walker.state();
    bool isEnvironment = program.at(layout).isEnvironment();
    state.noteShape(object, isEnvironment ? walker.types().forEnvironment(layout) : walker.types().forLayout(layout));
    std::span<const FieldIndex> fillers = walker.fillersOfAllocation(object);
    bool isAsAllocated = fillers.empty() && (numberOfFieldsWritten == SIZE_MAX || made.fields().size() == numberOfFieldsWritten);
    if (!isEnvironment) {
        state.noteFactsOfCell(object, FactOfCell::NotFrozen);
        if (!made.prototypeOperand())
            state.noteFactsOfCell(object, FactOfCell::HasLayoutPrototype);
        std::optional<PlacedAllocation> placed = isAsAllocated ? walker.plan().allocation(program, layout, made.fields()) : std::nullopt;
        if (isAsAllocated)
            state.noteFactsOfCell(object, FactOfCell::NoFieldOutsideSlot);
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
        return allocation.as<Opcode::NewObject>().fields().size();
    if (allocation.opcode() != Opcode::Data)
        return SIZE_MAX;
    DataID data = allocation.as<Opcode::Data>().data();
    const DataNode::Object* constant = data ? walker.program().at(data).root.tryAs<DataNode::Object>() : nullptr;
    return constant && constant->members.size() != made.fields().size() ? constant->members.size() : SIZE_MAX;
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
    walker.bind(allocation.id(), object);
    return true;
}

void noteFreshList(Walker& walker, Reference list)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !list || !list.isValue())
        return;
    const Operation& made = walker.code().at(list.id());
    if (made.opcode != Opcode::NewArray || !givesObjectThatNothingElseNames(made))
        return;
    noteWhatNewArrayFillsIn(walker, list, made);
    walker.state().noteFresh(list);
}

void noteFreshObject(Walker& walker, Reference name, size_t numberOfFieldsWritten)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !name || !name.isValue())
        return;
    ValueID cell = rootOfCellInSweptCode(walker.code(), name.id());
    const Operation& made = walker.code().at(cell);
    if (walker.code().isConstant(cell) || made.opcode != Opcode::NewObject || !givesObjectThatNothingElseNames(made))
        return;
    noteWhatNewObjectFillsIn(walker, Reference::value(cell), made, numberOfFieldsWritten);
    walker.state().noteFresh(Reference::value(cell));
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
    if (!placed || state.orderWordOf(object) != std::optional<uint32_t>(placed->orderWord))
        return std::nullopt;

    Vector<PropertyOfFreshObject, 8> properties;
    for (FieldIndex listed : made.fields()) {
        FieldReference field { layout, listed };
        Reference value = state.slot(object, field);
        TypeID type = value && !isClosureLeftUnmade(walker, value) ? walker.typeOf(value) : TypeID();
        if (!type || (walker.types().at(type).primitives & Primitive::Absent))
            return std::nullopt;
        properties.append({ field, value });
    }
    return properties;
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
    ValueID cell = rootOfCellInSweptCode(walker.code(), name.id());
    return walker.code().isConstant(cell) ? Reference() : Reference::value(cell);
}

void noteListAsMade(Walker& walker, Reference name)
{
    if (Reference list = rootOfListInEscapeAnalysis(walker, name))
        walker.state().noteShape(list, walker.code().at(list.id()).type);
}

bool isListAsMade(Walker& walker, Reference name)
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

bool isFreshListAsMade(Walker& walker, Reference name)
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
    if (!list || !(forItsOwnElementsAlone ? !!freshObjectNamedBy(walker, list) : isFreshListAsMade(walker, list)))
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

bool readLeavesFreshObjectAsItIs(Walker& walker, const BodyValue& read, uint32_t operandIndex, Reference name, TypeID typeOfKey)
{
    Reference object = freshObjectNamedBy(walker, name);
    if (!object)
        return false;
    bool isArguments = argumentsObjectsOfEscapeAnalysis.contains(walker.code().at(object.id()).opcode);

    if (read.opcode() == Opcode::PropGet) {
        if (operandIndex != OperandIndex::PropGet::object || read.isFreeName() || read.key() != walker.program().keyOf(BuiltinNames::length))
            return false;
        return isArguments ? !!lengthOfFreshArguments(walker, object) : hasKind(walker.types(), walker.typeOf(object), OperandKind::Array);
    }
    static_assert(OperandIndex::ElemGet::object == OperandIndex::ElemHas::object);
    if ((read.opcode() != Opcode::ElemGet && read.opcode() != Opcode::ElemHas) || operandIndex != OperandIndex::ElemGet::object || read.propertyOperand() == read.objectOperand() || !typeOfKey)
        return false;
    Type key = walker.types().at(typeOfKey);
    if (!isArguments)
        return key.isNumber() && isFreshListAsMade(walker, object);
    if (read.opcode() != Opcode::ElemGet || !key.isInteger())
        return false;
    if (key.integers.isSingleValue() && key.integers.minimum >= 0 && key.integers.minimum < UINT32_MAX && elementOfFreshArguments(walker, object, static_cast<uint32_t>(key.integers.minimum)))
        return true;
    return positionsOfFreshArgumentsAreInSight(walker, object, key.integers.minimum);
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
    ValueID root = rootOfCellInSweptCode(code, other.id());
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

constinit const OpcodeSet opcodesThatBuildFreshObjectTakes = opcodesOfEscapeAnalysis;

bool buildFreshObjectOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!opcodesOfEscapeAnalysis.contains(opcode))
        return false;

    if (opcode == Opcode::StrictEq)
        return foldIdentityOfFreshObject(walker, value);
    if (opcode == Opcode::GetMappedArgument)
        return answerMappedArgumentFromState(walker, value);
    if (namesOfOperandInEscapeAnalysis.contains(opcode))
        return bindNameToFreshObjectItself(walker, value);
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

static constexpr OpcodeSet appendsFollowedByEscapeAnalysis { Opcode::AppendElement, Opcode::AppendHole };
static_assert(!OperandIndex::AppendElement::array && !OperandIndex::AppendHole::array);

static bool followAppendToFreshList(Walker& walker, const Draft& append, const FreshOperandOfEscapeAnalysis& written)
{
    std::optional<uint32_t> length = lengthOfFreshList(walker, written.object);
    if (written.operandIndex || !length || *length == UINT32_MAX || !isListAsMade(walker, written.object))
        return false;

    WalkState& state = walker.state();
    state.markEscaped(written.object);
    if (append.opcode == Opcode::AppendElement && append.valueOperand().isRecord())
        state.noteElement(written.object, positionInFreshList(walker, *length), valueHeldByFreshObject(walker.code(), append.valueOperand().id()));
    state.noteElement(written.object, keyOfLengthOfFreshList(walker), positionInFreshList(walker, *length + 1));
    state.noteFresh(written.object);
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
    if (opcode == Opcode::SetMappedArgument)
        return followStoreToMappedArgument(walker, store, written);
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

void markEscapingOperands(Walker& walker, const Draft& draft, const Effects* foundByCaller)
{
    Opcode opcode = draft.opcode;
    if (!walker.state().holdsFreshObjects() || !draft.numberOfOperands() || opcodesThatGiveSameCell.contains(opcode))
        return;

    Vector<FreshOperandOfEscapeAnalysis, 4> freshOperands;
    draft.forEachOperand([&](Reference operand, uint32_t operandIndex) {
        if (Reference object = freshObjectNamedBy(walker, operand))
            freshOperands.append({ object, operandIndex });
    });
    if (freshOperands.isEmpty())
        return;

    WalkState& state = walker.state();
    bool doesGenericOperation = testsItsOwnGuess(draft);
    Effects effects = effectsOfDraftForEscapeAnalysis(walker.program(), draft, doesGenericOperation);
    if (foundByCaller && !doesGenericOperation)
        effects.intersect(*foundByCaller);
    bool runsOtherCode = effects.mayRunOtherCode();

    if (opcode == Opcode::CallDirect && returnsResultOfFunction(draft.role()) && !draft.numberOfBoundArguments()) {
        const LinkedFunction& callee = linkTablesForEscapeAnalysis(walker).function(draft.function());
        effects = callee.effectsOnReturn;
        effects.merge(callee.effectsOnThrow);
    }

    uint32_t numberOfNamedOperands = draft.numberOfNamedOperands();
    bool someObjectHasLeft = false;
    for (const FreshOperandOfEscapeAnalysis& operand : freshOperands) {
        // Wrong on purpose. No value shows it: Walker::endLivesOfCellsThatHaveNotLeft().
        if (opcode == Opcode::CallDirect && operand.operandIndex >= numberOfNamedOperands && walker.takesCalleesToKeepNothing())
            continue;
        if (!doesGenericOperation && !useLetsFreshObjectLeave(operandInfoOf(opcode, std::min(operand.operandIndex, numberOfNamedOperands)), runsOtherCode))
            continue;
        state.markEscaped(operand.object);
        someObjectHasLeft = true;
    }
    if (!effects.mayRunOtherCode() && effects.writes.containsOnly(heapsWhoseWritesNobodySees))
        return;

    Vector<Reference, 4> lent;
    for (const FreshOperandOfEscapeAnalysis& operand : freshOperands) {
        if (!state.isFresh(operand.object) || followStoreIntoFreshObject(walker, draft, operand))
            continue;
        state.markEscaped(operand.object);
        lent.append(operand.object);
    }
    if (someObjectHasLeft || !lent.isEmpty())
        state.invalidate(effects);
    for (Reference object : lent)
        state.noteFresh(object);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
