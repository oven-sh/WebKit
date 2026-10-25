/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include <optional>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

Witness witnessOfStatement(const Program& program, const ValueFlowStatement& statement)
{
    if (!statement.function || statement.function.index() >= program.functions.size())
        return noWitness;
    const Function& function = program.at(statement.function);
    if (!statement.site)
        return { statement.function.index(), Wire::none, function.declarationLocation() };
    return { statement.function.index(), statement.site.index(), function.locationOf(statement.site) };
}

static void keepLowerStatement(const ValueFlowSolver& solver, ValueFlowStatementID& current, ValueFlowStatementID candidate)
{
    auto orderOf = [&](ValueFlowStatementID id) {
        return std::tuple { solver.statementAt(id).function.index(), solver.statementAt(id).site.index() };
    };
    if (candidate && (!current || orderOf(candidate) < orderOf(current)))
        current = candidate;
}

static Vector<ValueFlowStatementID> findEscapesOfObjects(const ValueFlowResultInputs& inputs)
{
    const ValueFlowSolver& solver = inputs.solver();
    Vector<ValueFlowStatementID> statements;
    statements.fill(ValueFlowStatementID(), inputs.universe().numberOfObjects());
    Vector<ValueFlowObjectID> level;
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    HashMap<uint32_t, ValueFlowStatementID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> loggersOfEdgesIntoRetiredNodes;
    for (const ValueFlowSolver::LoggedEdge& logged : solver.edgesInOrderLogged()) {
        if (solver.graph().isRetired(logged.edge.to))
            keepLowerStatement(solver, loggersOfEdgesIntoRetiredNodes.add(logged.edge.from.index(), ValueFlowStatementID()).iterator->value, logged.statement);
    }

    HashMap<uint32_t, ValueFlowStatementID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> lowestOfSets;
    solver.graph().forEachPredecessor(escaped, [&](ValueFlowNodeID from) {
        ValueSetID held = solver.possibleValuesOf(from);
        if (ValueSets::isComplement(held))
            return;
        ValueFlowStatementID logger = solver.statementThatLogged({ from, escaped });
        if (!logger)
            logger = loggersOfEdgesIntoRetiredNodes.get(from.index());
        if (logger && hasFunction(witnessOfStatement(inputs.program, solver.statementAt(logger))))
            keepLowerStatement(solver, lowestOfSets.add(held.index(), ValueFlowStatementID()).iterator->value, logger);
    });
    for (auto& [set, statement] : lowestOfSets) {
        solver.sets().forEachMember(ValueSetID(set), [&](ValueFlowObjectID object) {
            if (!inputs.universe().isObject(object) || ValueFlowResultInputs::isPlaceholder(object))
                return;
            if (!statements[object.index()])
                level.append(object);
            keepLowerStatement(solver, statements[object.index()], statement);
        });
    }

    const ValueSets& sets = solver.sets();
    struct Written {
        ValueSetID held;
        ValueFlowStatementID lowestOfHolders;
        bool isDone { false };
    };
    Vector<Written> written;
    Vector<uint32_t> firstOfHolder;
    firstOfHolder.fill(0u, statements.size() + 1);
    auto forEachWrittenNode = [&](const auto& functor) {
        uint32_t index = 0;
        for (uint32_t node = 0; node < solver.graph().numberOfNodes(); ++node) {
            const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(node));
            if (!key || (key->kind != ValueFlowMadeNodeKind::Written && key->kind != ValueFlowMadeNodeKind::WrittenUnderKind) || !key->set || ValueSets::isComplement(key->set))
                continue;
            ValueSetID held = solver.possibleValuesOf(ValueFlowNodeID(node));
            if (held == ValueSets::nothing || ValueSets::isComplement(held))
                continue;
            functor(index++, *key, held);
        }
    };
    forEachWrittenNode([&](uint32_t, const ValueFlowNodeKey& key, ValueSetID held) {
        written.append(Written { held, { } });
        sets.forEachMember(key.set, [&](ValueFlowObjectID holder) {
            ++firstOfHolder[holder.index() + 1];
        });
    });
    for (size_t index = 0; index + 1 < firstOfHolder.size(); ++index)
        firstOfHolder[index + 1] += firstOfHolder[index];
    Vector<uint32_t> writtenOfHolders;
    writtenOfHolders.grow(firstOfHolder.last());
    Vector<uint32_t> next(firstOfHolder.span().first(statements.size()));
    forEachWrittenNode([&](uint32_t index, const ValueFlowNodeKey& key, ValueSetID) {
        sets.forEachMember(key.set, [&](ValueFlowObjectID holder) {
            writtenOfHolders[next[holder.index()]++] = index;
        });
    });

    Vector<uint32_t> touched;
    Vector<ValueFlowObjectID> nextLevel;
    BitVector isInNextLevel(statements.size());
    while (!level.isEmpty()) {
        touched.shrink(0);
        for (ValueFlowObjectID holder : level) {
            for (uint32_t index : writtenOfHolders.subspan(firstOfHolder[holder.index()], firstOfHolder[holder.index() + 1] - firstOfHolder[holder.index()])) {
                if (written[index].isDone)
                    continue;
                if (!written[index].lowestOfHolders)
                    touched.append(index);
                keepLowerStatement(solver, written[index].lowestOfHolders, statements[holder.index()]);
            }
        }
        nextLevel.shrink(0);
        for (uint32_t index : touched) {
            written[index].isDone = true;
            sets.forEachMember(written[index].held, [&](ValueFlowObjectID object) {
                if (object.index() >= statements.size() || ValueFlowResultInputs::isPlaceholder(object))
                    return;
                if (!statements[object.index()]) {
                    isInNextLevel.quickSet(object.index());
                    nextLevel.append(object);
                } else if (!isInNextLevel.quickGet(object.index()))
                    return;
                keepLowerStatement(solver, statements[object.index()], written[index].lowestOfHolders);
            });
        }
        for (ValueFlowObjectID object : nextLevel)
            isInNextLevel.quickClear(object.index());
        std::swap(level, nextLevel);
    }
    return statements;
}

struct ValueFlowBirthsOfUnknownObject::Holders {
    BitVector holdsUnknownObject;
    Vector<uint32_t> firstSuccessor;
    Vector<uint32_t> successors;
    Vector<ValueFlowEdge> edgesOutOfConstants;

    std::span<const uint32_t> successorsOf(uint32_t node) const LIFETIME_BOUND { return successors.subspan(firstSuccessor[node], firstSuccessor[node + 1] - firstSuccessor[node]); }
};

void ValueFlowBirthsOfUnknownObject::findRoots(const ValueFlowResultInputs& inputs, const Holders& holders)
{
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowProgramNodes& nodes = inputs.bodies().nodes();
    uint32_t numberOfNodes = solver.graph().numberOfNodes();
    m_rootOfNode.fill(none, numberOfNodes);

    Vector<uint64_t> derivations;
    auto deriveFromOperands = [&](const ValueFlowStatement& statement, size_t numberOfOperands, ValueFlowNodeID to) {
        if (statement.kind != ValueFlowStatementKind::Operation)
            return false;
        bool isDerived = false;
        for (const ValueFlowOperand& operand : inputs.bodies().operandsOf(statement).first(std::min<size_t>(numberOfOperands, statement.operandCount))) {
            if (!operand.node || !holders.holdsUnknownObject.quickGet(operand.node.index()))
                continue;
            derivations.append(static_cast<uint64_t>(operand.node.index()) << 32 | to.index());
            isDerived = true;
        }
        return isDerived;
    };
    auto mayCallBuiltin = [&](const ValueFlowStatement& statement) {
        if (accessClassOf(statement.opcode) != AccessClass::IndirectCall || !statement.operandCount)
            return false;
        const ValueFlowOperand& callee = inputs.bodies().operandsOf(statement)[0];
        auto holdsBuiltin = [&](ValueSetID set) {
            return set && !ValueSets::isComplement(set) && inputs.sets().meet(set, inputs.universe().builtinFunctions());
        };
        return holdsBuiltin(callee.constant) || (callee.node && holdsBuiltin(solver.possibleValuesOf(callee.node)));
    };
    for (uint32_t function = 0; function < inputs.program.functions.size(); ++function) {
        if (!inputs.isReached(FunctionID(function)))
            continue;
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(function)) {
            if (!statement.result || !holders.holdsUnknownObject.quickGet(statement.result.index()) || accessClassOf(statement.opcode) == AccessClass::DirectCall)
                continue;
            deriveFromOperands(statement, mayCallBuiltin(statement) ? statement.operandCount : 1, statement.result);
        }
    }

    Vector<uint32_t> found;
    m_hasSeveralRoots.ensureSize(numberOfNodes);
    HashMap<uint64_t, uint32_t> rootsOfOperations;
    Vector<std::pair<uint32_t, uint32_t>> seedsInLibrary;
    auto seed = [&](ValueFlowNodeID node, const Witness& witness, ValueFlowUnknownSource source) {
        if (!hasFunction(witness))
            return;
        uint32_t seeded = rootsOfOperations.ensure((static_cast<uint64_t>(witness.function) << 32 | witness.value) + 1, [&] {
            m_roots.append(Root { witness, source });
            return m_roots.size() - 1;
        }).iterator->value;
        ValueFlowUnknownSource& named = m_roots[seeded].source;
        if (source != ValueFlowUnknownSource::ThrownByRunTime && (named == ValueFlowUnknownSource::ThrownByRunTime || source < named))
            named = source;
        FunctionRole role = inputs.program.at(FunctionID(witness.function)).role;
        if (role == FunctionRole::Library || role == FunctionRole::Model) {
            seedsInLibrary.append({ node.index(), seeded });
            return;
        }
        uint32_t& root = m_rootOfNode[node.index()];
        if (root == none) {
            root = seeded;
            found.append(node.index());
            return;
        }
        if (root == seeded)
            return;
        m_hasSeveralRoots.quickSet(node.index());
        if (isLowerWitness(witness, m_roots[root].witness))
            root = seeded;
    };

    Vector<ValueFlowStatementID> escapes = findEscapesOfObjects(inputs);
    HashMap<uint32_t, ValueFlowStatementID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> lowestEscapesOfSets;
    auto lowestEscapeOfMembers = [&](ValueSetID set) -> ValueFlowStatementID {
        if (!set || ValueSets::isComplement(set))
            return { };
        auto result = lowestEscapesOfSets.add(set.index(), ValueFlowStatementID());
        if (result.isNewEntry) {
            solver.sets().forEachMember(set, [&](ValueFlowObjectID object) {
                if (object.index() < escapes.size())
                    keepLowerStatement(solver, result.iterator->value, escapes[object.index()]);
            });
        }
        return result.iterator->value;
    };
    struct Beginning {
        ValueFlowNodeID node;
        Witness witness;
        ValueFlowUnknownSource source;
    };
    Vector<Beginning> beginningsAtOperationsOnUnknownValues;
    auto beginAt = [&](ValueFlowStatementID beginning, ValueFlowNodeID node, ValueFlowUnknownSource source) {
        if (!beginning)
            return;
        const ValueFlowStatement& statement = solver.statementAt(beginning);
        if (deriveFromOperands(statement, statement.operandCount, node))
            beginningsAtOperationsOnUnknownValues.append({ node, witnessOfStatement(inputs.program, statement), source });
        else
            seed(node, witnessOfStatement(inputs.program, statement), source);
    };
    auto readsThroughSet = [&](uint32_t node) -> ValueSetID {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(node));
        bool reads = key && (key->kind == ValueFlowMadeNodeKind::Read || key->kind == ValueFlowMadeNodeKind::ReadUnderKind || key->kind == ValueFlowMadeNodeKind::Lookup);
        return reads ? key->set : ValueSetID();
    };

    for (ValueFlowEdge edge : holders.edgesOutOfConstants) {
        std::optional<ValueFlowUnknownSource> source = solver.unknownSourceOf(edge);
        if (!source)
            continue;
        if (*source == ValueFlowUnknownSource::ParameterOfEscapedFunction) {
            if (FunctionID function = nodes.functionOf(edge.to))
                beginAt(escapes[inputs.universe().closureOf(function).index()], edge.to, *source);
            continue;
        }
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(edge.to);
        if (*source == ValueFlowUnknownSource::ContentsOfEscapedObject && key && (key->kind == ValueFlowMadeNodeKind::Written || key->kind == ValueFlowMadeNodeKind::WrittenUnderKind)) {
            for (uint32_t reader : holders.successorsOf(edge.to.index())) {
                if (ValueSetID receivers = readsThroughSet(reader))
                    beginAt(lowestEscapeOfMembers(receivers), ValueFlowNodeID(reader), *source);
            }
            continue;
        }
        beginAt(solver.statementThatLogged(edge), edge.to, *source);
    }
    AOT::sort(derivations);

    BitVector isMeetingPlace(numberOfNodes);
    isMeetingPlace.quickSet(ValueFlowProgramNodes::escaped().index());
    isMeetingPlace.quickSet(ValueFlowProgramNodes::lost().index());
    for (size_t index : holders.holdsUnknownObject) {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(static_cast<uint32_t>(index)));
        if (!key || !key->set || key->kind == ValueFlowMadeNodeKind::Constant || key->kind == ValueFlowMadeNodeKind::Masked || key->kind == ValueFlowMadeNodeKind::Instance)
            continue;
        if (ValueSets::isComplement(key->set) || inputs.sets().contains(key->set, ValueFlowUniverse::unknownObject()))
            isMeetingPlace.quickSet(index);
    }
    auto forEachSuccessor = [&](uint32_t node, const auto& functor) {
        for (uint32_t successor : holders.successorsOf(node)) {
            if (!isMeetingPlace.quickGet(successor))
                functor(successor);
        }
        for (auto derived = std::ranges::lower_bound(derivations, static_cast<uint64_t>(node) << 32); derived != derivations.end() && *derived >> 32 == node; ++derived) {
            if (!isMeetingPlace.quickGet(static_cast<uint32_t>(*derived)))
                functor(static_cast<uint32_t>(*derived));
        }
    };

    BitVector isInNextLevel(numberOfNodes);
    size_t endOfLevel = 0;
    for (size_t next = 0; next < found.size() || !beginningsAtOperationsOnUnknownValues.isEmpty() || !seedsInLibrary.isEmpty(); ++next) {
        if (next == found.size()) {
            Vector<Beginning> beginnings = std::exchange(beginningsAtOperationsOnUnknownValues, { });
            beginnings.removeAllMatching([&](const Beginning& beginning) {
                return m_rootOfNode[beginning.node.index()] != none;
            });
            for (const Beginning& beginning : beginnings)
                seed(beginning.node, beginning.witness, beginning.source);
        }
        if (next == found.size()) {
            AOT::sort(seedsInLibrary, [&](const auto& a, const auto& b) {
                return isLowerWitness(m_roots[a.second].witness, m_roots[b.second].witness);
            });
            for (auto [node, root] : std::exchange(seedsInLibrary, { })) {
                if (m_rootOfNode[node] == none) {
                    m_rootOfNode[node] = root;
                    found.append(node);
                } else if (m_rootOfNode[node] != root)
                    m_hasSeveralRoots.quickSet(node);
            }
            if (next == found.size())
                break;
        }
        if (next == endOfLevel) {
            endOfLevel = found.size();
            for (uint32_t node : found.subspan(next))
                isInNextLevel.quickClear(node);
        }
        uint32_t node = found[next];
        forEachSuccessor(node, [&](uint32_t successor) {
            uint32_t& root = m_rootOfNode[successor];
            if (root == none) {
                root = m_rootOfNode[node];
                isInNextLevel.quickSet(successor);
                found.append(successor);
            } else if (isInNextLevel.quickGet(successor) && isLowerWitness(m_roots[m_rootOfNode[node]].witness, m_roots[root].witness))
                root = m_rootOfNode[node];
        });
    }

    Vector<uint32_t> reachedBySeveral;
    auto note = [&](uint32_t node) {
        if (!m_hasSeveralRoots.quickSet(node))
            reachedBySeveral.append(node);
    };
    for (uint32_t node : found) {
        if (m_hasSeveralRoots.quickGet(node))
            reachedBySeveral.append(node);
    }
    for (uint32_t node : found) {
        forEachSuccessor(node, [&](uint32_t successor) {
            if (m_rootOfNode[successor] != m_rootOfNode[node])
                note(successor);
        });
    }
    while (!reachedBySeveral.isEmpty())
        forEachSuccessor(reachedBySeveral.takeLast(), note);
}

ValueFlowBirthsOfUnknownObject::ValueFlowBirthsOfUnknownObject(const ValueFlowResultInputs& inputs)
{
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowGraph& graph = solver.graph();
    uint32_t numberOfNodes = graph.numberOfNodes();
    m_birthOfNode.fill(none, numberOfNodes);

    Holders holders;
    BitVector& holdsUnknownObject = holders.holdsUnknownObject;
    BitVector isConstant(numberOfNodes);
    holdsUnknownObject.ensureSize(numberOfNodes);
    for (uint32_t index = 0; index < numberOfNodes; ++index) {
        ValueFlowNodeID node(index);
        if (!inputs.sets().contains(solver.possibleValuesOf(node), ValueFlowUniverse::unknownObject()))
            continue;
        holdsUnknownObject.quickSet(index);
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(node);
        if (key && key->kind == ValueFlowMadeNodeKind::Constant)
            isConstant.quickSet(index);
    }
    auto forEachEdgeBetweenHolders = [&](const auto& functor) {
        for (size_t index : holdsUnknownObject) {
            if (isConstant.quickGet(index))
                continue;
            graph.forEachPredecessor(ValueFlowNodeID(static_cast<uint32_t>(index)), [&](ValueFlowNodeID predecessor) {
                if (holdsUnknownObject.quickGet(predecessor.index()))
                    functor(predecessor.index(), static_cast<uint32_t>(index));
            });
        }
    };

    Vector<uint32_t>& firstSuccessor = holders.firstSuccessor;
    firstSuccessor.fill(0u, numberOfNodes + 1);
    Vector<uint32_t> found;
    forEachEdgeBetweenHolders([&](uint32_t from, uint32_t to) {
        if (!isConstant.quickGet(from)) {
            ++firstSuccessor[from + 1];
            return;
        }
        holders.edgesOutOfConstants.append(ValueFlowEdge { ValueFlowNodeID(from), ValueFlowNodeID(to) });
        if (solver.unknownSourceOf(ValueFlowEdge { ValueFlowNodeID(from), ValueFlowNodeID(to) }) != solver.unknownSourceOf(ValueFlowNodeID(to)))
            return;
        ValueFlowStatementID logger = solver.statementThatLogged({ ValueFlowNodeID(from), ValueFlowNodeID(to) });
        Witness witness = logger ? witnessOfStatement(inputs.program, solver.statementAt(logger)) : noWitness;
        if (m_birthOfNode[to] == none) {
            m_birthOfNode[to] = m_witnesses.size();
            m_witnesses.append(witness);
            found.append(to);
            return;
        }
        keepLowerWitness(m_witnesses[m_birthOfNode[to]], witness);
    });
    for (uint32_t index = 0; index < numberOfNodes; ++index)
        firstSuccessor[index + 1] += firstSuccessor[index];
    Vector<uint32_t>& successors = holders.successors;
    successors.grow(firstSuccessor.last());
    Vector<uint32_t> nextSuccessor(firstSuccessor.span().first(numberOfNodes));
    forEachEdgeBetweenHolders([&](uint32_t from, uint32_t to) {
        if (!isConstant.quickGet(from))
            successors[nextSuccessor[from]++] = to;
    });

    BitVector isInNextLevel(numberOfNodes);
    size_t endOfLevel = 0;
    for (size_t next = 0; next < found.size(); ++next) {
        if (next == endOfLevel) {
            endOfLevel = found.size();
            for (uint32_t node : found.subspan(next))
                isInNextLevel.quickClear(node);
        }
        uint32_t node = found[next];
        for (uint32_t successor : successors.subspan(firstSuccessor[node], firstSuccessor[node + 1] - firstSuccessor[node])) {
            uint32_t& birth = m_birthOfNode[successor];
            if (birth == none) {
                birth = m_birthOfNode[node];
                isInNextLevel.quickSet(successor);
                found.append(successor);
            } else if (isInNextLevel.quickGet(successor) && isLowerWitness(m_witnesses[m_birthOfNode[node]], m_witnesses[birth]))
                birth = m_birthOfNode[node];
        }
    }
    findRoots(inputs, holders);
}

Witness ValueFlowBirthsOfUnknownObject::witnessOf(ValueFlowNodeID node) const
{
    if (!node || node.index() >= m_birthOfNode.size())
        return noWitness;
    if (m_rootOfNode[node.index()] != none)
        return m_roots[m_rootOfNode[node.index()]].witness;
    if (m_birthOfNode[node.index()] == none)
        return noWitness;
    return m_witnesses[m_birthOfNode[node.index()]];
}

std::optional<ValueFlowUnknownSource> ValueFlowBirthsOfUnknownObject::sourceOf(ValueFlowNodeID node) const
{
    if (!node || node.index() >= m_rootOfNode.size() || m_rootOfNode[node.index()] == none)
        return std::nullopt;
    return m_roots[m_rootOfNode[node.index()]].source;
}

Witness witnessOfAllocation(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    const ValueFlowUniverse& universe = inputs.universe();
    FunctionID id = universe.functionOf(object);
    if (!id || id.index() >= inputs.program.functions.size())
        return noWitness;
    const Function& function = inputs.program.at(id);
    const ValueFlowSite* site = universe.siteOf(object);
    if (!site || !site->value)
        return { id.index(), Wire::none, function.declarationLocation() };
    return { id.index(), site->value.index(), function.locationOf(site->value) };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
