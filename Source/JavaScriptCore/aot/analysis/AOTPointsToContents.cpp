/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPointsToSolverInternal.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTEffectSummaries.h"
#include "AOTFlowResults.h"
#include "AOTSchemaWalkers.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

bool isOrderedBefore(const SolvedContents& a, const SolvedContents& b)
{
    return std::tie(a.container, a.slot, a.object) < std::tie(b.container, b.slot, b.object);
}

static LayoutID layoutPayloadOf(const Value& value)
{
    return Value::opcodesWithLayout.contains(value.opcode) ? value.layout() : LayoutID();
}

static ObjectSlot fieldPayloadOf(const Value& value)
{
    if (!Value::opcodesWithField.contains(value.opcode))
        return std::nullopt;
    return value.field();
}

static PropertyKeyID keyPayloadOf(const Value& value)
{
    if (!Value::opcodesWithKey.contains(value.opcode) || Value::opcodesWithSecondKey.contains(value.opcode))
        return { };
    return value.key();
}

static FunctionID closureFunctionOf(const Value& definition)
{
    FunctionID result;
    if (definition.opcode != Opcode::NewClosure)
        return result;
    forEachFunctionReference(definition, [&](FunctionID function, FunctionReferenceKind kind) {
        if (kind == FunctionReferenceKind::Closure)
            result = function;
    });
    return result;
}

LayoutID PointsToSolver::environmentLayoutOf(ObjectID object) const
{
    LayoutID layout = layoutPayloadOf(definitionOf(object));
    if (!layout || m_program.at(layout).kind != LayoutKind::Environment)
        return { };
    return layout;
}

ContainerState& PointsToSolver::ensureContainer(ObjectID object)
{
    auto& state = m_containers[object.index()];
    if (!state) {
        state = makeUnique<ContainerState>();
        state->environment = environmentLayoutOf(object);
    }
    return *state;
}

OperandUses PointsToSolver::environmentFieldUses(const ContainerState& state, ObjectSlot slot) const
{
    const FlowResults* flow = m_inputs.context.flow;
    if (!state.environment || !flow)
        return { };
    return slot ? flow->storedValueUses(FieldReference { state.environment, *slot }) : OperandSummary().uses;
}

void PointsToSolver::addContents(ObjectID container, ObjectSlot slot, ObjectID object)
{
    ContainerState& state = ensureContainer(container);
    SolvedContents entry { container, slot, object, false };
    size_t position = std::ranges::lower_bound(state.contents, entry, isOrderedBefore) - state.contents.begin();
    if (position < state.contents.size() && !isOrderedBefore(entry, state.contents[position]))
        return;

    for (auto& [reader, readerSlot] : state.readers) {
        if (slotsOverlap(readerSlot, slot))
            add(reader, object);
    }
    auto apply = [&](OperandUses uses, EscapeReason reason) {
        m_applicationWorklist.append({ object, uses, reason });
        entry.isLeaked |= uses.containsAny(leakingUses);
    };
    for (const ContentsApplication& application : state.applications) {
        if (slotsOverlap(application.slot, slot))
            apply(application.uses, application.reason);
    }
    if (OperandUses uses = environmentFieldUses(state, slot); !uses.isEmpty())
        apply(uses, EscapeReason::RetainedByCallee);
    state.contents.insert(position, entry);
}

bool PointsToSolver::mayReadContents(const SolvedUse& solved, const UserOperand& operand) const
{
    if (!solved.reasons.isEmpty() || solved.uses.containsAny({ OperandUse::Retains, OperandUse::Returns }) || isSummarizedCall(operand.user))
        return false;
    return mayHoldReference(solved.use.user);
}

// An opaque value made from an object refers to it. A class token refers to nothing: it is read out of the object that holds it, as any value is.
static bool refersToOperand(const Type& result)
{
    return result.opaque && !result.isOpaque(OpaqueKind::ClassToken);
}

void PointsToSolver::solveContentsRead(SolvedUse& solved, const UserOperand& operand)
{
    ValueID reader = solved.use.user;
    if (!definesObject(reader)) {
        if (refersToOperand(m_program.typeOf(m_function, reader))) {
            solved.flowsTo = reader;
            add(reader, solved.object);
        } else
            addReader(solved.object, reader, slotInContainer(solved.object, operand.user));
    }
    if (operand.effects.onAnyEdge().mayRunOtherCode())
        applyToContents(solved.object, std::nullopt, retainedContentsUses, EscapeReason::LeakedByContainer);
}

void PointsToSolver::addReader(ObjectID container, ValueID reader, ObjectSlot slot)
{
    ContainerState& state = ensureContainer(container);
    state.readers.append({ reader, slot });
    forEachOverlappingEntry(state, slot, [&](SolvedContents& entry) {
        add(reader, entry.object);
    });
}

void PointsToSolver::callObject(ObjectID object)
{
    if (m_calledObjects.set(object.index()))
        return;
    SolvedObject& solved = at(object);
    if (solved.kind == ObjectKind::EntryParameter)
        return;

    FunctionID function = closureFunctionOf(definitionOf(object));
    if (!function) {
        solved.reasons.add(EscapeReason::PassedToUnknownCode);
        applyToContents(object, std::nullopt, retainedContentsUses, EscapeReason::LeakedByContainer);
        return;
    }

    const Function& callee = m_program.at(function);
    const FunctionSummary& summary = m_inputs.effects.summaries.of(function);
    for (uint32_t capture = 0; capture < callee.captures.size(); ++capture)
        applyToContents(object, capture, summary.parameter(capture).operand.uses, EscapeReason::RetainedByCallee);
    if (auto calleeParameter = callee.calleeParameterIndex())
        m_applicationWorklist.append({ object, summary.parameter(*calleeParameter).operand.uses, EscapeReason::RetainedByCallee });
}

void PointsToSolver::applyToContents(ObjectID container, ObjectSlot slot, OperandUses uses, EscapeReason reason)
{
    ContainerState& state = ensureContainer(container);
    ContentsApplication application { slot, uses, reason };
    if (state.applications.contains(application))
        return;
    state.applications.append(application);
    forEachOverlappingEntry(state, slot, [&](SolvedContents& entry) {
        m_applicationWorklist.append({ entry.object, uses, reason });
        entry.isLeaked |= uses.containsAny(leakingUses);
    });
}

void PointsToSolver::applyToObject(ObjectID object, OperandUses uses, EscapeReason reason)
{
    if (!takeWork())
        return;

    SolvedObject& solved = at(object);
    if (uses.containsAny({ OperandUse::Retains, OperandUse::Returns }))
        solved.reasons.add(reason);
    if (uses.containsAny({ OperandUse::AppliesTransitively, OperandUse::AppliesToContents }))
        applyToContents(object, std::nullopt, uses, reason);

    OperandUses newUses = uses - solved.appliedUses;
    solved.appliedUses.add(uses);
    for (OperandUse use : newUses & declaredOperandUses) {
        switch (use) {
        case OperandUse::Inspects:
        case OperandUse::Retains:
        case OperandUse::Returns:
        case OperandUse::ReadsContents:
        case OperandUse::WritesContents:
        case OperandUse::MovesElements:
        case OperandUse::StoresByKey:
        case OperandUse::DeletesByKey:
        case OperandUse::AssignsFrom:
        case OperandUse::DefinesName:
        case OperandUse::TakesDescriptor:
        case OperandUse::ChangesIntegrity:
        case OperandUse::ChangesPrototype:
        case OperandUse::AppliesTransitively:
        case OperandUse::AppliesToContents:
            break;
        case OperandUse::PassesToUserCode:
            solved.reasons.add(EscapeReason::PassedToUnknownCode);
            break;
        case OperandUse::BecomesPrototype:
            solved.reasons.add(EscapeReason::StoredInUnknownMemory);
            break;
        case OperandUse::RetainsContents:
            applyToContents(object, std::nullopt, retainedContentsUses, EscapeReason::LeakedByContainer);
            break;
        case OperandUse::Calls:
        case OperandUse::CallsPerElement:
        case OperandUse::CallsRepeatedly:
            callObject(object);
            break;
        }
    }
}

void PointsToSolver::applyQueuedUses()
{
    while (!m_applicationWorklist.isEmpty()) {
        QueuedApplication application = m_applicationWorklist.takeLast();
        applyToObject(application.object, application.uses, application.reason);
    }
}

ObjectSlot PointsToSolver::slotInContainer(ObjectID container, const Value& user) const
{
    if (user.opcode == Opcode::GetCapture)
        return user.index();
    LayoutID layout = layoutPayloadOf(definitionOf(container));
    if (!layout)
        return std::nullopt;
    if (layoutPayloadOf(user) == layout) {
        if (ObjectSlot field = fieldPayloadOf(user))
            return field;
    }
    PropertyKeyID key = keyPayloadOf(user);
    if (!key)
        return std::nullopt;
    return m_program.at(layout).findField(key);
}

bool PointsToSolver::isExactCapable(ValueID value) const
{
    ValueLocation location = m_useLists.locationOf(value);
    switch (location.placement) {
    case ValuePlacement::Detached:
        return false;
    case ValuePlacement::Parameter:
        return location.block != m_cfg.root();
    case ValuePlacement::InBlock:
        return isCastOrConversion(m_function.at(value).opcode) || m_valuesSharedByObjects.get(value.index());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool PointsToSolver::handsOperandOn(ValueID user, uint32_t operandIndex) const
{
    Opcode opcode = m_function.at(user).opcode;
    if (isCastOrConversion(opcode))
        return !operandIndex;
    return m_valuesSharedByObjects.get(user.index()) && operandUsesOf(opcode, std::min(operandIndex, numberOfNamedOperandsOf(opcode))).contains(OperandUse::Returns);
}

bool PointsToSolver::mayBeUntracked(ValueID value) const
{
    if (!value)
        return true;
    const ValueSet* set = m_results.setOf(value);
    return !set || set->mayBeUntracked;
}

bool PointsToSolver::hasUntrackedSource(ValueID value) const
{
    ValueLocation location = m_useLists.locationOf(value);
    if (location.placement == ValuePlacement::InBlock) {
        bool hasUntrackedOperand = false;
        m_function.at(value).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            hasUntrackedOperand |= handsOperandOn(value, operandIndex) && mayBeUntracked(operand);
        });
        return hasUntrackedOperand;
    }

    auto mayPassUntracked = [&](Edge edge) {
        IncomingValue incoming = incomingValueOf(m_function, edge, location.index);
        switch (incoming.kind) {
        case IncomingValue::Kind::Argument:
            return mayBeUntracked(incoming.argument);
        case IncomingValue::Kind::EdgeResult:
            break;
        }
        std::optional<EdgeResult> result = tryGetEdgeResult(m_function, incoming.result);
        if (!result)
            return true;
        switch (result->kind) {
        case EdgeResultKind::NarrowedOperand:
            return mayBeUntracked(result->subject);
        case EdgeResultKind::Guard:
            return false;
        case EdgeResultKind::DeclaredResult:
            return !makesObjectOnEdge(edge.from);
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    return std::ranges::any_of(m_cfg.incomingEdges(location.block), mayPassUntracked);
}

void PointsToSolver::forEachExactAlias(ValueID value, const Use& use, NOESCAPE const ScopedLambda<void(ValueID)>& functor) const
{
    ASSERT_UNUSED(value, usedValueOf(m_function, use) == value);
    switch (use.kind) {
    case Use::Kind::Operand:
        if (handsOperandOn(use.user, use.operandIndex))
            functor(use.user);
        return;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        if (ValueID parameter = parameterFor(use))
            functor(parameter);
        return;
    case Use::Kind::TerminatorOperand:
    case Use::Kind::BlockSubject:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void PointsToSolver::computeMayBeUntracked()
{
    Vector<ValueID, 16> exactValues;
    for (uint32_t index = 0; index < m_results.setOfValue.size(); ++index) {
        ValueSetID id = m_results.setOfValue[index];
        ValueID value(index);
        if (!id || definesObject(value) || !isExactCapable(value))
            continue;
        m_results.sets[id.index()].mayBeUntracked = false;
        exactValues.append(value);
    }

    Vector<ValueID, 16> worklist;
    auto markUntracked = [&](ValueID value) {
        ValueSetID id = m_results.setOfValue[value.index()];
        if (!id || m_results.sets[id.index()].mayBeUntracked || definesObject(value))
            return;
        m_results.sets[id.index()].mayBeUntracked = true;
        worklist.append(value);
    };
    for (ValueID value : exactValues) {
        if (hasUntrackedSource(value))
            markUntracked(value);
    }
    while (!worklist.isEmpty()) {
        ValueID value = worklist.takeLast();
        for (const Use& use : m_useLists.usesOf(value))
            forEachExactAlias(value, use, markUntracked);
        for (const NarrowedCopy& copy : narrowedCopiesOf(value))
            markUntracked(copy.parameter);
    }
}

void PointsToSolver::markLeakedEntries(ContainerState& state) const
{
    for (const ContentsApplication& application : state.applications) {
        if (!application.uses.containsAny(callingUses))
            continue;
        forEachOverlappingEntry(state, application.slot, [&](SolvedContents& entry) {
            entry.isLeaked |= at(entry.object).reasons.containsAny(callReasons);
        });
    }
    for (SolvedContents& entry : state.contents) {
        if (environmentFieldUses(state, entry.slot).containsAny(callingUses))
            entry.isLeaked |= at(entry.object).reasons.containsAny(callReasons);
    }
}

void PointsToSolver::collectContents()
{
    size_t numberOfEntries = 0;
    for (auto& state : m_containers)
        numberOfEntries += state ? state->contents.size() : 0;
    m_results.contents.reserveInitialCapacity(numberOfEntries);
    for (auto& state : m_containers) {
        if (!state)
            continue;
        markLeakedEntries(*state);
        m_results.contents.appendVector(state->contents);
    }
    ASSERT(std::ranges::is_sorted(m_results.contents, isOrderedBefore));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
