/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// EscapeAnalysis. `const e = { code, msg }; if (bad) throw wrap(e); return e.code;`: the last operation of a block that leaves the unit hands out an object which only this code has and which another block has made.
// The object is made again in front of the call, out of what the state knows it to hold. Nothing on that way names it behind the call, so nothing has to remember the new cell, and where no other way needs the first allocation the sweep drops it.
// At a suspension: nothing is left to undo. At a throw: never under a handler, which may name the object. Withheld: the call takes the object as it is.

#include "config.h"
#include "AOTWalkKnownJoins.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// Whoever builds one of these for a call of the body notes nothing of its operands behind it. A store is noted under the value that its builder has read in front of Walker::build().
static constexpr OpcodeSet callsThatHandOutInKnownLeaving { Opcode::Call, Opcode::Construct, Opcode::CallDirect };
// A closure has no mark that says that it is as it was made.
static constexpr OpcodeSet allocationsMadeAgainInKnownLeaving { Opcode::NewObject, Opcode::NewArray };

// usesThatKeepFreshObjectHere of AOTWalkEscapeAnalysis.cpp.
static constexpr OperandUses usesThatKeepObjectHereInKnownLeaving {
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

// As markEscapingOperands() decides, but for what leaves only because the operation may run other code.
static bool callHandsOutOperandInKnownLeaving(Walker& walker, const Draft& call, uint32_t operandIndex)
{
    uint32_t numberOfNamedOperands = call.numberOfNamedOperands();
    if (call.opcode == Opcode::CallDirect && operandIndex >= numberOfNamedOperands && returnsResultOfFunction(call.role()) && !call.numberOfBoundArguments()) {
        const LinkTables& link = walker.facts().linked();
        uint32_t argument = operandIndex - numberOfNamedOperands;
        if (call.function().index() < link.functions.size() && argument < link.parametersOf(call.function()).size() && !link.parametersOf(call.function())[argument].escapes)
            return false;
    }
    const OperandInfo& info = operandInfoOf(call.opcode, std::min(operandIndex, numberOfNamedOperands));
    return !info.uses.containsOnly(usesThatKeepObjectHereInKnownLeaving) || !info.protocols.isEmpty();
}

static bool namesObjectInKnownLeaving(Walker& walker, ValueID input, Reference object)
{
    Reference bound = input ? walker.referenceBoundTo(input) : Reference();
    return bound && bound.isValue() && rootOfCellInSweptCode(walker.code(), bound.id()) == object.id();
}

// The allocation of the body that the object is, by way of the operands of the call that name it. Null: they lead to a parameter, or to two values.
static ValueID findAllocationInBodyForKnownLeaving(Walker& walker, const BodyIndex& index, const BodyValue& call, Reference object)
{
    ValueID made;
    bool leadsToOneAllocation = true;
    call.forEachOperand([&](ValueID operand, uint32_t) {
        if (!namesObjectInKnownLeaving(walker, operand, object))
            return;
        for (ValueID name = operand; name && !index.isParameter(name); name = index.value(name).operand(0)) {
            if (inputOperationsThatHandOn.contains(index.opcodeOf(name)))
                continue;
            if (!made || made == name) {
                made = name;
                return;
            }
            break;
        }
        leadsToOneAllocation = false;
    });
    return leadsToOneAllocation && made && namesObjectInKnownLeaving(walker, made, object) ? made : ValueID();
}

// valueFitsSlotInChoices() of AOTWalkChoices.cpp: what does not fit is defined by name behind the allocation, which is no allocation any more.
static bool valueFitsSlotInKnownLeaving(Walker& walker, FieldReference field, Reference value)
{
    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(value);
    TypeID typeOfPlan = walker.plan().slot(field.layout, field.field).type;
    if (!proven || (typeOfPlan && !types.isSubset(proven, typeOfPlan)))
        return false;
    FieldFacts facts = walker.facts().field(field);
    if (facts.facts.contains(FieldFact::IsAccessor))
        return false;
    if (!facts.slotType || !typeOfPlan)
        return true;
    TypeID typeOfFacts = facts.facts.contains(FieldFact::SlotHoldsUndefinedAndNull) ? types.withPrimitives(facts.slotType, Primitive::UndefinedOrNull) : facts.slotType;
    return types.isSubset(proven, typeOfFacts);
}

// Null: the state does not know all that the object is.
static Reference makeObjectAgainInKnownLeaving(Walker& walker, const BodyValue& made, Reference object)
{
    const Plan& plan = walker.plan();
    const Operation& record = walker.code().at(object.id());
    if (record.prototypeOperand() || made.operand(OperandIndex::NewObject::prototype))
        return { };
    static constexpr OptionSet<FactOfCell> factsOfObjectAsAllocated { FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot, FactOfCell::HasLayoutPrototype };
    if (!walker.state().factsOfCell(object).containsAll(factsOfObjectAsAllocated))
        return { };
    std::optional<Vector<PropertyOfFreshObject, 8>> properties = propertiesOfFreshObject(walker, object);
    if (!properties)
        return { };

    LayoutID layout = record.layout();
    if (!plan.layout(layout).hasSlots())
        return { };
    Vector<FieldIndex, 8> fields;
    Vector<Reference, 8> values;
    for (const PropertyOfFreshObject& property : *properties) {
        if (property.field.layout != layout || !plan.slot(layout, property.field.field).hasStorage() || !valueFitsSlotInKnownLeaving(walker, property.field, property.value))
            return { };
        fields.append(property.field.field);
        values.append(property.value);
    }
    if (!plan.allocation(walker.program(), layout, AllocationClassID(), fields.span()))
        return { };

    Draft again = walker.draftOf(made);
    again.standsForNoValueOfInput = true;
    again.inputValueGiven = { };
    again.setFields(fields.span());
    again.setValuesOperands(values.span());
    walker.chooseConservativeFields(again);
    Reference built = walker.isUnreachable() ? Reference() : walker.build(WTF::move(again));
    if (built)
        noteFreshObject(walker, built, fields.size());
    return built;
}

static Reference makeListAgainInKnownLeaving(Walker& walker, const BodyValue& made, Reference list)
{
    // An iteration by index holds the list by its record.
    if (KnownJoin::someListIsWalkedByIndex(walker) || !isFreshListAsMade(walker, list))
        return { };
    std::optional<Vector<Reference, 8>> elements = elementsOfFreshList(walker, list);
    if (!elements || elements->size() > maxComputedElementsOfArrayLiteral)
        return { };

    Draft again = walker.draftOf(made);
    again.standsForNoValueOfInput = true;
    again.inputValueGiven = { };
    again.setElementsOperands(elements->span());
    walker.chooseConservativeFields(again);
    Reference built = walker.isUnreachable() ? Reference() : walker.build(WTF::move(again));
    if (built)
        noteFreshList(walker, built);
    return built;
}

void makeObjectsAgainWhereTheyLeave(Walker& walker, Draft& draft)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !walker.has(Mechanism::Folding) || !callsThatHandOutInKnownLeaving.contains(draft.opcode) || !draft.inputValueGiven)
        return;
    if (walker.isUnreachable() || walker.state().isEmpty())
        return;

    const Code& code = walker.code();
    Vector<Reference, 2> objects;
    draft.forEachOperand([&](Reference operand, uint32_t operandIndex) {
        if (!operand || !operand.isValue())
            return;
        ValueID cell = rootOfCellInSweptCode(code, operand.id());
        if (code.isConstant(cell) || !allocationsMadeAgainInKnownLeaving.contains(code.at(cell).opcode) || code.at(cell).block() == walker.currentBlock())
            return;
        Reference object = Reference::value(cell);
        if (walker.state().isFresh(object) && !objects.contains(object) && callHandsOutOperandInKnownLeaving(walker, draft, operandIndex))
            objects.append(object);
    });
    if (objects.isEmpty())
        return;

    // WHAT A STATE KNOWS MAY BE DROPPED ANYWHERE, so no state is trusted to remember that the object has become another cell: nothing may name it behind the call.
    // The call is the last operation of its block, the block leads nowhere, no handler stands round it, and in the copy of a callee a return leads on in the caller.
    const BodyIndex& index = walker.currentBodyIndex();
    BodyValue call = index.value(draft.inputValueGiven);
    if (index.isParameter(call.id()) || !callsThatHandOutInKnownLeaving.contains(call.opcode()))
        return;
    BodyBlock standing = index.block(index.blockOf(call.id()));
    BodyTerminator end = standing.terminator();
    bool endsInThrow = end.kind() == TerminatorKind::Throw;
    if (call.id().index() + 1 != standing.endOfValues().index() || end.numberOfTargets() || KnownJoin::standsUnderHandler(walker) || (!endsInThrow && walker.depthOfCopies()))
        return;
    // What runs once makes the object once, whichever block makes it.
    FunctionID function = walker.currentFunction();
    if (!walker.isInLoop() && (walker.facts().runsAtMostOnce(function) || walker.program().at(function).isModuleBody()))
        return;

    for (Reference object : objects) {
        // A way that is as likely as the one that has made the object would allocate twice where another way needs the first cell.
        if (!endsInThrow && code.at(walker.currentBlock()).frequency <= code.at(code.at(object.id()).block()).frequency)
            continue;
        bool isNamedByEnd = false;
        end.forEachOperand([&](ValueID operand, uint32_t) {
            isNamedByEnd |= namesObjectInKnownLeaving(walker, operand, object);
        });
        if (isNamedByEnd)
            continue;
        ValueID allocation = findAllocationInBodyForKnownLeaving(walker, index, call, object);
        if (!allocation || index.opcodeOf(allocation) != code.at(object.id()).opcode)
            continue;
        // Each turn of a loop makes its own, and an object from in front of a loop is one object in every turn.
        BlockID making = index.blockOf(allocation);
        if (making == standing.id() || index.loopOf(making) != index.loopOf(standing.id()))
            continue;

        BodyValue made = index.value(allocation);
        Reference built = made.opcode() == Opcode::NewObject ? makeObjectAgainInKnownLeaving(walker, made, object) : makeListAgainInKnownLeaving(walker, made, object);
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        if (!built || !built.isValue())
            continue;
        Reference again = Reference::value(rootOfCellInSweptCode(code, built.id()));
        for (uint32_t operandIndex = 0; operandIndex < draft.numberOfOperands(); ++operandIndex) {
            Reference operand = draft.operand(operandIndex);
            if (operand && operand.isValue() && rootOfCellInSweptCode(code, operand.id()) == object.id())
                draft.setOperand(operandIndex, again);
        }
    }
}

// ---- `arguments`

// BodyLiveness::isLiveBefore() says yes where the readers that are left stand on another way. This looks at the rest of the block and at the heads that its edges lead to.
static bool isNamedBehindInKnownLeaving(const BodyIndex& index, const BodyLiveness& liveness, ValueID value, ValueID point)
{
    BodyBlock block = index.block(index.blockOf(point));
    bool isNamed = block.hasHandler();
    auto look = [&](ValueID operand, uint32_t) {
        isNamed |= operand == value;
    };
    for (uint32_t next = point.index() + 1; !isNamed && next < block.endOfValues().index(); ++next) {
        if (index.opcodeOf(ValueID(next)) != Opcode::ReleaseHint)
            index.value(ValueID(next)).forEachOperand(look);
    }
    block.terminator().forEachOperand(look);
    for (BodyTarget target : block.terminator().targets())
        isNamed |= std::ranges::contains(target.arguments(), value) || liveness.isLiveAtHead(value, target.block());
    return isNamed;
}

// Every mention of the object in the body reads its length, an element or the variable of a parameter, which changes nothing of it, or is a call behind which nothing names it, in no other loop than the one that makes it.
// So each such call may make its own: no way makes two, and nobody can hold one against another.
static bool isOnlyReadAndHandedOutLastInKnownLeaving(const BodyIndex& index, const BodyLiveness& liveness, PropertyKeyID length, ValueID arguments)
{
    uint32_t mentionsLeft = index.useCount(arguments);
    if (mentionsLeft == BodyIndex::maxCountedUses)
        return false;
    LoopID loop = index.loopOf(index.blockOf(arguments));
    for (uint32_t next = arguments.index() + 1; mentionsLeft && next < index.numberOfValues(); ++next) {
        if (index.isParameter(ValueID(next)))
            continue;
        BodyValue user = index.value(ValueID(next));
        uint32_t mentions = 0;
        user.forEachOperand([&](ValueID operand, uint32_t) {
            mentions += operand == arguments;
        });
        if (!mentions)
            continue;
        mentionsLeft -= mentions;
        Opcode opcode = user.opcode();
        if (opcode == Opcode::ReleaseHint || opcode == Opcode::GetMappedArgument || (opcode == Opcode::PropGet && !user.isFreeName() && user.key() == length) || (opcode == Opcode::ElemGet && user.propertyOperand() != arguments))
            continue;
        if (!opcodesThatMayTakeArgumentsLast.contains(opcode) || index.loopOf(index.blockOf(user.id())) != loop || isNamedBehindInKnownLeaving(index, liveness, arguments, user.id()))
            return false;
    }
    // What is left stands at the end of a block, on an edge or at a head.
    return !mentionsLeft;
}

ArgumentsMadeWhereTheyLeave::ArgumentsMadeWhereTheyLeave(Walker& walker, const BodyValue& call)
    : m_walker(walker)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID name;
    call.forEachOperand([&](ValueID operand, uint32_t) {
        if (!name && operand && !index.isParameter(operand) && opcodesThatMakeArguments.contains(index.opcodeOf(operand)))
            name = operand;
    });
    Reference bound = name ? walker.referenceBoundTo(name) : Reference();
    if (!bound || !bound.isValue() || walker.isUnreachable() || KnownJoin::standsUnderHandler(walker))
        return;
    const Code& code = walker.code();
    ValueID cell = rootOfCellInSweptCode(code, bound.id());
    if (code.isConstant(cell) || !opcodesThatMakeArguments.contains(code.at(cell).opcode) || code.at(cell).block() == walker.currentBlock() || !walker.state().isFresh(Reference::value(cell)))
        return;
    // A rest parameter is a list of the program's, which may have changed since the entry. The language has no name for the hidden list.
    FunctionID function = walker.currentFunction();
    if (!walker.program().at(function).has(FunctionFlags::HasHiddenRest))
        return;
    // What runs once makes the object once, whichever block makes it.
    if (!walker.isInLoop() && (walker.facts().runsAtMostOnce(function) || walker.program().at(function).isModuleBody()))
        return;
    if (!isOnlyReadAndHandedOutLastInKnownLeaving(index, KnownJoin::livenessOf(walker, index), walker.program().keyOf(BuiltinNames::length), name))
        return;

    m_name = name;
    m_boundBefore = bound;
    buildFreshObject(walker, index.value(name));
}

ArgumentsMadeWhereTheyLeave::~ArgumentsMadeWhereTheyLeave()
{
    if (m_name)
        m_walker.bind(m_name, m_boundBefore);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
