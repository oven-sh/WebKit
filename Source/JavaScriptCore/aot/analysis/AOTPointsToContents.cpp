/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPointsToSolverInternal.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsCache.h"
#include "AOTFlowResults.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
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
    uint32_t& index = m_containerOfObject[object.index()];
    if (index == noContainer) {
        index = m_containers.size();
        RELEASE_ASSERT(m_containers.size() < m_containers.capacity());
        m_containers.append(ContainerState { });
        ContainerState& state = m_containers.last();
        state.environment = environmentLayoutOf(object);
        state.unknownSlot.objects.fill(0, m_wordsPerSet);
    }
    return m_containers[index];
}

OperandUses PointsToSolver::environmentFieldUses(const ContainerState& state, ObjectSlot slot) const
{
    const FlowResults* flow = m_inputs.context.flow;
    if (!state.environment || !flow)
        return { };
    return slot ? flow->storedValueUses(FieldReference { state.environment, *slot }) : OperandSummary().uses;
}

void PointsToSolver::addContents(ObjectID container, ObjectSlot slot, std::span<const ObjectWord> objects)
{
    if (objects.empty())
        return;

    ContainerState& state = ensureContainer(container);
    SlotState& holder = state.ensure(slot);
    ObjectWord added = 0;
    for (unsigned index = 0; index < m_wordsPerSet; ++index) {
        m_newContents[index] = objects[index] & ~holder.objects[index];
        holder.objects[index] |= m_newContents[index];
        added |= m_newContents[index];
    }
    if (!added)
        return;

    size_t firstNewEntry = holder.contents.size();
    forEachObjectIn(m_newContents.span(), [&](ObjectID object) {
        holder.contents.append(SolvedContents { container, slot, object, false });
    });
    auto apply = [&](OperandUses uses, EscapeReason reason) {
        for (SolvedContents& entry : holder.contents.mutableSpan().subspan(firstNewEntry)) {
            m_applicationWorklist.append({ entry.object, uses, reason });
            entry.isLeaked |= uses.containsAny(leakingUses);
        }
    };
    forEachOverlappingSlot(state, slot, [&](SlotState& overlapping) {
        for (ValueID reader : overlapping.readers)
            addAll(reader, m_newContents.span());
        for (const ContentsApplication& application : overlapping.applications)
            apply(application.uses, application.reason);
    });
    if (OperandUses uses = environmentFieldUses(state, slot); !uses.isEmpty())
        apply(uses, EscapeReason::RetainedByCallee);
}

bool PointsToSolver::mayReadContentsIntoReference(const SolvedUse& solved, const UserOperand& operand) const
{
    return solved.reasons.isEmpty() && !solved.uses.containsAny({ OperandUse::Retains, OperandUse::Returns }) && !isSummarizedCall(operand.user);
}

void PointsToSolver::solveContentsRead(SolvedUse& solved, const ArrivedObjects& arrived, const UserOperand& operand)
{
    ValueID reader = solved.use.user;
    if (!definesObject(reader)) {
        if (refersToOperand(reader)) {
            solved.flowsTo = reader;
            addAll(reader, arrived);
        } else {
            for (ObjectID object : arrived.objects)
                addReader(object, reader, slotInContainer(object, operand.user));
        }
    }
    if (!mayRunOtherCode(reader))
        return;
    for (ObjectID object : arrived.objects)
        applyToContents(object, std::nullopt, retainedContentsUses, EscapeReason::LeakedByContainer);
}

void PointsToSolver::addReader(ObjectID container, ValueID reader, ObjectSlot slot)
{
    ContainerState& state = ensureContainer(container);
    state.ensure(slot).readers.append(reader);
    forEachOverlappingSlot(state, slot, [&](SlotState& overlapping) {
        addAll(reader, overlapping.objects.span());
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
    FunctionSummaryView summary = m_inputs.effects.summaries.of(function);
    for (uint32_t capture = 0; capture < callee.captures.size(); ++capture)
        applyToContents(object, capture, summary.operand(capture).uses, EscapeReason::RetainedByCallee);
    if (auto calleeParameter = callee.calleeParameterIndex())
        m_applicationWorklist.append({ object, summary.operand(*calleeParameter).uses, EscapeReason::RetainedByCallee });
}

void PointsToSolver::applyToContents(ObjectID container, ObjectSlot slot, OperandUses uses, EscapeReason reason)
{
    if (!slot) {
        std::array<ApplicationToAll, 2>& last = m_lastApplicationsToAll[container.index()];
        for (const ApplicationToAll& application : last) {
            if (application.isSet && application.uses == uses && application.reason == reason)
                return;
        }
        last[1] = last[0];
        last[0] = { uses, reason, true };
    }
    ContainerState& state = ensureContainer(container);
    ContentsApplication application { slot, uses, reason };
    auto& applications = state.ensure(slot).applications;
    if (applications.contains(application))
        return;
    applications.append(application);
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
    const Value& definition = definitionOf(container);
    LayoutID layout = layoutPayloadOf(definition);
    // An environment that is handed in has the layout that its type names. Without it a store to one variable would come back from a read of any other.
    if (!layout)
        layout = m_program.at(definition.type).environment;
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
        return isCastOrConversion(m_function.at(value).opcode) || isSharedByObjects(value);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool PointsToSolver::handsOperandOn(ValueID user, uint32_t operandIndex) const
{
    Opcode opcode = m_function.at(user).opcode;
    if (isCastOrConversion(opcode))
        return !operandIndex;
    return isSharedByObjects(user) && operandUsesOf(opcode, std::min(operandIndex, numberOfNamedOperandsOf(opcode))).contains(OperandUse::Returns);
}

bool PointsToSolver::mayBeUntracked(ValueID value) const
{
    if (!value)
        return true;
    // It stands where nobody reads the value: on the way into a block from where the value does not exist yet. It brings no object.
    if (m_function.at(value).opcode == Opcode::Unspecified)
        return false;
    ValueSetID set = m_results.setOfValue[value.index()];
    return !set || m_setsThatMayBeUntracked[set.index()];
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
    for (uint32_t index = 0; index < m_valueOfSet.size(); ++index) {
        ValueSetID id(index);
        ValueID value = m_valueOfSet[index];
        if (definesObject(value) || !isExactCapable(value))
            continue;
        m_setsThatMayBeUntracked[id.index()] = false;
        exactValues.append(value);
    }

    Vector<ValueID, 16> worklist;
    auto markUntracked = [&](ValueID value) {
        ValueSetID id = m_results.setOfValue[value.index()];
        if (!id || m_setsThatMayBeUntracked[id.index()] || definesObject(value))
            return;
        m_setsThatMayBeUntracked[id.index()] = true;
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
    forEachOverlappingSlot(state, std::nullopt, [&](SlotState& applied) {
        for (const ContentsApplication& application : applied.applications) {
            if (!application.uses.containsAny(callingUses))
                continue;
            forEachOverlappingEntry(state, application.slot, [&](SolvedContents& entry) {
                entry.isLeaked |= at(entry.object).reasons.containsAny(callReasons);
            });
        }
    });
    forEachOverlappingEntry(state, std::nullopt, [&](SolvedContents& entry) {
        if (environmentFieldUses(state, entry.slot).containsAny(callingUses))
            entry.isLeaked |= at(entry.object).reasons.containsAny(callReasons);
    });
}

void PointsToSolver::collectContents()
{
    for (uint32_t index : m_containerOfObject) {
        if (index == noContainer)
            continue;
        ContainerState* state = &m_containers[index];
        markLeakedEntries(*state);
        size_t first = m_results.contents.size();
        forEachOverlappingSlot(*state, std::nullopt, [&](SlotState& slot) {
            m_results.contents.appendVector(slot.contents);
        });
        AOT::sort(m_results.contents.mutableSpan().subspan(first), isOrderedBefore);
    }
    ASSERT(std::ranges::is_sorted(m_results.contents, isOrderedBefore));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
