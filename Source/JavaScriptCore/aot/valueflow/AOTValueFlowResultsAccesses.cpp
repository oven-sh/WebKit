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

ValueFlowEscapeOrigins::ValueFlowEscapeOrigins(const ValueFlowResultInputs& inputs)
{
    using ByNumber = WTF::UnsignedWithZeroKeyHashTraits<uint32_t>;
    using SetOfNode = HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, ByNumber>;
    const Program& program = inputs.program;
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowProgramNodes& nodes = inputs.bodies().nodes();
    const ValueSets& sets = inputs.sets();
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();

    uint32_t numberOfObjects = universe.numberOfObjects();
    m_originOfObject.fill(ValueFlowStatementID(), numberOfObjects);
    m_firstPathThrough.fill(0u, numberOfObjects + 1);
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ValueFlowNodeID carriedOut = nodes.ofHost(ValueFlowHostNode::ValuesCarriedOut);
    // A statement created by a rule passes values on behalf of the operation, or the statement, that first requested it.
    auto requestingOperationOf = [&](ValueFlowStatementID logger) {
        AOT_THRESHOLD(unsigned, maxStatementsFollowed, 64);
        for (unsigned followed = 0; logger && solver.isMadeByRule(logger) && followed < maxStatementsFollowed; ++followed)
            logger = solver.firstRequesterOf(logger);
        return logger;
    };
    ValueSetID escapedObjects = maker.intersect(solver.possibleValuesOf(escaped), ValueSets::complement(universe.placeholdersAlone()));
    if (ValueSets::isComplement(escapedObjects))
        return;
    // A built-in that inspects its arguments returns some of what it finds. What it finds is derived from what it was passed.
    Vector<ValueFlowNodeID, 4> inspectedNodes;
    for (ValueFlowNodeID taking : { escaped, carriedOut }) {
        solver.graph().forEachPredecessor(taking, [&](ValueFlowNodeID predecessor) {
            const ValueFlowSolver::LoggedEdge* logged = solver.findLoggedEdge({ predecessor, taking });
            ValueFlowStatementID operation = logged ? requestingOperationOf(logged->statement) : ValueFlowStatementID();
            if (!operation || solver.statementAt(operation).kind != ValueFlowStatementKind::Behavior)
                return;
            ValueFlowNodeID inspectedNode = solver.statementAt(operation).result;
            if (inspectedNode && !ValueSets::isComplement(solver.seenValuesOf(inspectedNode)) && !inspectedNodes.contains(inspectedNode))
                inspectedNodes.append(inspectedNode);
        });
    }
    AOT::sort(inspectedNodes);
    for (ValueFlowNodeID inspectedNode : inspectedNodes)
        escapedObjects = maker.unite(escapedObjects, maker.intersect(solver.seenValuesOf(inspectedNode), ValueSets::complement(universe.placeholdersAlone())));
    auto forEachObject = [&](ValueSetID set, const auto& functor) {
        sets.forEachMember(set, [&](ValueFlowObjectID object) {
            if (universe.isObject(object) && object.index() < numberOfObjects)
                functor(object);
        });
    };

    // A node that retires connects its predecessors directly to the node that covers it, and that edge has no statement. The edge that brought the value to the retired node has one.
    HashMap<uint32_t, Vector<const ValueFlowSolver::LoggedEdge*, 1>, IntHash<uint32_t>, ByNumber> edgesIntoRetiredNodes;
    for (const ValueFlowSolver::LoggedEdge& logged : solver.edgesInOrderLogged()) {
        if (solver.graph().isRetired(logged.edge.to))
            edgesIntoRetiredNodes.add(logged.edge.from.index(), Vector<const ValueFlowSolver::LoggedEdge*, 1> { }).iterator->value.append(&logged);
    }
    auto isProgramFunction = [&](FunctionID function) {
        if (!function || function.index() >= program.functions.size())
            return false;
        FunctionRole role = program.at(function).role;
        return role != FunctionRole::Library && role != FunctionRole::Model;
    };
    auto isProgramStatement = [&](ValueFlowStatementID id) {
        return id && isProgramFunction(solver.statementAt(id).function);
    };
    // Any holder of a program object can reach what the program stored on it. What library code stores on its own object is traced back to where the program passed it in.
    Vector<ValueFlowObjectID> escapedProgramObjects;
    forEachObject(escapedObjects, [&](ValueFlowObjectID object) {
        FunctionID function = universe.closureFunction(object);
        if (!function)
            function = universe.functionOf(object);
        LayoutID layout = function ? LayoutID() : universe.kindOf(object) == ValueFlowObjectKind::Prototype ? universe.classLayoutOf(object) : universe.layoutOf(object);
        if (layout && program.at(layout).constructor)
            function = program.at(layout).constructor;
        if (function ? isProgramFunction(function) : !!layout)
            escapedProgramObjects.append(object);
    });
    ValueSetID escapedProgramSet = maker.of(escapedProgramObjects.span());
    Vector<uint32_t> firstFieldOfLayout(program.layouts.size() + 1, [&](size_t index) {
        return index < program.layouts.size() ? nodes.field(LayoutID(static_cast<uint32_t>(index)), 0).index() : nodes.field(LayoutID(static_cast<uint32_t>(index - 1)), program.layouts[index - 1].fields.size()).index();
    });
    struct EscapedHolders {
        ValueSetID objects { ValueSets::nothing };
        // The node for what is stored on them.
        ValueFlowNodeID contents { };
    };
    auto escapedHoldersOf = [&](ValueFlowNodeID from, ValueFlowStatementID logger) -> EscapedHolders {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(from);
        while (key && key->kind == ValueFlowMadeNodeKind::Masked) {
            from = key->maskedNode();
            key = solver.keyOfMadeNode(from);
        }
        if (!key && from.index() >= firstFieldOfLayout.first() && from.index() < firstFieldOfLayout.last()) {
            LayoutID layout(static_cast<uint32_t>(std::ranges::upper_bound(firstFieldOfLayout, from.index()) - firstFieldOfLayout.begin() - 1));
            return { maker.intersect(universe.objectsWith(layout), escapedObjects), from };
        }
        if (!key) {
            // A function's result escapes once code outside the program calls it.
            FunctionID function = logger && solver.statementAt(logger).kind != ValueFlowStatementKind::Operation ? nodes.functionOf(from) : FunctionID();
            ValueFlowObjectID closure = function && function.index() < program.functions.size() ? universe.closureOf(function) : ValueFlowObjectID();
            return { closure && sets.contains(escapedObjects, closure) ? maker.of(closure) : ValueSets::nothing };
        }
        ValueSetID through;
        switch (key->kind) {
        case ValueFlowMadeNodeKind::Written:
        case ValueFlowMadeNodeKind::WrittenUnderKind:
            return { key->set ? maker.intersect(key->set, escapedProgramSet) : ValueSets::nothing, from };
        case ValueFlowMadeNodeKind::CallGroup:
            if (key->callKind() == ValueFlowCallKind::CallByNativeCode || key->callKind() == ValueFlowCallKind::ConstructByNativeCode || key->callKind() == ValueFlowCallKind::CallByHost)
                through = key->set;
            break;
        case ValueFlowMadeNodeKind::Constant:
            if (logger && solver.isMadeByRule(logger))
                through = solver.keyOf(logger).set;
            break;
        default:
            break;
        }
        return { through ? maker.intersect(through, escapedObjects) : ValueSets::nothing };
    };

    Vector<EscapePath>& paths = m_paths;
    // What code outside the program stores on an object had already escaped.
    auto holdsEscapedObjects = [&](ValueFlowNodeID node) {
        for (const ValueFlowNodeKey* key = solver.keyOfMadeNode(node); key && key->kind == ValueFlowMadeNodeKind::Masked; key = solver.keyOfMadeNode(node))
            node = key->maskedNode();
        return node == escaped || node == carriedOut;
    };
    auto addPathsThrough = [&](const EscapedHolders& through, ValueSetID lookedFor, uint32_t round) {
        Vector<ValueFlowNodeID, 8> written;
        solver.graph().forEachPredecessor(through.contents, [&](ValueFlowNodeID value) {
            written.append(value);
        });
        AOT::sort(written);
        for (ValueFlowNodeID value : written) {
            const ValueFlowNodeKey* key = solver.keyOfMadeNode(value);
            bool mirrorsWrittenValues = key && (key->kind == ValueFlowMadeNodeKind::Written || key->kind == ValueFlowMadeNodeKind::WrittenUnderKind);
            ValueSetID escaping = mirrorsWrittenValues || holdsEscapedObjects(value) ? ValueSets::nothing : maker.intersect(solver.seenValuesOf(value), lookedFor);
            if (escaping == ValueSets::nothing)
                continue;
            const ValueFlowSolver::LoggedEdge* logged = solver.findLoggedEdge({ value, through.contents });
            paths.append({ std::max(round, logged ? logged->round : 0), { }, through.objects, escaping });
        }
    };
    SetOfNode seen;
    seen.add(escaped.index(), escapedObjects);
    Vector<std::pair<uint32_t, ValueSetID>> level { { escaped.index(), escapedObjects } };
    for (ValueFlowNodeID inspectedNode : inspectedNodes) {
        seen.add(inspectedNode.index(), escapedObjects);
        level.append({ inspectedNode.index(), escapedObjects });
    }
    Vector<ValueFlowNodeID> predecessors;
    while (!level.isEmpty()) {
        SetOfNode next;
        for (auto [node, lookedFor] : level) {
            bool receivesEscapedObjects = ValueFlowNodeID(node) == escaped || ValueFlowNodeID(node) == carriedOut;
            predecessors.shrink(0);
            solver.graph().forEachPredecessor(ValueFlowNodeID(node), [&](ValueFlowNodeID predecessor) {
                predecessors.append(predecessor);
            });
            AOT::sort(predecessors);
            for (ValueFlowNodeID predecessor : predecessors) {
                if (predecessor == escaped)
                    continue;
                ValueSetID escaping = maker.intersect(solver.seenValuesOf(predecessor), lookedFor);
                if (escaping == ValueSets::nothing)
                    continue;
                auto continueFrom = [&](ValueFlowNodeID from, ValueSetID held) {
                    auto seenThere = seen.add(from.index(), ValueSets::nothing);
                    ValueSetID fresh = maker.intersect(held, ValueSets::complement(seenThere.iterator->value));
                    if (fresh == ValueSets::nothing)
                        return;
                    seenThere.iterator->value = maker.unite(seenThere.iterator->value, fresh);
                    auto entry = next.add(from.index(), ValueSets::nothing);
                    entry.iterator->value = maker.unite(entry.iterator->value, fresh);
                };
                auto follow = [&](const ValueFlowSolver::LoggedEdge* logged) {
                    ValueFlowStatementID logger = logged ? logged->statement : ValueFlowStatementID();
                    uint32_t round = logged ? logged->round : 0;
                    if (ValueFlowStatementID operation = receivesEscapedObjects ? requestingOperationOf(logger) : ValueFlowStatementID(); operation && operation != logger) {
                        if (isProgramStatement(operation))
                            return paths.append({ round, operation, ValueSets::nothing, escaping });
                        if (solver.statementAt(operation).kind == ValueFlowStatementKind::Behavior)
                            return;
                        // What a library operation passes on arrived at one of its operands.
                        bool hasArrived = false;
                        for (const ValueFlowOperand& operand : inputs.bodies().operandsOf(solver.statementAt(operation))) {
                            ValueSetID arrived = operand.node ? maker.intersect(solver.seenValuesOf(operand.node), escaping) : ValueSets::nothing;
                            if (arrived == ValueSets::nothing)
                                continue;
                            hasArrived = true;
                            continueFrom(operand.node, arrived);
                        }
                        if (hasArrived)
                            return;
                    }
                    if (logged && logged->edge.to != ValueFlowNodeID(node)) {
                        if (EscapedHolders through = escapedHoldersOf(logged->edge.to, { }); through.objects != ValueSets::nothing)
                            return paths.append({ round, { }, through.objects, escaping });
                    }
                    if (isProgramStatement(logger))
                        return paths.append({ round, logger, ValueSets::nothing, escaping });
                    if (EscapedHolders through = escapedHoldersOf(predecessor, logger); through.objects != ValueSets::nothing) {
                        if (!through.contents)
                            return paths.append({ round, { }, through.objects, escaping });
                        if (!solver.keyOfMadeNode(through.contents))
                            addPathsThrough(through, escaping, round);
                        return;
                    }
                    const ValueFlowNodeKey* key = solver.keyOfMadeNode(predecessor);
                    if (key && key->kind == ValueFlowMadeNodeKind::Constant) {
                        // When a rule creates a program object, no statement passes it on.
                        if (ValueSetID ofLibrary = maker.intersect(escaping, ValueSets::complement(escapedProgramSet)); ofLibrary != ValueSets::nothing)
                            paths.append({ round, logger, ValueSets::nothing, ofLibrary });
                        return;
                    }
                    continueFrom(predecessor, escaping);
                };
                if (const ValueFlowSolver::LoggedEdge* logged = solver.findLoggedEdge({ predecessor, ValueFlowNodeID(node) })) {
                    follow(logged);
                    continue;
                }
                auto intoRetiredNodes = receivesEscapedObjects ? edgesIntoRetiredNodes.find(predecessor.index()) : edgesIntoRetiredNodes.end();
                if (intoRetiredNodes == edgesIntoRetiredNodes.end()) {
                    follow(nullptr);
                    continue;
                }
                for (const ValueFlowSolver::LoggedEdge* logged : intoRetiredNodes->value)
                    follow(logged);
            }
        }
        level.shrink(0);
        for (auto& [node, lookedFor] : next)
            level.append({ node, lookedFor });
        AOT::sort(level, { }, [](const auto& entry) { return entry.first; });
    }

    // Any holder of an object can reach what is stored on it and what it inherits from.
    for (uint32_t node = solver.input().numberOfNodes; node < solver.graph().numberOfNodes(); ++node) {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(node));
        if (!key || !key->set || ValueSets::isComplement(key->set) || (key->kind != ValueFlowMadeNodeKind::Written && key->kind != ValueFlowMadeNodeKind::WrittenUnderKind))
            continue;
        if (ValueSetID through = maker.intersect(key->set, escapedProgramSet); through != ValueSets::nothing)
            addPathsThrough({ through, ValueFlowNodeID(node) }, escapedObjects, 0);
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        ValueFlowObjectID prototype = universe.prototypeOf(LayoutID(index));
        if (!sets.contains(escapedObjects, prototype))
            continue;
        if (ValueSetID instances = maker.intersect(universe.objectsWith(LayoutID(index)), escapedObjects); instances != ValueSets::nothing)
            paths.append({ 0, { }, instances, maker.of(prototype) });
        if (LayoutID base = program.layouts[index].base; base && sets.contains(escapedObjects, universe.prototypeOf(base)))
            paths.append({ 0, { }, maker.of(prototype), maker.of(universe.prototypeOf(base)) });
    }
    Vector<uint32_t>& firstPathThrough = m_firstPathThrough;
    for (const EscapePath& path : paths) {
        forEachObject(path.through, [&](ValueFlowObjectID holder) {
            ++firstPathThrough[holder.index() + 1];
        });
    }
    for (uint32_t index = 0; index < numberOfObjects; ++index)
        firstPathThrough[index + 1] += firstPathThrough[index];
    m_pathsThrough.grow(firstPathThrough.last());
    Vector<uint32_t> nextPathThrough(firstPathThrough.span().first(numberOfObjects));
    for (uint32_t index = 0; index < paths.size(); ++index) {
        forEachObject(paths[index].through, [&](ValueFlowObjectID holder) {
            m_pathsThrough[nextPathThrough[holder.index()]++] = index;
        });
    }
    // The choice among several origins must not depend on the rounds, which depend on the number of threads: it is the first in source order, program before library.
    // A statement that passes values on only because its callee or holder has unknown type may itself depend on an origin, so it only names what nothing else has named.
    auto sourceOrderOf = [&](ValueFlowStatementID id) {
        const ValueFlowStatement& statement = solver.statementAt(id);
        return std::tuple { !isProgramStatement(id), statement.function.index(), statement.site.index() };
    };
    auto passesToValueOfUnknownType = [&](ValueFlowStatementID id) {
        if (!id || solver.statementAt(id).kind != ValueFlowStatementKind::Operation || !solver.statementAt(id).operandCount)
            return false;
        ValueFlowNodeID taking = inputs.bodies().operandsOf(solver.statementAt(id))[0].node;
        return taking && sets.contains(solver.possibleValuesOf(taking), ValueFlowUniverse::unknownObject());
    };
    BitVector hasOrigin(numberOfObjects);
    BitVector hasFinalOrigin(numberOfObjects);
    Vector<ValueFlowObjectID> objectsToFollow;
    auto follow = [&](uint32_t index, ValueFlowStatementID origin) {
        forEachObject(paths[index].escaping, [&](ValueFlowObjectID object) {
            if (hasFinalOrigin.quickGet(object.index()))
                return;
            ValueFlowStatementID& named = m_originOfObject[object.index()];
            if (hasOrigin.quickSet(object.index()) && (!origin || (named && sourceOrderOf(origin) >= sourceOrderOf(named))))
                return;
            named = origin;
            objectsToFollow.append(object);
        });
    };
    for (bool isConditional : { false, true }) {
        for (uint32_t index = 0; index < paths.size(); ++index) {
            if (paths[index].through == ValueSets::nothing && passesToValueOfUnknownType(paths[index].statement) == isConditional)
                follow(index, paths[index].statement);
        }
        while (!objectsToFollow.isEmpty()) {
            ValueFlowObjectID holder = objectsToFollow.takeLast();
            for (uint32_t index : pathsThrough(holder))
                follow(index, m_originOfObject[holder.index()]);
        }
        hasFinalOrigin = hasOrigin;
    }
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
    auto rootAt = [&](const Witness& witness, ValueFlowUnknownSource source) {
        return rootsOfOperations.ensure(hasFunction(witness) ? (static_cast<uint64_t>(witness.function) << 32 | witness.value) + 1 : UINT64_MAX - 1, [&] {
            m_roots.append(Root { witness, source });
            return m_roots.size() - 1;
        }).iterator->value;
    };
    enum class SeedDependency : uint8_t { None, OnEscapedObject };
    Vector<std::pair<uint32_t, uint32_t>> independentSeeds;
    auto seed = [&](ValueFlowNodeID node, const Witness& witness, ValueFlowUnknownSource source, SeedDependency dependency = SeedDependency::None) {
        if (!hasFunction(witness))
            return;
        uint32_t seeded = rootAt(witness, source);
        if (dependency == SeedDependency::None)
            independentSeeds.append({ node.index(), seeded });
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

    ValueFlowEscapeOrigins origins(inputs);
    Vector<std::pair<uint32_t, uint32_t>> nodesReachedThroughObjects;
    auto seedThroughObject = [&](ValueFlowObjectID object, ValueFlowNodeID node, ValueFlowUnknownSource source) {
        ValueFlowStatementID origin = origins.originOf(object);
        if (!origin)
            return;
        nodesReachedThroughObjects.append({ object.index(), node.index() });
        // Everything that depends on one root is reported under one cause. At a statement that passes an object on, the cause is that statement, whatever a rule later derived for the object.
        bool keepsSource = source == ValueFlowUnknownSource::ParameterOfEscapedFunction || source == ValueFlowUnknownSource::ContentsOfEscapedObject;
        seed(node, witnessOfStatement(inputs.program, solver.statementAt(origin)), keepsSource ? source : ValueFlowUnknownSource::ParameterOfEscapedFunction, SeedDependency::OnEscapedObject);
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
                seedThroughObject(inputs.universe().closureOf(function), edge.to, *source);
            continue;
        }
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(edge.to);
        if (*source == ValueFlowUnknownSource::ContentsOfEscapedObject && key && (key->kind == ValueFlowMadeNodeKind::Written || key->kind == ValueFlowMadeNodeKind::WrittenUnderKind)) {
            for (uint32_t reader : holders.successorsOf(edge.to.index())) {
                ValueSetID receivers = readsThroughSet(reader);
                if (!receivers || ValueSets::isComplement(receivers))
                    continue;
                // Of the objects read through, those that code outside the program has stored to.
                solver.sets().forEachMember(receivers, [&](ValueFlowObjectID receiver) {
                    if (!key->set || ValueSets::isComplement(key->set) || solver.sets().contains(key->set, receiver))
                        seedThroughObject(receiver, ValueFlowNodeID(reader), *source);
                });
            }
            continue;
        }
        // What a rule derives for escaped objects starts where they escaped.
        ValueFlowStatementID logger = solver.statementThatLogged(edge);
        ValueSetID asked = logger && solver.isMadeByRule(logger) ? solver.keyOf(logger).set : ValueSetID();
        bool isReachedThroughObjects = false;
        if (asked && !ValueSets::isComplement(asked)) {
            solver.sets().forEachMember(asked, [&](ValueFlowObjectID object) {
                if (!origins.originOf(object))
                    return;
                isReachedThroughObjects = true;
                seedThroughObject(object, edge.to, *source);
            });
        }
        if (!isReachedThroughObjects)
            beginAt(logger, edge.to, *source);
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

    // What each value of unknown type, and each escaped object, depends on: nothing, one root, or several. A statement that passes objects to a callee or a holder
    // of unknown type does so only while that value has unknown type. If the value depends on a single escape, so does what the statement passes on:
    // that is how a cycle of objects that make each other escape is attributed to the place where the first of them escaped.
    constexpr uint32_t several = none - 1;
    std::span<const ValueFlowEscapeOrigins::EscapePath> paths = origins.paths();
    Vector<uint32_t> rootOfPath(FillWith { }, paths.size(), none);
    Vector<uint32_t> rootOfObject(FillWith { }, inputs.universe().numberOfObjects(), none);
    Vector<uint32_t> onlyRootOfNode(FillWith { }, numberOfNodes, none);
    Vector<uint32_t> pathsToFollow;
    Vector<uint32_t> objectsToFollow;
    Vector<uint32_t> nodesToFollow;
    auto join = [&](Vector<uint32_t>& roots, uint32_t index, uint32_t root, Vector<uint32_t>& toFollow) {
        uint32_t& into = roots[index];
        if (root == none || into == root || into == several)
            return;
        into = into == none ? root : several;
        toFollow.append(index);
    };
    BitVector isEscapeRoot;
    Vector<uint32_t> rootOfPathStatement(FillWith { }, paths.size(), none);
    Vector<std::pair<uint32_t, uint32_t>> pathsThroughNodes;
    for (uint32_t index = 0; index < paths.size(); ++index) {
        if (paths[index].through != ValueSets::nothing)
            continue;
        Witness witness = paths[index].statement ? witnessOfStatement(inputs.program, solver.statementAt(paths[index].statement)) : noWitness;
        uint32_t root = rootAt(witness, ValueFlowUnknownSource::ParameterOfEscapedFunction);
        rootOfPathStatement[index] = root;
        isEscapeRoot.set(root);
        const ValueFlowStatement* statement = paths[index].statement ? &solver.statementAt(paths[index].statement) : nullptr;
        ValueFlowNodeID taking = statement && statement->kind == ValueFlowStatementKind::Operation && statement->operandCount ? inputs.bodies().operandsOf(*statement)[0].node : ValueFlowNodeID();
        if (taking && holders.holdsUnknownObject.quickGet(taking.index()))
            pathsThroughNodes.append({ taking.index(), index });
        else
            join(rootOfPath, index, root, pathsToFollow);
    }
    AOT::sort(pathsThroughNodes);
    AOT::sort(nodesReachedThroughObjects);
    for (auto [node, root] : independentSeeds)
        join(onlyRootOfNode, node, root, nodesToFollow);
    bool havePathsWithoutRootStarted = false;
    for (;;) {
        if (pathsToFollow.isEmpty() && objectsToFollow.isEmpty() && nodesToFollow.isEmpty()) {
            if (std::exchange(havePathsWithoutRootStarted, true))
                break;
            for (auto [taking, path] : pathsThroughNodes)
                join(rootOfPath, path, rootOfPath[path] == none ? rootOfPathStatement[path] : none, pathsToFollow);
        }
        while (!pathsToFollow.isEmpty()) {
            uint32_t path = pathsToFollow.takeLast();
            solver.sets().forEachMember(paths[path].escaping, [&](ValueFlowObjectID object) {
                if (object.index() < rootOfObject.size())
                    join(rootOfObject, object.index(), rootOfPath[path], objectsToFollow);
            });
        }
        while (!objectsToFollow.isEmpty()) {
            uint32_t object = objectsToFollow.takeLast();
            for (uint32_t path : origins.pathsThrough(ValueFlowObjectID(object)))
                join(rootOfPath, path, rootOfObject[object], pathsToFollow);
            for (auto [holder, node] : std::ranges::equal_range(nodesReachedThroughObjects, object, { }, &std::pair<uint32_t, uint32_t>::first))
                join(onlyRootOfNode, node, rootOfObject[object], nodesToFollow);
        }
        while (!nodesToFollow.isEmpty()) {
            uint32_t node = nodesToFollow.takeLast();
            uint32_t root = onlyRootOfNode[node];
            forEachSuccessor(node, [&](uint32_t successor) {
                join(onlyRootOfNode, successor, root, nodesToFollow);
            });
            for (auto [taking, path] : std::ranges::equal_range(pathsThroughNodes, node, { }, &std::pair<uint32_t, uint32_t>::first))
                join(rootOfPath, path, root != several && isEscapeRoot.get(root) ? root : rootOfPathStatement[path], pathsToFollow);
        }
    }
    m_originOfObject.fill(noWitness, rootOfObject.size());
    for (uint32_t object = 0; object < rootOfObject.size(); ++object) {
        if (rootOfObject[object] == several)
            m_hasOtherOrigins.set(object);
        if (rootOfObject[object] < several && hasFunction(m_roots[rootOfObject[object]].witness))
            m_originOfObject[object] = m_roots[rootOfObject[object]].witness;
        else if (ValueFlowStatementID origin = origins.originOf(ValueFlowObjectID(object)))
            m_originOfObject[object] = witnessOfStatement(inputs.program, solver.statementAt(origin));
    }
    m_hasSeveralRoots.clearAll();
    for (uint32_t node = 0; node < numberOfNodes; ++node) {
        if (onlyRootOfNode[node] == several)
            m_hasSeveralRoots.quickSet(node);
        else if (onlyRootOfNode[node] != none && hasFunction(m_roots[onlyRootOfNode[node]].witness))
            m_rootOfNode[node] = onlyRootOfNode[node];
    }
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
