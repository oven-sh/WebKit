/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPointsToSolver.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTEffectsCache.h"
#include "AOTPointsToSolverInternal.h"
#include "AOTResultRules.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static bool isRetainedInOperand(const OperandSummary& summary)
{
    return summary.uses.contains(OperandUse::Retains) && summary.retainedIn == RetainedIn::Operand;
}

static bool isRetainingOperand(Opcode opcode, uint32_t operandIndex)
{
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
    for (uint32_t index = 0; index < numberOfDeclaredOperands; ++index) {
        if (retainedInOf(opcode, index) == RetainedIn::Operand && retainingOperandOf(opcode, index) == operandIndex)
            return true;
    }
    return false;
}

static bool allocatesOrReturnsOperand(Opcode opcode)
{
    if (resultRuleOf(opcode) != ResultRule::Allocation)
        return false;
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
    for (uint32_t index = 0; index < numberOfDeclaredOperands; ++index) {
        if (operandUsesOf(opcode, index).contains(OperandUse::Returns))
            return true;
    }
    return false;
}

static bool isSuperConstruct(Opcode opcode)
{
    return OperandPosition::parent(opcode).has_value();
}

static EscapeReason reasonForRetainingTerminator(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
        return EscapeReason::Returned;
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
        return EscapeReason::Thrown;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return EscapeReason::StoredInUnknownMemory;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ObjectSlot slotInResult(const Value& user, uint32_t operandIndex)
{
    std::optional<uint32_t> position = user.variadicOperandIndex(operandIndex);
    if (!position)
        return std::nullopt;
    if (!Value::opcodesWithFields.contains(user.opcode))
        return position;
    auto fields = user.fields();
    if (*position >= fields.size())
        return std::nullopt;
    return fields[*position];
}

PointsToSolver::PointsToSolver(const EscapeAnalysisInputs& inputs)
    : m_inputs(inputs)
    , m_program(inputs.context.program)
    , m_function(inputs.function)
    , m_cfg(inputs.cfg)
    , m_useLists(inputs.useLists)
    , m_maxWork(inputs.context.build.limit(LimitID::MaxEscapeAnalysisWork))
{
    m_results.setOfValue.fill(ValueSetID(), m_function.values.size());
}

PointsToResults PointsToSolver::solve()
{
    findNarrowedCopies();
    findObjects();
    while (!m_flowWorklist.isEmpty() && !m_results.gaveUp) {
        auto [value, object] = m_flowWorklist.takeLast();
        if (!takeWork())
            break;
        for (const Use& use : m_useLists.usesOf(value))
            solveUse(object, use);
        for (const NarrowedCopy& copy : narrowedCopiesOf(value))
            add(copy.parameter, object);
        applyQueuedUses();
    }
    if (!m_results.gaveUp) {
        computeMayBeUntracked();
        collectContents();
    }
    return WTF::move(m_results);
}

bool PointsToSolver::takeWork()
{
    if (++m_work <= m_maxWork)
        return true;
    giveUp();
    return false;
}

void PointsToSolver::giveUp()
{
    if (std::exchange(m_results.gaveUp, true))
        return;
    m_inputs.context.build.gaveUp(LimitID::MaxEscapeAnalysisWork, m_inputs.subject);
    m_flowWorklist.clear();
    m_applicationWorklist.clear();
    m_results.uses.clear();
    m_results.contents.clear();
    for (size_t index = 0; index < m_results.setOfValue.size(); ++index) {
        if (m_results.setOfValue[index] && !definesObject(ValueID(index)))
            m_results.setOfValue[index] = ValueSetID();
    }
}

bool PointsToSolver::definesObject(ValueID value) const
{
    if (m_valuesSharedByObjects.get(value.index()))
        return false;
    const ValueSet* set = m_results.setOf(value);
    return set && set->objects.bitCount() == 1 && at(ObjectID(*set->objects.begin())).definition == value;
}

bool PointsToSolver::mayHoldReference(ValueID value) const
{
    return mayBeReference(m_program.typeOf(m_function, value)) || numberOfResultsOf(m_program, m_function, m_function.at(value)).has_value();
}

Vector<ObjectID, 8> PointsToSolver::objectsOf(ValueID value) const
{
    Vector<ObjectID, 8> objects;
    if (const ValueSet* set = m_results.setOf(value)) {
        for (size_t index : set->objects)
            objects.append(ObjectID(index));
    }
    return objects;
}

ValueID PointsToSolver::parameterFor(const Use& edgeUse) const
{
    const Target& target = targetOf(m_function, edgeUse.edge());
    auto& parameters = m_function.at(target.block).parameters;
    if (edgeUse.operandIndex >= parameters.size())
        return { };
    return parameters[edgeUse.operandIndex];
}

std::span<const NarrowedCopy> PointsToSolver::narrowedCopiesOf(ValueID subject) const
{
    auto copies = std::ranges::equal_range(m_narrowedCopies, subject, { }, &NarrowedCopy::subject);
    return m_narrowedCopies.subspan(copies.begin() - m_narrowedCopies.begin(), copies.size());
}

void PointsToSolver::findNarrowedCopies()
{
    for (BlockID from : m_cfg.reversePostOrder()) {
        forEachEdgeResult(m_function, from, [&](const EdgeResult& result, ValueID parameter) {
            if (result.kind == EdgeResultKind::NarrowedOperand)
                m_narrowedCopies.append(NarrowedCopy { result.subject, parameter });
        });
    }
    std::ranges::sort(m_narrowedCopies);
    removeRepeatedElements(m_narrowedCopies);
}

void PointsToSolver::findObjects()
{
    for (ValueID parameter : m_function.at(m_cfg.root()).parameters) {
        if (mayBeReference(m_program.typeOf(m_function, parameter)))
            addObject(parameter, ObjectKind::EntryParameter);
    }
    for (BlockID block : m_cfg.reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (freshnessOf(m_inputs.effects, m_function, value) != ResultFreshness::Unknown)
                addObject(id, ObjectKind::Allocation);
            else if (allocatesOrReturnsOperand(value.opcode))
                addObjectSharingItsValue(id);
            else if (isSuperConstruct(value.opcode) && mayBeReference(m_program.typeOf(m_function, id)))
                addObject(id, ObjectKind::EntryParameter);
        }
        if (!makesObjectOnEdge(block))
            continue;
        forEachEdgeResult(m_function, block, [&](const EdgeResult& result, ValueID parameter) {
            if (result.kind == EdgeResultKind::DeclaredResult)
                addObjectSharingItsValue(parameter);
        });
    }
    m_containers.grow(m_results.objects.size());
    m_calledObjects.ensureSize(m_results.objects.size());
}

void PointsToSolver::addObject(ValueID definition, ObjectKind kind)
{
    ObjectID object(m_results.objects.size());
    m_results.objects.append(SolvedObject { definition, kind, { }, { } });
    m_results.setOfValue[definition.index()] = ValueSetID(m_results.sets.size());
    ValueSet set;
    set.objects.set(object.index());
    set.mayBeUntracked = false;
    m_results.sets.append(WTF::move(set));
    m_flowWorklist.append({ definition, object });
}

bool PointsToSolver::makesObjectOnEdge(BlockID from) const
{
    return freshnessOf(m_inputs.effects, m_function, m_function.at(from).terminator) != ResultFreshness::Unknown;
}

// No value stands for the object alone: another edge may pass another object to the parameter, and the operation may hand back its operand.
void PointsToSolver::addObjectSharingItsValue(ValueID value)
{
    ObjectID object(m_results.objects.size());
    m_results.objects.append(SolvedObject { value, ObjectKind::Allocation, { }, { } });
    m_valuesSharedByObjects.set(value.index());
    add(value, object);
}

void PointsToSolver::add(ValueID value, ObjectID object)
{
    if (definesObject(value))
        return;

    ValueSetID& id = m_results.setOfValue[value.index()];
    if (!id) {
        id = ValueSetID(m_results.sets.size());
        m_results.sets.append(ValueSet { });
    }
    if (m_results.sets[id.index()].objects.set(object.index()))
        return;
    m_flowWorklist.append({ value, object });
}

void PointsToSolver::solveUse(ObjectID object, const Use& use)
{
    SolvedUse solved { .object = object, .use = use };
    switch (use.kind) {
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        solveEdgeUse(solved);
        break;
    case Use::Kind::TerminatorOperand:
        solveTerminatorOperand(solved);
        break;
    case Use::Kind::Operand:
        solveOperand(solved);
        break;
    case Use::Kind::BlockSubject:
        solved.uses = OperandUse::Inspects;
        break;
    }
    m_results.uses.append(solved);
}

void PointsToSolver::solveEdgeUse(SolvedUse& solved)
{
    ValueID parameter = parameterFor(solved.use);
    if (!parameter) {
        solved.uses = OperandUses::all();
        solved.reasons = EscapeReason::StoredInUnknownMemory;
        solved.isContentsStore = true;
        return;
    }
    solved.uses = { OperandUse::Inspects, OperandUse::Returns };
    solved.flowsTo = parameter;
    add(parameter, solved.object);
}

void PointsToSolver::solveTerminatorOperand(SolvedUse& solved)
{
    const Terminator& terminator = m_function.at(solved.use.block).terminator;
    solved.uses = summarizeOperand(m_inputs.effects, m_function, terminator, solved.use.operandIndex).uses;
    solved.isContentsStore = solved.uses.contains(OperandUse::WritesContents);
    for (OperandUse use : solved.uses & declaredOperandUses)
        solveTerminatorOperandUse(solved, terminator.kind, use);
}

void PointsToSolver::solveTerminatorOperandUse(SolvedUse& solved, TerminatorKind kind, OperandUse use)
{
    switch (use) {
    case OperandUse::Inspects:
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
        return;
    case OperandUse::Returns:
        if (returnsToCaller(kind))
            solved.reasons.add(EscapeReason::Returned);
        else
            addToDeclaredResults(solved);
        return;
    case OperandUse::Retains:
    case OperandUse::BecomesPrototype:
        solved.reasons.add(reasonForRetainingTerminator(kind));
        return;
    case OperandUse::PassesToUserCode:
        solved.reasons.add(EscapeReason::PassedToUnknownCode);
        return;
    case OperandUse::Calls:
    case OperandUse::CallsPerElement:
    case OperandUse::CallsRepeatedly:
        callObject(solved.object);
        return;
    case OperandUse::RetainsContents:
        applyToContents(solved.object, std::nullopt, retainedContentsUses, EscapeReason::LeakedByContainer);
        return;
    case OperandUse::AppliesTransitively:
    case OperandUse::AppliesToContents:
        applyToContents(solved.object, std::nullopt, solved.uses, EscapeReason::RetainedByCallee);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void PointsToSolver::addToDeclaredResults(SolvedUse& solved)
{
    forEachEdgeResult(m_function, solved.use.block, [&](const EdgeResult& result, ValueID parameter) {
        if (result.kind != EdgeResultKind::DeclaredResult || !mayHoldReference(parameter))
            return;
        solved.flowsTo = parameter;
        add(parameter, solved.object);
    });
}

void PointsToSolver::solveOperand(SolvedUse& solved)
{
    const Value& user = m_function.at(solved.use.user);
    UserOperand operand { user, solved.use.operandIndex, m_inputs.effectsCache.of(solved.use.user), summarizeOperand(m_inputs.effects, m_function, user, solved.use.operandIndex) };
    if (!mayHoldReference(solved.use.user))
        operand.summary.uses.remove(OperandUse::Returns);
    solved.uses = operand.summary.uses;
    solved.isContentsStore = isContentsStore(operand);
    for (OperandUse use : solved.uses & declaredOperandUses)
        solveOperandUse(solved, operand, use);
    addStoredOperands(solved.object, operand);
    solved.readsContents = mayReadContents(solved, operand);
    if (solved.readsContents)
        solveContentsRead(solved, operand);
}

void PointsToSolver::solveOperandUse(SolvedUse& solved, const UserOperand& operand, OperandUse use)
{
    switch (use) {
    case OperandUse::Inspects:
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
        return;
    case OperandUse::Returns:
        if (definesObject(solved.use.user) || (isSummarizedCall(operand.user) && m_inputs.context.callResults == CallResults::NotFollowed)) {
            solved.reasons.add(EscapeReason::RetainedByCallee);
            return;
        }
        solved.flowsTo = solved.use.user;
        add(solved.flowsTo, solved.object);
        return;
    case OperandUse::PassesToUserCode:
        if (isSummarizedCall(operand.user) || !isRetainedInOperand(operand.summary))
            solved.reasons.add(EscapeReason::PassedToUnknownCode);
        return;
    case OperandUse::Retains:
        solveRetainedOperand(solved, operand);
        return;
    case OperandUse::Calls:
    case OperandUse::CallsPerElement:
    case OperandUse::CallsRepeatedly:
        callObject(solved.object);
        return;
    case OperandUse::RetainsContents:
        applyToContents(solved.object, std::nullopt, retainedContentsUses, EscapeReason::LeakedByContainer);
        return;
    case OperandUse::AppliesTransitively:
    case OperandUse::AppliesToContents:
        applyToContents(solved.object, std::nullopt, operand.summary.uses, EscapeReason::RetainedByCallee);
        return;
    case OperandUse::BecomesPrototype:
        if (!operand.summary.uses.contains(OperandUse::Retains) || operand.summary.retainedIn == RetainedIn::UnknownMemory)
            solved.reasons.add(EscapeReason::StoredInUnknownMemory);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void PointsToSolver::solveRetainedOperand(SolvedUse& solved, const UserOperand& operand)
{
    switch (operand.summary.retainedIn) {
    case RetainedIn::UnknownMemory:
        solved.reasons.add(isSummarizedCall(operand.user) ? EscapeReason::RetainedByCallee : EscapeReason::StoredInUnknownMemory);
        return;
    case RetainedIn::Result:
        solved.retainedBy = solved.use.user;
        for (ObjectID container : objectsOf(solved.retainedBy))
            addContents(container, slotInResult(operand.user, operand.index), solved.object);
        return;
    case RetainedIn::Operand:
        if (operand.summary.retainingOperand < operand.user.numberOfOperands())
            solved.retainedBy = operand.user.operand(operand.summary.retainingOperand);
        if (!solved.retainedBy) {
            solved.reasons.add(EscapeReason::StoredInUnknownMemory);
            return;
        }
        for (ObjectID container : objectsOf(solved.retainedBy))
            addContents(container, slotInContainer(container, operand.user), solved.object);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void PointsToSolver::addStoredOperands(ObjectID container, const UserOperand& operand)
{
    const Value& user = operand.user;
    if (!isSummarizedCall(user) && !isRetainingOperand(user.opcode, operand.index))
        return;
    user.forEachOperand([&](ValueID stored, uint32_t index) {
        if (index == operand.index || !m_results.setOf(stored))
            return;
        OperandSummary summary = summarizeOperand(m_inputs.effects, m_function, user, index);
        if (!isRetainedInOperand(summary) || summary.retainingOperand != operand.index)
            return;
        ObjectSlot slot = slotInContainer(container, user);
        for (ObjectID object : objectsOf(stored))
            addContents(container, slot, object);
    });
}

bool PointsToSolver::isContentsStore(const UserOperand& operand) const
{
    if (!operand.summary.uses.contains(OperandUse::WritesContents) || isSummarizedCall(operand.user))
        return false;
    Opcode opcode = operand.user.opcode;
    OperandUses ownUses = operandUsesOf(opcode, std::min(operand.index, numberOfNamedOperandsOf(opcode)));
    return ownUses.contains(OperandUse::WritesContents) && !ownUses.containsAny({ OperandUse::Retains, OperandUse::Calls });
}

PointsToResults solvePointsTo(const EscapeAnalysisInputs& inputs)
{
    return PointsToSolver(inputs).solve();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
