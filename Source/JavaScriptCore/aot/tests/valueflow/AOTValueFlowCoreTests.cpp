/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTThreads.h"
#include "AOTValueFlowGraph.h"
#include "AOTValueFlowSolver.h"
#include "AOTValueSets.h"
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT { namespace Typed {

std::span<const NativeTest> valueFlowCoreTests();

namespace {

class PlainSet {
public:
    explicit PlainSet(uint32_t sizeOfUniverse)
        : m_sizeOfUniverse(sizeOfUniverse)
    {
        m_bits.ensureSize(sizeOfUniverse);
    }

    bool contains(uint32_t member) const { return m_bits.quickGet(member); }
    bool add(uint32_t member) { return !m_bits.quickSet(member); }
    uint32_t size() const { return m_bits.bitCount(); }
    uint32_t sizeOfUniverse() const { return m_sizeOfUniverse; }

    bool unite(const PlainSet& other, const PlainSet* mask = nullptr)
    {
        bool hasGrown = false;
        for (uint32_t member = 0; member < sizeOfUniverse(); ++member) {
            if (other.contains(member) && (!mask || mask->contains(member)))
                hasGrown |= add(member);
        }
        return hasGrown;
    }

    friend bool operator==(const PlainSet& first, const PlainSet& second) { return first.m_bits == second.m_bits; }

private:
    uint32_t m_sizeOfUniverse;
    BitVector m_bits;
};

} // anonymous namespace

static bool holdsTheSame(const ValueSets& sets, ValueSetID set, const PlainSet& plain)
{
    if (sets.size(set) != plain.size())
        return false;
    for (uint32_t member = 0; member < plain.sizeOfUniverse(); ++member) {
        if (sets.contains(set, ValueFlowObjectID(member)) != plain.contains(member))
            return false;
    }
    uint32_t listed = 0;
    uint32_t before = 0;
    bool isAscending = true;
    sets.forEachMember(set, [&](ValueFlowObjectID member) {
        isAscending &= !listed || member.index() > before;
        isAscending &= plain.contains(member.index());
        before = member.index();
        ++listed;
    });
    return isAscending && listed == plain.size();
}

static std::pair<ValueSetID, PlainSet> makeRandomSet(ValueSetMaker& maker, WeakRandom& random, uint32_t sizeOfUniverse, uint32_t mostMembers)
{
    PlainSet plain(sizeOfUniverse);
    Vector<ValueFlowObjectID> members;
    for (uint32_t count = random.getUint32(mostMembers + 1); count; --count) {
        uint32_t member = random.getUint32(sizeOfUniverse);
        members.append(ValueFlowObjectID(member));
        plain.add(member);
    }
    return { maker.of(members.span()), plain };
}

static void valueSetsAgreeWithPlainSets()
{
    constexpr uint32_t sizeOfUniverse = 3 * ValueSets::membersPerBlock + 77;
    WeakRandom random(11);
    ValueSets sets(sizeOfUniverse);
    ValueSetMaker maker(sets);

    Vector<std::pair<ValueSetID, PlainSet>> pool;
    pool.append({ ValueSets::nothing, PlainSet(sizeOfUniverse) });
    PlainSet everything(sizeOfUniverse);
    for (uint32_t member = 0; member < sizeOfUniverse; ++member)
        everything.add(member);
    pool.append({ ValueSets::anything, everything });
    for (unsigned index = 0; index < 60; ++index)
        pool.append(makeRandomSet(maker, random, sizeOfUniverse, index % 3 ? 6 : 900));
    for (auto& [set, plain] : pool)
        AOT_TEST_CHECK(holdsTheSame(sets, set, plain));

    for (unsigned round = 0; round < 3000; ++round) {
        auto& [first, firstPlain] = pool[random.getUint32(pool.size())];
        auto& [second, secondPlain] = pool[random.getUint32(pool.size())];
        PlainSet expected(sizeOfUniverse);
        ValueSetID result;
        bool shareMember = false;
        bool isSubset = true;
        for (uint32_t member = 0; member < sizeOfUniverse; ++member) {
            shareMember |= firstPlain.contains(member) && secondPlain.contains(member);
            isSubset &= !firstPlain.contains(member) || secondPlain.contains(member);
        }
        AOT_TEST_CHECK(sets.meet(first, second) == shareMember);
        AOT_TEST_CHECK(sets.isSubset(first, second) == isSubset);
        switch (random.getUint32(3)) {
        case 0:
            result = maker.unite(first, second);
            expected.unite(firstPlain);
            expected.unite(secondPlain);
            break;
        case 1:
            result = maker.intersect(first, second);
            expected.unite(firstPlain, &secondPlain);
            break;
        default:
            result = ValueSets::complement(first);
            for (uint32_t member = 0; member < sizeOfUniverse; ++member) {
                if (!firstPlain.contains(member))
                    expected.add(member);
            }
            break;
        }
        AOT_TEST_CHECK(holdsTheSame(sets, result, expected));
        for (auto& [other, otherPlain] : pool) {
            if (ValueSets::isComplement(other) == ValueSets::isComplement(result))
                AOT_TEST_CHECK((other == result) == (otherPlain == expected));
            if (otherPlain == expected && ValueSets::isComplement(other) == ValueSets::isComplement(result))
                AOT_TEST_CHECK(sets.hashOfMembers(other) == sets.hashOfMembers(result));
        }
        if (pool.size() < 300)
            pool.append({ result, expected });
    }

    Vector<ValueSetID> many;
    PlainSet unionOfMany(sizeOfUniverse);
    for (auto& [set, plain] : pool) {
        if (ValueSets::isComplement(set) || many.size() == 40)
            continue;
        many.append(set);
        unionOfMany.unite(plain);
    }
    AOT_TEST_CHECK(holdsTheSame(sets, maker.uniteAll(many.span()), unionOfMany));

    ValueSetID single = maker.of(ValueFlowObjectID(4097u));
    AOT_TEST_CHECK(sets.onlyMember(single) == ValueFlowObjectID(4097u));
    AOT_TEST_CHECK(!sets.onlyMember(ValueSets::nothing));
    AOT_TEST_CHECK(!sets.onlyMember(maker.unite(single, maker.of(ValueFlowObjectID(1u)))));
}

static void valueSetsAreTheSameFromEveryThread()
{
    constexpr uint32_t sizeOfUniverse = 5 * ValueSets::membersPerBlock;
    constexpr unsigned numberOfThreads = 8;
    constexpr unsigned numberOfSets = 4000;
    ParallelLoops loops(numberOfThreads);
    ValueSets sets(sizeOfUniverse);
    Vector<Vector<ValueSetID>> made(numberOfThreads);
    forEachIndexInParallel(loops, numberOfThreads, [&](unsigned thread) {
        ValueSetMaker maker(sets);
        WeakRandom random(5);
        ValueSetID growing = ValueSets::nothing;
        for (unsigned index = 0; index < numberOfSets; ++index) {
            growing = maker.unite(growing, maker.of(ValueFlowObjectID(random.getUint32(sizeOfUniverse))));
            made[thread].append(index % 7 ? growing : maker.intersect(growing, ValueSets::complement(maker.of(ValueFlowObjectID(random.getUint32(sizeOfUniverse))))));
        }
    });
    for (unsigned thread = 1; thread < numberOfThreads; ++thread)
        AOT_TEST_CHECK(made[thread] == made[0]);
    AOT_TEST_CHECK(sets.size(made[0].last()) > numberOfSets / 2);
}

static void checkGraphAgainstPlainWorklist(unsigned numberOfThreads, unsigned seed)
{
    constexpr uint32_t sizeOfUniverse = 200;
    WeakRandom random(seed);
    ParallelLoops loops(numberOfThreads);
    ValueSets sets(sizeOfUniverse);
    Vector<std::unique_ptr<ValueSetMaker>> makers(numberOfThreads, [&](size_t) {
        return makeUnique<ValueSetMaker>(sets);
    });
    ValueFlowGraph graph(loops, makers.span());

    Vector<PlainSet> values;
    Vector<std::optional<PlainSet>> masks;
    Vector<bool> isConstant;
    Vector<ValueFlowEdge> allEdges;
    Vector<ValueFlowNodeID> moved;

    unsigned numberOfBatches = 1 + random.getUint32(6);
    for (unsigned batch = 0; batch < numberOfBatches; ++batch) {
        uint32_t numberOfNewNodes = 1 + random.getUint32(batch ? 12 : 70);
        ValueFlowNodeID first = graph.addNodes(numberOfNewNodes);
        for (uint32_t offset = 0; offset < numberOfNewNodes; ++offset) {
            ValueFlowNodeID node(first.index() + offset);
            values.append(PlainSet(sizeOfUniverse));
            masks.append(std::nullopt);
            isConstant.append(false);
            unsigned kind = random.getUint32(10);
            if (kind < 3) {
                auto [set, plain] = makeRandomSet(*makers[0], random, sizeOfUniverse, 4);
                graph.makeConstant(node, set);
                values.last() = plain;
                isConstant.last() = true;
            } else if (kind < 5) {
                auto [set, plain] = makeRandomSet(*makers[0], random, sizeOfUniverse, 120);
                bool isComplement = random.getUint32(2);
                graph.setMask(node, isComplement ? ValueSets::complement(set) : set);
                PlainSet mask(sizeOfUniverse);
                for (uint32_t member = 0; member < sizeOfUniverse; ++member) {
                    if (plain.contains(member) != isComplement)
                        mask.add(member);
                }
                masks.last() = mask;
            }
        }

        Vector<ValueFlowEdge> edges;
        for (uint32_t count = random.getUint32(batch ? 30 : 200); count; --count) {
            ValueFlowEdge edge { ValueFlowNodeID(random.getUint32(values.size())), ValueFlowNodeID(random.getUint32(values.size())) };
            if (isConstant[edge.to.index()])
                continue;
            edges.append(edge);
            allEdges.append(edge);
        }
        if (batch)
            graph.addEdges(edges.span());
        else
            graph.addInitialEdges(edges.span());
        Vector<PlainSet> before = values;
        graph.solve(moved);

        for (bool hasGrown = true; hasGrown;) {
            hasGrown = false;
            for (ValueFlowEdge edge : allEdges)
                hasGrown |= values[edge.to.index()].unite(values[edge.from.index()], masks[edge.to.index()] ? &*masks[edge.to.index()] : nullptr);
        }
        size_t nextMoved = 0;
        for (uint32_t node = 0; node < values.size(); ++node) {
            AOT_TEST_CHECK(holdsTheSame(sets, graph.valueOf(ValueFlowNodeID(node)), values[node]));
            bool hasMoved = nextMoved < moved.size() && moved[nextMoved].index() == node;
            nextMoved += hasMoved;
            AOT_TEST_CHECK(hasMoved == !(before[node] == values[node]));
        }
        AOT_TEST_CHECK(nextMoved == moved.size());
    }
}

static void valueFlowGraphFindsTheLeastSolution()
{
    for (unsigned seed = 1; seed <= 150; ++seed)
        checkGraphAgainstPlainWorklist(seed % 3 ? 1 : 4, seed);
}

static void maskMadeFromTypeTakesItsObjectsInPlaceOfTheStandIn()
{
    ParallelLoops loops(1);
    ValueSets sets(16);
    Vector<std::unique_ptr<ValueSetMaker>> makers(1, [&](size_t) {
        return makeUnique<ValueSetMaker>(sets);
    });
    ValueSetMaker& maker = *makers[0];
    auto setOf = [&](std::initializer_list<uint32_t> members) {
        Vector<ValueFlowObjectID> list;
        for (uint32_t member : members)
            list.append(ValueFlowObjectID(member));
        return maker.of(list.span());
    };

    // Member 1 stands for objects nobody follows. Masks made from types list the objects 2 to 7; 8 and above are objects of other kinds; 0 is no object.
    ValueFlowStandIn standIn { ValueFlowObjectID(1u), setOf({ 2, 3, 4, 5, 6, 7 }) };
    ValueSetID listed = setOf({ 0, 2, 3, 9 });
    AOT_TEST_CHECK(maker.pass(setOf({ 1, 5 }), listed, standIn) == setOf({ 2, 3 }));
    AOT_TEST_CHECK(maker.pass(setOf({ 0, 1, 2 }), listed, standIn) == setOf({ 0, 2, 3 }));
    AOT_TEST_CHECK(maker.pass(setOf({ 1, 9 }), listed, standIn) == setOf({ 2, 3, 9 }));
    AOT_TEST_CHECK(maker.pass(setOf({ 2, 5 }), listed, standIn) == setOf({ 2 }));
    AOT_TEST_CHECK(maker.pass(setOf({ 1, 5 }), setOf({ 0 }), standIn) == ValueSets::nothing);
    AOT_TEST_CHECK(maker.pass(setOf({ 1, 8 }), setOf({ 8, 9 }), standIn) == setOf({ 8 }));
    AOT_TEST_CHECK(maker.pass(setOf({ 1, 2, 5 }), setOf({ 1, 2, 3 }), standIn) == setOf({ 1, 2 }));
    AOT_TEST_CHECK(maker.pass(setOf({ 1, 2, 5 }), ValueSets::complement(setOf({ 2 })), standIn) == setOf({ 1, 5 }));
    AOT_TEST_CHECK(maker.pass(setOf({ 1, 5 }), ValueSets::anything, standIn) == setOf({ 1, 5 }));
    AOT_TEST_CHECK(maker.pass(setOf({ 1, 5 }), listed, ValueFlowStandIn()) == ValueSets::nothing);

    // followed -> typed <-> copy, followed -> plain. The stand-in comes to `typed` and to `plain` with edges that are added after the first solve.
    ValueFlowGraph graph(loops, makers.span());
    graph.setStandIn(standIn);
    ValueFlowNodeID followed = graph.addNodes(5);
    ValueFlowNodeID unfollowed(followed.index() + 1);
    ValueFlowNodeID typed(followed.index() + 2);
    ValueFlowNodeID copy(followed.index() + 3);
    ValueFlowNodeID plain(followed.index() + 4);
    graph.makeConstant(followed, setOf({ 2, 5 }));
    graph.makeConstant(unfollowed, setOf({ 1 }));
    graph.setMask(typed, listed);
    graph.setMask(plain, listed, ValueFlowGraph::MaskKind::Plain);
    ValueFlowEdge edges[] = { { followed, typed }, { typed, copy }, { copy, typed }, { followed, plain } };
    graph.addInitialEdges(edges);
    Vector<ValueFlowNodeID> moved;
    graph.solve(moved);
    AOT_TEST_CHECK(graph.valueOf(typed) == setOf({ 2 }));
    AOT_TEST_CHECK(graph.valueOf(copy) == setOf({ 2 }));
    AOT_TEST_CHECK(graph.valueOf(plain) == setOf({ 2 }));

    ValueFlowEdge late[] = { { unfollowed, typed }, { unfollowed, plain } };
    AOT_TEST_CHECK(graph.addEdges(late) == 2);
    graph.solve(moved);
    AOT_TEST_CHECK(graph.valueOf(typed) == setOf({ 2, 3 }));
    AOT_TEST_CHECK(graph.valueOf(copy) == setOf({ 2, 3 }));
    AOT_TEST_CHECK(graph.valueOf(plain) == setOf({ 2 }));
}

static void valueFlowGraphSolvesLongChainsAndWideLevelsOnThreads()
{
    constexpr uint32_t width = 3000;
    constexpr uint32_t depth = 40;
    ParallelLoops loops(8);
    ValueSets sets(width + 1);
    Vector<std::unique_ptr<ValueSetMaker>> makers(8, [&](size_t) {
        return makeUnique<ValueSetMaker>(sets);
    });
    ValueFlowGraph graph(loops, makers.span());
    ValueFlowNodeID constants = graph.addNodes(width);
    ValueFlowNodeID grid = graph.addNodes(width * depth);
    Vector<ValueFlowEdge> edges;
    for (uint32_t column = 0; column < width; ++column) {
        graph.makeConstant(ValueFlowNodeID(constants.index() + column), makers[0]->of(ValueFlowObjectID(column)));
        edges.append(ValueFlowEdge { ValueFlowNodeID(constants.index() + column), ValueFlowNodeID(grid.index() + column) });
        for (uint32_t row = 1; row < depth; ++row) {
            edges.append(ValueFlowEdge { ValueFlowNodeID(grid.index() + (row - 1) * width + column), ValueFlowNodeID(grid.index() + row * width + column) });
            edges.append(ValueFlowEdge { ValueFlowNodeID(grid.index() + (row - 1) * width + (column + 1) % width), ValueFlowNodeID(grid.index() + row * width + column) });
        }
    }
    graph.addInitialEdges(edges.span());
    Vector<ValueFlowNodeID> moved;
    graph.solve(moved);
    AOT_TEST_CHECK(moved.size() == width * depth);
    for (uint32_t row = 0; row < depth; ++row) {
        for (uint32_t column = 0; column < width; column += 97) {
            ValueSetID value = graph.valueOf(ValueFlowNodeID(grid.index() + row * width + column));
            AOT_TEST_CHECK(sets.size(value) == row + 1);
            AOT_TEST_CHECK(sets.contains(value, ValueFlowObjectID((column + row) % width)));
        }
    }

    // An edge from the last row back to the first closes a cycle through everything.
    ValueFlowEdge back { ValueFlowNodeID(grid.index() + (depth - 1) * width), ValueFlowNodeID(grid.index()) };
    AOT_TEST_CHECK(graph.addEdges(singleElementSpan(back)) == 1);
    AOT_TEST_CHECK(!graph.addEdges(singleElementSpan(back)));
    graph.solve(moved);
    AOT_TEST_CHECK(sets.size(graph.valueOf(ValueFlowNodeID(grid.index()))) == depth);
}

// A small language for the rounds: objects have properties, the first objects are closures of functions with one parameter and one result.
namespace {

enum class ToyOperation : uint32_t { Allocate, Load, Store, Call, TakeStoredInto, TakeFunctions, LoadEveryProperty, StoreUnderUnknownKey };

struct ToyProgram {
    uint32_t numberOfFunctions { 0 };
    uint32_t numberOfObjects { 0 };
    uint32_t numberOfKeys { 0 };
    uint32_t nodesPerFunction { 0 };
    Vector<ValueFlowEdge> edges;
    Vector<ValueFlowStatement> statements;
    Vector<ValueFlowOperand> operands;
    Vector<ValueFlowBodyRange> ranges;
    Vector<uint32_t> payloads;

    ValueFlowNodeID node(uint32_t function, uint32_t index) const { return ValueFlowNodeID(function * nodesPerFunction + index); }
    ValueFlowNodeID parameter(uint32_t function) const { return node(function, 0); }
    ValueFlowNodeID result(uint32_t function) const { return node(function, 1); }
    uint32_t numberOfNodes() const { return (numberOfFunctions + 1) * nodesPerFunction; }
};

const ToyProgram* toyProgram;
ValueSetID toyFunctions;

} // anonymous namespace

static void runToyRule(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ToyProgram& program = *toyProgram;
    if (statement.kind == ValueFlowStatementKind::CallGroup) {
        ValueFlowCallGroup group = context.group(context.setOfKey(), context.callKindOfKey());
        context.forEachMember(context.setOfKey(), [&](ValueFlowObjectID callee) {
            if (callee.index() >= program.numberOfFunctions)
                return;
            context.reach(FunctionID(callee.index()));
            context.connect(group.argument(0), program.parameter(callee.index()));
            context.connect(program.result(callee.index()), group.result);
        });
        return;
    }

    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    uint32_t payload = program.payloads[context.statement().index()];
    switch (static_cast<ToyOperation>(statement.site.index())) {
    case ToyOperation::Allocate:
        context.add(statement.result, context.maker().of(ValueFlowObjectID(payload)));
        return;
    case ToyOperation::Load:
        context.connect(context.read(context.valueOf(operands[0]), ValueFlowName::property(PropertyKeyID(payload))), statement.result);
        return;
    case ToyOperation::LoadEveryProperty:
        context.connect(context.read(context.valueOf(operands[0]), ValueFlowName { ValueFlowNameKind::EveryProperty, 0 }), statement.result);
        return;
    case ToyOperation::StoreUnderUnknownKey:
        context.connect(operands[1], context.written(context.valueOf(operands[0]), ValueFlowName { ValueFlowNameKind::UnknownKey, 0 }));
        return;
    case ToyOperation::TakeStoredInto:
        // The operand's objects that something has been stored into.
        context.add(statement.result, context.markedAmong(context.valueOf(operands[0]), ObjectMutation::StoreAfterCreation));
        return;
    case ToyOperation::TakeFunctions:
        context.add(statement.result, context.valueWithin(ClimbingSet(toyFunctions), operands[0].node));
        return;
    case ToyOperation::Store:
        context.connect(operands[1], context.written(context.valueOf(operands[0]), ValueFlowName::property(PropertyKeyID(payload))));
        context.mark(context.valueOf(operands[0]), ObjectMutation::StoreAfterCreation);
        return;
    case ToyOperation::Call: {
        ValueFlowCallGroup group = context.group(context.valueOf(operands[0]), ValueFlowCallKind::Call);
        context.connect(operands[1], group.argument(0));
        context.connect(group.result, statement.result);
        return;
    }
    }
}

static void makeToyFunctions(ValueSets& sets, const ToyProgram& program)
{
    ValueSetMaker maker(sets);
    toyFunctions = maker.ofRange(ValueFlowObjectID(0u), ValueFlowObjectID(program.numberOfFunctions));
}

static ToyProgram makeToyProgram(WeakRandom& random)
{
    ToyProgram program;
    program.numberOfFunctions = 2 + random.getUint32(12);
    program.numberOfObjects = program.numberOfFunctions + 1 + random.getUint32(20);
    program.numberOfKeys = 1 + random.getUint32(4);
    program.nodesPerFunction = 4 + random.getUint32(8);
    for (uint32_t body = 0; body <= program.numberOfFunctions; ++body) {
        auto anyNode = [&] {
            return program.node(body, random.getUint32(program.nodesPerFunction));
        };
        for (uint32_t count = random.getUint32(program.nodesPerFunction); count; --count)
            program.edges.append(ValueFlowEdge { anyNode(), anyNode() });

        ValueFlowBodyRange range;
        range.statement = program.statements.size();
        auto addStatement = [&](ToyOperation operation, uint32_t payload, unsigned numberOfOperands) {
            ValueFlowStatement statement;
            statement.function = FunctionID(body);
            statement.site = ValueID(static_cast<uint32_t>(operation));
            statement.firstOperand = program.operands.size();
            statement.operandCount = numberOfOperands;
            statement.result = anyNode();
            for (unsigned operand = 0; operand < numberOfOperands; ++operand)
                program.operands.append(ValueFlowOperand { anyNode(), { }, { } });
            program.statements.append(statement);
            program.payloads.append(payload);
        };
        for (uint32_t count = random.getUint32(4); count; --count)
            addStatement(ToyOperation::Load, random.getUint32(program.numberOfKeys), 1);
        if (!random.getUint32(3))
            addStatement(ToyOperation::LoadEveryProperty, 0, 1);
        if (!random.getUint32(3))
            addStatement(ToyOperation::TakeStoredInto, 0, 1);
        if (!random.getUint32(3))
            addStatement(ToyOperation::TakeFunctions, 0, 1);
        range.statementLeavingBody = program.statements.size();
        for (uint32_t count = 1 + random.getUint32(6); count; --count) {
            switch (random.getUint32(4)) {
            case 0:
                addStatement(ToyOperation::Allocate, random.getUint32(program.numberOfObjects), 0);
                break;
            case 2:
                addStatement(random.getUint32(3) ? ToyOperation::Allocate : ToyOperation::StoreUnderUnknownKey, random.getUint32(program.numberOfObjects), 2);
                break;
            case 1:
                addStatement(ToyOperation::Store, random.getUint32(program.numberOfKeys), 2);
                break;
            default:
                addStatement(ToyOperation::Call, 0, 2);
                break;
            }
        }
        program.ranges.append(range);
    }
    ValueFlowBodyRange end;
    end.statement = end.statementLeavingBody = program.statements.size();
    program.ranges.append(end);
    return program;
}

namespace {

struct ToySolution {
    Vector<PlainSet> values;
    Vector<bool> isReached;
    Vector<bool> isStoredInto;
};

} // anonymous namespace

// One set per (object, key), one step at a time, until nothing moves.
static ToySolution solveToyProgramObjectByObject(const ToyProgram& program)
{
    ToySolution solution;
    solution.values.fill(PlainSet(program.numberOfObjects), program.numberOfNodes());
    solution.isReached.fill(false, program.numberOfFunctions + 1);
    solution.isReached[program.numberOfFunctions] = true;
    solution.isStoredInto.fill(false, program.numberOfObjects);
    Vector<PlainSet> fields;
    fields.fill(PlainSet(program.numberOfObjects), program.numberOfObjects * program.numberOfKeys);
    Vector<PlainSet> storedUnderUnknownKey;
    storedUnderUnknownKey.fill(PlainSet(program.numberOfObjects), program.numberOfObjects);

    auto& values = solution.values;
    for (bool hasMoved = true; hasMoved;) {
        hasMoved = false;
        for (ValueFlowEdge edge : program.edges)
            hasMoved |= values[edge.to.index()].unite(values[edge.from.index()]);
        for (uint32_t body = 0; body <= program.numberOfFunctions; ++body) {
            uint32_t end = solution.isReached[body] ? program.ranges[body + 1].statement : program.ranges[body].statementLeavingBody;
            for (uint32_t index = program.ranges[body].statement; index < end; ++index) {
                const ValueFlowStatement& statement = program.statements[index];
                auto operand = [&](unsigned number) -> PlainSet& {
                    return values[program.operands[statement.firstOperand + number].node.index()];
                };
                PlainSet& result = values[statement.result.index()];
                uint32_t payload = program.payloads[index];
                switch (static_cast<ToyOperation>(statement.site.index())) {
                case ToyOperation::Allocate:
                    hasMoved |= result.add(payload);
                    break;
                case ToyOperation::Load:
                    for (uint32_t object = 0; object < program.numberOfObjects; ++object) {
                        if (!operand(0).contains(object))
                            continue;
                        hasMoved |= result.unite(fields[object * program.numberOfKeys + payload]);
                        hasMoved |= result.unite(storedUnderUnknownKey[object]);
                    }
                    break;
                case ToyOperation::LoadEveryProperty:
                    for (uint32_t object = 0; object < program.numberOfObjects; ++object) {
                        if (!operand(0).contains(object))
                            continue;
                        for (uint32_t key = 0; key < program.numberOfKeys; ++key)
                            hasMoved |= result.unite(fields[object * program.numberOfKeys + key]);
                        hasMoved |= result.unite(storedUnderUnknownKey[object]);
                    }
                    break;
                case ToyOperation::StoreUnderUnknownKey:
                    for (uint32_t object = 0; object < program.numberOfObjects; ++object) {
                        if (operand(0).contains(object))
                            hasMoved |= storedUnderUnknownKey[object].unite(operand(1));
                    }
                    break;
                case ToyOperation::TakeStoredInto:
                    for (uint32_t object = 0; object < program.numberOfObjects; ++object) {
                        if (operand(0).contains(object) && solution.isStoredInto[object])
                            hasMoved |= result.add(object);
                    }
                    break;
                case ToyOperation::TakeFunctions:
                    for (uint32_t object = 0; object < program.numberOfFunctions; ++object) {
                        if (operand(0).contains(object))
                            hasMoved |= result.add(object);
                    }
                    break;
                case ToyOperation::Store:
                    for (uint32_t object = 0; object < program.numberOfObjects; ++object) {
                        if (!operand(0).contains(object))
                            continue;
                        hasMoved |= fields[object * program.numberOfKeys + payload].unite(operand(1));
                        hasMoved |= !std::exchange(solution.isStoredInto[object], true);
                    }
                    break;
                case ToyOperation::Call:
                    for (uint32_t callee = 0; callee < program.numberOfFunctions; ++callee) {
                        if (!operand(0).contains(callee))
                            continue;
                        hasMoved |= !std::exchange(solution.isReached[callee], true);
                        hasMoved |= values[program.parameter(callee).index()].unite(operand(1));
                        hasMoved |= result.unite(values[program.result(callee).index()]);
                    }
                    break;
                }
            }
        }
    }
    return solution;
}

static void valueFlowRoundsAgreeWithAnAnalysisObjectByObject()
{
    for (unsigned seed = 1; seed <= 120; ++seed) {
        WeakRandom random(seed);
        ToyProgram program = makeToyProgram(random);
        toyProgram = &program;
        ToySolution expected = solveToyProgramObjectByObject(program);

        ValueFlowSolverInput input;
        input.numberOfNodes = program.numberOfNodes();
        input.edges = program.edges.span();
        input.statements = program.statements.span();
        input.operands = program.operands.span();
        input.ranges = program.ranges.span();
        input.remembersWhoLoggedEachEdge = true;

        struct Way {
            unsigned numberOfThreads;
            ValueFlowSolver::Rounds rounds;
        };
        for (Way way : { Way { 1, ValueFlowSolver::Rounds::Due }, Way { 4, ValueFlowSolver::Rounds::Due }, Way { 3, ValueFlowSolver::Rounds::Full } }) {
            ParallelLoops loops(way.numberOfThreads);
            ValueSets sets(program.numberOfObjects);
            makeToyFunctions(sets, program);
            ValueFlowSolver solver(input, sets, runToyRule, loops, way.rounds);
            AOT_TEST_CHECK(solver.solve());
            for (uint32_t node = 0; node < program.numberOfNodes(); ++node)
                AOT_TEST_CHECK(holdsTheSame(sets, solver.valueOf(ValueFlowNodeID(node)), expected.values[node]));
            for (uint32_t function = 0; function < program.numberOfFunctions; ++function)
                AOT_TEST_CHECK(solver.isReached(FunctionID(function)) == expected.isReached[function]);
            for (uint32_t object = 0; object < program.numberOfObjects; ++object)
                AOT_TEST_CHECK(solver.marksOf(ValueFlowObjectID(object)).contains(ObjectMutation::StoreAfterCreation) == expected.isStoredInto[object]);
            AOT_TEST_CHECK(solver.counters().fullRounds >= 1);

            // Every function is reached by a call group that holds its closure, and every member of a set can be followed back to where it was made.
            for (uint32_t function = 0; function < program.numberOfFunctions; ++function) {
                if (!solver.isReached(FunctionID(function)))
                    continue;
                ValueFlowStatementID witness = solver.reachedBy(FunctionID(function)).statement;
                AOT_TEST_CHECK(witness && solver.statementAt(witness).kind == ValueFlowStatementKind::CallGroup);
                AOT_TEST_CHECK(sets.contains(solver.keyOf(witness).set, ValueFlowObjectID(function)));
            }
            for (uint32_t node = 0; node < program.numberOfNodes(); node += 3) {
                sets.forEachMember(solver.valueOf(ValueFlowNodeID(node)), [&](ValueFlowObjectID member) {
                    Vector<ValueFlowNodeID> way = solver.explain(ValueFlowNodeID(node), member);
                    AOT_TEST_CHECK(way.size() >= 2 && way[0] == ValueFlowNodeID(node));
                    for (ValueFlowNodeID step : way)
                        AOT_TEST_CHECK(sets.contains(solver.valueOf(step), member));
                    const ValueFlowNodeKey* key = solver.keyOfMadeNode(way.last());
                    AOT_TEST_CHECK(key && key->kind == ValueFlowMadeNodeKind::Constant);
                    ValueFlowStatementID allocation = solver.statementThatLogged(ValueFlowEdge { way.last(), way[way.size() - 2] });
                    AOT_TEST_CHECK(allocation && !solver.isMadeByRule(allocation));
                });
            }
            uint64_t runs = 0;
            uint64_t edgesLogged = 0;
            solver.forEachKindOfStatementRun([&](ValueFlowStatementKind, Opcode, const ValueFlowSolver::RunsAndEdges& counted) {
                runs += counted.runs;
                edgesLogged += counted.edgesLogged;
            });
            AOT_TEST_CHECK(runs == solver.counters().statementRuns && edgesLogged == solver.counters().edgesLogged);
        }
    }
    toyProgram = nullptr;
}

static void valueFlowSolveStopsAtTheLimitOnLoggedEdges()
{
    WeakRandom random(7);
    ToyProgram program = makeToyProgram(random);
    toyProgram = &program;
    ValueFlowSolverInput input;
    input.numberOfNodes = program.numberOfNodes();
    input.edges = program.edges.span();
    input.statements = program.statements.span();
    input.operands = program.operands.span();
    input.ranges = program.ranges.span();

    ParallelLoops loops(2);
    uint64_t edgesLogged;
    {
        ValueSets sets(program.numberOfObjects);
        makeToyFunctions(sets, program);
        ValueFlowSolver solver(input, sets, runToyRule, loops);
        AOT_TEST_CHECK(solver.solve());
        edgesLogged = solver.counters().edgesLogged;
        AOT_TEST_CHECK(edgesLogged > 4);
    }
    for (uint64_t limit : { static_cast<uint64_t>(0), static_cast<uint64_t>(1), edgesLogged / 2, edgesLogged - 1, edgesLogged }) {
        input.mostEdgesLogged = limit;
        ValueSets sets(program.numberOfObjects);
        makeToyFunctions(sets, program);
        ValueFlowSolver solver(input, sets, runToyRule, loops);
        AOT_TEST_CHECK(solver.solve() == (limit == edgesLogged));
    }
    toyProgram = nullptr;
}

#define FOR_EACH_AOT_VALUE_FLOW_CORE_TEST(v) \
    v(valueSetsAgreeWithPlainSets) v(valueSetsAreTheSameFromEveryThread) v(valueFlowGraphFindsTheLeastSolution) \
    v(maskMadeFromTypeTakesItsObjectsInPlaceOfTheStandIn) \
    v(valueFlowGraphSolvesLongChainsAndWideLevelsOnThreads) v(valueFlowRoundsAgreeWithAnAnalysisObjectByObject) \
    v(valueFlowSolveStopsAtTheLimitOnLoggedEdges)

std::span<const NativeTest> valueFlowCoreTests()
{
#define AOT_VALUE_FLOW_CORE_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_VALUE_FLOW_CORE_TEST(AOT_VALUE_FLOW_CORE_TEST_ENTRY) };
#undef AOT_VALUE_FLOW_CORE_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
