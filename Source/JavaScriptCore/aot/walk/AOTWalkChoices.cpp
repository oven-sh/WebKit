/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"
#include "AOTWalkEnforcedFillers.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTNameClass.h"
#include "AOTResultRules.h"
#include "AOTSlotRepresentation.h"
#include "AOTTypeArena.h"
#include "AOTTypeClosure.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// Closed operations, step 3: each field that the engine chooses gets the value under which the emitter calls a shared stub and leans on nothing. Never withheld.
// At a suspension and at a throw it does nothing of its own: what it builds beside a draft goes through Walker::build() like any other operation.
// What the rows mark `from Placed` (site caches, allocations, requests, records of regular expressions, offsets in the statics) stays absent. No emitter reads those words:
// each asks the plan or its MentionList while it generates, and Image numbers what was mentioned. So the walk owes only that the plan and the tables of the image have what the emitter will ask for.

// A draft starts with the row's defaults: the first member of an enumeration, nothing in an optional field, no flag, no operand of the engine's own.
// Where that claims nothing the field is left as it stands: no site, no receivers, no callees, no allocation class, no room, no called built-in, no storage or header handed in.
// Every emitter asks whether such an operand is there, but for that of ForInStep, which has no code without a position.
static_assert(!static_cast<unsigned>(AllocationStorage::Heap) && !static_cast<unsigned>(ArrayLiteralStorage::Fresh) && !static_cast<unsigned>(ArrayStorage::Other));
static_assert(!static_cast<unsigned>(ExpectedElements::Anything) && !static_cast<unsigned>(ClosureForm::Full) && !static_cast<unsigned>(CodeRole::Body));
static_assert(!static_cast<unsigned>(TailCallRequirement::Required));
// These two defaults have no code.
static_assert(!static_cast<unsigned>(OperatorForm::Unselected) && !static_cast<unsigned>(DataEncoding::Unselected));

static constexpr OpcodeSet opcodesThatNeedChoices = Operation::opcodesWithForm | OpcodeSet {
    Opcode::Data, Opcode::NewClosure, Opcode::NewObject, Opcode::NewArray, Opcode::NewArrayOfElements, Opcode::ForInOpen, Opcode::ForInStep, Opcode::ResultAt, Opcode::CallDirect
};

// The new operation gives what the old one gave.
static void becomeInChoices(Draft& draft, Opcode opcode)
{
    ValueID inputValueGiven = draft.inputValueGiven;
    draft.become(opcode);
    draft.inputValueGiven = inputValueGiven;
}

static void becomeFinishedObjectInChoices(Draft& draft, Reference object)
{
    becomeInChoices(draft, Opcode::FinishObject);
    draft.setObjectOperand(object);
}

static void defineBehindAllocationInChoices(Walker& walker, Origin origin, Reference object, PropertyKeyID key, Reference value)
{
    Draft definition(Opcode::PropDefine, origin);
    definition.setObjectOperand(object);
    definition.setKey(key);
    definition.setValueOperand(value);
    // The row of PropDefine speaks of any object, a proxy among them. This one is a cell that was made a moment ago: it takes properties, stands above nobody, and nothing else has it. So the definition runs no code of the program
    // and writes nothing that anybody sees: it may allocate, as the allocation that it completes. One NewObject of the input stands for both, and whoever has looked at a loop has counted on that row alone.
    Effects ofAllocation = declaredBoundOf(Opcode::NewObject);
    walker.build(WTF::move(definition), &ofAllocation);
}

// ---- NewClosure

// A capture that the plan has removed is no word of a closure.
static bool closuresHoldCapturesInChoices(const Walker& walker, FunctionID function)
{
    return walker.program().at(function).captures.size() > walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(function);
}

// `function F() {}` that captures nothing and is evaluated once. The record of its class says ProvenOnce (classOfLayoutRecordIsEvaluatedOnce() of image/AOTLayoutTables.cpp),
// so the run time makes the one function object for whoever asks first, and what `new F` makes inherits from the prototype object of that one.
static bool isOnlyClassObjectInChoices(const Walker& walker, FunctionID function)
{
    const Program& program = walker.program();
    LayoutID constructed = program.at(function).constructedClass;
    if (!constructed || closuresHoldCapturesInChoices(walker, function))
        return false;
    const Layout& layout = program.at(constructed);
    return layout.has(LayoutFlags::IsConstructorFunction) && layout.constructor == function && walker.facts().isEvaluatedAtMostOnce(constructed);
}

// The form is the plan's: the function tables, GetCapture and the run time go by it. Without a word of the plan it is Full.
static void chooseFormOfClosureInChoices(Walker& walker, Draft& closure)
{
    const Plan& plan = walker.plan();
    FunctionID function = closure.function();
    closure.setStorage(AllocationStorage::Heap);
    // It has no value: the allocation that lists it writes the mark of its slot.
    if (closure.isMadeOnDemand()) {
        closure.setClosureForm(ClosureForm::Deferred);
        return;
    }

    if (closure.isHeldByClosureTable() || plan.closureIdentityOf(function) == ClosureIdentity::TableCell || isOnlyClassObjectInChoices(walker, function)) {
        if (closuresHoldCapturesInChoices(walker, function)) {
            walker.fail("the closure table makes closures that capture nothing"_s);
            return;
        }
        if (!plan.hasClosureCell(function)) {
            walker.fail("the closure table is to hold the closure of a function that has no cell in it"_s);
            return;
        }
        closure.setIsHeldByClosureTable(true);
        closure.setClosureForm(ClosureForm::Singleton);
        return;
    }

    ClosureForm form = plan.slots().closureFormOf(function);
    switch (form) {
    case ClosureForm::Full:
    case ClosureForm::Slim:
        closure.setClosureForm(form);
        return;
    case ClosureForm::Deferred:
    case ClosureForm::Singleton:
        walker.fail("the plan gives a function a form that belongs to one closure"_s);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Reference findScopeOfClosureReadOffObjectInSight(const Walker& walker, FunctionID function, Reference closure)
{
    const Code& code = walker.code();
    if (!closure || !closure.isValue())
        return { };
    const Operation& read = code.at(closure.id());
    if (read.opcode != Opcode::GetDeferredClosureSlot || walker.program().at(function).captures.size() != 1)
        return { };
    auto findScopeReadBesideSlot = [&]() -> Reference {
        ValueID next(closure.id().index() + 1);
        if (next.index() >= code.numberOfValues() || code.isConstant(next))
            return { };
        const Operation& capture = code.at(next);
        return capture.opcode == Opcode::GetCapture && capture.closureOperand() == closure.id() && capture.function() == function && !capture.index() ? Reference::value(next) : Reference();
    };
    // The read was built because the state no longer says what lies in the slot: something has run since the allocation, and it may have stored there. What the allocation lists says nothing of that.
    return findScopeReadBesideSlot();
}

// ---- NewObject

// A property that lies in its slot has the form that the record of the layout gives the field.
static bool fieldIsPlainPropertyInChoices(const Walker& walker, FieldReference field)
{
    FieldFacts facts = walker.facts().field(field);
    if (facts.facts.contains(FieldFact::IsAccessor) || !facts.formOfValue.isEmpty())
        return false;
    const Program& program = walker.program();
    return field.field >= program.at(field.layout).fields.size() || fieldAt(program, declarationOf(program, field)).form.isEmpty();
}

// Null: only an allocation fills the field.
static PropertyKeyID keyDefinedBehindAllocationInChoices(const Walker& walker, FieldReference field)
{
    const Program& program = walker.program();
    bool onlyFactsSayAccessor = walker.facts().field(field).facts.contains(FieldFact::IsAccessor) && walker.facts().field(field).formOfValue.isEmpty()
        && field.field < program.at(field.layout).fields.size() && fieldAt(program, declarationOf(program, field)).form.isEmpty();
    if (!onlyFactsSayAccessor && !fieldIsPlainPropertyInChoices(walker, field))
        return { };
    if (field.field >= program.at(field.layout).fields.size())
        return walker.plan().slots().keyOf(program, field);

    // PropDefine cannot name the key of any of these. A method of a literal is a property like any other.
    constexpr OptionSet<FieldFlags> filledByAllocationAlone { FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden };
    const Field& declaration = fieldAt(program, declarationOf(program, field));
    return declaration.flags.containsAny(filledByAllocationAlone) ? PropertyKeyID() : declaration.key;
}

// An allocation fills a slot without a test, and whoever reads the slot takes what lies there for a value of both types.
static bool valueFitsSlotInChoices(Walker& walker, FieldReference field, Reference value)
{
    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(value);
    TypeID typeOfPlan = walker.plan().slot(field.layout, field.field).type;
    if (typeOfPlan && !types.isSubset(proven, typeOfPlan))
        return false;
    // Only Facts says that the slot holds the functions of an accessor: nothing in sight was seen to give the field a value. Should something do so after all, it defines the property by name, beside the slot.
    if (walker.facts().field(field).facts.contains(FieldFact::IsAccessor))
        return false;

    FieldFacts facts = walker.facts().field(field);
    // Where the plan holds the slot to nothing, nobody does.
    if (!facts.slotType || !typeOfPlan)
        return true;
    TypeID typeOfFacts = facts.facts.contains(FieldFact::SlotHoldsUndefinedAndNull) ? types.withPrimitives(facts.slotType, Primitive::UndefinedOrNull) : facts.slotType;
    return types.isSubset(proven, typeOfFacts);
}

namespace {

// A value that is not proven to fit its slot, in the middle of what an allocation lists.
struct MisfitInChoices {
    size_t position { 0 };
    PropertyKeyID key;
    Reference value;
};

} // anonymous namespace

// `{ id: 1, tag: maybe, more: 2 }`. True: a filler stands in the place of the value, which is defined by name behind the allocation, and the fields behind it stay listed.
// An absent that is listed leaves the slot empty, which no definition behind a filler does.
static bool listFillerInPlaceOfMisfitInChoices(Walker& walker, LayoutID layout, std::span<const FieldIndex> fields, Vector<Reference, 16>& values, size_t position, Vector<MisfitInChoices, 2>& misfits)
{
    if (walker.types().at(walker.typeOf(values[position])).primitives & Primitive::Absent)
        return false;
    FieldReference field { layout, fields[position] };
    PropertyKeyID key = keyDefinedBehindAllocationInChoices(walker, field);
    std::optional<Draft> draftOfFiller = key ? fillerThatFitsSlot(walker, field) : std::nullopt;
    if (!draftOfFiller)
        return false;
    Reference filler = walker.constant(WTF::move(*draftOfFiller));
    if (!filler || !valueFitsSlotInChoices(walker, field, filler))
        return false;
    misfits.append({ position, key, values[position] });
    values[position] = filler;
    return true;
}

// `{ scan, getText: () => text }`. A closure that the allocation alone takes, for a slot that holds a mark for its function, stays unmade: a record without a value stands in its place, and the one that was built has no reader left.
// What it captures is listed once, for the field of the scope.
// WHERE AN OBJECT HAS THE SCOPE, EVERY CLOSURE IN SUCH A SLOT OF IT IS ONE OVER THAT SCOPE: whoever calls through the slot takes the scope out of the object (CodeGenerator::slotOfScopeReadInPlaceOfClosure()).
// So either each value for such a slot is seen to be made over one and the same scope, or none stays unmade and the object has no scope: every closure then lies in its slot as it is.
// True: the lists have changed.
static bool leaveClosuresUnmadeInChoices(Walker& walker, LayoutID layout, Vector<FieldIndex, 16>& fields, Vector<Reference, 16>& values, size_t& numberListed)
{
    const PlannedLayout& planned = walker.plan().layout(layout);
    if (!planned.environmentOfDeferredClosures)
        return false;
    const Code& code = walker.code();
    ValueID scope;
    for (size_t position = 0; position < fields.size(); ++position) {
        const PlacedSlot& slot = planned.slots[fields[position]];
        if (slot.representation != SlotRepresentation::DeferredClosure)
            continue;
        Reference closure = values[position];
        if (position >= numberListed || !closure.isValue())
            return false;
        const Operation& made = code.at(closure.id());
        if (made.opcode != Opcode::NewClosure || made.function() != slot.deferredFunction || made.numberOfVariadicOperands() != 1)
            return false;
        ValueID captured = made.variadicOperands()[0];
        if (code.isConstant(captured) || (scope && captured != scope))
            return false;
        scope = captured;
    }
    if (!scope)
        return false;

    std::span<const Reference> takenByAllocationAlone = walker.closuresThatAllocationAloneTakes();
    for (size_t position = 0; position < numberListed; ++position) {
        const PlacedSlot& slot = planned.slots[fields[position]];
        if (slot.representation != SlotRepresentation::DeferredClosure || !std::ranges::contains(takenByAllocationAlone, values[position]))
            continue;
        const Operation& made = code.at(values[position].id());
        if (made.closureForm() != ClosureForm::Slim || made.storage() != AllocationStorage::Heap)
            continue;
        Draft unmade(Opcode::NewClosure, made.origin());
        unmade.setFunction(slot.deferredFunction);
        unmade.setIsMadeOnDemand(true);
        Reference capturedByIt = Reference::value(scope);
        unmade.setVariadicOperands(singleElementSpan(capturedByIt));
        walker.chooseConservativeFields(unmade);
        if (Reference mark = walker.build(WTF::move(unmade)))
            values[position] = mark;
    }
    fields.insert(numberListed, *planned.environmentOfDeferredClosures);
    values.insert(numberListed, Reference::value(scope));
    ++numberListed;
    return true;
}

// The emitter asks Plan::allocation() for the fields that the record lists, and stops the build where there is none. So the record lists the longest beginning of what is written
// that fits its slots and has an order word, and each field behind that is defined by name, in the order in which it is written.
// Where the plan keeps the objects of the layout as ordinary objects, a dictionary takes all of them so.
// `built`: the object, where something was defined behind its allocation. The draft has then become the FinishObject of it. Null: the draft is the allocation. False after a failure.
static bool settleAllocationOfObjectInChoices(Walker& walker, Draft& draft, Reference& built)
{
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    LayoutID layout = draft.layout();
    bool hasSlots = plan.layout(layout).hasSlots();
    draft.setStorage(AllocationStorage::Heap);

    size_t numberOfFieldsAsWritten = draft.fields().size();
    if (numberOfFieldsAsWritten != draft.numberOfVariadicOperands()) {
        walker.fail("an allocation lists other values than fields"_s);
        return false;
    }

    Vector<FieldIndex, 16> fields;
    Vector<Reference, 16> values;
    for (size_t index = 0; index < numberOfFieldsAsWritten; ++index) {
        FieldIndex field = draft.fields()[index];
        if (hasSlots && !plan.slot(layout, field).hasStorage()) {
            // A name that the plan has made no field, because no maker in sight gives it anything. A value that is listed for it all the same would be lost without a word.
            TypeID listed = walker.typeOf(draft.variadicOperands()[index]);
            if (!plan.slot(layout, field).indexInLayoutRecord && plan.layout(layout).holdsCompiledObjects() && listed && !isNeverType(listed) && !walker.types().at(listed).hasOnlyPrimitives(Primitive::Absent)) {
                walker.fail("an allocation lists a value for a name that has no slot"_s);
                return false;
            }
            continue;
        }
        fields.append(field);
        values.append(draft.variadicOperands()[index]);
    }

    size_t numberListed = 0;
    Vector<MisfitInChoices, 2> misfits;
    if (hasSlots) {
        while (numberListed < fields.size() && (valueFitsSlotInChoices(walker, { layout, fields[numberListed] }, values[numberListed]) || listFillerInPlaceOfMisfitInChoices(walker, layout, fields.span(), values, numberListed, misfits)))
            ++numberListed;
        if (!plan.allocation(program, layout, AllocationClassID(), fields.span().first(numberListed))) {
            numberListed = plan.keyOrders().lengthOfLongestBeginningWithOrderWord(layout, fields.span().first(numberListed));
            if (!plan.allocation(program, layout, AllocationClassID(), fields.span().first(numberListed))) {
                walker.fail("the plan has no allocation of the layout that begins with these fields"_s);
                return false;
            }
        }
    } else if (program.at(layout).kind != LayoutKind::Record || draft.prototypeOperand()) {
        walker.fail("the plan keeps no cells of the layout of an allocation, and a dictionary cannot stand for its objects"_s);
        return false;
    }

    // What the allocation does not list after all is defined by name in its turn.
    misfits.removeAllMatching([&](const MisfitInChoices& misfit) {
        if (misfit.position < numberListed)
            return false;
        values[misfit.position] = misfit.value;
        return true;
    });

    bool leavesClosuresUnmade = hasSlots && leaveClosuresUnmadeInChoices(walker, layout, fields, values, numberListed);

    // A slot without an absent state counts as filled from the allocation on, and no store lists its key.
    Vector<PropertyKeyID, 16> keysDefinedBehind;
    for (size_t index = numberListed; index < fields.size(); ++index) {
        PropertyKeyID key = keyDefinedBehindAllocationInChoices(walker, { layout, fields[index] });
        if (!key || (hasSlots && !canLeaveSlot(plan.slot(layout, fields[index]).representation))) {
            walker.fail("a field that only an allocation can create stands behind what the allocation can list"_s);
            return false;
        }
        keysDefinedBehind.append(key);
    }

    if (!hasSlots)
        becomeInChoices(draft, Opcode::NewDictionary);
    else if (numberListed != numberOfFieldsAsWritten || !misfits.isEmpty() || leavesClosuresUnmade) {
        draft.setFields(fields.span().first(numberListed));
        draft.setVariadicOperands(values.span().first(numberListed));
    }
    if (keysDefinedBehind.isEmpty() && misfits.isEmpty())
        return true;

    Origin origin = draft.origin();
    built = walker.build(Draft(draft));
    if (!built)
        return false;
    if (!misfits.isEmpty()) {
        Vector<FieldIndex, 2> fieldsWithFillers;
        for (const MisfitInChoices& misfit : misfits)
            fieldsWithFillers.append(fields[misfit.position]);
        walker.noteFillersOfAllocation(built, WTF::move(fieldsWithFillers));
    }
    for (const MisfitInChoices& misfit : misfits)
        defineBehindAllocationInChoices(walker, origin, built, misfit.key, misfit.value);
    for (size_t index = 0; index < keysDefinedBehind.size(); ++index)
        defineBehindAllocationInChoices(walker, origin, built, keysDefinedBehind[index], values[numberListed + index]);
    becomeFinishedObjectInChoices(draft, built);
    return true;
}

static void chooseAllocationOfObjectInChoices(Walker& walker, Draft& allocation)
{
    LayoutID layout = allocation.layout();
    if (!walker.program().at(layout).isEnvironment()) {
        Reference built;
        settleAllocationOfObjectInChoices(walker, allocation, built);
        return;
    }

    // Plan::environmentAllocation() has a row for every environment, whatever is listed.
    const Plan& plan = walker.plan();
    const PlannedLayout& planned = plan.layout(layout);
    if (!planned.hasSlots()) {
        walker.fail("the plan keeps no cells of the layout of an environment"_s);
        return;
    }
    // No store into such an environment runs a barrier.
    if (planned.liesOnlyInFrames) {
        walker.fail("the plan keeps an environment in frames, and the closed form makes it on the heap"_s);
        return;
    }
    allocation.setStorage(AllocationStorage::Heap);
    if (planned.isEscapedOffset)
        noteEnvironmentThatUnitMakes(walker, allocation);

    size_t numberOfFields = allocation.fields().size();
    if (numberOfFields != allocation.numberOfVariadicOperands()) {
        walker.fail("an allocation lists other values than fields"_s);
        return;
    }
    // No store into an environment is tested, and no definition by name can take the place of one.
    for (size_t index = 0; index < numberOfFields; ++index) {
        TypeID typeOfPlan = plan.slot(layout, allocation.fields()[index]).type;
        if (typeOfPlan && !walker.types().isSubset(walker.typeOf(allocation.variadicOperands()[index]), typeOfPlan)) {
            walker.fail("the plan holds a slot of an environment to a type that the value listed for it is not proven to have"_s);
            return;
        }
    }
}

// ---- Data

static bool isAggregateOfDataInChoices(const DataNode& node)
{
    switch (node.kind()) {
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        return true;
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Hole:
    case DataNode::Kind::Boolean:
    case DataNode::Kind::Number:
    case DataNode::Kind::String:
    case DataNode::Kind::BigInt:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Null: any value.
static bool dataFitsTypeOfSlotInChoices(Walker& walker, TypeID slotType, const DataNode& value)
{
    if (!slotType)
        return true;
    TypeArena& types = walker.types();
    if (!includesData(types, types.at(slotType), value))
        return false;
    // That answer goes by the classes and layouts of all the objects of the type, and a node of data is none of the objects that the type names.
    if (value.is<DataNode::Array>())
        return types.isSubset(types.builtinClass(BuiltinClasses::Array), slotType);
    if (!value.is<DataNode::Object>())
        return true;
    TypingContext typing { walker.program(), types, PositionTypes::knowingNothing() };
    return types.isSubset(typeOfData(typing, value), slotType);
}

static bool tableOfImageTakesMemberInChoices(Walker& walker, FieldReference field, const DataNode& value)
{
    const PlacedSlot& slot = walker.plan().slot(field.layout, field.field);
    if (!slot.hasStorage())
        return true;
    if (!slot.indexInLayoutRecord || !fieldIsPlainPropertyInChoices(walker, field))
        return false;
    FieldFacts facts = walker.facts().field(field);
    bool isMarkerThatSlotHolds = (value.is<DataNode::Undefined>() || value.is<DataNode::Null>()) && facts.facts.contains(FieldFact::SlotHoldsUndefinedAndNull);
    return dataFitsTypeOfSlotInChoices(walker, slot.type, value) && (isMarkerThatSlotHolds || !slot.type || dataFitsTypeOfSlotInChoices(walker, facts.slotType, value));
}

// The rules of appendObject() of image/AOTDataTable.cpp, which stops the build on a no: the fields stand in front of the other names, which are texts, and the cell is made by an allocation of the plan that begins with the fields.
static bool tableOfImageHoldsObjectInChoices(Walker& walker, const DataNode::Object& object, Vector<FieldIndex, 16>& fields)
{
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    LayoutID layout = object.layout;
    bool isCompiledObject = false;
    if (layout) {
        const PlannedLayout& planned = plan.layout(layout);
        switch (planned.storage) {
        case LayoutStorage::CompiledObject:
            if (planned.isRecordAlone)
                return false;
            isCompiledObject = true;
            break;
        case LayoutStorage::OrdinaryObject:
            break;
        case LayoutStorage::None:
        case LayoutStorage::Environment:
            return false;
        }
    }

    fields.shrink(0);
    bool hasNamedMemberOutsideFields = false;
    for (const DataMember& member : object.members) {
        if (member.value.is<DataNode::Hole>())
            return false;
        std::optional<FieldIndex> field = isCompiledObject ? plan.slots().findField(program, layout, member.key) : std::nullopt;
        if (!field) {
            NameClass nameClass = program.nameClassOf(member.key);
            if (program.at(member.key).kind != PropertyKeyKind::Text || nameClass == NameClass::Proto)
                return false;
            hasNamedMemberOutsideFields |= nameClass != NameClass::Index;
            continue;
        }
        if (hasNamedMemberOutsideFields || !tableOfImageTakesMemberInChoices(walker, { layout, *field }, member.value))
            return false;
        fields.append(*field);
    }
    return !isCompiledObject || plan.allocation(program, layout, AllocationClassID(), fields.span());
}

namespace {

struct SurveyOfDataInChoices {
    bool tableOfImageHoldsIt { true };
    // While the table holds it: each object of the tree is a cell of a layout, and each of its members lies in a slot, or it is an ordinary object without members. So one allocation makes it, and nothing is defined by name behind that.
    bool allocationsAloneMakeIt { true };
    uint32_t numberOfNodes { 0 };
};

} // namespace

static SurveyOfDataInChoices surveyDataInChoices(Walker& walker, const DataNode& root)
{
    SurveyOfDataInChoices survey;
    Vector<const DataNode*, 16> aggregates;
    Vector<FieldIndex, 16> fields;
    auto add = [&](const DataNode& node) {
        ++survey.numberOfNodes;
        if (isAggregateOfDataInChoices(node))
            aggregates.append(&node);
    };
    add(root);
    while (!aggregates.isEmpty()) {
        const DataNode& aggregate = *aggregates.takeLast();
        if (const auto* array = aggregate.tryAs<DataNode::Array>()) {
            for (const DataNode& element : array->elements)
                add(element);
            continue;
        }
        const auto& object = aggregate.as<DataNode::Object>();
        if (!tableOfImageHoldsObjectInChoices(walker, object, fields)) {
            survey.tableOfImageHoldsIt = false;
            return survey;
        }
        bool isCellOfLayout = object.layout && walker.plan().layout(object.layout).holdsCompiledObjects();
        survey.allocationsAloneMakeIt &= isCellOfLayout ? fields.size() == object.members.size() : object.members.isEmpty();
        for (const DataMember& member : object.members)
            add(member.value);
    }
    return survey;
}

static void becomeLeafOfDataInChoices(Walker& walker, Draft& draft, const DataNode& leaf)
{
    switch (leaf.kind()) {
    case DataNode::Kind::Undefined:
        becomeInChoices(draft, Opcode::Undefined);
        return;
    case DataNode::Kind::Null:
        becomeInChoices(draft, Opcode::Null);
        return;
    case DataNode::Kind::Boolean:
        becomeInChoices(draft, Opcode::Boolean);
        draft.setBoolean(leaf.as<DataNode::Boolean>().value);
        return;
    case DataNode::Kind::Number: {
        double number = leaf.as<DataNode::Number>().value;
        if (isTrackedInteger(number)) {
            becomeInChoices(draft, Opcode::Integer);
            draft.setInteger(static_cast<int64_t>(number));
            return;
        }
        becomeInChoices(draft, Opcode::Double);
        draft.setNumber(number);
        return;
    }
    case DataNode::Kind::String:
        becomeInChoices(draft, Opcode::String);
        draft.setString(leaf.as<DataNode::String>().value);
        return;
    case DataNode::Kind::BigInt:
        becomeInChoices(draft, Opcode::BigInt);
        draft.setDigits(leaf.as<DataNode::BigInt>().digits);
        return;
    case DataNode::Kind::Hole:
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        break;
    }
    walker.fail("a hole of constant data stands outside an array"_s);
}

static void becomeNodeOfDataInChoices(Walker&, Draft&, const DataNode&);

// Null after a failure.
static Reference buildNodeOfDataInChoices(Walker& walker, Origin origin, const DataNode& node)
{
    Draft draft(Opcode::Absent, origin);
    becomeNodeOfDataInChoices(walker, draft, node);
    if (draft.opcode == Opcode::Absent)
        return { };
    if (primitiveConstants.contains(draft.opcode))
        return walker.constant(WTF::move(draft));
    // `{ height: 10, style: { bold: true } }`: the object inside is an allocation like any other. It leaves where the one around it lists it, and is as it was made until something is done to it.
    // `{ codes: { name: "http", 404: "missing" } }`: what the allocation cannot list is defined behind it, and the draft is the FinishObject behind those definitions: that object has more than its allocation says, and nothing is noted of it.
    bool isAsAllocated = draft.opcode == Opcode::NewObject;
    Reference built = walker.build(WTF::move(draft));
    if (isAsAllocated)
        noteFreshObject(walker, built, SIZE_MAX);
    return built;
}

// Array indexes are listed in front of the other own keys, which are listed in the order of their creation.
// So the first other name that the allocation cannot fill ends what it lists, and from there on each member is defined in its turn. Its value is made in front of its definition, so the values of a long object are not all held at once.
static void becomeObjectOfDataInChoices(Walker& walker, Draft& draft, const DataNode::Object& object)
{
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    Origin origin = draft.origin();
    LayoutID layout = object.layout;

    Vector<FieldIndex, 16> fields;
    Vector<Reference, 16> values;
    Vector<uint32_t, 4> membersDefinedBehind;
    bool allocationMayListMore = layout && plan.layout(layout).holdsCompiledObjects();
    for (uint32_t index = 0; index < object.members.size(); ++index) {
        const DataMember& member = object.members[index];
        std::optional<FieldIndex> field = allocationMayListMore ? plan.slots().findField(program, layout, member.key) : std::nullopt;
        if (!field || (plan.slot(layout, *field).hasStorage() && !fieldIsPlainPropertyInChoices(walker, { layout, *field }))) {
            membersDefinedBehind.append(index);
            allocationMayListMore &= program.nameClassOf(member.key) == NameClass::Index;
            continue;
        }
        Reference value = buildNodeOfDataInChoices(walker, origin, member.value);
        if (!value)
            return;
        fields.append(*field);
        values.append(value);
    }

    Draft allocation(layout ? Opcode::NewObject : Opcode::NewDictionary, origin);
    allocation.inputValueGiven = draft.inputValueGiven;
    Reference built;
    if (layout) {
        allocation.setLayout(layout);
        allocation.setFields(fields.span());
        allocation.setVariadicOperands(values.span());
        if (!settleAllocationOfObjectInChoices(walker, allocation, built))
            return;
    }
    if (membersDefinedBehind.isEmpty()) {
        draft = WTF::move(allocation);
        return;
    }

    if (!built)
        built = walker.build(WTF::move(allocation));
    if (!built)
        return;
    for (uint32_t index : membersDefinedBehind) {
        Reference value = buildNodeOfDataInChoices(walker, origin, object.members[index].value);
        if (!value)
            return;
        defineBehindAllocationInChoices(walker, origin, built, object.members[index].key, value);
    }
    becomeFinishedObjectInChoices(draft, built);
}

// `const found = []` that goes on growing, with nothing to plan its room by. Its site remembers how long its lists came to be and which kind of storage they turned into, and later lists are born so.
// No program sees the choice, and the literal names no storage that anybody could lean on. A site that makes one list has no later list to teach.
static void chooseWhatListTakesFromItsSiteInChoices(Walker& walker, Draft& list)
{
    bool isEmpty = !list.numberOfVariadicOperands() && list.holes().empty() && !list.capacityOperand();
    // Storage for any value is the most general kind: a list that is written to hold texts or objects is born with it and has no kind to learn.
    bool kindIsWritten = list.contents() && !walker.types().at(list.contents()).isAnything() && arrayStorageHolding(walker.types(), list.contents()) == ArrayStorage::Contiguous;
    if (isEmpty && !kindIsWritten && list.storage() == AllocationStorage::Heap && list.arrayStorage() == ArrayStorage::Other && (walker.isInLoop() || !walker.facts().runsAtMostOnce(walker.unit().function)))
        list.setLiteralStorage(ArrayLiteralStorage::Profiled);
}

// The draft becomes the operation that gives the node, and what that operation names is built in front of it. After a failure the draft is what it was.
static void becomeNodeOfDataInChoices(Walker& walker, Draft& draft, const DataNode& node)
{
    if (const auto* object = node.tryAs<DataNode::Object>()) {
        becomeObjectOfDataInChoices(walker, draft, *object);
        return;
    }
    const auto* array = node.tryAs<DataNode::Array>();
    if (!array) {
        becomeLeafOfDataInChoices(walker, draft, node);
        return;
    }

    Origin origin = draft.origin();
    Vector<Reference, 16> elements;
    Vector<uint32_t, 4> holes;
    for (uint32_t position = 0; position < array->elements.size(); ++position) {
        const DataNode& element = array->elements[position];
        if (element.is<DataNode::Hole>()) {
            holes.append(position);
            continue;
        }
        Reference built = buildNodeOfDataInChoices(walker, origin, element);
        if (!built)
            return;
        elements.append(built);
    }
    becomeInChoices(draft, Opcode::NewArray);
    draft.setContents(array->contents);
    draft.setHoles(holes.span());
    draft.setVariadicOperands(elements.span());
    chooseWhatListTakesFromItsSiteInChoices(walker, draft);
}

static bool canLieInSharedStorageInChoices(const DataNode& element)
{
    switch (element.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Boolean:
    case DataNode::Kind::Number:
    case DataNode::Kind::String:
        return true;
    case DataNode::Kind::Hole:
    case DataNode::Kind::BigInt:
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool tableOfImageHoldsData(Walker& walker, DataID data)
{
    return surveyDataInChoices(walker, walker.program().at(data).root).tableOfImageHoldsIt;
}

// Storage without cells lies in the image. Any other is created by the first array of the site and kept for those that follow.
// Where the code runs once none follows, and the array has storage of its own.
static bool sharesStorageOfArrayInChoices(Walker& walker, const DataNode::Array& array)
{
    // An empty list is written to be filled, and the first write to shared storage goes through the run time.
    if (array.elements.isEmpty())
        return false;
    if (isStorageOfArray(array))
        return true;
    FunctionID function = walker.currentFunction();
    if ((walker.program().at(function).isModuleBody() || walker.facts().runsAtMostOnce(function)) && !walker.isInLoop())
        return false;
    return std::ranges::all_of(array.elements, canLieInSharedStorageInChoices);
}

// ReadOnlyTree makes every object of the tree anew in one call of the run time, and CopyOnWriteArray a new cell whose storage is copied by the first write: both are right whatever is done with the result.
// A tree that the table of the image cannot hold is written out. So is a small one that allocations alone make: a few stores in line in place of the call, and the walk sees what the objects hold.
static void chooseEncodingOfDataInChoices(Walker& walker, Draft& data)
{
    const DataNode& root = walker.program().at(data.data()).root;
    if (const auto* array = root.tryAs<DataNode::Array>(); array && sharesStorageOfArrayInChoices(walker, *array)) {
        data.setEncoding(DataEncoding::CopyOnWriteArray);
        return;
    }
    data.setEncoding(DataEncoding::ReadOnlyTree);
    if (isAggregateOfDataInChoices(root)) {
        SurveyOfDataInChoices survey = surveyDataInChoices(walker, root);
        if (survey.tableOfImageHoldsIt && !(survey.allocationsAloneMakeIt && survey.numberOfNodes <= maxNodesOfExpandedData))
            return;
    }
    becomeNodeOfDataInChoices(walker, data, root);
}

// ---- ResultAt

// A list of results takes each value as it is, and its record does not say how that is. Only whoever sees what it lists can.
static void chooseFormOfResultInChoices(Walker& walker, Draft& read)
{
    Reference results = read.resultsOperand();
    if (read.representation != Representation::Unchosen || !results || !results.isValue())
        return;
    const Code& code = walker.code();
    const Operation& producer = code.at(results.id());
    if (producer.opcode == Opcode::Results && read.index() < producer.valuesOperands().size())
        read.representation = code.at(producer.valuesOperands()[read.index()]).representation;
}

// ---- ForInOpen, ForInStep

// All who enumerate objects of one shape share one enumerator, so it holds no position. A step looks for the first key at or behind the position that it is handed, and says where it found one.
// The next position goes round the loop in a word of the heap. No such word takes an enumerator, which is no value of JavaScript, so a list of the enumerator and the cell stands for the open.
// Nobody reads the list, and the sweep drops it: a body names an open by its definition alone, so a step finds both parts there.
enum class PartOfOpenForInInChoices : uint32_t { Enumerator, Cell };
static constexpr uint32_t numberOfPartsOfOpenForInInChoices = 2;
// By the row of ForInStep: as many indexes and as many names as 32 bits count.
static constexpr int64_t lastPositionFoundByForInInChoices = 2 * static_cast<int64_t>(UINT32_MAX);

static Reference boxedIntegerInChoices(Walker& walker, Origin origin, int64_t integer)
{
    Draft constant(Opcode::Integer, origin);
    constant.setInteger(integer);
    constant.representation = Representation::JSValue;
    return walker.constant(WTF::move(constant));
}

// A number is no cell, so the cell is not remembered.
static void storePositionOfForInInChoices(Walker& walker, Origin origin, Reference cell, Reference position)
{
    Draft store(Opcode::PutHeapFrameWord, origin);
    store.setFrameOperand(cell);
    store.setIndex(0);
    store.setValueOperand(position);
    walker.build(WTF::move(store));
}

static Reference buildResultOfForInStepInChoices(Walker& walker, Origin origin, Reference step, ForInStepResult result)
{
    Draft read(Opcode::ResultAt, origin);
    read.setResultsOperand(step);
    read.setIndex(static_cast<uint32_t>(result));
    return walker.build(WTF::move(read));
}

// The position is the compiler's own, so this is no guess and has no closed arm. Code has no way to state a type: the type is tested for, and the failing side traps. Null after a failure.
static Reference proveOrTrapInChoices(Walker& walker, Origin origin, Reference value, TypeID promised)
{
    TypeArena& types = walker.types();
    if (types.isSubset(walker.typeOf(value), promised))
        return value;

    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(value);
    test.setTestedType(types.testablePartOf(promised));
    Reference passes = walker.build(WTF::move(test));
    if (!passes)
        return { };

    BlockID passing = walker.addBlock();
    BlockID failing = walker.addBlock(BlockFrequency::Rare);
    walker.branch(passes, passing, failing);
    walker.continueIn(failing);
    walker.code().end(failing, TerminatorDraft(TerminatorKind::Unreachable, origin), 0);
    walker.continueIn(passing);

    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(passes);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(true);
    return walker.build(WTF::move(narrowed));
}

static void openForInInChoices(Walker& walker, Draft& open)
{
    Origin origin = open.origin();
    Reference enumerator = walker.build(Draft(open));
    if (!enumerator)
        return;

    Draft allocation(Opcode::NewHeapWords, origin);
    allocation.setNumberOfWords(1);
    Reference cell = walker.build(WTF::move(allocation));
    if (!cell)
        return;
    storePositionOfForInInChoices(walker, origin, cell, boxedIntegerInChoices(walker, origin, 0));

    static_assert(!static_cast<uint32_t>(PartOfOpenForInInChoices::Enumerator) && static_cast<uint32_t>(PartOfOpenForInInChoices::Cell) == 1);
    becomeInChoices(open, Opcode::Results);
    open.setValuesOperands(std::array { enumerator, cell });
}

// The draft becomes the list of what a body reads of a step. After a failure it is what it was, and nothing is built of it.
static void stepForInInChoices(Walker& walker, Draft& step)
{
    if (step.positionOperand())
        return;

    Reference opened = step.enumeratorOperand();
    bool isOpenedHere = opened && opened.isValue() && walker.code().at(opened.id()).opcode == Opcode::Results
        && walker.code().at(opened.id()).valuesOperands().size() == numberOfPartsOfOpenForInInChoices;
    if (!isOpenedHere) {
        walker.fail("a step of for-in names no position and no open that holds one"_s);
        return;
    }
    std::span<const ValueID> parts = walker.code().at(opened.id()).valuesOperands();
    Reference enumerator = Reference::value(parts[static_cast<uint32_t>(PartOfOpenForInInChoices::Enumerator)]);
    Reference cell = Reference::value(parts[static_cast<uint32_t>(PartOfOpenForInInChoices::Cell)]);

    Origin origin = step.origin();
    TypeID typeOfPosition = walker.types().integer(Range { 0, lastPositionFoundByForInInChoices + 1 });

    Draft read(Opcode::GetHeapFrameWord, origin);
    read.setFrameOperand(cell);
    read.setIndex(0);
    read.setValueType(typeOfPosition);
    // The word holds a boxed value, whatever form the type has elsewhere.
    read.representation = Representation::JSValue;
    Reference position = walker.build(WTF::move(read));
    if (!position)
        return;

    Draft search(step);
    search.setEnumeratorOperand(enumerator);
    search.setPositionOperand(position);
    Reference results = walker.build(WTF::move(search));
    if (!results)
        return;
    Reference isDone = buildResultOfForInStepInChoices(walker, origin, results, ForInStepResult::Done);
    Reference key = buildResultOfForInStepInChoices(walker, origin, results, ForInStepResult::Key);
    Reference found = buildResultOfForInStepInChoices(walker, origin, results, ForInStepResult::Position);
    if (!isDone || !key || !found)
        return;

    Draft sum(Opcode::Add, origin);
    sum.setLeftOperand(found);
    sum.setRightOperand(boxedIntegerInChoices(walker, origin, 1));
    sum.setForm(OperatorForm::Generic);
    Reference next = walker.build(WTF::move(sum));
    if (next)
        next = proveOrTrapInChoices(walker, origin, next, typeOfPosition);
    if (!next)
        return;
    storePositionOfForInInChoices(walker, origin, cell, next);

    becomeInChoices(step, Opcode::Results);
    step.setValuesOperands(std::array { isDone, key });
}

void Walker::chooseConservativeFields(Draft& draft)
{
    Opcode opcode = draft.opcode;
    if (!opcodesThatNeedChoices.contains(opcode))
        return;
    if (Operation::opcodesWithForm.contains(opcode)) {
        draft.setForm(OperatorForm::Generic);
        return;
    }
    if (opcode == Opcode::NewObject) {
        chooseAllocationOfObjectInChoices(*this, draft);
        return;
    }
    if (opcode == Opcode::NewClosure) {
        chooseFormOfClosureInChoices(*this, draft);
        return;
    }
    if (opcode == Opcode::NewArray) {
        chooseWhatListTakesFromItsSiteInChoices(*this, draft);
        return;
    }
    if (opcode == Opcode::Data) {
        chooseEncodingOfDataInChoices(*this, draft);
        return;
    }
    if (opcode == Opcode::ResultAt) {
        chooseFormOfResultInChoices(*this, draft);
        return;
    }
    if (opcode == Opcode::ForInOpen) {
        openForInInChoices(*this, draft);
        return;
    }
    if (opcode == Opcode::ForInStep) {
        stepForInInChoices(*this, draft);
        return;
    }
    // Whoever claims nothing of what he hands over enters in front of the guard.
    if (opcode == Opcode::CallDirect) {
        if (draft.function() && draft.role() == CodeRole::Body)
            draft.setRole(plan().conventions().directEntryOf(draft.function()).role);
        return;
    }
    // The one row whose emitter has no code for storage without a form. Storage for any value takes whatever the source holds.
    RELEASE_ASSERT(opcode == Opcode::NewArrayOfElements);
    if (!hasArrayStorageForm(draft.arrayStorage()))
        draft.setArrayStorage(ArrayStorage::Contiguous);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
