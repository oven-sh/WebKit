/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static IncomingBinding bindingOf(const EdgeResult& result)
{
    switch (result.kind) {
    case EdgeResultKind::NarrowedOperand:
        return { IncomingBinding::Kind::NarrowedOperand, result.subject };
    case EdgeResultKind::Guard:
        return { IncomingBinding::Kind::Guard, result.subject };
    case EdgeResultKind::DeclaredResult:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Rewrite asks while the target has head parameters that this edge does not fill yet, so the bindings come from the edge alone.
OutgoingEdge outgoingEdgeOf(const Function& function, Edge edge)
{
    const Target& target = targetOf(function, edge);
    uint32_t numberOfResults = edge.isHandlerEdge() ? 0 : numberOfEdgeResults(function, edge.from, edge.targetIndex);

    OutgoingEdge outgoing { edge, target.block, { } };
    outgoing.bindings.reserveInitialCapacity(target.arguments.size() + numberOfResults);
    for (ValueID argument : target.arguments)
        outgoing.bindings.append(IncomingBinding { IncomingBinding::Kind::Argument, argument });
    for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex)
        outgoing.bindings.append(bindingOf(edgeResultAt(function, { edge.from, edge.targetIndex, resultIndex })));
    return outgoing;
}

static Vector<ValueID, 4> originalParametersOf(AllocationSinking& sinking, SinkingMode mode, BlockID block)
{
    Vector<ValueID, 4> parameters(sinking.function().at(block).parameters.span());
    if (mode != SinkingMode::Rewrite)
        return parameters;

    const RewriteTables& tables = sinking.tables();
    if (tables.blocksWithHeadParameters.get(block.index()))
        parameters.removeAt(tables.firstHeadParameter[block.index()], tables.headValues[block.index()].size());
    return parameters;
}

static const IncomingBinding& bindingOfJoin(AllocationSinking& sinking, SinkingMode mode, const OutgoingEdge& outgoing, VirtualObjectID join)
{
    size_t index = originalParametersOf(sinking, mode, outgoing.target).find(sinking.at(join).definition);
    RELEASE_ASSERT(index != notFound);
    return outgoing.bindings[index];
}

static bool isJoinReboundOnEdge(const ObjectInterpreter& interpreter, AllocationSinking& sinking, const OutgoingEdge& outgoing, VirtualObjectID object)
{
    const Candidate& candidate = sinking.at(object);
    if (!candidate.isJoin || candidate.block != outgoing.target)
        return false;
    return interpreter.objectOf(bindingOfJoin(sinking, interpreter.mode(), outgoing, object).value) != object;
}

static bool isReferenced(const SinkingState& state, VirtualObjectID object)
{
    bool isAliased = state.aliases.containsIf([&](const AliasEntry& alias) {
        return alias.object == object;
    });
    return isAliased || state.objects.containsIf([&](const ObjectEntry& holder) {
        return holder.state.heldObjects.containsIf([&](const HeldObject& entry) {
            return entry.object == object;
        });
    });
}

static bool hasFieldHoldingCells(AllocationSinking& sinking, VirtualObjectID object)
{
    uint32_t numberOfFields = sinking.familyOf(object).numberOfFields();
    for (uint32_t field = 0; field < numberOfFields; ++field) {
        if (sinking.decisions().holdsCells(object, field))
            return true;
    }
    return false;
}

static BitVector loadableFieldsOf(const AllocationSinking& sinking, VirtualObjectID object)
{
    const VirtualFamily& family = sinking.familyOf(object);
    const VirtualObjectDescription& description = sinking.descriptionOf(object);
    BitVector fields;
    for (uint32_t field = 0; field < family.numberOfFields(); ++field) {
        if (canLoadSpeculatively(sinking.objectContext(), description, family.fieldAt(field)) == SpeculativeLoad::Safe)
            fields.set(field);
    }
    return fields;
}

static void requireCellInCoveredBlock(AllocationSinking& sinking, SinkingState& state, BlockID covered, VirtualObjectID object)
{
    if (sinking.decisions().isBuiltAtHead(covered, object))
        sinking.abandon(object);
    else
        sinking.decisions().buildAtHead(covered, object);
    state.find(object)->becomeCell();
}

static void buildOnEdge(ObjectInterpreter& interpreter, AllocationSinking& sinking, const OutgoingEdge& outgoing, VirtualObjectID object)
{
    if (outgoing.edge.isHandlerEdge())
        requireCellInCoveredBlock(sinking, interpreter.state(), outgoing.edge.from, object);
    else
        interpreter.build(object, BuildCause { });
}

static VirtualObjectID objectGuardedByTest(const ObjectInterpreter& interpreter, const Terminator& terminator, ValueID subject)
{
    const SinkingState& state = interpreter.state();
    const TestEntry* test = terminator.kind == TerminatorKind::Branch ? state.findTest(terminator.conditionOperand()) : nullptr;
    if (!test || !test->object)
        return interpreter.objectOf(subject);
    return state.find(test->object) ? test->object : VirtualObjectID();
}

static AliasEntry aliasOfBinding(const ObjectInterpreter& interpreter, const Terminator* terminator, ValueID parameter, const IncomingBinding& binding)
{
    switch (binding.kind) {
    case IncomingBinding::Kind::Other:
        return { };
    case IncomingBinding::Kind::Guard:
        RELEASE_ASSERT(terminator);
        return { parameter, objectGuardedByTest(interpreter, *terminator, binding.value), AliasKind::Guard };
    case IncomingBinding::Kind::Argument:
    case IncomingBinding::Kind::NarrowedOperand:
        if (VirtualObjectID object = interpreter.objectOf(binding.value))
            return { parameter, object, AliasKind::Object };
        return { parameter, interpreter.guardedObjectOf(binding.value), AliasKind::Guard };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ObjectValues valuesEnteringJoin(ObjectInterpreter& interpreter, AllocationSinking& sinking, const EnteredEdge& entered, const ObjectEntry& join)
{
    ObjectValues values;
    for (const ConsumedSource& consumed : entered.consumedSources) {
        if (consumed.join != join.object)
            continue;
        if (const ObjectValues* carried = interpreter.values().find(consumed.source))
            values = *carried;
        if (join.state.isCell())
            values.cell = interpreter.cellOf(consumed.source);
        return values;
    }

    if (join.state.isCell())
        values.cell = interpreter.resolve(bindingOfJoin(sinking, interpreter.mode(), *entered.edge, join.object).value);
    return values;
}

static void removeDeadEntries(AllocationSinking& sinking, SinkingState& state, BlockID target)
{
    if (state.objects.isEmpty() && state.aliases.isEmpty() && state.tests.isEmpty())
        return;

    const Liveness& liveness = sinking.analyses().liveness();
    const CompactList<ValueID>& parameters = sinking.function().at(target).parameters;
    auto isLive = [&](ValueID value) {
        return parameters.contains(value) || liveness.isLiveAtHead(value, target);
    };
    state.aliases.removeAllMatching([&](const AliasEntry& alias) {
        return !isLive(alias.value);
    });
    state.tests.removeAllMatching([&](const TestEntry& test) {
        return !isLive(test.test);
    });

    BitVector isLiveObject(sinking.numberOfCandidates());
    Vector<VirtualObjectID, 8> worklist;
    auto markLive = [&](VirtualObjectID object) {
        if (object && state.find(object) && !isLiveObject.quickSet(object.index()))
            worklist.append(object);
    };
    for (const AliasEntry& alias : state.aliases)
        markLive(alias.object);
    for (const TestEntry& test : state.tests)
        markLive(test.object);
    for (const ObjectEntry& entry : state.objects) {
        if (isLive(sinking.at(entry.object).definition))
            markLive(entry.object);
    }
    while (!worklist.isEmpty()) {
        for (const HeldObject& entry : state.find(worklist.takeLast())->heldObjects)
            markLive(entry.object);
    }
    state.objects.removeAllMatching([&](const ObjectEntry& entry) {
        return !isLiveObject.quickGet(entry.object.index());
    });
}

void ObjectInterpreter::enterEdge(const OutgoingEdge& outgoing, const Terminator* terminator, const SinkingState* headOfTarget)
{
    m_enteredEdge = EnteredEdge { &outgoing, terminator, { }, { }, { } };
    uint32_t targetIndex = outgoing.edge.targetIndex;
    bool isTaken = m_state.isReachable && !m_sinking.isIgnored(outgoing.edge) && !(terminator && isDeadEdge(*terminator, targetIndex));
    if (!isTaken || (m_mode != SinkingMode::Analyze && !headOfTarget)) {
        m_state = SinkingState::unreachableAt(outgoing.target);
        return;
    }

    if (terminator)
        narrowOnEdge(*terminator, targetIndex);
    bindParameters(outgoing);
    applyDecisions(outgoing);
    commitBindings();
    dropDeadEntries(outgoing.target, headOfTarget);
    abandonJoinsOfLiveSources();
    makeFieldsOfCellsValid(outgoing, headOfTarget);

    bool meetsStatesOfCoveredBlock = m_mode == SinkingMode::Analyze && outgoing.edge.isHandlerEdge();
    for (ObjectEntry& entry : m_state.objects) {
        entry.state.normalize();
        if (!meetsStatesOfCoveredBlock)
            continue;
        m_sinking.tagVariants(outgoing.target, entry.object, entry.state);
        if (entry.state.hasConflictingTags())
            requireCellInCoveredBlock(m_sinking, m_state, outgoing.edge.from, entry.object);
    }
    m_state.block = outgoing.target;
    m_state.isAtHead = true;
}

void ObjectInterpreter::applyDecisions(const OutgoingEdge& outgoing)
{
    // A block reads its subject at its head, in front of everything the block could build.
    VirtualObjectID subject = objectOf(function().at(outgoing.target).subject);
    Vector<VirtualObjectID, 4> objects;
    for (const ObjectEntry& entry : m_state.objects) {
        if (!entry.state.isCell() && (entry.object == subject || m_sinking.decisions().isBuiltAtHead(outgoing.target, entry.object)))
            objects.append(entry.object);
    }
    for (VirtualObjectID object : objects) {
        if (stateOf(object)->isCell())
            continue;
        willEmitOnEdge(outgoing);
        buildOnEdge(*this, m_sinking, outgoing, object);
    }
}

void ObjectInterpreter::bindParameters(const OutgoingEdge& outgoing)
{
    Vector<ValueID, 4> parameters = originalParametersOf(m_sinking, m_mode, outgoing.target);
    RELEASE_ASSERT(parameters.size() == outgoing.bindings.size());

    // Asked before anything is built on the edge: a build turns the subject of an interpreted SwitchOnShape into a cell.
    const Terminator* terminator = m_enteredEdge.terminator;
    bool isTestKept = terminator && !hasInterpretedTest(*terminator);
    for (size_t index = 0; index < parameters.size(); ++index) {
        IncomingBinding binding = outgoing.bindings[index];
        if (isTestKept && binding.kind != IncomingBinding::Kind::Argument)
            binding.kind = IncomingBinding::Kind::Other;
        bindParameter(parameters[index], binding);
    }
}

void ObjectInterpreter::bindParameter(ValueID parameter, const IncomingBinding& binding)
{
    if (VirtualObjectID join = m_sinking.candidateDefinedBy(parameter)) {
        enterJoin(join, binding);
        return;
    }

    const OutgoingEdge& outgoing = *m_enteredEdge.edge;
    AliasEntry alias = aliasOfBinding(*this, m_enteredEdge.terminator, parameter, binding);
    if (!alias.object)
        return;
    if (isJoinReboundOnEdge(*this, m_sinking, outgoing, alias.object))
        m_sinking.abandon(alias.object);
    // A parameter stands for the cell as it is built. Where a test that ran before the object was built made the parameter
    // narrower than that, the uses of the parameter would get a value outside its type.
    TypeID parameterType = function().at(parameter).type;
    if (alias.kind == AliasKind::Object && !fitsType(alias.object, parameterType))
        m_sinking.abandon(alias.object);
    if (alias.kind == AliasKind::Object)
        assumeTypeOfAlias(alias.object, parameterType);
    bool isSubject = parameter == function().at(outgoing.target).subject;
    if (!isSubject && !m_sinking.decisions().holdsCells(parameter)) {
        m_enteredEdge.newAliases.append(alias);
        return;
    }

    if (alias.kind == AliasKind::Object && outgoing.edge.isHandlerEdge() && !fitsTypeWithoutCasts(alias.object, parameterType))
        m_sinking.abandon(alias.object);
    if (alias.kind == AliasKind::Guard && m_mode == SinkingMode::Count)
        m_sinking.at(alias.object).needsGuard = true;
    if (stateOf(alias.object)->isCell())
        return;
    willEmitOnEdge(outgoing);
    buildOnEdge(*this, m_sinking, outgoing, alias.object);
}

void ObjectInterpreter::enterJoin(VirtualObjectID join, const IncomingBinding& binding)
{
    const OutgoingEdge& outgoing = *m_enteredEdge.edge;
    VirtualObjectID source = objectOf(binding.value);
    if (source == join)
        return;
    if (binding.kind == IncomingBinding::Kind::Other)
        m_sinking.abandon(join);
    if (source) {
        // The cell of the join has the type of the join. A cell that may be unspecified takes no cast, and neither does a handler edge.
        const ObjectState& sourceState = *stateOf(source);
        TypeID joinType = function().at(m_sinking.at(join).definition).type;
        bool canCastCell = !outgoing.edge.isHandlerEdge() && (sourceState.isCell() || !sourceState.hasCell());
        if (!(canCastCell ? fitsType(source, joinType) : fitsTypeWithoutCasts(source, joinType)))
            m_sinking.abandon(join);
    }

    bool isOfOtherFamily = source && m_sinking.at(source).family != m_sinking.at(join).family;
    bool entersAsCell = isOfOtherFamily || m_sinking.decisions().isBuiltAtHead(outgoing.target, join);
    if (source && !entersAsCell) {
        m_enteredEdge.consumedSources.append(ConsumedSource { source, join });
        return;
    }
    if (source && !stateOf(source)->isCell()) {
        willEmitOnEdge(outgoing);
        buildOnEdge(*this, m_sinking, outgoing, source);
    }

    VirtualShapeID shape = cellShape;
    if (!entersAsCell) {
        if (std::optional<VirtualConstant> constant = virtualConstantOf(program(), function(), binding.value))
            shape = m_sinking.familyOf(join).intern(VirtualShape::ofConstant(*constant));
    }
    m_enteredEdge.newJoinStates.append(ObjectEntry { join, ObjectState::withShape(shape) });
}

void ObjectInterpreter::commitBindings()
{
    // Building the holder of a source for another parameter builds the source too, so its state is read after every build of the edge.
    for (const ConsumedSource& consumed : m_enteredEdge.consumedSources)
        m_enteredEdge.newJoinStates.append(ObjectEntry { consumed.join, *stateOf(consumed.source) });

    Vector<std::pair<VirtualObjectID, ObjectValues>, 1> valuesOfJoins;
    if (isRewriting()) {
        for (const ObjectEntry& join : m_enteredEdge.newJoinStates)
            valuesOfJoins.append({ join.object, valuesEnteringJoin(*this, m_sinking, m_enteredEdge, join) });
    }

    const CompactList<ValueID>& parameters = function().at(m_enteredEdge.edge->target).parameters;
    m_state.aliases.removeAllMatching([&](const AliasEntry& alias) {
        return parameters.contains(alias.value);
    });
    for (const AliasEntry& alias : m_enteredEdge.newAliases)
        m_state.setAlias(alias.value, alias.object, alias.kind);
    for (const ObjectEntry& join : m_enteredEdge.newJoinStates) {
        m_state.remove(join.object);
        m_state.add(join.object, ObjectState(join.state));
    }
    for (auto& [join, joinValues] : valuesOfJoins)
        values().ensure(join) = WTF::move(joinValues);
}

void ObjectInterpreter::abandonJoinsOfLiveSources()
{
    if (m_mode != SinkingMode::Analyze)
        return;

    for (const ConsumedSource& consumed : m_enteredEdge.consumedSources) {
        if (stateOf(consumed.join)->isCell())
            continue;
        bool hasSeveralJoins = std::ranges::count(m_enteredEdge.consumedSources, consumed.source, &ConsumedSource::source) > 1;
        if (hasSeveralJoins || stateOf(consumed.source))
            m_sinking.abandon(consumed.join);
    }
    for (const ObjectEntry& join : m_enteredEdge.newJoinStates) {
        if (isReferenced(m_state, join.object))
            m_sinking.abandon(join.object);
    }
}

void ObjectInterpreter::makeFieldsOfCellsValid(const OutgoingEdge& outgoing, const SinkingState* headOfTarget)
{
    bool isHandlerEdge = outgoing.edge.isHandlerEdge();
    bool canMeetOtherShapes = m_sinking.inputs().mergeBlocks.get(outgoing.target.index());
    for (ObjectEntry& entry : m_state.objects) {
        ObjectState& state = entry.state;
        if (!state.hasCell())
            continue;
        bool isJoin = m_sinking.at(entry.object).isJoin;
        if (isHandlerEdge || (isJoin && state.isCell())) {
            state.fieldsValidInCell.clearAll();
            continue;
        }
        if (isJoin || !state.isCell())
            continue;
        if (m_mode == SinkingMode::Analyze) {
            state.fieldsValidInCell = canMeetOtherShapes ? loadableFieldsOf(m_sinking, entry.object) : BitVector();
            continue;
        }

        const ObjectState* head = headOfTarget->find(entry.object);
        if (!head || !head->hasCell() || head->isCell())
            continue;
        if (m_mode == SinkingMode::Count && hasFieldHoldingCells(m_sinking, entry.object))
            m_sinking.didBuildAfterMerge(entry.object, outgoing.target);
        state.fieldsValidInCell = head->fieldsValidInCell;
        if (state.fieldsValidInCell.isEmpty())
            continue;

        willEmitOnEdge(outgoing);
        const VirtualObjectDescription& description = m_sinking.descriptionOf(entry.object);
        if (m_mode == SinkingMode::Count) {
            if (description.kind == VirtualObjectKind::Object)
                m_sinking.at(entry.object).needsGuard = true;
            continue;
        }

        const VirtualFamily& family = m_sinking.familyOf(entry.object);
        ObjectValues& carried = values().ensure(entry.object);
        for (size_t bit : state.fieldsValidInCell) {
            uint32_t field = static_cast<uint32_t>(bit);
            Value load = makeFieldRead(program(), description, family.fieldAt(field), carried.cell, carried.guard);
            if (Value::opcodesWithReceivers.contains(load.opcode))
                setExactReceivers(load, description.layout, cursor().builder().types());
            carried.setField(field, cursor().emit(WTF::move(load)));
        }
    }
}

void ObjectInterpreter::dropDeadEntries(BlockID target, const SinkingState* headOfTarget)
{
    if (m_mode == SinkingMode::Analyze) {
        removeDeadEntries(m_sinking, m_state, target);
        return;
    }

    // fillTarget() resolves the arguments of the edge after this, so a cell stays although the head does not have it.
    Vector<VirtualObjectID, 4> deadObjects;
    for (const ObjectEntry& entry : m_state.objects) {
        if (!entry.state.isCell() && !headOfTarget->find(entry.object))
            deadObjects.append(entry.object);
    }
    for (VirtualObjectID object : deadObjects) {
        m_state.remove(object);
        if (isRewriting())
            values().remove(object);
        else
            m_sinking.at(object).diesVirtual = true;
    }
    m_state.aliases.removeAllMatching([&](const AliasEntry& alias) {
        return !stateOf(alias.object);
    });
    m_state.tests.removeAllMatching([&](const TestEntry& test) {
        return !headOfTarget->findTest(test.test);
    });
}

void ObjectInterpreter::willEmitOnEdge(const OutgoingEdge& outgoing)
{
    if (m_mode != SinkingMode::Count || outgoing.edge.isHandlerEdge())
        return;
    if (function().at(outgoing.edge.from).terminator.targets.size() > 1)
        m_sinking.addEdgeNeedingCode(outgoing.edge);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
