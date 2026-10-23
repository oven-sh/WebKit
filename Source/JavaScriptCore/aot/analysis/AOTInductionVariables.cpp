/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInductionVariables.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIntegerRelations.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/CheckedArithmetic.h>
#include <wtf/EnumTraits.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

Opcode mirroredComparison(Opcode comparison)
{
    switch (comparison) {
    case Opcode::Less:
        return Opcode::Greater;
    case Opcode::LessEq:
        return Opcode::GreaterEq;
    case Opcode::Greater:
        return Opcode::Less;
    case Opcode::GreaterEq:
        return Opcode::LessEq;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

Opcode negatedComparison(Opcode comparison)
{
    switch (comparison) {
    case Opcode::Less:
        return Opcode::GreaterEq;
    case Opcode::LessEq:
        return Opcode::Greater;
    case Opcode::Greater:
        return Opcode::LessEq;
    case Opcode::GreaterEq:
        return Opcode::Less;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool isInvariantIn(FunctionAnalyses& analyses, const NaturalLoop& loop, ValueID value)
{
    if (constantThroughCasts(analyses.program(), analyses.function(), value))
        return true;

    ValueLocation location = analyses.useLists().locationOf(analyses.identityOf(value));
    switch (location.placement) {
    case ValuePlacement::Detached:
        return false;
    case ValuePlacement::Parameter:
    case ValuePlacement::InBlock:
        return !analyses.naturalLoops().belongsTo(location.block, loop);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LoopExits exitsOf(FunctionAnalyses& analyses, const NaturalLoop& loop, BlockID testingBlock)
{
    const Function& function = analyses.function();
    const NaturalLoops& loops = analyses.naturalLoops();
    if (!loops.belongsTo(testingBlock, loop))
        return LoopExits::Unknown;

    for (unsigned i = 0; i < loop.size(); ++i) {
        BlockID id = loop.at(i);
        const Block& block = function.at(id);
        if (returnsToCaller(block.terminator.kind))
            return LoopExits::AlsoElsewhere;
        if (id == testingBlock)
            continue;
        for (const Target& target : block.terminator.targets) {
            if (!loops.belongsTo(target.block, loop))
                return LoopExits::AlsoElsewhere;
        }
    }
    return LoopExits::OnlyAtTest;
}

static InductionDirection directionOfAmounts(Range amounts)
{
    ASSERT(!amounts.isEmpty());
    if (amounts.minimum > 0)
        return InductionDirection::Increasing;
    if (amounts.maximum < 0)
        return InductionDirection::Decreasing;
    return InductionDirection::Unknown;
}

static InductionDirection oppositeOf(InductionDirection direction)
{
    switch (direction) {
    case InductionDirection::Unknown:
        return InductionDirection::Unknown;
    case InductionDirection::Increasing:
        return InductionDirection::Decreasing;
    case InductionDirection::Decreasing:
        return InductionDirection::Increasing;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isBoundingRelation(Opcode relation, InductionDirection direction)
{
    switch (direction) {
    case InductionDirection::Unknown:
        return false;
    case InductionDirection::Increasing:
        return relation == Opcode::Less || relation == Opcode::LessEq;
    case InductionDirection::Decreasing:
        return relation == Opcode::Greater || relation == Opcode::GreaterEq;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool dominatesAfterSplit(FunctionAnalyses& analyses, Edge edge, BlockID block)
{
    const CFG& cfg = analyses.cfg();
    if (!cfg.isReachable(edge.from) || !cfg.isReachable(block))
        return false;

    const Dominators& dominators = analyses.dominators();
    BlockID target = targetOf(analyses.function(), edge).block;
    if (!dominators.dominates(target, block))
        return false;
    return std::ranges::all_of(cfg.incomingEdges(target), [&](Edge incoming) {
        return incoming == edge || dominators.dominates(target, incoming.from);
    });
}

namespace {

struct PendingValue {
    ValueID value;
    int64_t accumulated { 0 };
};

struct InductionSteps {
    ValueID parameter;
    Range totals;
    std::optional<InductionStep> invariant;
    bool isUnknown { false };
    Vector<PendingValue, 8> pending;
    UncheckedKeyHashMap<ValueID, int64_t> accumulatedAtParameter;
};

struct TestedVariable {
    InductionVariable* variable { nullptr };
    TestedValue tested { TestedValue::BeforeStep };
};

class InductionFinder {
public:
    InductionFinder(FunctionAnalyses&, const NaturalLoop&);

    LoopInduction find();

private:
    using Variables = Vector<InductionVariable, 2>;
    using EdgeArguments = Vector<ValueID, 2>;

    std::optional<InductionVariable> findVariable(uint32_t parameterIndex);
    std::optional<EdgeArguments> argumentsOn(std::span<const Edge>, uint32_t parameterIndex) const;
    ValueID findStart(std::span<const ValueID> enteringArguments);
    bool haveEqualValues(ValueID start, ValueID argument);

    void collectSteps(ValueID stepped, InductionSteps&);
    void visit(PendingValue, InductionSteps&);
    void visitBlockParameter(ValueID, ValueLocation, int64_t accumulated, InductionSteps&);
    void visitOperation(const Value&, int64_t accumulated, InductionSteps&);
    void visitInvariantStep(const Value& sum, int64_t accumulated, InductionSteps&);
    static void enqueueOperand(ValueID operand, CheckedInt64 accumulated, InductionSteps&);
    static InductionStep stepOf(const InductionSteps&);
    InductionDirection directionOf(const InductionSteps&) const;

    void findTests(Variables&);
    Vector<BlockID, 4> findTestingBlocks() const;
    void findTestsIn(BlockID, Variables&);
    static std::optional<TestedVariable> findTestedVariable(Variables&, ValueID);
    LoopBoundKind boundKindOf(ValueID bound);
    bool isComputedInLoop(ValueID bound);
    EntryRelation entryRelationOf(ValueID start, const InductionTest&);
    IntegerRelations::Answer proveRelation(Opcode relation, ValueID left, ValueID right, BlockID at);
    bool isOnEveryTrip(BlockID testingBlock) const;

    ValueID numericIdentityOf(ValueID);
    std::optional<int64_t> integerConstantOf(ValueID id) const { return integerConstantThroughCasts(m_program, m_function, id); }
    bool isInLoop(BlockID block) const { return m_loops.belongsTo(block, m_loop); }
    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    bool isNeverNaN(ValueID) const;

    FunctionAnalyses& m_analyses;
    const Program& m_program;
    const Function& m_function;
    const CFG& m_cfg;
    const Dominators& m_dominators;
    const NaturalLoops& m_loops;
    const UseLists& m_useLists;
    const NaturalLoop& m_loop;
    IntegerRelations m_relations;
    Vector<Edge, 2> m_enteringEdges;
    Vector<Edge, 2> m_backEdges;
};

InductionFinder::InductionFinder(FunctionAnalyses& analyses, const NaturalLoop& loop)
    : m_analyses(analyses)
    , m_program(analyses.program())
    , m_function(analyses.function())
    , m_cfg(analyses.cfg())
    , m_dominators(analyses.dominators())
    , m_loops(analyses.naturalLoops())
    , m_useLists(analyses.useLists())
    , m_loop(loop)
    , m_relations(analyses)
{
    for (Edge edge : m_cfg.incomingEdges(loop.header()))
        (isInLoop(edge.from) ? m_backEdges : m_enteringEdges).append(edge);
}

LoopInduction InductionFinder::find()
{
    LoopInduction induction { .header = m_loop.header(), .loopIndex = m_loop.index(), .variables = { } };
    size_t numberOfParameters = m_function.at(m_loop.header()).parameters.size();
    for (uint32_t parameterIndex = 0; parameterIndex < numberOfParameters; ++parameterIndex) {
        if (std::optional<InductionVariable> variable = findVariable(parameterIndex))
            induction.variables.append(WTF::move(*variable));
    }
    findTests(induction.variables);
    return induction;
}

std::optional<InductionVariable> InductionFinder::findVariable(uint32_t parameterIndex)
{
    std::optional<EdgeArguments> enteringArguments = argumentsOn(m_enteringEdges.span(), parameterIndex);
    std::optional<EdgeArguments> carriedArguments = argumentsOn(m_backEdges.span(), parameterIndex);
    if (!enteringArguments || !carriedArguments)
        return std::nullopt;

    InductionVariable variable;
    variable.parameter = m_function.at(m_loop.header()).parameters[parameterIndex];
    variable.parameterIndex = parameterIndex;
    variable.start = findStart(enteringArguments->span());

    InductionSteps steps;
    steps.parameter = variable.parameter;
    for (ValueID argument : *carriedArguments) {
        if (!argument)
            return std::nullopt;
        ValueID stepped = m_analyses.identityOf(argument);
        if (variable.steppedValues.contains(stepped))
            continue;
        variable.steppedValues.append(stepped);
        collectSteps(stepped, steps);
        if (steps.isUnknown)
            return std::nullopt;
    }

    bool hasOnlyTotals = !steps.invariant;
    if (hasOnlyTotals && (steps.totals.isEmpty() || steps.totals == Range::one(0)))
        return std::nullopt;

    variable.step = stepOf(steps);
    variable.direction = directionOf(steps);
    return variable;
}

std::optional<InductionFinder::EdgeArguments> InductionFinder::argumentsOn(std::span<const Edge> edges, uint32_t parameterIndex) const
{
    EdgeArguments arguments;
    for (Edge edge : edges) {
        IncomingValue incoming = incomingValueOf(m_function, edge, parameterIndex);
        switch (incoming.kind) {
        case IncomingValue::Kind::Argument:
            arguments.append(incoming.argument);
            continue;
        case IncomingValue::Kind::EdgeResult:
            return std::nullopt;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    return arguments;
}

ValueID InductionFinder::findStart(std::span<const ValueID> enteringArguments)
{
    bool isPassedOnEveryEdge = !enteringArguments.empty() && std::ranges::all_of(enteringArguments, [](ValueID argument) {
        return !!argument;
    });
    if (!isPassedOnEveryEdge)
        return { };

    ValueID first = enteringArguments.front();
    bool isOneRunTimeValue = std::ranges::all_of(enteringArguments, [&](ValueID argument) {
        return haveEqualValues(first, argument);
    });
    if (!isOneRunTimeValue)
        return { };

    bool isOneDefinition = std::ranges::all_of(enteringArguments, [&](ValueID argument) {
        return argument == first;
    });
    if (isOneDefinition || integerConstantOf(first))
        return first;
    return m_analyses.identityOf(first);
}

bool InductionFinder::haveEqualValues(ValueID start, ValueID argument)
{
    if (m_analyses.identities().areSameValue(start, argument))
        return true;
    std::optional<int64_t> constant = integerConstantOf(argument);
    return constant && constant == integerConstantOf(start);
}

void InductionFinder::collectSteps(ValueID stepped, InductionSteps& steps)
{
    steps.accumulatedAtParameter.clear();
    steps.pending.append(PendingValue { stepped, 0 });
    while (!steps.pending.isEmpty() && !steps.isUnknown)
        visit(steps.pending.takeLast(), steps);
}

void InductionFinder::visit(PendingValue pending, InductionSteps& steps)
{
    ValueID id = numericIdentityOf(pending.value);
    if (id == steps.parameter) {
        steps.totals = steps.totals.unionWith(Range::one(pending.accumulated));
        return;
    }

    ValueLocation location = m_useLists.locationOf(id);
    if (!isInLoop(location.block)) {
        steps.isUnknown = true;
        return;
    }

    switch (location.placement) {
    case ValuePlacement::Detached:
        steps.isUnknown = true;
        return;
    case ValuePlacement::Parameter:
        visitBlockParameter(id, location, pending.accumulated, steps);
        return;
    case ValuePlacement::InBlock:
        visitOperation(m_function.at(id), pending.accumulated, steps);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void InductionFinder::visitBlockParameter(ValueID parameter, ValueLocation location, int64_t accumulated, InductionSteps& steps)
{
    if (location.block == m_loop.header()) {
        steps.isUnknown = true;
        return;
    }

    auto visited = steps.accumulatedAtParameter.add(parameter, accumulated);
    if (!visited.isNewEntry) {
        steps.isUnknown |= visited.iterator->value != accumulated;
        return;
    }

    for (Edge edge : m_cfg.incomingEdges(location.block)) {
        ValueID incoming = valuePassedOn(m_function, edge, location.index);
        if (!incoming) {
            steps.isUnknown = true;
            return;
        }
        steps.pending.append(PendingValue { incoming, accumulated });
    }
}

void InductionFinder::visitOperation(const Value& value, int64_t accumulated, InductionSteps& steps)
{
    switch (value.opcode) {
    case Opcode::Increment:
        enqueueOperand(value.operand(0), CheckedInt64(accumulated) + int64_t { 1 }, steps);
        return;
    case Opcode::Decrement:
        enqueueOperand(value.operand(0), CheckedInt64(accumulated) - int64_t { 1 }, steps);
        return;
    case Opcode::Add:
        if (std::optional<int64_t> constant = integerConstantOf(value.operand(1))) {
            enqueueOperand(value.operand(0), CheckedInt64(accumulated) + *constant, steps);
            return;
        }
        if (std::optional<int64_t> constant = integerConstantOf(value.operand(0))) {
            enqueueOperand(value.operand(1), CheckedInt64(accumulated) + *constant, steps);
            return;
        }
        visitInvariantStep(value, accumulated, steps);
        return;
    case Opcode::Sub:
        if (std::optional<int64_t> constant = integerConstantOf(value.operand(1))) {
            enqueueOperand(value.operand(0), CheckedInt64(accumulated) - *constant, steps);
            return;
        }
        visitInvariantStep(value, accumulated, steps);
        return;
    default:
        steps.isUnknown = true;
        return;
    }
}

void InductionFinder::visitInvariantStep(const Value& sum, int64_t accumulated, InductionSteps& steps)
{
    auto isStep = [&](ValueID variable, ValueID amount) {
        return numericIdentityOf(variable) == steps.parameter && isInvariantIn(m_analyses, m_loop, amount);
    };

    ValueID amount;
    if (isStep(sum.operand(0), sum.operand(1)))
        amount = sum.operand(1);
    else if (sum.opcode == Opcode::Add && isStep(sum.operand(1), sum.operand(0)))
        amount = sum.operand(0);
    if (!amount || accumulated) {
        steps.isUnknown = true;
        return;
    }

    InductionStep step { InductionStepKind::Invariant, 0, m_analyses.identityOf(amount), sum.opcode == Opcode::Sub };
    if (!steps.invariant)
        steps.invariant = step;
    else if (*steps.invariant != step)
        steps.isUnknown = true;
}

void InductionFinder::enqueueOperand(ValueID operand, CheckedInt64 accumulated, InductionSteps& steps)
{
    if (accumulated.hasOverflowed()) {
        steps.isUnknown = true;
        return;
    }
    steps.pending.append(PendingValue { operand, accumulated.value() });
}

InductionStep InductionFinder::stepOf(const InductionSteps& steps)
{
    if (steps.invariant && steps.totals.isEmpty())
        return *steps.invariant;
    if (!steps.invariant && steps.totals.isSingleValue())
        return { InductionStepKind::Constant, steps.totals.minimum, ValueID(), false };
    return { };
}

InductionDirection InductionFinder::directionOf(const InductionSteps& steps) const
{
    if (!steps.invariant)
        return directionOfAmounts(steps.totals);

    const Type& type = typeOf(steps.invariant->invariant);
    InductionDirection direction = type.isInteger() ? directionOfAmounts(type.integers) : InductionDirection::Unknown;
    if (steps.invariant->isSubtracted)
        direction = oppositeOf(direction);
    if (!steps.totals.isEmpty() && directionOfAmounts(steps.totals) != direction)
        return InductionDirection::Unknown;
    return direction;
}

void InductionFinder::findTests(Variables& variables)
{
    if (variables.isEmpty())
        return;
    for (BlockID block : findTestingBlocks())
        findTestsIn(block, variables);
}

Vector<BlockID, 4> InductionFinder::findTestingBlocks() const
{
    Vector<BlockID, 4> blocks;
    for (unsigned i = 0; i < m_loop.size(); ++i) {
        BlockID block = m_loop.at(i);
        const Terminator& terminator = m_function.at(block).terminator;
        if (!terminator.isConditionalBranch())
            continue;
        if (isInLoop(terminator.taken().block) == isInLoop(terminator.notTaken().block))
            continue;
        if (relationalOperators.contains(m_function.at(terminator.conditionOperand()).opcode))
            blocks.append(block);
    }
    AOT::sort(blocks, { }, [&](BlockID block) {
        return m_cfg.reversePostOrderIndex(block);
    });
    return blocks;
}

void InductionFinder::findTestsIn(BlockID block, Variables& variables)
{
    const Terminator& terminator = m_function.at(block).terminator;
    const Value& comparison = m_function.at(terminator.conditionOperand());
    bool staysOnTaken = isInLoop(terminator.taken().block);
    if (!staysOnTaken && (!isNeverNaN(comparison.operand(0)) || !isNeverNaN(comparison.operand(1))))
        return;

    std::optional<LoopExits> exits;
    for (uint32_t side = 0; side < 2; ++side) {
        std::optional<TestedVariable> tested = findTestedVariable(variables, numericIdentityOf(comparison.operand(side)));
        if (!tested)
            continue;
        if (!exits)
            exits = isOnEveryTrip(block) ? exitsOf(m_analyses, m_loop, block) : LoopExits::Unknown;

        Opcode relation = side ? mirroredComparison(comparison.opcode) : comparison.opcode;
        ValueID bound = comparison.operand(1 - side);
        InductionTest test {
            .block = block,
            .comparison = terminator.conditionOperand(),
            .relation = staysOnTaken ? relation : negatedComparison(relation),
            .tested = tested->tested,
            .bound = bound,
            .boundKind = boundKindOf(bound),
            .stayingEdge = Edge { block, staysOnTaken ? 0u : 1u },
            .leavingEdge = Edge { block, staysOnTaken ? 1u : 0u },
            .entryRelation = EntryRelation::Unknown,
            .exits = *exits,
        };
        test.entryRelation = entryRelationOf(tested->variable->start, test);
        tested->variable->tests.append(test);
    }
}

std::optional<TestedVariable> InductionFinder::findTestedVariable(Variables& variables, ValueID id)
{
    for (InductionVariable& variable : variables) {
        if (variable.parameter == id)
            return TestedVariable { &variable, TestedValue::BeforeStep };
    }
    for (InductionVariable& variable : variables) {
        if (variable.steppedValues.contains(id))
            return TestedVariable { &variable, TestedValue::AfterStep };
    }
    return std::nullopt;
}

LoopBoundKind InductionFinder::boundKindOf(ValueID bound)
{
    if (isInvariantIn(m_analyses, m_loop, bound))
        return LoopBoundKind::Invariant;
    if (isComputedInLoop(bound))
        return LoopBoundKind::ComputedInLoop;
    return LoopBoundKind::Unknown;
}

bool InductionFinder::isComputedInLoop(ValueID bound)
{
    ValueID definition = m_analyses.identityOf(bound);
    ValueLocation location = m_useLists.locationOf(definition);
    if (location.placement != ValuePlacement::InBlock || !isInLoop(location.block))
        return false;
    if (!m_analyses.isUnobservable(definition))
        return false;

    bool hasInvariantOperands = true;
    m_function.at(definition).forEachOperand([&](ValueID operand, uint32_t) {
        hasInvariantOperands &= isInvariantIn(m_analyses, m_loop, operand);
    });
    return hasInvariantOperands;
}

EntryRelation InductionFinder::entryRelationOf(ValueID start, const InductionTest& test)
{
    if (!start || test.boundKind != LoopBoundKind::Invariant)
        return EntryRelation::Unknown;

    ValueID bound = isInLoop(m_useLists.blockOf(test.bound)) ? m_analyses.identityOf(test.bound) : test.bound;
    if (!isNeverNaN(start) || !isNeverNaN(bound))
        return EntryRelation::Unknown;

    bool holdsOnEveryEdge = std::ranges::all_of(m_enteringEdges, [&](Edge edge) {
        return proveRelation(test.relation, start, bound, edge.from) == IntegerRelations::Answer::Proven;
    });
    return holdsOnEveryEdge ? EntryRelation::Holds : EntryRelation::Unknown;
}

IntegerRelations::Answer InductionFinder::proveRelation(Opcode relation, ValueID left, ValueID right, BlockID at)
{
    switch (relation) {
    case Opcode::Less:
        return m_relations.isAtMost(left, right, -1, at);
    case Opcode::LessEq:
        return m_relations.isAtMost(left, right, 0, at);
    case Opcode::Greater:
        return m_relations.isAtMost(right, left, -1, at);
    case Opcode::GreaterEq:
        return m_relations.isAtMost(right, left, 0, at);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool InductionFinder::isOnEveryTrip(BlockID testingBlock) const
{
    return std::ranges::all_of(m_backEdges, [&](Edge edge) {
        if (edge.from == testingBlock)
            return !edge.isHandlerEdge();
        return m_dominators.terminatorDominates(testingBlock, edge.from);
    });
}

ValueID InductionFinder::numericIdentityOf(ValueID id)
{
    id = m_analyses.identityOf(id);
    for (;;) {
        const Value& value = m_function.at(id);
        bool isConversion = value.opcode == Opcode::ToNumber || value.opcode == Opcode::ToNumeric;
        if (!isConversion || !typeOf(value.operand(0)).isNumber())
            return id;
        id = m_analyses.identityOf(value.operand(0));
    }
}

bool InductionFinder::isNeverNaN(ValueID id) const
{
    const Type& type = typeOf(id);
    return type.isNumber() && !type.mayBeNaN();
}

} // anonymous namespace

const InductionTest* InductionVariable::controllingTest() const
{
    for (const InductionTest& test : tests) {
        if (test.exits == LoopExits::OnlyAtTest)
            return &test;
    }
    return nullptr;
}

const InductionTest* InductionVariable::boundingTest() const
{
    for (const InductionTest& test : tests) {
        if (test.exits != LoopExits::Unknown && isBoundingRelation(test.relation, direction))
            return &test;
    }
    return nullptr;
}

const InductionVariable* LoopInduction::find(ValueID parameter) const
{
    for (const InductionVariable& variable : variables) {
        if (variable.parameter == parameter)
            return &variable;
    }
    return nullptr;
}

const InductionVariable* LoopInduction::counter() const
{
    for (const InductionVariable& variable : variables) {
        if (variable.step.kind != InductionStepKind::Constant || !variable.step.constant)
            continue;
        const InductionTest* test = variable.controllingTest();
        if (test && isBoundingRelation(test->relation, variable.direction))
            return &variable;
    }
    return nullptr;
}

InductionVariables::InductionVariables(FunctionAnalyses& analyses)
{
    const NaturalLoops& loops = analyses.naturalLoops();
    m_loops.reserveInitialCapacity(loops.numLoops());
    for (unsigned index = 0; index < loops.numLoops(); ++index)
        m_loops.append(InductionFinder(analyses, loops.loop(index)).find());
}

const LoopInduction& InductionVariables::of(const NaturalLoop& loop) const
{
    return m_loops[loop.index()];
}

const LoopInduction* InductionVariables::ofHeader(BlockID block) const
{
    for (const LoopInduction& loop : m_loops) {
        if (loop.header == block)
            return &loop;
    }
    return nullptr;
}

const InductionVariable* InductionVariables::find(ValueID headerParameter) const
{
    for (const LoopInduction& loop : m_loops) {
        if (const InductionVariable* variable = loop.find(headerParameter))
            return variable;
    }
    return nullptr;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
