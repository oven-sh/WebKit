/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// An allocation is built as ever and noted as fresh with what it fills in. Every draft is asked, by the roles of its row, which fresh operands it lets leave. Nothing is put off.
// At a suspension: a fresh object stays fresh, because the saved frame alone has it. One that the operation lists as held or awaited is retained, so it leaves.
// At a throw: an operation that may run other code lets leave whatever it looks at, but for a list that it only reads out. What an edge hands on leaves in Walker::letLeave().
// Withheld: nothing is noted as fresh, so WalkState::isFresh() holds of nothing and what is known of an object ends as its effects say.

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

// What isAllocationWhoseStoresAreHeldBack() and isClosureWhoseStoresAreHeldBack() of AOTSweepStores.cpp ask, as far as the row answers. givesObjectThatNothingElseNames() asks the record for the rest.
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

// In Code each would be a second name of its operand. An operation that states a type is bound to its operand by replaceOperation().
static constexpr OpcodeSet namesOfOperandInEscapeAnalysis = opcodesThatHandOn({ HandedOn::PassedCheck, HandedOn::Mark });
static constexpr OpcodeSet callsOfEscapeAnalysis = callsByValue | constructionsByValue | OpcodeSet { Opcode::CallDirect };
static constexpr OpcodeSet argumentsObjectsOfEscapeAnalysis { Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments };
static_assert(allocationsOfEscapeAnalysis.contains(Opcode::CreateUnmappedArguments) && allocationsOfEscapeAnalysis.contains(Opcode::CreateMappedArguments));
static constexpr OpcodeSet opcodesOfEscapeAnalysis = allocationsOfEscapeAnalysis | namesOfOperandInEscapeAnalysis | callsOfEscapeAnalysis | OpcodeSet { Opcode::StrictEq, Opcode::GetMappedArgument };

// The operation looks at the object or writes into it, and hands it to nobody.
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

// A conversion, a Narrowed, a check that has passed: the names that the sweep looks through too. Null: what is named is not fresh.
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

// The same value of JavaScript in the form in which it was built. Null: a closure that is made on demand, which has no value.
static Reference valueHeldByFreshObject(const Code& code, ValueID held)
{
    while (conversionsOfForm.contains(code.at(held).opcode))
        held = code.at(held).operand(0);
    if (code.at(held).representation == Representation::Void)
        return { };
    return code.isConstant(held) ? Reference::constant(held) : Reference::value(held);
}

// ---- What is made here

// The links of Walker::walkOperation() behind this one, in its order.
// openGuessAt() is not among them. Only the Walker knows whether guesses are opened where it stands, and a guess walks on past the operation, so behind it nobody knows what has become of the object.
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

// The state knows an object under its root: definitions may stand behind an allocation and a mark behind those, and a conversion behind a call.
// Null: nothing was bound, or what was bound was there before the operation was built.
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

// When it has run, no memory and no other value names its result.
static bool givesObjectThatNothingElseNames(const Operation& made)
{
    Opcode opcode = made.opcode;
    EffectTraits traits = made.traits();
    if (!allocationsOfEscapeAnalysis.contains(opcode) || traits.mayWriteWhatIsSeen() || traits.mayRunOtherCode())
        return false;

    // The closure table hands its one cell to whoever reads the function elsewhere.
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
    if ((opcode == Opcode::Data || opcode == Opcode::NewRegExp) && made.isNeverToldApart())
        return false;

    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        if (operandUsesOf(opcode, operandIndex).contains(OperandUse::Returns))
            return false;
    }
    return true;
}

// Under it the state has the object itself while the record of its allocation lists every property that it was made with. What it has got since shows in its order word.
static Reference keyOfPropertiesAsListed()
{
    return Reference::postponed(Reference::indexMask - 2);
}

// What is defined behind the allocation lies in other fields than the ones that it lists. `numberOfFieldsWritten`: how many fields the allocation had as it was written.
static void noteWhatNewObjectFillsIn(Walker& walker, Reference object, const Operation& made, size_t numberOfFieldsWritten)
{
    const Program& program = walker.program();
    LayoutID layout = made.layout();
    // `new.target.prototype` is read where the cell is made, and may belong to a class below the one that is written.
    if (made.prototypeOperand() && program.prototypeMaySelectOtherClass(layout))
        return;
    if (!walker.plan().layout(layout).hasSlots())
        return;

    WalkState& state = walker.state();
    bool isEnvironment = program.at(layout).isEnvironment();
    state.noteShape(object, isEnvironment ? walker.types().forEnvironment(layout) : walker.types().forLayout(layout));
    // A cell is made with an order word without flags, which lists what the allocation lists. One that is handed no prototype stands below that of its layout.
    // A definition behind the allocation has listed its key since, and what did not fit its slot lies outside of it.
    // A value that stood behind a filler may have left its slot too.
    std::span<const FieldIndex> fillers = walker.fillersOfAllocation(object);
    bool isAsAllocated = fillers.empty() && (numberOfFieldsWritten == SIZE_MAX || made.fields().size() == numberOfFieldsWritten);
    if (!isEnvironment) {
        state.noteFactsOfCell(object, FactOfCell::NotFrozen);
        if (!made.prototypeOperand())
            state.noteFactsOfCell(object, FactOfCell::HasLayoutPrototype);
        std::optional<PlacedAllocation> placed = isAsAllocated ? walker.plan().allocation(program, layout, made.allocationClass(), made.fields()) : std::nullopt;
        if (isAsAllocated)
            state.noteFactsOfCell(object, FactOfCell::NoFieldOutsideSlot);
        if (placed)
            state.noteOrderWord(object, placed->orderWord);
    }

    auto fields = made.fields();
    std::span<const ValueID> values = made.valuesOperands();
    if (made.listsOnlyStoredValues() || fields.size() != values.size())
        return;
    const Code& code = walker.code();
    for (size_t index = 0; index < fields.size(); ++index) {
        // A mark is no value, and the field of the scope is no property.
        if (!values[index] || !walker.plan().slot(layout, fields[index]).hasStorage() || std::ranges::find(fillers, fields[index]) != fillers.end()
            || walker.plan().slot(layout, fields[index]).representation == SlotRepresentation::DeferredClosure || walker.plan().layout(layout).environmentOfDeferredClosures == fields[index])
            continue;
        FieldReference field { layout, fields[index] };
        Reference value = valueHeldByFreshObject(code, values[index]);
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

// The state holds the length of a list beside its elements, under the key by which a program reads it. Whatever writes the storage of the list ends both.
static Reference keyOfLengthOfFreshList(Walker& walker)
{
    Draft key(Opcode::String);
    key.setString(walker.program().stringOf(BuiltinNames::length));
    return walker.constant(WTF::move(key));
}

// Walker::constant() has one record for one integer, so a read under a written index asks under the same Reference. A hole is answered by the prototypes: nothing is noted under its position.
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
    // TODO(known-state-and-effects-2a): noteKnownOfNewList(walker, object) (K9 K10) stood here. Every operation that runs other code went through the notes of every fresh list: the square of the number of lists (65 G against 16 at 6,000). It comes back when the notes of fresh holders are kept apart from those that any other code ends.
}

// The variable of a parameter of sloppy code is a word of the object, whether or not an argument was passed for it. The element under the same number is that word only where so many arguments were passed,
// and only until something cuts the two apart. So the state has the word under a name that no operand has.
static Reference variableInMappedArguments(uint32_t parameter)
{
    return Reference::postponed(parameter);
}

// Under it the state has the list of the arguments behind the parameters, until something may write an element of the object.
static Reference keyOfListBehindParameters()
{
    return Reference::postponed(Reference::indexMask - 1);
}

// The parameters are what they were when the function was entered: the object is made in front of all that the program wrote. For an argument that was left out the caller has handed over undefined, which is what a read of the element finds too.
// The language has no name for the hidden list, so nothing ever changes it. A rest parameter is a list of the program's.
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

// Constant data is written out as an allocation and, behind it, a definition by name for each member that the allocation may not list: becomeObjectOfDataInChoices().
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

    // WalkState takes a note about a fresh object for a store into it, so the places come in front of the mark.
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
    // It has the keys that the allocation lists while its order word is the one that the allocation wrote. `if (c) o.x = 1;` leaves no note of the slot behind the join, and no word either.
    std::optional<PlacedAllocation> placed = walker.plan().allocation(program, layout, made.allocationClass(), made.fields());
    if (!placed || state.orderWordOf(object) != std::optional<uint32_t>(placed->orderWord))
        return std::nullopt;

    Vector<PropertyOfFreshObject, 8> properties;
    for (FieldIndex listed : made.fields()) {
        FieldReference field { layout, listed };
        Reference value = state.slot(object, field);
        TypeID type = value ? walker.typeOf(value) : TypeID();
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

// ---- `arguments`

// Null: what is named is no `arguments`, or something else has it by now.
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

// So many arguments were passed in every run.
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
        // The element is the variable while the object is as it was made and an argument was passed. For one that was left out the element is missing, and the variable holds undefined until it is stored to.
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
    // An object that is no longer as it was made may lie below another, which answers for an index behind the last argument, or may have lost an element or got one of another form.
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

// The variable of a parameter of sloppy code, read out of an object that nothing else has.
static bool answerMappedArgumentFromState(Walker& walker, const BodyValue& read)
{
    Reference object = freshArgumentsNamedBy(walker, walker.referenceOf(read.as<Opcode::GetMappedArgument>().argumentsOperand()));
    Reference held = object ? walker.state().element(object, variableInMappedArguments(read.as<Opcode::GetMappedArgument>().index())) : Reference();
    if (!held || mayBeAbsentInEscapeAnalysis(walker, held))
        return false;
    walker.bind(read.id(), held);
    return true;
}

// Whatever form the call takes, it enters the one function, which hands back an object that it has made and kept nowhere.
static bool buildCallThatGivesFreshObject(Walker& walker, const BodyValue& call)
{
    const LinkTables& link = linkTablesForEscapeAnalysis(walker);
    FunctionID callee = call.opcode() == Opcode::CallDirect ? call.function() : link.calleeAt(walker.currentFunction(), call.id());
    if (!callee || !link.function(callee).resultIsFresh)
        return false;

    uint32_t numberOfRecordsBefore = walker.code().numberOfValues();
    buildByLinksBehindEscapeAnalysis(walker, call);

    // A call in tail position gives nothing here, and a call whose results are split holds none of them.
    Reference object = rootOfObjectBuiltFor(walker, call, numberOfRecordsBefore);
    if (!object || !holdsCells(walker.representationOf(object)))
        return true;
    walker.state().noteFresh(object);
    walker.bind(call.id(), object);
    return true;
}

// ---- What a fresh object answers without code

// The state knows the object under one name. None of these throws for an object.
static bool bindNameToFreshObjectItself(Walker& walker, const BodyValue& name)
{
    Reference object = freshObjectNamedBy(walker, walker.referenceOf(name.operand(0)));
    if (!object)
        return false;
    walker.bind(name.id(), object);
    return true;
}

// Code is appended to, so an older record was there before the object was made. In a loop it holds at most the object that an earlier turn has made.
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
    if (callsOfEscapeAnalysis.contains(opcode))
        return buildCallThatGivesFreshObject(walker, value);
    return buildAllocationOfFreshObject(walker, value);
}

// ---- What lets a fresh object leave

// An upper bound of what Walker::build() finds. The declared effects leave out the failing side of an access that tests its own guess and the code that an operand may run.
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

// What a fresh holder keeps leaves like what any other holder keeps. The walk, the handlers and the builders of typed forms let a holder leave by WalkState::markEscaped(), which knows nothing of what it holds.
static bool useLetsFreshObjectLeave(const OperandInfo& info, bool runsOtherCode)
{
    if (!info.uses.containsOnly(usesThatKeepFreshObjectHere) || !info.protocols.isEmpty())
        return true;
    // A getter, a setter or a trap is handed what the row only says is looked at. A list that the operation itself reads out is handed to nobody.
    return runsOtherCode && !info.uses.contains(OperandUse::ReadsContents);
}

static constexpr OpcodeSet appendsFollowedByEscapeAnalysis { Opcode::AppendElement, Opcode::AppendHole };
static_assert(!OperandIndex::AppendElement::array && !OperandIndex::AppendHole::array);

// It puts one element behind the last, or leaves a hole there, and changes nothing else of a list that is as it was made.
static bool followAppendToFreshList(Walker& walker, const Draft& append, const FreshOperandOfEscapeAnalysis& written)
{
    std::optional<uint32_t> length = lengthOfFreshList(walker, written.object);
    if (written.operandIndex || !length || *length == UINT32_MAX || !isListAsMade(walker, written.object))
        return false;

    // WalkState takes a note about a fresh object for a store that may have replaced what it holds under any other key.
    WalkState& state = walker.state();
    state.markEscaped(written.object);
    if (append.opcode == Opcode::AppendElement && append.valueOperand().isRecord())
        state.noteElement(written.object, positionInFreshList(walker, *length), valueHeldByFreshObject(walker.code(), append.valueOperand().id()));
    state.noteElement(written.object, keyOfLengthOfFreshList(walker), positionInFreshList(walker, *length + 1));
    state.noteFresh(written.object);
    return true;
}

// It writes the one word of the variable, which the operation names by its number.
static bool followStoreToMappedArgument(Walker& walker, const Draft& store, const FreshOperandOfEscapeAnalysis& written)
{
    if (written.operandIndex != OperandIndex::SetMappedArgument::arguments || !store.valueOperand().isRecord())
        return false;
    // As in followAppendToFreshList().
    WalkState& state = walker.state();
    state.markEscaped(written.object);
    state.noteElement(written.object, variableInMappedArguments(store.index()), valueHeldByFreshObject(walker.code(), store.valueOperand().id()));
    state.noteFresh(written.object);
    return true;
}

// The effects of no operation end what is known of a fresh object, so a store into one is followed here.
// True: the store has changed one place of the object and nothing else of it, and the state holds what lies there now.
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
    // A store that may list a key leaves the word that it names, or one that nobody knows.
    if (opcode == Opcode::PutSlotOrAdd)
        state.noteOrderWord(written.object, store.orderWordAfter() ? std::optional<uint32_t>(store.orderWordAfter()->value()) : std::nullopt);
    return true;
}

void markEscapingOperands(Walker& walker, const Draft& draft, const Effects* foundByCaller)
{
    Opcode opcode = draft.opcode;
    // Another name of the same cell: freshObjectNamedBy() looks through it.
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
    // The roles of the row speak of the side on which the guess holds. The other side does the generic operation, which hands its operands to whatever it finds.
    bool doesGenericOperation = testsItsOwnGuess(draft);
    Effects effects = effectsOfDraftForEscapeAnalysis(walker.program(), draft, doesGenericOperation);
    // What whoever builds the operation has found of it holds in every run, as for Walker::build(): a read by name that finds a method runs no getter, so nobody is handed the object.
    if (foundByCaller && !doesGenericOperation)
        effects.intersect(*foundByCaller);
    bool runsOtherCode = effects.mayRunOtherCode();

    // The arguments of a call of a body are its entry parameters, one for one. What the callee does not keep it may still write into.
    std::span<const LinkedParameter> parameters;
    if (opcode == Opcode::CallDirect && returnsResultOfFunction(draft.role()) && !draft.numberOfBoundArguments()) {
        const LinkTables& link = linkTablesForEscapeAnalysis(walker);
        const LinkedFunction& callee = link.function(draft.function());
        parameters = link.parametersOf(draft.function());
        effects = callee.effectsOnReturn;
        effects.merge(callee.effectsOnThrow);
    }

    uint32_t numberOfNamedOperands = draft.numberOfNamedOperands();
    bool someObjectHasLeft = false;
    for (const FreshOperandOfEscapeAnalysis& operand : freshOperands) {
        if (operand.operandIndex >= numberOfNamedOperands) {
            uint32_t argument = operand.operandIndex - numberOfNamedOperands;
            if (argument < parameters.size() && !parameters[argument].escapes)
                continue;
        }
        if (!doesGenericOperation && !useLetsFreshObjectLeave(operandInfoOf(opcode, std::min(operand.operandIndex, numberOfNamedOperands)), runsOtherCode))
            continue;
        state.markEscaped(operand.object);
        someObjectHasLeft = true;
    }
    if (!effects.mayRunOtherCode() && effects.writes.containsOnly(heapsWhoseWritesNobodySees))
        return;

    // An object that stays is an object like any other while what the operation may write is ended, and fresh again behind that.
    Vector<Reference, 4> lent;
    for (const FreshOperandOfEscapeAnalysis& operand : freshOperands) {
        if (!state.isFresh(operand.object) || followStoreIntoFreshObject(walker, draft, operand))
            continue;
        state.markEscaped(operand.object);
        lent.append(operand.object);
    }
    // Walker::build() ends the same once more. Ended here, it holds wherever build() asks.
    if (someObjectHasLeft || !lent.isEmpty())
        state.invalidate(effects);
    for (Reference object : lent)
        state.noteFresh(object);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
