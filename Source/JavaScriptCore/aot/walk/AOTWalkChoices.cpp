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
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypeClosure.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!static_cast<unsigned>(AllocationStorage::Heap) && !static_cast<unsigned>(ArrayLiteralStorage::Fresh) && !static_cast<unsigned>(ArrayStorage::Other));
static_assert(!static_cast<unsigned>(ExpectedElements::Anything) && !static_cast<unsigned>(ClosureForm::Full) && !static_cast<unsigned>(CodeRole::Body));
static_assert(!static_cast<unsigned>(TailCallRequirement::Required));
static_assert(!static_cast<unsigned>(OperatorForm::Unselected) && !static_cast<unsigned>(DataEncoding::Unselected));

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
    Effects ofAllocation = declaredBoundOf(Opcode::NewObject);
    walker.build(WTF::move(definition), &ofAllocation);
}

static bool closuresHoldCapturesInChoices(const Walker& walker, FunctionID function)
{
    return walker.program().at(function).captures.size() > walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(function);
}

static bool isOnlyClassObjectInChoices(const Walker& walker, FunctionID function)
{
    const Program& program = walker.program();
    LayoutID constructed = program.at(function).constructedClass;
    if (!constructed || closuresHoldCapturesInChoices(walker, function))
        return false;
    const Layout& layout = program.at(constructed);
    return layout.has(LayoutFlags::IsConstructorFunction) && layout.constructor == function && walker.facts().isEvaluatedAtMostOnce(constructed);
}

static void chooseFormOfClosureInChoices(Walker& walker, Draft& closure)
{
    const Plan& plan = walker.plan();
    FunctionID function = closure.function();
    closure.setStorage(AllocationStorage::Heap);
    if (closure.isMadeOnDemand()) {
        closure.setClosureForm(ClosureForm::Deferred);
        return;
    }

    if (closure.isHeldByClosureTable() || isOnlyClassObjectInChoices(walker, function)) {
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

bool isClosureLeftUnmade(const Walker& walker, Reference reference)
{
    if (!reference || !reference.isValue())
        return false;
    const Operation& made = walker.code().at(reference.id());
    return made.opcode == Opcode::NewClosure && made.closureForm() == ClosureForm::Deferred;
}

Reference findScopeOfClosureReadOffObjectInSight(const Walker& walker, FunctionID function, Reference closure)
{
    const Code& code = walker.code();
    if (!closure || !closure.isValue())
        return { };
    const Operation& read = code.at(closure.id());
    if (read.opcode != Opcode::GetDeferredClosureSlot || walker.program().at(function).captures.size() != 1)
        return { };
    auto findScopeOfClosureThatStateHolds = [&]() -> Reference {
        if (!walker.has(Mechanism::Folding) || walker.checksProofs())
            return { };
        ValueID holder = underlyingCellOf(code, read.objectOperand());
        if (code.isConstant(holder) || code.at(holder).opcode != Opcode::NewObject)
            return { };
        std::optional<FieldReference> field = findField(walker.program(), code.at(holder).layout(), read.key());
        Reference unmade = field ? walker.state().slot(Reference::value(holder), *field) : Reference();
        if (!isClosureLeftUnmade(walker, unmade))
            return { };
        const Operation& made = code.at(unmade.id());
        if (made.function() != function || made.numberOfVariadicOperands() != 1)
            return { };
        ValueID scope = made.variadicOperands()[0];
        return code.isConstant(scope) ? Reference::constant(scope) : Reference::value(scope);
    };
    if (Reference scope = findScopeOfClosureThatStateHolds())
        return scope;
    auto findScopeReadBesideSlot = [&]() -> Reference {
        ValueID next(closure.id().index() + 1);
        if (next.index() >= code.numberOfValues() || code.isConstant(next))
            return { };
        const Operation& capture = code.at(next);
        return capture.opcode == Opcode::GetCapture && capture.closureOperand() == closure.id() && capture.function() == function && !capture.index() ? Reference::value(next) : Reference();
    };
    return findScopeReadBesideSlot();
}

static bool fieldIsPlainPropertyInChoices(const Walker& walker, FieldReference field)
{
    FieldFacts facts = walker.facts().field(field);
    if (facts.facts.contains(FieldFact::IsAccessor) || !facts.formOfValue.isEmpty())
        return false;
    const Program& program = walker.program();
    return field.field >= program.at(field.layout).fields.size() || fieldAt(program, declarationOf(program, field)).form.isEmpty();
}

static PropertyKeyID keyDefinedBehindAllocationInChoices(const Walker& walker, FieldReference field)
{
    const Program& program = walker.program();
    bool onlyFactsSayAccessor = walker.facts().field(field).facts.contains(FieldFact::IsAccessor)
        && field.field < program.at(field.layout).fields.size() && fieldAt(program, declarationOf(program, field)).form.isEmpty();
    if (!onlyFactsSayAccessor && !fieldIsPlainPropertyInChoices(walker, field))
        return { };
    if (field.field >= program.at(field.layout).fields.size())
        return walker.plan().slots().keyOf(program, field);

    constexpr OptionSet<FieldFlags> filledByAllocationAlone { FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden };
    const Field& declaration = fieldAt(program, declarationOf(program, field));
    return declaration.flags.containsAny(filledByAllocationAlone) ? PropertyKeyID() : declaration.key;
}

bool valueFitsSlot(Walker& walker, FieldReference field, Reference value)
{
    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(value);
    TypeID typeOfPlan = walker.plan().slot(field.layout, field.field).type;
    if (typeOfPlan && !types.isSubset(proven, typeOfPlan))
        return false;
    if (TypeID provenOfField = walker.plan().slot(field.layout, field.field).provenType; provenOfField && !types.isSubset(proven, provenOfField))
        return false;
    if (walker.facts().field(field).facts.contains(FieldFact::IsAccessor))
        return false;

    FieldFacts facts = walker.facts().field(field);
    if (!facts.slotType || !typeOfPlan)
        return true;
    TypeID typeOfFacts = facts.facts.contains(FieldFact::SlotHoldsUndefinedAndNull) ? types.withPrimitives(facts.slotType, Primitive::UndefinedOrNull) : facts.slotType;
    return types.isSubset(proven, typeOfFacts);
}

namespace {

struct MisfitInChoices {
    size_t position { 0 };
    PropertyKeyID key;
    Reference value;
};

} // anonymous namespace

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
    if (!filler || !valueFitsSlot(walker, field, filler))
        return false;
    misfits.append({ position, key, values[position] });
    values[position] = filler;
    return true;
}

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

static bool settleAllocationOfObjectInChoices(Walker& walker, Draft& draft, Reference& built)
{
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    LayoutID layout = draft.layout();
    bool hasSlots = plan.layout(layout).hasSlots();
    draft.setStorage(AllocationStorage::Heap);

    if (auto behind = walker.takeWhatIsDefinedBehindBirthOf(layout); !behind.isEmpty() && draft.fields().size() == draft.numberOfVariadicOperands()) {
        Vector<FieldIndex, 16> fieldsWithWhatIsBehind;
        Vector<Reference, 16> valuesWithWhatIsBehind;
        fieldsWithWhatIsBehind.append(draft.fields());
        valuesWithWhatIsBehind.append(draft.variadicOperands());
        for (const Walker::ValueDefinedBehindBirth& defined : behind) {
            fieldsWithWhatIsBehind.append(defined.field);
            valuesWithWhatIsBehind.append(defined.value);
        }
        draft.setFields(fieldsWithWhatIsBehind.span());
        draft.setVariadicOperands(valuesWithWhatIsBehind.span());
    }
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
            TypeID listed = walker.typeOf(draft.variadicOperands()[index]);
            if (plan.slot(layout, field).indexInLayoutRecord || !plan.layout(layout).holdsCompiledObjects() || !listed || isNeverType(listed) || walker.types().at(listed).hasOnlyPrimitives(Primitive::Absent))
                continue;
            if (!walker.facts().linked().layout(layout).possibleChanges.contains(LayoutChange::GainsProperty)) {
                walker.fail("an allocation lists a value for a name that has no slot"_s);
                return false;
            }
        }
        fields.append(field);
        values.append(draft.variadicOperands()[index]);
    }

    size_t numberListed = 0;
    Vector<MisfitInChoices, 2> misfits;
    if (hasSlots) {
        while (numberListed < fields.size() && plan.slot(layout, fields[numberListed]).hasStorage() && (valueFitsSlot(walker, { layout, fields[numberListed] }, values[numberListed]) || listFillerInPlaceOfMisfitInChoices(walker, layout, fields.span(), values, numberListed, misfits)))
            ++numberListed;
        if (!plan.allocation(program, layout, fields.span().first(numberListed))) {
            numberListed = plan.keyOrders().lengthOfLongestBeginningWithOrderWord(layout, fields.span().first(numberListed));
            if (!plan.allocation(program, layout, fields.span().first(numberListed))) {
                walker.fail("the plan has no allocation of the layout that begins with these fields"_s);
                return false;
            }
        }
    } else if (program.at(layout).kind != LayoutKind::Record || draft.prototypeOperand()) {
        walker.fail("the plan keeps no cells of the layout of an allocation, and a dictionary cannot stand for its objects"_s);
        return false;
    }

    misfits.removeAllMatching([&](const MisfitInChoices& misfit) {
        if (misfit.position < numberListed)
            return false;
        values[misfit.position] = misfit.value;
        return true;
    });

    bool leavesClosuresUnmade = hasSlots && leaveClosuresUnmadeInChoices(walker, layout, fields, values, numberListed);

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
    Draft cut(draft);
    cut.restIsDefinedBehind = true;
    bool owesItsRest = cut.opcode == Opcode::NewObject && walker.makerLacksWhatIsHeldFilled(cut);
    built = walker.build(WTF::move(cut));
    if (!built)
        return false;
    Vector<FieldIndex, 2> fieldsDefinedBehind;
    for (const MisfitInChoices& misfit : misfits)
        fieldsDefinedBehind.append(fields[misfit.position]);
    for (size_t index = numberListed; index < fields.size(); ++index)
        fieldsDefinedBehind.append(fields[index]);
    walker.noteFillersOfAllocation(built, WTF::move(fieldsDefinedBehind));
    for (const MisfitInChoices& misfit : misfits)
        defineBehindAllocationInChoices(walker, origin, built, misfit.key, misfit.value);
    for (size_t index = 0; index < keysDefinedBehind.size(); ++index)
        defineBehindAllocationInChoices(walker, origin, built, keysDefinedBehind[index], values[numberListed + index]);
    if (owesItsRest && !walker.isUnreachable() && !walker.hasFailed())
        walker.showThatObjectHoldsWhatIsHeldFilled(built, layout);
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

    const Plan& plan = walker.plan();
    const PlannedLayout& planned = plan.layout(layout);
    if (!planned.hasSlots()) {
        walker.fail("the plan keeps no cells of the layout of an environment"_s);
        return;
    }
    allocation.setStorage(AllocationStorage::Heap);
    if (planned.cellsAreHandedBack)
        noteEnvironmentThatUnitMakes(walker, allocation);

    if (allocation.fields().size() != allocation.numberOfVariadicOperands())
        walker.fail("an allocation lists other values than fields"_s);
}

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

static bool dataFitsTypeOfSlotInChoices(Walker& walker, TypeID slotType, const DataNode& value)
{
    if (!slotType)
        return true;
    TypeArena& types = walker.types();
    if (!includesData(types, types.at(slotType), value))
        return false;
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
    if (slot.provenType && !dataFitsTypeOfSlotInChoices(walker, slot.provenType, value))
        return false;
    return dataFitsTypeOfSlotInChoices(walker, slot.type, value) && (isMarkerThatSlotHolds || !slot.type || dataFitsTypeOfSlotInChoices(walker, facts.slotType, value));
}

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
        std::optional<FieldIndex> field = isCompiledObject && !hasNamedMemberOutsideFields ? plan.slots().findField(program, layout, member.key) : std::nullopt;
        if (!field) {
            NameClass nameClass = program.nameClassOf(member.key);
            if (program.at(member.key).kind != PropertyKeyKind::Text || nameClass == NameClass::Proto)
                return false;
            hasNamedMemberOutsideFields |= nameClass != NameClass::Index;
            continue;
        }
        if (!tableOfImageTakesMemberInChoices(walker, { layout, *field }, member.value))
            return false;
        fields.append(*field);
    }
    return !isCompiledObject || plan.allocation(program, layout, fields.span());
}

static Walker::SurveyOfConstant surveyNodesOfDataInChoices(Walker& walker, const DataNode& root)
{
    Walker::SurveyOfConstant survey;
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

static Walker::SurveyOfConstant surveyDataInChoices(Walker& walker, DataID data)
{
    return walker.surveysOfConstants().ensure(data.index(), [&] {
        return surveyNodesOfDataInChoices(walker, walker.program().at(data).root);
    }).iterator->value;
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

static Reference buildNodeOfDataInChoices(Walker& walker, Origin origin, const DataNode& node)
{
    Draft draft(Opcode::Absent, origin);
    becomeNodeOfDataInChoices(walker, draft, node);
    if (draft.opcode == Opcode::Absent)
        return { };
    if (primitiveConstants.contains(draft.opcode))
        return walker.constant(WTF::move(draft));
    bool isAsAllocated = draft.opcode == Opcode::NewObject;
    Reference built = walker.build(WTF::move(draft));
    if (isAsAllocated)
        noteFreshObject(walker, built, SIZE_MAX);
    return built;
}

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

static void chooseWhatListTakesFromItsSiteInChoices(Walker& walker, Draft& list)
{
    bool isEmpty = !list.numberOfVariadicOperands() && list.holes().empty();
    bool kindIsWritten = list.contents() && !walker.types().at(list.contents()).isAnything() && arrayStorageHolding(walker.types(), list.contents()) == ArrayStorage::Contiguous;
    if (isEmpty && (!kindIsWritten || walker.isInLoop()) && list.storage() == AllocationStorage::Heap && list.arrayStorage() == ArrayStorage::Other && (walker.isInLoop() || !walker.program().at(walker.unit().function).isModuleBody()))
        list.setLiteralStorage(ArrayLiteralStorage::Profiled);
}

static bool elementsMayBeInt32(const Walker& walker, const Draft& list)
{
    return list.numberOfVariadicOperands() <= maxArrayLengthInStubKey && std::ranges::all_of(list.variadicOperands(), [&](Reference element) {
        TypeID type = walker.typeOf(element);
        return type && !isNeverType(type) && (walker.types().at(type).primitives & Primitive::Integer);
    });
}

static void nameStorageOfLiteralOfNumbersInChoices(Walker& walker, Draft& list)
{
    if (!walker.has(Mechanism::TypedOperations))
        return;
    if (!list.numberOfVariadicOperands() || !list.holes().empty() || list.storage() != AllocationStorage::Heap || list.arrayStorage() != ArrayStorage::Other)
        return;
    bool holdsNumbersAsWritten = list.contents() && isNumericStorage(arrayStorageHolding(walker.types(), list.contents()));
    if (list.contents() && !walker.types().at(list.contents()).isAnything() && !holdsNumbersAsWritten)
        return;
    if (ArrayStorage asked = walker.requestedLiteralStorage(); isNumericStorage(asked)) {
        bool everyElementFits = std::ranges::all_of(list.variadicOperands(), [&](Reference element) {
            TypeID proven = walker.typeOf(element);
            return proven && walker.types().at(proven).isNumber() && (asked != ArrayStorage::Int32 || walker.types().at(proven).isInt32());
        });
        if (everyElementFits) {
            list.setArrayStorage(asked);
            walker.count(StatisticID::ArrayLiteralsAllocatedForLoops);
            return;
        }
    }
    bool numbersAreWritten = holdsNumbersAsWritten && walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked);
    bool isOnlyHandedOn = list.inputValueGiven && walker.currentBodyIndex().hasOneUse(list.inputValueGiven);
    if (!numbersAreWritten && !isOnlyHandedOn)
        return;
    bool areInt32 = true;
    bool mayHaveFraction = false;
    for (Reference element : list.variadicOperands()) {
        TypeID proven = walker.typeOf(element);
        if (!proven || !walker.types().at(proven).isNumber()) {
            if (numbersAreWritten && elementsMayBeInt32(walker, list))
                list.setLiteralStorage(ArrayLiteralStorage::ByElements);
            return;
        }
        areInt32 &= walker.types().at(proven).isInt32();
        mayHaveFraction |= !!(walker.types().at(proven).primitives & Primitive::Number & ~Primitive::NumberWithoutFraction);
    }
    if (!areInt32 && !numbersAreWritten && !mayHaveFraction)
        return;
    if (!areInt32)
        list.setArrayStorage(ArrayStorage::Double);
    else if (isOnlyHandedOn)
        list.setArrayStorage(ArrayStorage::Int32);
}

static unsigned widthOfStorageInChoices(ArrayStorage storage)
{
    const ArrayStorageForm* form = findArrayStorageForm(storage);
    return form ? static_cast<unsigned>(form - arrayStorageForms.data()) + 1 : 0;
}
static_assert(arrayStorageForms[0].kind == ArrayStorage::Int32 && arrayStorageForms[1].kind == ArrayStorage::Double && arrayStorageForms[2].kind == ArrayStorage::Contiguous);

ArrayStorage askBirthOfListForStorage(Walker& walker, Reference list, ArrayStorage wanted, bool widerListRefusesStore)
{
    if (!list || !list.isValue() || !walker.has(Mechanism::TypedOperations))
        return ArrayStorage::Other;
    ValueID root = underlyingCellOf(walker.code(), list.id());
    const Operation& born = walker.code().at(root);
    if (born.opcode != Opcode::NewArrayLike)
        return ArrayStorage::Other;
    if (born.sourceOperand() && !walker.types().at(walker.code().at(born.sourceOperand()).type).isUndefinedOrNull())
        return born.arrayStorage();
    if (widthOfStorageInChoices(wanted) > widthOfStorageInChoices(born.arrayStorage())) {
        if (widerListRefusesStore && !hasArrayStorageForm(born.arrayStorage()) && born.remembersStorageAtSite())
            walker.code().letListForgetStorageAtSite(root);
        walker.code().bearListWithStorage(root, wanted);
    }
    return born.arrayStorage();
}

static bool stepsElementOfListItselfInChoices(Walker& walker, Reference list, Reference stored)
{
    if (!list || !list.isValue() || !stored.isValue())
        return false;
    const Code& code = walker.code();
    const Operation& made = code.at(stored.id());
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
    if (!steps.contains(made.opcode))
        return false;
    ValueID root = underlyingCellOf(code, list.id());
    bool readsListItself = false;
    bool restAreIntegers = true;
    made.forEachChild([&](ValueID child) {
        const Operation& read = code.at(underlyingCellOf(code, child));
        if (read.opcode == Opcode::GetArrayElement && underlyingCellOf(code, read.arrayOperand()) == root)
            readsListItself = true;
        else
            restAreIntegers &= walker.types().at(read.type).isInteger();
    });
    return readsListItself && restAreIntegers;
}

ArrayStorage askBirthOfListForStorageThatTakes(Walker& walker, Reference list, Reference stored, bool widerListRefusesStore)
{
    if (!stored || isNeverType(walker.typeOf(stored)))
        return askBirthOfListForStorage(walker, list, ArrayStorage::Other);
    Type type = walker.types().at(walker.typeOf(stored));
    if (type.isNumberWithoutFraction() && stepsElementOfListItselfInChoices(walker, list, stored))
        return askBirthOfListForStorage(walker, list, ArrayStorage::Other);
    ArrayStorage taking = ArrayStorage::Other;
    if (type.isNumberWithoutFraction() && type.primitives & Primitive::Integer)
        taking = ArrayStorage::Int32;
    else if (type.isNumber())
        taking = list && list.isValue() && walker.runOverBoxedValuesTakesFirst(underlyingCellOf(walker.code(), list.id())) ? ArrayStorage::Contiguous : ArrayStorage::Double;
    else if (!(type.primitives & Primitive::Number))
        taking = ArrayStorage::Contiguous;
    ArrayStorage born = askBirthOfListForStorage(walker, list, taking, widerListRefusesStore);
    if (born == ArrayStorage::Int32 && taking == ArrayStorage::Int32 && !type.isInt32()) {
        ValueID root = underlyingCellOf(walker.code(), list.id());
        if (!walker.code().at(root).remembersStorageAtSite()) {
            walker.code().letListRememberStorageAtSite(root);
            walker.count(StatisticID::SizedArraysGivenAllocationProfiles);
        }
    }
    return born;
}

static bool isBornHereWithoutSourceInChoices(Walker& walker, Reference list)
{
    if (!list || !list.isValue())
        return false;
    const Operation& born = walker.code().at(underlyingCellOf(walker.code(), list.id()));
    return born.opcode == Opcode::NewArrayLike && (!born.sourceOperand() || walker.types().at(walker.code().at(born.sourceOperand()).type).isUndefinedOrNull());
}

static void chooseStorageOfListBeingFilledInChoices(Walker& walker, Draft& store)
{
    Reference list = store.arrayOperand();
    Reference stored = store.valueOperand();
    if (!list || !stored)
        return;
    ArrayStorage born = askBirthOfListForStorageThatTakes(walker, list, stored, store.opcode == Opcode::TryPutByValDirect);
    if (store.opcode != Opcode::TryPutByValDirect)
        return;
    if (hasArrayStorageForm(born) || !isBornHereWithoutSourceInChoices(walker, list)) {
        if (walker.depthOfCopies() && isNumericStorage(born) && isBornHereWithoutSourceInChoices(walker, list) && !walker.types().at(walker.typeOf(stored)).isNumber()) {
            walker.count(StatisticID::InlineeArrayStoresLeftGeneric);
            return;
        }
        store.setArrayStorage(born);
        return;
    }
    if (walker.depthOfCopies()) {
        ValueID root = underlyingCellOf(walker.code(), list.id());
        if (!walker.code().at(root).remembersStorageAtSite()) {
            walker.code().letListRememberStorageAtSite(root, true);
            walker.count(StatisticID::InlineeArraysGivenAllocationProfiles);
        }
        walker.count(StatisticID::InlineeArrayStoresLeftGeneric);
        return;
    }
    Reference position = store.positionOperand();
    becomeInChoices(store, Opcode::TryPutIndex);
    store.setArrayOperand(list);
    store.setPositionOperand(position);
    store.setValueOperand(stored);
}

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
    if (ArrayStorage written = holes.isEmpty() && !elements.isEmpty() && isStorageOfArray(*array) ? arrayStorageOfData(walker.program(), *array) : ArrayStorage::Other; isNumericStorage(written) && walker.has(Mechanism::TypedOperations))
        draft.setArrayStorage(written);
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
    return surveyDataInChoices(walker, data).tableOfImageHoldsIt;
}

static bool holdsNothingYetInChoices(const DataNode& element)
{
    return element.is<DataNode::Undefined>() || element.is<DataNode::Null>();
}

static bool sharesStorageOfArrayInChoices(Walker& walker, const DataNode::Array& array, DataID data)
{
    if (std::ranges::all_of(array.elements, holdsNothingYetInChoices))
        return false;
    if (isStorageOfArray(array))
        return true;
    FunctionID function = walker.currentFunction();
    if (walker.program().at(function).isModuleBody() && !walker.isInLoop())
        return isShortStringList(array) && walker.facts().summaries().at(function).holdsWrittenList(data);
    return std::ranges::all_of(array.elements, canLieInSharedStorageInChoices);
}

static bool isZeroFilledArray(const DataNode::Array& array)
{
    return array.elements.size() > 1 && std::ranges::all_of(array.elements, [](const DataNode& element) {
        const auto* number = element.tryAs<DataNode::Number>();
        return number && !number->value;
    });
}

static bool variableHoldsListInChoices(Walker& walker, DataID data)
{
    const Program& program = walker.program();
    const Function& function = program.at(walker.currentFunction());
    FunctionID body = program.at(function.module).body;
    return function.isModuleBody() || (body && walker.facts().summaries().at(body).holdsWrittenList(data));
}

static bool someLoopPushesOntoInChoices(Walker& walker, ValueID literal)
{
    const BodyIndex& index = walker.currentBodyIndex();
    for (uint32_t number = 0; literal && number < index.numberOfLoops(); ++number) {
        LoopID loop(number);
        if (index.loop(loop).parent)
            continue;
        for (uint32_t push : walker.pushesOntoLiteralsOf(loop)) {
            if (index.stripCasts(index.value(ValueID(push)).as<Opcode::Call>().thisValueOperand()) == literal)
                return true;
        }
    }
    return false;
}

static void chooseEncodingOfDataInChoices(Walker& walker, Draft& data)
{
    RootOfConstant whereRootEnds = walker.constantEndsWhereItLeaves(data.inputValueGiven, data.data()) ? RootOfConstant::EndsWhereItLeaves : RootOfConstant::IsAsked;
    if (!walker.constantListsFieldsHeldFilled(data.data(), whereRootEnds)) {
        walker.stopAtMaker(data.origin());
        return;
    }
    SetForScope makingObjectsOfConstant = walker.makingObjectsOfConstant();
    const DataNode& root = walker.program().at(data.data()).root;
    if (const auto* array = root.tryAs<DataNode::Array>(); array && sharesStorageOfArrayInChoices(walker, *array, data.data())) {
        if (array->elements.size() <= maxNodesOfExpandedData && isZeroFilledArray(*array) && !variableHoldsListInChoices(walker, data.data())) {
            becomeNodeOfDataInChoices(walker, data, root);
            return;
        }
        bool isStoredInto = ((data.inputValueGiven && walker.facts().summaries().at(walker.currentFunction()).constantListIsStoredIntoInLoop(data.inputValueGiven)) || walker.facts().summaries().storesElementInto(data.data())) && surveyDataInChoices(walker, data.data()).tableOfImageHoldsIt;
        if (isStoredInto)
            walker.count(StatisticID::ArrayLiteralsWithOwnStorage);
        if (isStoredInto && array->elements.size() <= maxNodesOfExpandedData && walker.runsInIterations() && someLoopPushesOntoInChoices(walker, data.inputValueGiven)) {
            becomeNodeOfDataInChoices(walker, data, root);
            return;
        }
        data.setEncoding(isStoredInto ? DataEncoding::TreeWithOwnStorage : DataEncoding::CopyOnWriteArray);
        return;
    }
    data.setEncoding(DataEncoding::ReadOnlyTree);
    if (isAggregateOfDataInChoices(root)) {
        Walker::SurveyOfConstant survey = surveyDataInChoices(walker, data.data());
        if (survey.tableOfImageHoldsIt && !(survey.allocationsAloneMakeIt && survey.numberOfNodes <= maxNodesOfExpandedData))
            return;
    }
    becomeNodeOfDataInChoices(walker, data, root);
}

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

enum class PartOfOpenForInInChoices : uint32_t { Enumerator, Cell };
static constexpr uint32_t numberOfPartsOfOpenForInInChoices = 2;
static constexpr int64_t lastPositionFoundByForInInChoices = 2 * static_cast<int64_t>(UINT32_MAX);

static Reference boxedIntegerInChoices(Walker& walker, Origin origin, int64_t integer)
{
    Draft constant(Opcode::Integer, origin);
    constant.setInteger(integer);
    constant.representation = Representation::JSValue;
    return walker.constant(WTF::move(constant));
}

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

static void nameClassOfSuperBaseInChoices(Walker& walker, Draft& draft)
{
    Reference home = draft.homeOperand();
    if (draft.homeKind() != HomeObjectKind::ClassPrototype || !home)
        return;
    Type type = walker.types().at(walker.typeOf(home));
    FunctionID constructor = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    LayoutID classLayout = constructor ? classOfClassObject(walker.program(), constructor) : LayoutID();
    if (!classLayout || !walker.plan().hasClassRecord(classLayout) || !walker.facts().isEvaluatedAtMostOnce(classLayout))
        return;
    draft.setLayout(classLayout);
    walker.count(StatisticID::SuperBasesFromClassRecords);
}

void Walker::chooseConservativeFieldsOfOpcodeThatNeedsChoices(Draft& draft)
{
    Opcode opcode = draft.opcode;
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
        nameStorageOfLiteralOfNumbersInChoices(*this, draft);
        return;
    }
    if (opcode == Opcode::TryPutByValDirect || opcode == Opcode::TryFillNewArray) {
        chooseStorageOfListBeingFilledInChoices(*this, draft);
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
    if (opcode == Opcode::GetSuperBase) {
        nameClassOfSuperBaseInChoices(*this, draft);
        return;
    }
    if (opcode == Opcode::ForInStep) {
        stepForInInChoices(*this, draft);
        return;
    }
    RELEASE_ASSERT(opcode == Opcode::CallDirect);
    if (draft.function() && draft.role() == CodeRole::Body)
        draft.setRole(plan().conventions().directEntryOf(draft.function()).role);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
