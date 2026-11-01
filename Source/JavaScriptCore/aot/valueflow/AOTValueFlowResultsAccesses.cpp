/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include <algorithm>
#include <optional>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// What depends on what: an edge leads from a cause to a consequence, and any one cause is enough. Everything starts at vertex 0.
class DependencyGraph {
    WTF_MAKE_NONCOPYABLE(DependencyGraph);
public:
    static constexpr uint32_t root = 0;
    static constexpr uint32_t none = UINT32_MAX;

    explicit DependencyGraph(uint32_t numberOfVertices)
        : m_numberOfVertices(numberOfVertices)
    {
    }

    uint32_t numberOfVertices() const { return m_numberOfVertices; }
    uint32_t addVertex() { return m_numberOfVertices++; }
    void addEdge(uint32_t from, uint32_t to) { m_edges.append({ from, to }); }

    void buildLists()
    {
        auto build = [&](Vector<uint32_t>& first, Vector<uint32_t>& list, uint32_t Edge::* from, uint32_t Edge::* to) {
            first.fill(0u, m_numberOfVertices + 1);
            for (const Edge& edge : m_edges)
                ++first[edge.*from + 1];
            for (uint32_t vertex = 0; vertex < m_numberOfVertices; ++vertex)
                first[vertex + 1] += first[vertex];
            list.grow(m_edges.size());
            Vector<uint32_t> next(first.span().first(m_numberOfVertices));
            for (const Edge& edge : m_edges)
                list[next[edge.*from]++] = edge.*to;
        };
        build(m_firstSuccessor, m_successors, &Edge::from, &Edge::to);
        build(m_firstPredecessor, m_predecessors, &Edge::to, &Edge::from);
    }

    std::span<const uint32_t> successorsOf(uint32_t vertex) const LIFETIME_BOUND { return m_successors.subspan(m_firstSuccessor[vertex], m_firstSuccessor[vertex + 1] - m_firstSuccessor[vertex]); }
    std::span<const uint32_t> predecessorsOf(uint32_t vertex) const LIFETIME_BOUND { return m_predecessors.subspan(m_firstPredecessor[vertex], m_firstPredecessor[vertex + 1] - m_firstPredecessor[vertex]); }

    BitVector findReachable() const
    {
        BitVector isReachable(m_numberOfVertices);
        Vector<uint32_t> worklist { root };
        isReachable.quickSet(root);
        while (!worklist.isEmpty()) {
            for (uint32_t successor : successorsOf(worklist.takeLast())) {
                if (!isReachable.quickSet(successor))
                    worklist.append(successor);
            }
        }
        return isReachable;
    }

    // Tarjan's algorithm over the vertices outside `excluded`. Returns the component of each of them.
    Vector<uint32_t> findStronglyConnectedComponents(const BitVector& excluded, uint32_t& numberOfComponents) const
    {
        Vector<uint32_t> component(FillWith { }, m_numberOfVertices, none);
        Vector<uint32_t> number(FillWith { }, m_numberOfVertices, none);
        Vector<uint32_t> lowest(FillWith { }, m_numberOfVertices, none);
        Vector<uint32_t> open;
        struct Visit {
            uint32_t vertex;
            uint32_t nextSuccessor;
        };
        Vector<Visit> visits;
        uint32_t nextNumber = 0;
        numberOfComponents = 0;
        for (uint32_t start = 0; start < m_numberOfVertices; ++start) {
            if (excluded.quickGet(start) || number[start] != none)
                continue;
            visits.append({ start, 0 });
            while (!visits.isEmpty()) {
                auto [vertex, nextSuccessor] = visits.last();
                if (!nextSuccessor) {
                    number[vertex] = lowest[vertex] = nextNumber++;
                    open.append(vertex);
                }
                std::span<const uint32_t> successors = successorsOf(vertex);
                if (nextSuccessor < successors.size()) {
                    ++visits.last().nextSuccessor;
                    uint32_t successor = successors[nextSuccessor];
                    if (excluded.quickGet(successor))
                        continue;
                    if (number[successor] == none)
                        visits.append({ successor, 0 });
                    else if (component[successor] == none)
                        lowest[vertex] = std::min(lowest[vertex], number[successor]);
                    continue;
                }
                visits.removeLast();
                if (!visits.isEmpty())
                    lowest[visits.last().vertex] = std::min(lowest[visits.last().vertex], lowest[vertex]);
                if (lowest[vertex] != number[vertex])
                    continue;
                for (uint32_t member = none; member != vertex;) {
                    member = open.takeLast();
                    component[member] = numberOfComponents;
                }
                ++numberOfComponents;
            }
        }
        return component;
    }

    struct DominatorTree {
        // Reachable vertices, each after its immediate dominator.
        Vector<uint32_t> order;
        Vector<uint32_t> immediateDominator;
    };

    // Semi-NCA (Georgiadis).
    DominatorTree findDominators() const
    {
        Vector<uint32_t> numberOf(FillWith { }, m_numberOfVertices, none);
        Vector<uint32_t> order;
        Vector<uint32_t> parent;
        struct Visit {
            uint32_t vertex;
            uint32_t nextSuccessor;
        };
        Vector<Visit> visits { { root, 0 } };
        numberOf[root] = 0;
        order.append(root);
        parent.append(0);
        while (!visits.isEmpty()) {
            auto [vertex, nextSuccessor] = visits.last();
            std::span<const uint32_t> successors = successorsOf(vertex);
            if (nextSuccessor == successors.size()) {
                visits.removeLast();
                continue;
            }
            ++visits.last().nextSuccessor;
            uint32_t successor = successors[nextSuccessor];
            if (numberOf[successor] != none)
                continue;
            numberOf[successor] = order.size();
            order.append(successor);
            parent.append(numberOf[vertex]);
            visits.append({ successor, 0 });
        }

        uint32_t count = order.size();
        Vector<uint32_t> semi(count, [](size_t index) { return static_cast<uint32_t>(index); });
        Vector<uint32_t> label = semi;
        Vector<uint32_t> ancestor(FillWith { }, count, none);
        Vector<uint32_t> dominator = parent;
        Vector<uint32_t> compressed;
        auto evaluate = [&](uint32_t number) {
            if (ancestor[number] == none)
                return number;
            for (uint32_t at = number; ancestor[ancestor[at]] != none; at = ancestor[at])
                compressed.append(at);
            while (!compressed.isEmpty()) {
                uint32_t at = compressed.takeLast();
                if (semi[label[ancestor[at]]] < semi[label[at]])
                    label[at] = label[ancestor[at]];
                ancestor[at] = ancestor[ancestor[at]];
            }
            return label[number];
        };
        for (uint32_t number = count - 1; number; --number) {
            for (uint32_t predecessor : predecessorsOf(order[number])) {
                if (numberOf[predecessor] != none)
                    semi[number] = std::min(semi[number], semi[evaluate(numberOf[predecessor])]);
            }
            ancestor[number] = parent[number];
        }
        DominatorTree tree { WTF::move(order), Vector<uint32_t>(FillWith { }, m_numberOfVertices, none) };
        for (uint32_t number = 1; number < count; ++number) {
            while (dominator[number] > semi[number])
                dominator[number] = dominator[dominator[number]];
            tree.immediateDominator[tree.order[number]] = tree.order[dominator[number]];
        }
        return tree;
    }

private:
    struct Edge {
        uint32_t from;
        uint32_t to;
    };

    uint32_t m_numberOfVertices;
    Vector<Edge> m_edges;
    Vector<uint32_t> m_firstSuccessor;
    Vector<uint32_t> m_successors;
    Vector<uint32_t> m_firstPredecessor;
    Vector<uint32_t> m_predecessors;
};

} // namespace

Witness witnessOfStatement(const Program& program, const ValueFlowStatement& statement)
{
    if (!statement.function || statement.function.index() >= program.functions.size())
        return noWitness;
    const Function& function = program.at(statement.function);
    if (!statement.site)
        return { statement.function.index(), Wire::none, function.declarationLocation() };
    return { statement.function.index(), statement.site.index(), function.locationOf(statement.site) };
}

// A statement created by a rule passes values on behalf of the operation, or the statement, that first requested it.
static ValueFlowStatementID requestingOperationOf(const ValueFlowSolver& solver, ValueFlowStatementID logger)
{
    AOT_THRESHOLD(unsigned, maxStatementsFollowed, 64);
    for (unsigned followed = 0; logger && solver.isMadeByRule(logger) && followed < maxStatementsFollowed; ++followed)
        logger = solver.firstRequesterOf(logger);
    return logger;
}

ValueFlowEscapeOrigins::ValueFlowEscapeOrigins(const ValueFlowResultInputs& inputs)
{
    using ByNumber = WTF::UnsignedWithZeroKeyHashTraits<uint32_t>;
    const Program& program = inputs.program;
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowProgramNodes& nodes = inputs.bodies().nodes();
    const ValueSets& sets = inputs.sets();
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();

    uint32_t numberOfObjects = universe.numberOfObjects();
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ValueFlowNodeID carriedOut = nodes.ofHost(ValueFlowHostNode::ValuesCarriedOut);
    ValueSetID escapedObjects = maker.intersect(solver.possibleValuesOf(escaped), ValueSets::complement(universe.placeholdersAlone()));
    if (ValueSets::isComplement(escapedObjects))
        return;
    // A built-in that inspects its arguments returns some of what it finds. What it finds is derived from what it was passed.
    Vector<ValueFlowNodeID, 4> inspectedNodes;
    for (ValueFlowNodeID taking : { escaped, carriedOut }) {
        solver.forEachPredecessorIncludingRemoved(taking, [&](ValueFlowNodeID predecessor) {
            const ValueFlowSolver::LoggedEdge* logged = solver.findLoggedEdge({ predecessor, taking });
            ValueFlowStatementID operation = logged ? requestingOperationOf(solver, logged->statement) : ValueFlowStatementID();
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
    // Any holder of a program object can reach what the program stored on it, and so for an object that a built-in created for an operation, such as an array iterator.
    // What library code stores on its own object is traced back to where the program passed it in.
    Vector<ValueFlowObjectID> escapedProgramObjects;
    Vector<ValueFlowObjectID> escapedBuiltinResults;
    forEachObject(escapedObjects, [&](ValueFlowObjectID object) {
        if (universe.kindOf(object) == ValueFlowObjectKind::ResultOfBuiltin)
            escapedBuiltinResults.append(object);
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
    ValueSetID escapedHolders = maker.unite(escapedProgramSet, maker.of(escapedBuiltinResults.span()));
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
            return { key->set ? maker.intersect(key->set, escapedHolders) : ValueSets::nothing, from };
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
    auto addPathsThrough = [&](const EscapedHolders& through, ValueSetID lookedFor) {
        solver.forEachPredecessorIncludingRemoved(through.contents, [&](ValueFlowNodeID value) {
            const ValueFlowNodeKey* key = solver.keyOfMadeNode(value);
            bool mirrorsWrittenValues = key && (key->kind == ValueFlowMadeNodeKind::Written || key->kind == ValueFlowMadeNodeKind::WrittenUnderKind);
            ValueSetID escaping = mirrorsWrittenValues || holdsEscapedObjects(value) ? ValueSets::nothing : maker.intersect(solver.seenValuesOf(value), lookedFor);
            if (escaping != ValueSets::nothing)
                paths.append({ { }, through.objects, escaping });
        });
    };
    // The walk goes back from the escaped objects to the statements that passed them on.
    uint32_t numberOfNodes = solver.graph().numberOfNodes();
    Vector<ValueSetID> seen(FillWith { }, numberOfNodes, ValueSets::nothing);
    Vector<ValueSetID> lookedForNext(FillWith { }, numberOfNodes, ValueSets::nothing);
    Vector<ValueFlowNodeID> level { escaped };
    level.appendVector(inspectedNodes);
    for (ValueFlowNodeID node : level)
        seen[node.index()] = lookedForNext[node.index()] = escapedObjects;
    Vector<ValueFlowNodeID> next;
    while (!level.isEmpty()) {
        for (ValueFlowNodeID node : level) {
            ValueSetID lookedFor = std::exchange(lookedForNext[node.index()], ValueSets::nothing);
            bool receivesEscapedObjects = node == escaped || node == carriedOut;
            solver.forEachPredecessorIncludingRemoved(node, [&](ValueFlowNodeID predecessor) {
                if (predecessor == escaped)
                    return;
                ValueSetID escaping = maker.intersect(solver.seenValuesOf(predecessor), lookedFor);
                if (escaping == ValueSets::nothing)
                    return;
                auto continueFrom = [&](ValueFlowNodeID from, ValueSetID held) {
                    ValueSetID fresh = maker.intersect(held, ValueSets::complement(seen[from.index()]));
                    if (fresh == ValueSets::nothing)
                        return;
                    seen[from.index()] = maker.unite(seen[from.index()], fresh);
                    if (lookedForNext[from.index()] == ValueSets::nothing)
                        next.append(from);
                    lookedForNext[from.index()] = maker.unite(lookedForNext[from.index()], fresh);
                };
                auto follow = [&](const ValueFlowSolver::LoggedEdge* logged) {
                    ValueFlowStatementID logger = logged ? logged->statement : ValueFlowStatementID();
                    if (ValueFlowStatementID operation = receivesEscapedObjects ? requestingOperationOf(solver, logger) : ValueFlowStatementID(); operation && operation != logger) {
                        if (isProgramStatement(operation))
                            return paths.append({ operation, ValueSets::nothing, escaping });
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
                    if (logged && logged->edge.to != node) {
                        if (EscapedHolders holders = escapedHoldersOf(logged->edge.to, { }); holders.objects != ValueSets::nothing)
                            return paths.append({ { }, holders.objects, escaping });
                    }
                    if (isProgramStatement(logger))
                        return paths.append({ logger, ValueSets::nothing, escaping });
                    if (EscapedHolders holders = escapedHoldersOf(predecessor, logger); holders.objects != ValueSets::nothing) {
                        if (!holders.contents)
                            return paths.append({ { }, holders.objects, escaping });
                        if (!solver.keyOfMadeNode(holders.contents))
                            addPathsThrough(holders, escaping);
                        return;
                    }
                    const ValueFlowNodeKey* key = solver.keyOfMadeNode(predecessor);
                    if (key && key->kind == ValueFlowMadeNodeKind::Constant) {
                        // When a rule creates a program object, no statement passes it on.
                        if (ValueSetID ofLibrary = maker.intersect(escaping, ValueSets::complement(escapedProgramSet)); ofLibrary != ValueSets::nothing)
                            paths.append({ logger, ValueSets::nothing, ofLibrary });
                        return;
                    }
                    continueFrom(predecessor, escaping);
                };
                if (const ValueFlowSolver::LoggedEdge* logged = solver.findLoggedEdge({ predecessor, node }))
                    return follow(logged);
                auto intoRetiredNodes = receivesEscapedObjects ? edgesIntoRetiredNodes.find(predecessor.index()) : edgesIntoRetiredNodes.end();
                if (intoRetiredNodes == edgesIntoRetiredNodes.end())
                    return follow(nullptr);
                for (const ValueFlowSolver::LoggedEdge* logged : intoRetiredNodes->value)
                    follow(logged);
            });
        }
        level.shrink(0);
        std::swap(level, next);
    }

    // Any holder of an object can reach what is stored on it and what it inherits from.
    for (uint32_t node = solver.input().numberOfNodes; node < solver.graph().numberOfNodes(); ++node) {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(node));
        if (!key || !key->set || ValueSets::isComplement(key->set) || (key->kind != ValueFlowMadeNodeKind::Written && key->kind != ValueFlowMadeNodeKind::WrittenUnderKind))
            continue;
        if (ValueSetID holders = maker.intersect(key->set, escapedHolders); holders != ValueSets::nothing)
            addPathsThrough({ holders, ValueFlowNodeID(node) }, escapedObjects);
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        ValueFlowObjectID prototype = universe.prototypeOf(LayoutID(index));
        if (!sets.contains(escapedObjects, prototype))
            continue;
        if (ValueSetID instances = maker.intersect(universe.objectsWith(LayoutID(index)), escapedObjects); instances != ValueSets::nothing)
            paths.append({ { }, instances, maker.of(prototype) });
        if (LayoutID base = program.layouts[index].base; base && sets.contains(escapedObjects, universe.prototypeOf(base)))
            paths.append({ { }, maker.of(prototype), maker.of(universe.prototypeOf(base)) });
    }
    for (const EscapePath& path : paths)
        m_objectsWithPaths = maker.unite(m_objectsWithPaths, path.escaping);
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
    uint32_t numberOfObjects = inputs.universe().numberOfObjects();
    m_rootOfNode.fill(none, numberOfNodes);
    m_hasSeveralRoots.ensureSize(numberOfNodes);
    m_originOfObject.fill(noWitness, numberOfObjects);
    m_hasOtherOrigins.ensureSize(numberOfObjects);

    // A node for every access to a receiver of unknown type collects values from the whole program, so nothing is traced through it.
    BitVector isMeetingPlace(numberOfNodes);
    isMeetingPlace.quickSet(ValueFlowProgramNodes::escaped().index());
    isMeetingPlace.quickSet(ValueFlowProgramNodes::lost().index());
    // One object stands for what a built-in answers wherever it is called without a site. It holds what every such call in the program stored on it.
    BitVector readsSharedResultsOfBuiltins(numberOfNodes);
    uint32_t firstHostObject = inputs.universe().hostObject(0).index();
    for (size_t index : holders.holdsUnknownObject) {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(static_cast<uint32_t>(index)));
        if (!key || !key->set || key->kind == ValueFlowMadeNodeKind::Constant || key->kind == ValueFlowMadeNodeKind::Masked || key->kind == ValueFlowMadeNodeKind::Instance)
            continue;
        if (ValueSets::isComplement(key->set) || inputs.sets().contains(key->set, ValueFlowUniverse::unknownObject())) {
            isMeetingPlace.quickSet(index);
            continue;
        }
        if (key->kind != ValueFlowMadeNodeKind::Read && key->kind != ValueFlowMadeNodeKind::ReadUnderKind && key->kind != ValueFlowMadeNodeKind::Lookup)
            continue;
        bool readsOnlySharedResults = key->set != ValueSets::nothing;
        inputs.sets().forEachMember(key->set, [&](ValueFlowObjectID object) {
            readsOnlySharedResults &= inputs.universe().kindOf(object) == ValueFlowObjectKind::ResultOfBuiltin && object.index() < firstHostObject;
        });
        if (readsOnlySharedResults)
            readsSharedResultsOfBuiltins.quickSet(index);
    }
    auto isTraced = [&](ValueFlowNodeID node) {
        return node && holders.holdsUnknownObject.quickGet(node.index()) && !isMeetingPlace.quickGet(node.index());
    };
    // A statement calls built-ins without a site for what an operand of unknown type may be, so it reads their shared results only while the operand has unknown type.
    auto forEachOperandOfUnknownType = [&](const ValueFlowStatement& statement, const auto& functor) {
        bool hasFound = false;
        if (statement.kind != ValueFlowStatementKind::Operation)
            return hasFound;
        for (const ValueFlowOperand& operand : inputs.bodies().operandsOf(statement)) {
            if (!isTraced(operand.node))
                continue;
            functor(operand.node);
            hasFound = true;
        }
        return hasFound;
    };
    // Where a statement finds the callee or the holder it passes values to: in its first operands, or in a lookup its rule asked for.
    auto forEachUnknownInput = [&](ValueFlowStatementID id, size_t numberOfOperands, const auto& functor) {
        const ValueFlowStatement& statement = solver.statementAt(id);
        if (statement.kind == ValueFlowStatementKind::Operation) {
            for (const ValueFlowOperand& operand : inputs.bodies().operandsOf(statement).first(std::min<size_t>(numberOfOperands, statement.operandCount))) {
                if (isTraced(operand.node))
                    functor(operand.node);
            }
        }
        inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::ReadUnderKind, ValueFlowMadeNodeKind::Lookup }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID, ValueFlowNodeID first) {
            bool reads = key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::ReadUnderKind || key.kind == ValueFlowMadeNodeKind::Lookup;
            if (!reads || !isTraced(first))
                return;
            if (!readsSharedResultsOfBuiltins.quickGet(first.index()) || !forEachOperandOfUnknownType(statement, functor))
                functor(first);
        });
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

    ValueFlowEscapeOrigins origins(inputs);
    std::span<const ValueFlowEscapeOrigins::EscapePath> paths = origins.paths();
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();

    HashMap<uint64_t, uint32_t> rootsOfOperations;
    BitVector hasSource;
    auto rootAt = [&](const Witness& witness) {
        return rootsOfOperations.ensure((static_cast<uint64_t>(witness.function) << 32 | witness.value) + 1, [&] {
            m_roots.append(Root { witness, ValueFlowUnknownSource::ParameterOfEscapedFunction });
            return m_roots.size() - 1;
        }).iterator->value;
    };
    // Everything that depends on one root is reported under one cause.
    auto addSource = [&](uint32_t root, ValueFlowUnknownSource source) {
        ValueFlowUnknownSource& named = m_roots[root].source;
        if (!hasSource.get(root) || (source != ValueFlowUnknownSource::ThrownByRunTime && (named == ValueFlowUnknownSource::ThrownByRunTime || source < named)))
            named = source;
        hasSource.set(root);
    };

    struct Edge {
        uint32_t from;
        uint32_t to;
    };
    struct Seed {
        uint32_t node;
        uint32_t root;
        ValueFlowUnknownSource source;
    };
    struct SeedThroughObjects {
        ValueSetID objects;
        uint32_t node;
        ValueFlowUnknownSource source;
    };
    Vector<Edge> derivations;
    Vector<Seed> seeds;
    Vector<Seed> seedsUnlessDerived;
    Vector<SeedThroughObjects> seedsThroughObjects;
    // The node holds a value of unknown type once one of the objects has escaped.
    auto seedThroughObjects = [&](ValueSetID objects, ValueFlowNodeID node, ValueFlowUnknownSource source) {
        objects = maker.intersect(objects, origins.objectsWithPaths());
        if (objects == ValueSets::nothing)
            return false;
        // At a statement that passes an object on, the cause is that statement, whatever a rule later derived for the object.
        bool keepsSource = source == ValueFlowUnknownSource::ParameterOfEscapedFunction || source == ValueFlowUnknownSource::ContentsOfEscapedObject;
        seedsThroughObjects.append({ objects, node.index(), keepsSource ? source : ValueFlowUnknownSource::ParameterOfEscapedFunction });
        return true;
    };
    auto seedAt = [&](ValueFlowStatementID id, ValueFlowNodeID node, ValueFlowUnknownSource source) {
        if (!id)
            return;
        bool isDerived = false;
        forEachUnknownInput(id, solver.statementAt(id).operandCount, [&](ValueFlowNodeID input) {
            derivations.append({ input.index(), node.index() });
            isDerived = true;
        });
        Witness witness = witnessOfStatement(inputs.program, solver.statementAt(id));
        if (!hasFunction(witness))
            return;
        (isDerived ? seedsUnlessDerived : seeds).append({ node.index(), rootAt(witness), source });
    };
    auto isWritten = [](const ValueFlowNodeKey* key) {
        return key && (key->kind == ValueFlowMadeNodeKind::Written || key->kind == ValueFlowMadeNodeKind::WrittenUnderKind);
    };
    auto readsThroughSet = [&](uint32_t node) -> ValueSetID {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(node));
        bool reads = key && (key->kind == ValueFlowMadeNodeKind::Read || key->kind == ValueFlowMadeNodeKind::ReadUnderKind || key->kind == ValueFlowMadeNodeKind::Lookup);
        return reads ? key->set : ValueSetID();
    };

    for (uint32_t index = 0; index < solver.numberOfStatements(); ++index) {
        ValueFlowStatementID id(index);
        const ValueFlowStatement& statement = solver.statementAt(id);
        if (!isTraced(statement.result))
            continue;
        bool isOperation = statement.kind == ValueFlowStatementKind::Operation;
        if (isOperation && (!statement.function || !inputs.isReached(statement.function) || accessClassOf(statement.opcode) == AccessClass::DirectCall))
            continue;
        forEachUnknownInput(id, isOperation && mayCallBuiltin(statement) ? statement.operandCount : 1, [&](ValueFlowNodeID input) {
            derivations.append({ input.index(), statement.result.index() });
        });
    }

    for (ValueFlowEdge edge : holders.edgesOutOfConstants) {
        std::optional<ValueFlowUnknownSource> source = solver.unknownSourceOf(edge);
        if (!source)
            continue;
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(edge.to);
        if (*source == ValueFlowUnknownSource::ParameterOfEscapedFunction) {
            auto seedParameter = [&](ValueFlowNodeID parameter) {
                if (FunctionID function = nodes.functionOf(parameter))
                    seedThroughObjects(maker.of(inputs.universe().closureOf(function)), parameter, *source);
            };
            seedParameter(edge.to);
            // Code outside the program calls all escaped functions through one group of calls.
            if (key && key->kind == ValueFlowMadeNodeKind::CallGroup) {
                for (uint32_t parameter : holders.successorsOf(edge.to.index()))
                    seedParameter(ValueFlowNodeID(parameter));
            }
            continue;
        }
        if (*source == ValueFlowUnknownSource::ContentsOfEscapedObject && isWritten(key)) {
            // What is written under one name is also listed under the kind of that name, and read from there.
            Vector<uint32_t, 4> written { edge.to.index() };
            for (size_t next = 0; next < written.size(); ++next) {
                for (uint32_t reader : holders.successorsOf(written[next])) {
                    if (isWritten(solver.keyOfMadeNode(ValueFlowNodeID(reader)))) {
                        if (!written.contains(reader))
                            written.append(reader);
                        continue;
                    }
                    ValueSetID receivers = readsThroughSet(reader);
                    if (!receivers || ValueSets::isComplement(receivers))
                        continue;
                    // Of the objects read through, those that code outside the program has stored to.
                    seedThroughObjects(key->set ? maker.intersect(receivers, key->set) : receivers, ValueFlowNodeID(reader), *source);
                }
            }
            continue;
        }
        // What a rule derives for escaped objects starts where they escaped.
        ValueFlowStatementID logger = solver.statementThatLogged(edge);
        ValueSetID asked = logger && solver.isMadeByRule(logger) ? solver.keyOf(logger).set : ValueSetID();
        if (!asked || ValueSets::isComplement(asked) || !seedThroughObjects(asked, edge.to, *source))
            seedAt(logger, edge.to, *source);
    }

    // A vertex for each root as the start of unknown values, for each root as a statement that passes objects on, for each object and node.
    // Then one for each set of holders, reached once a member has escaped, and one for each set of objects that escape together.
    Vector<uint32_t> rootOfPath(paths.size(), [&](size_t index) {
        Witness witness = paths[index].statement ? witnessOfStatement(inputs.program, solver.statementAt(paths[index].statement)) : noWitness;
        return hasFunction(witness) ? rootAt(witness) : none;
    });
    uint32_t numberOfRoots = m_roots.size();
    uint32_t firstSeedVertex = 1;
    uint32_t firstStatementVertex = firstSeedVertex + numberOfRoots;
    uint32_t firstObjectVertex = firstStatementVertex + numberOfRoots;
    uint32_t firstNodeVertex = firstObjectVertex + numberOfObjects;
    DependencyGraph graph(firstNodeVertex + numberOfNodes);
    auto rootNamedBy = [&](uint32_t vertex) {
        return vertex >= firstSeedVertex && vertex < firstObjectVertex ? (vertex - firstSeedVertex) % numberOfRoots : none;
    };
    using VertexOfSet = HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    auto vertexOf = [&](VertexOfSet& vertices, ValueSetID set, const auto& connect) {
        return vertices.ensure(set.index(), [&] {
            uint32_t vertex = graph.addVertex();
            solver.sets().forEachMember(set, [&](ValueFlowObjectID object) {
                if (inputs.universe().isObject(object) && object.index() < numberOfObjects)
                    connect(firstObjectVertex + object.index(), vertex);
            });
            return vertex;
        }).iterator->value;
    };
    VertexOfSet verticesOfHolders;
    VertexOfSet verticesOfEscapingObjects;
    auto vertexOfHolders = [&](ValueSetID set) {
        return vertexOf(verticesOfHolders, set, [&](uint32_t object, uint32_t vertex) {
            graph.addEdge(object, vertex);
        });
    };
    auto vertexOfEscapingObjects = [&](ValueSetID set) {
        return vertexOf(verticesOfEscapingObjects, set, [&](uint32_t object, uint32_t vertex) {
            graph.addEdge(vertex, object);
        });
    };

    BitVector hasConditions(solver.numberOfStatements());
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> edgesBetweenSets;
    for (uint32_t index = 0; index < paths.size(); ++index) {
        const ValueFlowEscapeOrigins::EscapePath& path = paths[index];
        uint32_t to = vertexOfEscapingObjects(path.escaping);
        if (path.through != ValueSets::nothing) {
            uint32_t from = vertexOfHolders(path.through);
            if (edgesBetweenSets.add(static_cast<uint64_t>(from) << 32 | to).isNewEntry)
                graph.addEdge(from, to);
            continue;
        }
        if (rootOfPath[index] == none) {
            graph.addEdge(DependencyGraph::root, to);
            continue;
        }
        uint32_t from = firstStatementVertex + rootOfPath[index];
        graph.addEdge(from, to);
        if (hasConditions.quickSet(path.statement.index()))
            continue;
        // A statement that passes objects to a callee or a holder of unknown type does so only while that value has unknown type.
        bool isConditional = false;
        forEachUnknownInput(path.statement, 1, [&](ValueFlowNodeID input) {
            graph.addEdge(firstNodeVertex + input.index(), from);
            isConditional = true;
        });
        if (!isConditional)
            graph.addEdge(DependencyGraph::root, from);
    }
    BitVector hasSeedVertex(numberOfRoots);
    auto addSeed = [&](const Seed& seed) {
        addSource(seed.root, seed.source);
        if (!hasSeedVertex.quickSet(seed.root))
            graph.addEdge(DependencyGraph::root, firstSeedVertex + seed.root);
        graph.addEdge(firstSeedVertex + seed.root, firstNodeVertex + seed.node);
    };
    for (const Seed& seed : seeds)
        addSeed(seed);
    for (const SeedThroughObjects& seed : seedsThroughObjects)
        graph.addEdge(vertexOfHolders(seed.objects), firstNodeVertex + seed.node);
    for (const Edge& derivation : derivations) {
        if (!isMeetingPlace.quickGet(derivation.to))
            graph.addEdge(firstNodeVertex + derivation.from, firstNodeVertex + derivation.to);
    }
    for (size_t node : holders.holdsUnknownObject) {
        if (isMeetingPlace.quickGet(node))
            continue;
        for (uint32_t successor : holders.successorsOf(static_cast<uint32_t>(node))) {
            if (isMeetingPlace.quickGet(successor))
                continue;
            if (readsSharedResultsOfBuiltins.quickGet(node) && !readsSharedResultsOfBuiltins.quickGet(successor)) {
                const ValueFlowSolver::LoggedEdge* logged = solver.findLoggedEdge({ ValueFlowNodeID(static_cast<uint32_t>(node)), ValueFlowNodeID(successor) });
                ValueFlowStatementID reader = logged ? requestingOperationOf(solver, logged->statement) : ValueFlowStatementID();
                bool dependsOnOperands = reader && forEachOperandOfUnknownType(solver.statementAt(reader), [&](ValueFlowNodeID operand) {
                    graph.addEdge(firstNodeVertex + operand.index(), firstNodeVertex + successor);
                });
                if (dependsOnOperands)
                    continue;
            }
            graph.addEdge(firstNodeVertex + node, firstNodeVertex + successor);
        }
    }
    graph.buildLists();

    // A statement that nothing above reaches depends on itself, on another such statement, or on a value that nothing explains.
    // Those that depend on no statement outside their own cycle are where the rest starts.
    auto forEachEdgeBelowUnreachedStatements = [&](const BitVector& isReachable, const auto& functor) {
        BitVector isVisited(graph.numberOfVertices());
        Vector<uint32_t> worklist;
        for (uint32_t vertex = firstStatementVertex; vertex < firstObjectVertex; ++vertex) {
            if (isReachable.quickGet(vertex) || rootNamedBy(vertex) == none || !graph.successorsOf(vertex).size())
                continue;
            isVisited.quickSet(vertex);
            worklist.append(vertex);
        }
        while (!worklist.isEmpty()) {
            uint32_t vertex = worklist.takeLast();
            for (uint32_t successor : graph.successorsOf(vertex)) {
                if (isReachable.quickGet(successor))
                    continue;
                functor(vertex, successor);
                if (!isVisited.quickSet(successor))
                    worklist.append(successor);
            }
        }
        return isVisited;
    };
    {
        BitVector isReachable = graph.findReachable();
        BitVector isBelowStatement = forEachEdgeBelowUnreachedStatements(isReachable, [](uint32_t, uint32_t) { });
        bool hasAdded = false;
        for (const Seed& seed : seedsUnlessDerived) {
            if (isReachable.quickGet(firstNodeVertex + seed.node) || isBelowStatement.quickGet(firstNodeVertex + seed.node))
                continue;
            addSeed(seed);
            hasAdded = true;
        }
        if (hasAdded)
            graph.buildLists();
    }
    {
        BitVector isReachable = graph.findReachable();
        uint32_t numberOfComponents = 0;
        Vector<uint32_t> component = graph.findStronglyConnectedComponents(isReachable, numberOfComponents);
        BitVector dependsOnOtherStatement(numberOfComponents);
        BitVector isVisited = forEachEdgeBelowUnreachedStatements(isReachable, [&](uint32_t from, uint32_t to) {
            if (component[from] != component[to])
                dependsOnOtherStatement.quickSet(component[to]);
        });
        bool hasAdded = false;
        for (uint32_t vertex = firstStatementVertex; vertex < firstObjectVertex; ++vertex) {
            if (!isVisited.quickGet(vertex) || rootNamedBy(vertex) == none || dependsOnOtherStatement.quickGet(component[vertex]))
                continue;
            graph.addEdge(DependencyGraph::root, vertex);
            hasAdded = true;
        }
        if (hasAdded)
            graph.buildLists();
    }

    auto isInLibrary = [&](uint32_t root) {
        FunctionRole role = inputs.program.at(FunctionID(m_roots[root].witness.function)).role;
        return role == FunctionRole::Library || role == FunctionRole::Model;
    };
    // Without the statement that dominates a vertex, nothing reaches the vertex. The outermost such statement is where it all starts; one in the program is preferred to one in the library.
    DependencyGraph::DominatorTree tree = graph.findDominators();
    Vector<uint32_t> onlyRoot(FillWith { }, graph.numberOfVertices(), none);
    for (uint32_t vertex : tree.order.span().subspan(1)) {
        uint32_t above = onlyRoot[tree.immediateDominator[vertex]];
        uint32_t own = rootNamedBy(vertex);
        onlyRoot[vertex] = above != none && (own == none || !isInLibrary(above) || isInLibrary(own)) ? above : own;
    }

    // A vertex that several independent roots reach lists the first few of them in source order, program before library, behind the statement that it is reported at.
    Vector<uint32_t> independentRoots;
    for (uint32_t vertex : graph.successorsOf(DependencyGraph::root)) {
        if (rootNamedBy(vertex) != none)
            independentRoots.append(vertex);
    }
    AOT::sort(independentRoots, [&](uint32_t a, uint32_t b) {
        uint32_t first = rootNamedBy(a);
        uint32_t second = rootNamedBy(b);
        if (isInLibrary(first) != isInLibrary(second))
            return isInLibrary(second);
        return first != second ? isLowerWitness(m_roots[first].witness, m_roots[second].witness) : a < b;
    });
    constexpr unsigned numberOfOtherOrigins = std::tuple_size_v<decltype(Wire::OtherOriginsRecord::locations)>;
    using RootList = std::array<uint32_t, 1 + numberOfOtherOrigins>;
    RootList noRoots;
    noRoots.fill(none);
    Vector<RootList> firstRoots(FillWith { }, graph.numberOfVertices(), noRoots);
    Vector<uint32_t> worklist;
    for (uint32_t start : independentRoots) {
        auto list = [&, root = rootNamedBy(start)](uint32_t vertex) {
            for (uint32_t& listed : firstRoots[vertex]) {
                if (listed == none) {
                    listed = root;
                    worklist.append(vertex);
                    return;
                }
                if (listed == root)
                    return;
            }
        };
        list(start);
        while (!worklist.isEmpty()) {
            for (uint32_t successor : graph.successorsOf(worklist.takeLast()))
                list(successor);
        }
    }

    // In a large program most roots reach most vertices: more than a record lists, and the first of them in source order has nothing to do with what is asked about. There the statement that stands nearest on a way to the vertex, the one that passed it on, goes first.
    // A statement of the library is looked through while one of the program leads there. Of two that stand equally near, the one that depends on nothing is where it starts.
    Vector<uint32_t> nearestRoot(FillWith { }, graph.numberOfVertices(), none);
    BitVector isIndependent(graph.numberOfVertices());
    for (uint32_t vertex : independentRoots)
        isIndependent.quickSet(vertex);
    for (bool namesLibrary : { false, true }) {
        Vector<uint32_t> frontier;
        for (uint32_t vertex = firstSeedVertex; vertex < firstObjectVertex; ++vertex) {
            if (firstRoots[vertex][0] != none && nearestRoot[vertex] == none && isInLibrary(rootNamedBy(vertex)) == namesLibrary)
                frontier.append(vertex);
        }
        AOT::sort(frontier, [&](uint32_t a, uint32_t b) {
            if (isIndependent.quickGet(a) != isIndependent.quickGet(b))
                return isIndependent.quickGet(a);
            uint32_t first = rootNamedBy(a);
            uint32_t second = rootNamedBy(b);
            return first != second ? isLowerWitness(m_roots[first].witness, m_roots[second].witness) : a < b;
        });
        for (uint32_t vertex : frontier)
            nearestRoot[vertex] = rootNamedBy(vertex);
        for (Vector<uint32_t> next; !frontier.isEmpty(); frontier = std::exchange(next, { })) {
            for (uint32_t vertex : frontier) {
                for (uint32_t successor : graph.successorsOf(vertex)) {
                    if (nearestRoot[successor] != none)
                        continue;
                    nearestRoot[successor] = nearestRoot[vertex];
                    next.append(successor);
                }
            }
        }
    }
    auto rootsNearestFirst = [&](uint32_t vertex) {
        RootList roots = firstRoots[vertex];
        uint32_t nearest = nearestRoot[vertex];
        // A list with room left names every root there is, and each of them is where something starts: the nearest statement may only pass on what they let out of sight.
        if (nearest == none || roots[0] == nearest || roots.back() == none)
            return roots;
        auto end = std::ranges::find(roots, nearest);
        std::shift_right(roots.begin(), end == roots.end() ? end : end + 1, 1);
        roots[0] = nearest;
        return roots;
    };

    struct VertexWithSeveralRoots {
        RootList roots;
        uint16_t* otherOrigins;
    };
    Vector<VertexWithSeveralRoots> verticesWithSeveralRoots;
    m_otherOriginsOfObject.fill(0, numberOfObjects);
    m_otherOriginsOfNode.fill(0, numberOfNodes);
    auto rootOf = [&](uint32_t vertex, uint16_t& otherOrigins) {
        if (onlyRoot[vertex] != none)
            return onlyRoot[vertex];
        RootList roots = rootsNearestFirst(vertex);
        if (roots[1] != none)
            verticesWithSeveralRoots.append({ roots, &otherOrigins });
        return roots[0];
    };
    for (uint32_t object = 0; object < numberOfObjects; ++object) {
        uint32_t vertex = firstObjectVertex + object;
        uint32_t root = rootOf(vertex, m_otherOriginsOfObject[object]);
        if (root == none)
            continue;
        m_originOfObject[object] = m_roots[root].witness;
        if (onlyRoot[vertex] == none)
            m_hasOtherOrigins.quickSet(object);
    }
    for (uint32_t node = 0; node < numberOfNodes; ++node) {
        uint32_t vertex = firstNodeVertex + node;
        m_rootOfNode[node] = rootOf(vertex, m_otherOriginsOfNode[node]);
        if (m_rootOfNode[node] != none && onlyRoot[vertex] == none)
            m_hasSeveralRoots.quickSet(node);
    }
    AOT::sort(verticesWithSeveralRoots, { }, &VertexWithSeveralRoots::roots);
    const RootList* last = nullptr;
    for (const VertexWithSeveralRoots& vertex : verticesWithSeveralRoots) {
        bool isNew = !last || !std::ranges::equal(std::span(*last).subspan(1), std::span(vertex.roots).subspan(1));
        if (isNew && m_otherOrigins.size() == UINT16_MAX)
            break;
        if (isNew) {
            Wire::OtherOriginsRecord record;
            for (unsigned index = 0; index < numberOfOtherOrigins && vertex.roots[1 + index] != none; ++index)
                record.locations[index] = m_roots[vertex.roots[1 + index]].witness.location;
            m_otherOrigins.append(record);
        }
        last = &vertex.roots;
        *vertex.otherOrigins = m_otherOrigins.size();
    }
    for (const SeedThroughObjects& seed : seedsThroughObjects) {
        if (m_rootOfNode[seed.node] != none)
            addSource(m_rootOfNode[seed.node], seed.source);
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
            solver.forEachPredecessorIncludingRemoved(ValueFlowNodeID(static_cast<uint32_t>(index)), [&](ValueFlowNodeID predecessor) {
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
